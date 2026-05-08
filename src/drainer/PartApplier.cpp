/* Copyright (c) 2024 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include "drainer/PartApplier.h"

#include <glog/logging.h>

#include <chrono>
#include <thread>

#include "common/datatypes/KeyValue.h"
#include "common/utils/NebulaKeyUtils.h"
#include "drainer/BackupClientCache.h"
#include "drainer/CheckpointStore.h"
#include "drainer/DrainerEnv.h"
#include "drainer/DrainerFlags.h"
#include "drainer/MetaApplier.h"
#include "kvstore/LogEncoder.h"

namespace nebula {
namespace drainer {

// Exponential backoff multipliers: base, base*5, base*20, ...
// e.g., with base=100ms: 100ms, 500ms, 2000ms
static const int kBackoffMultipliers[] = {1, 5, 20};
static const int kBackoffMultipliersSize = sizeof(kBackoffMultipliers) / sizeof(kBackoffMultipliers[0]);

PartApplier::PartApplier(GraphSpaceID spaceId, PartitionID partId, DrainerEnv* env)
    : spaceId_(spaceId), partId_(partId), env_(env) {}

PartApplier::~PartApplier() {
  stop();
}

void PartApplier::start() {
  stop_.store(false, std::memory_order_release);
  worker_ = std::thread([this]() { run_(); });
  LOG(INFO) << "PartApplier started for space=" << spaceId_ << " part=" << partId_;
}

void PartApplier::stop() {
  if (stop_.load(std::memory_order_acquire)) {
    return;
  }
  stop_.store(true, std::memory_order_release);
  cv_.notify_one();
  if (worker_.joinable()) {
    worker_.join();
  }
  LOG(INFO) << "PartApplier stopped for space=" << spaceId_ << " part=" << partId_;
}

folly::Future<LogID> PartApplier::enqueue(sync::cpp2::SyncLogBatch&& batch) {
  folly::Promise<LogID> promise;
  auto future = promise.getFuture();
  {
    std::lock_guard<std::mutex> lk(queueLock_);
    queue_.emplace_back(QueueItem{std::move(batch), std::move(promise)});
  }
  cv_.notify_one();
  return future;
}

void PartApplier::run_() {
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

    // Apply the batch
    applyBatch_(item.batch);

    // Update last applied and fulfill the promise
    auto lastLogId = *item.batch.lastLogId_ref();
    lastApplied_.store(lastLogId, std::memory_order_release);
    item.promise.setValue(lastLogId);

    VLOG(2) << "PartApplier applied batch for space=" << spaceId_ << " part=" << partId_
             << " lastLogId=" << lastLogId;
  }

  // Drain remaining items on shutdown and reject them
  std::lock_guard<std::mutex> lk(queueLock_);
  for (auto& item : queue_) {
    item.promise.setException(
        std::runtime_error("PartApplier shutting down"));
  }
  queue_.clear();
}

bool PartApplier::waitForSchema_(const sync::cpp2::SyncLogEntry& entry) {
  if (!entry.schemaVer_ref().has_value()) {
    return true;
  }
  auto schemaVer = *entry.schemaVer_ref();
  if (env_->metaApplier()->schemaVisible(spaceId_, schemaVer)) {
    return true;
  }

  // Poll until the schema becomes visible or we hit the timeout
  auto pollMs = FLAGS_drainer_schema_visibility_poll_ms;
  auto timeoutMs = FLAGS_drainer_schema_visibility_timeout_ms;
  int64_t elapsedMs = 0;

  VLOG(2) << "waitForSchema_ waiting for schemaVer=" << schemaVer
           << " space=" << spaceId_ << " part=" << partId_;

  while (elapsedMs < timeoutMs) {
    /* sleep override */ std::this_thread::sleep_for(std::chrono::milliseconds(pollMs));
    elapsedMs += pollMs;
    if (env_->metaApplier()->schemaVisible(spaceId_, schemaVer)) {
      VLOG(2) << "waitForSchema_ schemaVer=" << schemaVer << " became visible after "
               << elapsedMs << "ms, space=" << spaceId_ << " part=" << partId_;
      return true;
    }
  }

  LOG(ERROR) << "waitForSchema_ timed out waiting for schemaVer=" << schemaVer
             << " after " << timeoutMs << "ms, space=" << spaceId_
             << " part=" << partId_ << " logId=" << *entry.logId_ref();
  return false;
}

void PartApplier::retryOnFailure_(const std::string& opName,
                                  LogID logId,
                                  std::function<bool()> func) {
  int maxRetries = FLAGS_drainer_apply_retry_times;
  int baseIntervalMs = FLAGS_drainer_apply_retry_interval_ms;

  // First attempt (not a retry)
  if (func()) {
    return;
  }

  // Retry loop with exponential backoff
  for (int attempt = 0; attempt < maxRetries; ++attempt) {
    int multiplierIdx = std::min(attempt, kBackoffMultipliersSize - 1);
    int sleepMs = baseIntervalMs * kBackoffMultipliers[multiplierIdx];
    LOG(WARNING) << opName << " retry " << (attempt + 1) << "/" << maxRetries
                 << " after " << sleepMs << "ms, space=" << spaceId_
                 << " part=" << partId_ << " logId=" << logId;
    /* sleep override */
    std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));

    if (func()) {
      LOG(INFO) << opName << " succeeded on retry " << (attempt + 1)
                << ", space=" << spaceId_ << " part=" << partId_ << " logId=" << logId;
      return;
    }
  }

  // All retries exhausted — throw to propagate to DrainerService
  auto msg = folly::stringPrintf(
      "%s failed after %d retries, space=%d part=%d logId=%ld",
      opName.c_str(), maxRetries, spaceId_, partId_, logId);
  LOG(ERROR) << msg;
  throw std::runtime_error(msg);
}

// static
bool PartApplier::isInEdgeKey_(folly::StringPiece key) {
  // Edge key format: type(1) + partId(3) + srcId(vIdLen) + edgeType(4) +
  //                  edgeRank(8) + dstId(vIdLen) + placeHolder(1)
  // Total: kEdgeLen + 2 * vIdLen
  //   where kEdgeLen = sizeof(PartitionID) + sizeof(EdgeType) +
  //                    sizeof(EdgeRanking) + sizeof(EdgeVerPlaceHolder)
  //                  = 4 + 4 + 8 + 1 = 17
  //
  // First, check the key type byte to see if it could be an edge.
  if (key.size() <= static_cast<size_t>(kEdgeLen)) {
    return false;
  }
  constexpr int32_t typeLen = static_cast<int32_t>(sizeof(NebulaKeyType));
  auto typeVal = readInt<uint32_t>(key.data(), typeLen) & kTypeMask;
  if (static_cast<NebulaKeyType>(typeVal) != NebulaKeyType::kEdge) {
    return false;
  }

  // Derive vIdLen from key size: keyLen = kEdgeLen + 2 * vIdLen
  auto remaining = static_cast<int64_t>(key.size()) - kEdgeLen;
  if (remaining <= 0 || remaining % 2 != 0) {
    return false;
  }
  auto vIdLen = static_cast<size_t>(remaining / 2);

  // Verify it's actually a valid edge key (checks size and trailing placeholder)
  if (!NebulaKeyUtils::isEdge(vIdLen, key)) {
    return false;
  }

  // Read the edgeType. Negative edgeType means it's an in-edge.
  auto edgeType = NebulaKeyUtils::getEdgeType(vIdLen, key);
  return edgeType < 0;
}

void PartApplier::applyBatch_(const sync::cpp2::SyncLogBatch& batch) {
  const auto& entries = *batch.entries_ref();
  for (const auto& entry : entries) {
    auto op = *entry.op_ref();
    switch (op) {
      case sync::cpp2::LogOp::OP_PUT:
        applyPut_(entry);
        break;
      case sync::cpp2::LogOp::OP_MULTI_PUT:
        applyMultiPut_(entry);
        break;
      case sync::cpp2::LogOp::OP_REMOVE:
        applyRemove_(entry);
        break;
      case sync::cpp2::LogOp::OP_MULTI_REMOVE:
        applyMultiRemove_(entry);
        break;
      case sync::cpp2::LogOp::OP_REMOVE_RANGE:
        applyRemoveRange_(entry);
        break;
      case sync::cpp2::LogOp::OP_BATCH_WRITE:
        applyBatchWrite_(entry);
        break;
      default:
        LOG(WARNING) << "Unknown LogOp: " << static_cast<int>(op)
                     << " in space=" << spaceId_ << " part=" << partId_
                     << " logId=" << *entry.logId_ref();
        break;
    }
  }

  // Persist checkpoint after the entire batch has been applied
  auto lastLogId = *batch.lastLogId_ref();
  if (!env_->checkpointStore()->commit(spaceId_, partId_, lastLogId)) {
    LOG(ERROR) << "Failed to persist checkpoint, space=" << spaceId_
               << " part=" << partId_ << " lastLogId=" << lastLogId;
  }
}

void PartApplier::applyPut_(const sync::cpp2::SyncLogEntry& entry) {
  const auto& payload = *entry.payload_ref();
  auto logId = *entry.logId_ref();

  // Schema barrier: wait for the referenced schema to be visible on the backup cluster
  if (!waitForSchema_(entry)) {
    LOG(WARNING) << "applyPut_ skipping entry due to schema barrier timeout, space="
                 << spaceId_ << " part=" << partId_ << " logId=" << logId;
    return;
  }

  // OP_PUT is encoded via encodeMultiValues(OP_PUT, key, value), so decode
  // yields exactly 2 pieces: [key, value].
  auto pieces = kvstore::decodeMultiValues(folly::StringPiece(payload));
  if (pieces.size() != 2) {
    LOG(ERROR) << "applyPut_ unexpected decode size=" << pieces.size()
               << " space=" << spaceId_ << " part=" << partId_ << " logId=" << logId;
    return;
  }

  auto key = pieces[0];
  auto val = pieces[1];

  // Skip system keys (commit markers, partition metadata, balance keys) — these
  // are local to each cluster and must not be replicated.
  if (NebulaKeyUtils::isSystem(key)) {
    VLOG(3) << "applyPut_ skipping system key, space=" << spaceId_
             << " part=" << partId_ << " logId=" << logId;
    return;
  }

  // Skip in-edge keys — the backup cluster's storage layer auto-generates
  // in-edges when the corresponding out-edge is inserted.
  if (isInEdgeKey_(key)) {
    VLOG(3) << "applyPut_ skipping in-edge key (dedup), space=" << spaceId_
             << " part=" << partId_ << " logId=" << logId;
    return;
  }

  // TODO: Partition rerouting — the key's embedded partId may differ between
  //   clusters with different partition_num. Extract vertexId, hash to the
  //   correct partId in the backup cluster, and rewrite the key prefix.

  // Build KeyValue and send to backup cluster with retry
  auto keyStr = key.toString();
  auto valStr = val.toString();

  retryOnFailure_("applyPut_", logId, [this, &keyStr, &valStr]() -> bool {
    std::vector<KeyValue> kvs;
    KeyValue kv;
    kv.key = keyStr;
    kv.value = valStr;
    kvs.emplace_back(std::move(kv));
    auto* client = env_->backupClientCache()->backupStorageClient();
    auto future = client->put(spaceId_, std::move(kvs));
    auto resp = std::move(future).get();
    return resp.succeeded();
  });

  VLOG(3) << "applyPut_ space=" << spaceId_ << " part=" << partId_
           << " logId=" << logId << " payloadSize=" << payload.size();
}

void PartApplier::applyMultiPut_(const sync::cpp2::SyncLogEntry& entry) {
  const auto& payload = *entry.payload_ref();
  auto logId = *entry.logId_ref();

  // Schema barrier: wait for the referenced schema to be visible on the backup cluster
  if (!waitForSchema_(entry)) {
    LOG(WARNING) << "applyMultiPut_ skipping entry due to schema barrier timeout, space="
                 << spaceId_ << " part=" << partId_ << " logId=" << logId;
    return;
  }

  // OP_MULTI_PUT is encoded via encodeMultiValues(OP_MULTI_PUT, vector<KV>),
  // which stores 2*N strings: [key0, val0, key1, val1, ...].
  auto pieces = kvstore::decodeMultiValues(folly::StringPiece(payload));
  if (pieces.size() % 2 != 0) {
    LOG(ERROR) << "applyMultiPut_ odd decode size=" << pieces.size()
               << " space=" << spaceId_ << " part=" << partId_ << " logId=" << logId;
    return;
  }

  std::vector<KeyValue> kvs;
  kvs.reserve(pieces.size() / 2);
  for (size_t i = 0; i < pieces.size(); i += 2) {
    auto key = pieces[i];

    // Skip system keys
    if (NebulaKeyUtils::isSystem(key)) {
      continue;
    }

    // Skip in-edge keys (dedup — backup cluster auto-generates in-edges)
    if (isInEdgeKey_(key)) {
      continue;
    }

    // TODO: Partition rerouting for different partition_num across clusters.
    KeyValue kv;
    kv.key = key.toString();
    kv.value = pieces[i + 1].toString();
    kvs.emplace_back(std::move(kv));
  }

  if (kvs.empty()) {
    VLOG(3) << "applyMultiPut_ all keys filtered, space=" << spaceId_
             << " part=" << partId_ << " logId=" << logId;
    return;
  }

  retryOnFailure_("applyMultiPut_", logId, [this, &kvs]() -> bool {
    auto kvsCopy = kvs;
    auto* client = env_->backupClientCache()->backupStorageClient();
    auto future = client->put(spaceId_, std::move(kvsCopy));
    auto resp = std::move(future).get();
    return resp.succeeded();
  });

  VLOG(3) << "applyMultiPut_ space=" << spaceId_ << " part=" << partId_
           << " logId=" << logId << " payloadSize=" << payload.size();
}

void PartApplier::applyRemove_(const sync::cpp2::SyncLogEntry& entry) {
  const auto& payload = *entry.payload_ref();
  auto logId = *entry.logId_ref();

  // OP_REMOVE is encoded via encodeSingleValue(OP_REMOVE, key).
  auto key = kvstore::decodeSingleValue(folly::StringPiece(payload));

  // Skip system keys
  if (NebulaKeyUtils::isSystem(key)) {
    VLOG(3) << "applyRemove_ skipping system key, space=" << spaceId_
             << " part=" << partId_ << " logId=" << logId;
    return;
  }

  // TODO: Partition rerouting for different partition_num across clusters.

  auto keyStr = key.toString();

  retryOnFailure_("applyRemove_", logId, [this, &keyStr]() -> bool {
    std::vector<std::string> keys;
    keys.emplace_back(keyStr);
    auto* client = env_->backupClientCache()->backupStorageClient();
    auto future = client->remove(spaceId_, std::move(keys));
    auto resp = std::move(future).get();
    return resp.succeeded();
  });

  VLOG(3) << "applyRemove_ space=" << spaceId_ << " part=" << partId_
           << " logId=" << logId << " payloadSize=" << payload.size();
}

void PartApplier::applyMultiRemove_(const sync::cpp2::SyncLogEntry& entry) {
  const auto& payload = *entry.payload_ref();
  auto logId = *entry.logId_ref();

  // OP_MULTI_REMOVE is encoded via encodeMultiValues(OP_MULTI_REMOVE, keys),
  // which stores N strings: [key0, key1, ...].
  auto pieces = kvstore::decodeMultiValues(folly::StringPiece(payload));

  std::vector<std::string> keys;
  keys.reserve(pieces.size());
  for (const auto& key : pieces) {
    // Skip system keys
    if (NebulaKeyUtils::isSystem(key)) {
      continue;
    }
    // TODO: Partition rerouting for different partition_num across clusters.
    keys.emplace_back(key.toString());
  }

  if (keys.empty()) {
    VLOG(3) << "applyMultiRemove_ all keys filtered, space=" << spaceId_
             << " part=" << partId_ << " logId=" << logId;
    return;
  }

  retryOnFailure_("applyMultiRemove_", logId, [this, &keys]() -> bool {
    auto keysCopy = keys;
    auto* client = env_->backupClientCache()->backupStorageClient();
    auto future = client->remove(spaceId_, std::move(keysCopy));
    auto resp = std::move(future).get();
    return resp.succeeded();
  });

  VLOG(3) << "applyMultiRemove_ space=" << spaceId_ << " part=" << partId_
           << " logId=" << logId << " payloadSize=" << payload.size();
}

void PartApplier::applyRemoveRange_(const sync::cpp2::SyncLogEntry& entry) {
  const auto& payload = *entry.payload_ref();
  auto logId = *entry.logId_ref();

  // OP_REMOVE_RANGE is encoded via encodeMultiValues(OP_REMOVE_RANGE, start, end),
  // yielding 2 pieces: [startKey, endKey).
  auto pieces = kvstore::decodeMultiValues(folly::StringPiece(payload));
  if (pieces.size() != 2) {
    LOG(ERROR) << "applyRemoveRange_ unexpected decode size=" << pieces.size()
               << " space=" << spaceId_ << " part=" << partId_ << " logId=" << logId;
    return;
  }

  // StorageClient does not expose a removeRange RPC. We cannot enumerate keys
  // in the range without access to the backup's local KVStore. For now, log a
  // warning. A full implementation would either:
  //   (a) Use an admin/internal RPC that supports range deletion, or
  //   (b) Scan the range on the backup side and issue individual removes.
  // TODO: Implement removeRange support via an internal storage RPC or
  //   by scanning the backup KVStore.
  LOG(WARNING) << "applyRemoveRange_ not fully supported yet (no removeRange RPC), "
               << "space=" << spaceId_ << " part=" << partId_ << " logId=" << logId
               << " startKeySize=" << pieces[0].size()
               << " endKeySize=" << pieces[1].size();
}

void PartApplier::applyBatchWrite_(const sync::cpp2::SyncLogEntry& entry) {
  const auto& payload = *entry.payload_ref();
  auto logId = *entry.logId_ref();

  // Schema barrier: wait for the referenced schema to be visible on the backup cluster
  if (!waitForSchema_(entry)) {
    LOG(WARNING) << "applyBatchWrite_ skipping entry due to schema barrier timeout, space="
                 << spaceId_ << " part=" << partId_ << " logId=" << logId;
    return;
  }

  // OP_BATCH_WRITE is encoded via encodeBatchValue(). Each element is
  // (BatchLogType, key, value) — value is empty for REMOVE operations.
  auto batchData = kvstore::decodeBatchValue(folly::StringPiece(payload));

  // Decompose the batch into puts and removes, since StorageClient only
  // exposes put() and remove() RPCs.
  std::vector<KeyValue> puts;
  std::vector<std::string> removes;

  for (const auto& op : batchData) {
    auto key = op.second.first;

    // Skip system keys
    if (NebulaKeyUtils::isSystem(key)) {
      continue;
    }

    // Skip in-edge keys (dedup — backup cluster auto-generates in-edges)
    if (isInEdgeKey_(key)) {
      continue;
    }

    // TODO: Partition rerouting for different partition_num across clusters.

    switch (op.first) {
      case kvstore::BatchLogType::OP_BATCH_PUT: {
        {
          KeyValue kv;
          kv.key = key.toString();
          kv.value = op.second.second.toString();
          puts.emplace_back(std::move(kv));
        }
        break;
      }
      case kvstore::BatchLogType::OP_BATCH_REMOVE: {
        removes.emplace_back(key.toString());
        break;
      }
      case kvstore::BatchLogType::OP_BATCH_REMOVE_RANGE: {
        // Same limitation as applyRemoveRange_ — no range delete RPC.
        // TODO: Implement via internal storage RPC or scan + remove.
        LOG(WARNING) << "applyBatchWrite_ skipping REMOVE_RANGE sub-op, space="
                     << spaceId_ << " part=" << partId_ << " logId=" << logId;
        break;
      }
    }
  }

  // Apply puts with retry
  if (!puts.empty()) {
    retryOnFailure_("applyBatchWrite_put", logId, [this, &puts]() -> bool {
      auto putsCopy = puts;
      auto* client = env_->backupClientCache()->backupStorageClient();
      auto future = client->put(spaceId_, std::move(putsCopy));
      auto resp = std::move(future).get();
      return resp.succeeded();
    });
  }

  // Apply removes with retry
  if (!removes.empty()) {
    retryOnFailure_("applyBatchWrite_remove", logId, [this, &removes]() -> bool {
      auto removesCopy = removes;
      auto* client = env_->backupClientCache()->backupStorageClient();
      auto future = client->remove(spaceId_, std::move(removesCopy));
      auto resp = std::move(future).get();
      return resp.succeeded();
    });
  }

  VLOG(3) << "applyBatchWrite_ space=" << spaceId_ << " part=" << partId_
           << " logId=" << logId << " payloadSize=" << payload.size();
}

}  // namespace drainer
}  // namespace nebula
