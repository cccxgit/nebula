/* Copyright (c) 2024 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include "drainer/MetaApplier.h"

#include <glog/logging.h>

#include "clients/meta/MetaClient.h"
#include "drainer/BackupClientCache.h"
#include "drainer/DrainerEnv.h"

namespace nebula {
namespace drainer {

MetaApplier::MetaApplier(DrainerEnv* env) : env_(env) {}

MetaApplier::~MetaApplier() {
  stop();
}

void MetaApplier::start() {
  stop_.store(false, std::memory_order_release);
  worker_ = std::thread([this]() { run_(); });
  LOG(INFO) << "MetaApplier started";
}

void MetaApplier::stop() {
  if (stop_.load(std::memory_order_acquire)) {
    return;
  }
  stop_.store(true, std::memory_order_release);
  cv_.notify_one();
  if (worker_.joinable()) {
    worker_.join();
  }
  LOG(INFO) << "MetaApplier stopped";
}

folly::Future<LogID> MetaApplier::enqueue(sync::cpp2::SyncLogBatch&& batch) {
  folly::Promise<LogID> promise;
  auto future = promise.getFuture();
  {
    std::lock_guard<std::mutex> lk(queueLock_);
    queue_.emplace_back(QueueItem{std::move(batch), std::move(promise)});
  }
  cv_.notify_one();
  return future;
}

bool MetaApplier::schemaVisible(GraphSpaceID spaceId, int64_t schemaVer) const {
  std::lock_guard<std::mutex> lk(schemaLock_);
  auto it = schemaVisible_.find(std::make_pair(spaceId, schemaVer));
  if (it == schemaVisible_.end()) {
    return false;
  }
  return it->second;
}

void MetaApplier::parkUntilSchemaReady(GraphSpaceID spaceId,
                                       int64_t schemaVer,
                                       std::function<void()> callback) {
  // Check if already visible
  {
    std::lock_guard<std::mutex> lk(schemaLock_);
    auto it = schemaVisible_.find(std::make_pair(spaceId, schemaVer));
    if (it != schemaVisible_.end() && it->second) {
      // Already visible, invoke immediately
      callback();
      return;
    }
  }

  // Park the callback
  std::lock_guard<std::mutex> lk(parkedLock_);
  parked_[std::make_pair(spaceId, schemaVer)].emplace_back(std::move(callback));
}

void MetaApplier::run_() {
  while (!stop_.load(std::memory_order_acquire)) {
    QueueItem item;
    {
      std::unique_lock<std::mutex> lk(queueLock_);
      cv_.wait(lk, [this]() {
        return !queue_.empty() || stop_.load(std::memory_order_acquire);
      });
      if (stop_.load(std::memory_order_acquire) && queue_.empty()) {
        break;
      }
      if (queue_.empty()) {
        continue;
      }
      item = std::move(queue_.front());
      queue_.pop_front();
    }

    // Extract spaceId from the batch context
    auto spaceId = *item.batch.spaceId_ref();

    // Apply each entry in the batch
    const auto& entries = *item.batch.entries_ref();
    for (const auto& entry : entries) {
      applyOne_(entry, spaceId);
    }

    // Fulfill promise with the last log id
    auto lastLogId = *item.batch.lastLogId_ref();
    item.promise.setValue(lastLogId);

    VLOG(2) << "MetaApplier applied batch lastLogId=" << lastLogId;
  }

  // Drain remaining items on shutdown and reject them
  std::lock_guard<std::mutex> lk(queueLock_);
  for (auto& item : queue_) {
    item.promise.setException(
        std::runtime_error("MetaApplier shutting down"));
  }
  queue_.clear();
}

void MetaApplier::applyOne_(const sync::cpp2::SyncLogEntry& entry,
                            GraphSpaceID spaceId) {
  auto logId = *entry.logId_ref();
  auto op = *entry.op_ref();
  const auto& payload = *entry.payload_ref();

  LOG(INFO) << "MetaApplier applyOne_ logId=" << logId
            << " op=" << static_cast<int>(op)
            << " spaceId=" << spaceId
            << " payloadSize=" << payload.size();

  // Decode the payload into (key, value) pairs.
  // The payload was encoded by MetaSyncListener::encodeMetaPayload_():
  //   [keyLen (4 bytes)][key][valueLen (4 bytes)][value]
  auto kvPairs = decodeMetaPayload_(folly::StringPiece(payload));
  if (kvPairs.empty()) {
    LOG(WARNING) << "MetaApplier applyOne_ logId=" << logId
                 << " decoded 0 KV pairs from payload, skipping";
    return;
  }

  // Get the backup cluster's MetaClient
  auto* backupCache = env_->backupClientCache();
  meta::MetaClient* backupMeta = nullptr;
  if (backupCache != nullptr) {
    backupMeta = backupCache->backupMetaClient();
  }

  for (const auto& kv : kvPairs) {
    const auto& key = kv.first;
    const auto& value = kv.second;
    auto keyType = classifyMetaKey_(folly::StringPiece(key));

    bool isRemove = (op == sync::cpp2::LogOp::OP_REMOVE ||
                     op == sync::cpp2::LogOp::OP_MULTI_REMOVE);

    LOG(INFO) << "MetaApplier applying meta key type=" << keyType
              << " keySize=" << key.size()
              << " valueSize=" << value.size()
              << " isRemove=" << isRemove
              << " logId=" << logId;

    if (backupMeta == nullptr) {
      LOG(WARNING) << "MetaApplier: backup MetaClient not available, "
                   << "skipping apply for key type=" << keyType;
      continue;
    }

    if (isRemove) {
      // For remove operations, we need type-specific handling.
      // TODO(sync-P3): Implement remove for each meta type:
      //   - spaces: dropSpace
      //   - tags: dropTagSchema
      //   - edges: dropEdgeSchema
      //   - indexes: dropTagIndex / dropEdgeIndex
      //   - users: dropUser
      //   - roles: revokeRole
      LOG(INFO) << "MetaApplier: REMOVE op for key type=" << keyType
                << " not yet implemented, skipping";
    } else {
      // For put/multi-put operations, we need to replay the DDL on the
      // backup cluster using the typed MetaClient API. MetaClient does not
      // expose a raw KV put, so we must decode each key type and call the
      // appropriate high-level method.
      //
      // TODO(sync-P3): Implement typed replay for each meta key type:
      //   - SPACE:  decode SpaceDesc via MetaKeyUtils::parseSpace(value),
      //             call backupMeta->createSpace(spaceDesc)
      //   - TAG:    decode schema via MetaKeyUtils::parseSchema(value),
      //             extract tagId via MetaKeyUtils::parseTagId(key),
      //             call backupMeta->createTagSchema() or alterTagSchema()
      //   - EDGE:   decode schema via MetaKeyUtils::parseSchema(value),
      //             extract edgeType via MetaKeyUtils::parseEdgeType(key),
      //             call backupMeta->createEdgeSchema() or alterEdgeSchema()
      //   - INDEX:  decode IndexItem via MetaKeyUtils::parseIndex(value),
      //             call backupMeta->createTagIndex() or createEdgeIndex()
      //   - USER:   decode user info, call backupMeta->createUser() or alterUser()
      //   - ROLE:   decode role info, call backupMeta->grantRole()
      //
      // For now, log the operation so we can verify the decoding pipeline
      // works end-to-end, and mark schema as visible so PartAppliers unblock.
      LOG(INFO) << "MetaApplier: PUT op for key type=" << keyType
                << " keySize=" << key.size()
                << " valueSize=" << value.size()
                << " (typed replay not yet implemented, logId=" << logId << ")";
    }
  }

  // After successful meta application, mark schema as visible.
  // The schemaVer in the entry tells PartAppliers which version was
  // created/altered so they can proceed with dependent DML.
  if (entry.schemaVer_ref().has_value()) {
    auto schemaVer = *entry.schemaVer_ref();
    markVisible_(spaceId, schemaVer);
  }
}

void MetaApplier::markVisible_(GraphSpaceID spaceId, int64_t schemaVer) {
  {
    std::lock_guard<std::mutex> lk(schemaLock_);
    schemaVisible_[std::make_pair(spaceId, schemaVer)] = true;
  }
  releaseParked_(spaceId, schemaVer);
  VLOG(2) << "MetaApplier marked schema visible: space=" << spaceId
           << " schemaVer=" << schemaVer;
}

// static
std::vector<std::pair<std::string, std::string>>
MetaApplier::decodeMetaPayload_(folly::StringPiece payload) {
  std::vector<std::pair<std::string, std::string>> result;
  size_t offset = 0;
  while (offset + sizeof(uint32_t) <= payload.size()) {
    // Read key length
    uint32_t keyLen = 0;
    memcpy(&keyLen, payload.data() + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);

    if (offset + keyLen > payload.size()) {
      LOG(ERROR) << "MetaApplier decodeMetaPayload_: truncated key, "
                 << "keyLen=" << keyLen << " remaining=" << (payload.size() - offset);
      break;
    }
    std::string key(payload.data() + offset, keyLen);
    offset += keyLen;

    if (offset + sizeof(uint32_t) > payload.size()) {
      LOG(ERROR) << "MetaApplier decodeMetaPayload_: truncated value length header";
      break;
    }

    // Read value length
    uint32_t valueLen = 0;
    memcpy(&valueLen, payload.data() + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);

    if (offset + valueLen > payload.size()) {
      LOG(ERROR) << "MetaApplier decodeMetaPayload_: truncated value, "
                 << "valueLen=" << valueLen << " remaining=" << (payload.size() - offset);
      break;
    }
    std::string value(payload.data() + offset, valueLen);
    offset += valueLen;

    result.emplace_back(std::move(key), std::move(value));
  }
  return result;
}

// static
std::string MetaApplier::classifyMetaKey_(folly::StringPiece key) {
  // These prefixes match MetaSyncListener's replicable key prefixes
  static const std::string kMetaSpacesPrefix = "__spaces__";
  static const std::string kMetaTagsPrefix = "__tags__";
  static const std::string kMetaEdgesPrefix = "__edges__";
  static const std::string kMetaIndexesPrefix = "__indexes__";
  static const std::string kMetaUsersPrefix = "__users__";
  static const std::string kMetaRolesPrefix = "__roles__";

  if (key.startsWith(kMetaSpacesPrefix)) {
    return "SPACE";
  }
  if (key.startsWith(kMetaTagsPrefix)) {
    return "TAG";
  }
  if (key.startsWith(kMetaEdgesPrefix)) {
    return "EDGE";
  }
  if (key.startsWith(kMetaIndexesPrefix)) {
    return "INDEX";
  }
  if (key.startsWith(kMetaUsersPrefix)) {
    return "USER";
  }
  if (key.startsWith(kMetaRolesPrefix)) {
    return "ROLE";
  }
  return "UNKNOWN(" + key.subpiece(0, std::min<size_t>(key.size(), 16)).toString() + ")";
}

void MetaApplier::releaseParked_(GraphSpaceID spaceId, int64_t schemaVer) {
  std::vector<std::function<void()>> callbacks;
  {
    std::lock_guard<std::mutex> lk(parkedLock_);
    auto key = std::make_pair(spaceId, schemaVer);
    auto it = parked_.find(key);
    if (it == parked_.end()) {
      return;
    }
    callbacks = std::move(it->second);
    parked_.erase(it);
  }

  // Invoke all parked callbacks outside the lock
  for (auto& cb : callbacks) {
    cb();
  }

  VLOG(2) << "MetaApplier released " << callbacks.size()
           << " parked callbacks for space=" << spaceId
           << " schemaVer=" << schemaVer;
}

}  // namespace drainer
}  // namespace nebula
