/* Copyright (c) 2024 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include "meta/listener/MetaSyncListener.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/fs/FileUtils.h"
#include "kvstore/LogEncoder.h"
#include "meta/listener/MetaSyncListenerFlags.h"

namespace nebula {
namespace meta {

// Replicable meta key prefixes (whitelist)
// These correspond to schema/user/role operations that should be synced cross-cluster.
static const std::string kMetaSpacesPrefix = "__spaces__";
static const std::string kMetaTagsPrefix = "__tags__";
static const std::string kMetaEdgesPrefix = "__edges__";
static const std::string kMetaIndexesPrefix = "__indexes__";
static const std::string kMetaUsersPrefix = "__users__";
static const std::string kMetaRolesPrefix = "__roles__";

void MetaSyncListener::init() {
  // Determine dump path
  if (FLAGS_meta_sync_listener_dump_path.empty()) {
    dumpPath_ =
        folly::stringPrintf("%s/meta_sync_dump_%d_%d", walPath_.c_str(), spaceId_, partId_);
  } else {
    dumpPath_ = folly::stringPrintf(
        "%s/meta_sync_dump_%d_%d", FLAGS_meta_sync_listener_dump_path.c_str(), spaceId_, partId_);
  }

  // Create dump directory if it does not exist
  if (!fs::FileUtils::exist(dumpPath_)) {
    if (!fs::FileUtils::makeDir(dumpPath_)) {
      LOG(FATAL) << "Failed to create meta sync dump directory: " << dumpPath_;
    }
  }

  // Load last sent log id from checkpoint
  lastSentLogId_ = loadLastSent_();
  LOG(INFO) << idStr_ << "MetaSyncListener initialized, dumpPath=" << dumpPath_
            << ", lastSentLogId=" << lastSentLogId_ << ", clusterId=" << clusterId_;
}

LogID MetaSyncListener::lastApplyLogId() {
  return loadLastSent_();
}

std::pair<LogID, TermID> MetaSyncListener::lastCommittedLogId() {
  std::string checkpointFile = dumpPath_ + "/checkpoint";
  if (access(checkpointFile.c_str(), 0) != 0) {
    VLOG(3) << "Invalid or nonexistent checkpoint file: " << checkpointFile;
    return {0, 0};
  }
  int32_t fd = open(checkpointFile.c_str(), O_RDONLY);
  if (fd < 0) {
    LOG(FATAL) << "Failed to open checkpoint file \"" << checkpointFile << "\" (" << errno
               << "): " << strerror(errno);
  }

  LogID logId;
  TermID termId;
  CHECK_EQ(pread(fd, reinterpret_cast<char*>(&logId), sizeof(LogID), 0),
           static_cast<ssize_t>(sizeof(LogID)));
  CHECK_EQ(pread(fd, reinterpret_cast<char*>(&termId), sizeof(TermID), sizeof(LogID)),
           static_cast<ssize_t>(sizeof(TermID)));
  close(fd);
  return {logId, termId};
}

bool MetaSyncListener::persist(LogID commitLogId, TermID commitLogTerm, LogID lastApplyLogId) {
  std::string checkpointFile = dumpPath_ + "/checkpoint";
  int32_t fd = open(checkpointFile.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) {
    LOG(ERROR) << "Failed to open checkpoint file \"" << checkpointFile << "\" (" << errno
               << "): " << strerror(errno);
    return false;
  }

  // Format: [commitLogId (8 bytes)][commitLogTerm (8 bytes)][lastApplyLogId (8 bytes)]
  std::string val;
  val.reserve(sizeof(LogID) * 2 + sizeof(TermID));
  val.append(reinterpret_cast<const char*>(&commitLogId), sizeof(LogID))
      .append(reinterpret_cast<const char*>(&commitLogTerm), sizeof(TermID))
      .append(reinterpret_cast<const char*>(&lastApplyLogId), sizeof(LogID));

  ssize_t written = write(fd, val.c_str(), val.size());
  if (written != static_cast<ssize_t>(val.size())) {
    LOG(ERROR) << idStr_ << "Checkpoint write failed, bytesWritten:" << written
               << ", expected:" << val.size() << ", error:" << strerror(errno);
    close(fd);
    return false;
  }
  close(fd);
  return true;
}

void MetaSyncListener::processLogs() {
  std::unique_ptr<LogIterator> iter;
  {
    std::lock_guard<std::mutex> guard(raftLock_);
    if (lastApplyLogId_ >= committedLogId_) {
      return;
    }
    iter = wal_->iterator(lastApplyLogId_ + 1, committedLogId_);
  }

  LogID lastApplyId = -1;
  std::string buffer;
  int64_t batchCount = 0;

  while (iter->valid()) {
    lastApplyId = iter->logId();
    auto log = iter->logMsg();

    if (log.empty()) {
      // Skip heartbeat
      ++(*iter);
      continue;
    }

    // Meta WAL log format: the log message contains encoded KV operations.
    // For meta operations, the WAL entry is typically:
    //   [clusterId (8 bytes)][op_type (1 byte)][encoded KV pairs...]
    // We need at least the header to determine if it's a multi-put or single-put.
    if (log.size() < static_cast<size_t>(sizeof(int64_t) + 1)) {
      ++(*iter);
      continue;
    }

    // Skip Raft membership command WALs (same op types as SyncListener)
    char opType = log[sizeof(int64_t)];
    switch (opType) {
      case kvstore::OP_TRANS_LEADER:
      case kvstore::OP_ADD_LEARNER:
      case kvstore::OP_ADD_PEER:
      case kvstore::OP_REMOVE_PEER:
        ++(*iter);
        continue;
      default:
        break;
    }

    // Decode the KV pairs from the WAL log message using LogEncoder.
    // Meta WAL entries use the same encoding as storaged:
    //   OP_PUT / OP_MULTI_PUT: key-value pairs [key1, val1, key2, val2, ...]
    //   OP_REMOVE / OP_MULTI_REMOVE: keys only [key1, key2, ...]
    //   OP_BATCH_WRITE: batch of mixed operations
    // We handle PUT and REMOVE differently.
    if (opType == kvstore::OP_PUT || opType == kvstore::OP_MULTI_PUT) {
      auto kvs = kvstore::decodeMultiValues(log);
      // kvs contains [key1, val1, key2, val2, ...]
      for (size_t i = 0; i + 1 < kvs.size(); i += 2) {
        auto key = kvs[i];
        auto value = kvs[i + 1];

        if (!isReplicableMetaKey_(key)) {
          continue;
        }

        // Encode and append to buffer
        std::string encoded = encodeMetaPayload_(key, value);
        buffer.append(std::move(encoded));
        batchCount++;
      }
    } else if (opType == kvstore::OP_REMOVE || opType == kvstore::OP_MULTI_REMOVE) {
      auto keys = kvstore::decodeMultiValues(log);
      // keys contains [key1, key2, ...] (no values for remove operations)
      for (const auto& key : keys) {
        if (!isReplicableMetaKey_(key)) {
          continue;
        }

        // For remove operations, encode with empty value
        std::string encoded = encodeMetaPayload_(key, folly::StringPiece());
        buffer.append(std::move(encoded));
        batchCount++;
      }
    } else if (opType == kvstore::OP_BATCH_WRITE) {
      // Decode the batch into individual sub-operations
      auto batchData = kvstore::decodeBatchValue(log);
      for (const auto& op : batchData) {
        auto key = op.second.first;
        if (!isReplicableMetaKey_(key)) {
          continue;
        }

        switch (op.first) {
          case kvstore::BatchLogType::OP_BATCH_PUT: {
            auto value = op.second.second;
            std::string encoded = encodeMetaPayload_(key, value);
            buffer.append(std::move(encoded));
            batchCount++;
            break;
          }
          case kvstore::BatchLogType::OP_BATCH_REMOVE: {
            // For remove operations, encode with empty value
            std::string encoded = encodeMetaPayload_(key, folly::StringPiece());
            buffer.append(std::move(encoded));
            batchCount++;
            break;
          }
          case kvstore::BatchLogType::OP_BATCH_REMOVE_RANGE: {
            // Range removes are rare for meta operations; skip for now
            VLOG(2) << idStr_ << "Skipping OP_BATCH_REMOVE_RANGE in meta batch write";
            break;
          }
        }
      }
    }
    // Skip OP_REMOVE_RANGE and other unknown op types

    ++(*iter);
  }

  if (lastApplyId == -1 || buffer.empty()) {
    // Even if buffer is empty, we still need to advance lastApplyLogId
    if (lastApplyId != -1) {
      std::lock_guard<std::mutex> guard(raftLock_);
      lastApplyLogId_ = lastApplyId;
      lastSentLogId_ = lastApplyId;
      persist(committedLogId_, term_, lastApplyLogId_);
    }
    return;
  }

  // Write to dump file (append mode)
  std::string dumpFile = dumpPath_ + "/meta_sync_dump.log";
  int32_t fd = open(dumpFile.c_str(), O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0644);
  if (fd < 0) {
    LOG(ERROR) << idStr_ << "Failed to open dump file \"" << dumpFile << "\" (" << errno
               << "): " << strerror(errno);
    return;
  }

  ssize_t written = write(fd, buffer.c_str(), buffer.size());
  close(fd);

  if (written != static_cast<ssize_t>(buffer.size())) {
    LOG(ERROR) << idStr_ << "Dump write incomplete, bytesWritten:" << written
               << ", expected:" << buffer.size() << ", error:" << strerror(errno);
    return;
  }

  // Update state
  {
    std::lock_guard<std::mutex> guard(raftLock_);
    lastApplyLogId_ = lastApplyId;
    lastSentLogId_ = lastApplyId;
    persist(committedLogId_, term_, lastApplyLogId_);
  }
  VLOG(2) << idStr_ << "MetaSyncListener dumped meta logs up to " << lastApplyId
          << ", batchCount=" << batchCount;
}

std::tuple<nebula::cpp2::ErrorCode, int64_t, int64_t> MetaSyncListener::commitSnapshot(
    const std::vector<std::string>& data,
    LogID committedLogId,
    TermID committedLogTerm,
    bool finished) {
  VLOG(2) << idStr_ << "MetaSyncListener committing snapshot.";
  int64_t count = 0;
  int64_t size = 0;

  std::string buffer;
  for (const auto& row : data) {
    count++;
    size += row.size();
    // Snapshot data for meta is raw KV, encode it as payload
    // Format: each row in snapshot is a single KV pair encoded as [keyLen][key][valueLen][value]
    buffer.append(row);
  }

  if (!buffer.empty()) {
    std::string dumpFile = dumpPath_ + "/meta_sync_dump.log";
    int32_t fd = open(dumpFile.c_str(), O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) {
      LOG(ERROR) << idStr_ << "Failed to open dump file for snapshot: " << strerror(errno);
      return {nebula::cpp2::ErrorCode::E_RAFT_PERSIST_SNAPSHOT_FAILED,
              kNoSnapshotCount,
              kNoSnapshotSize};
    }
    ssize_t written = write(fd, buffer.c_str(), buffer.size());
    close(fd);
    if (written != static_cast<ssize_t>(buffer.size())) {
      LOG(ERROR) << idStr_ << "Snapshot dump write incomplete";
      return {nebula::cpp2::ErrorCode::E_RAFT_PERSIST_SNAPSHOT_FAILED,
              kNoSnapshotCount,
              kNoSnapshotSize};
    }
  }

  if (finished) {
    CHECK(!raftLock_.try_lock());
    leaderCommitId_ = committedLogId;
    lastApplyLogId_ = committedLogId;
    lastSentLogId_ = committedLogId;
    persist(committedLogId, committedLogTerm, lastApplyLogId_);
    LOG(INFO) << idStr_
              << "MetaSyncListener snapshot committed: committedLogId=" << committedLogId
              << ", committedLogTerm=" << committedLogTerm;
  }
  return {nebula::cpp2::ErrorCode::SUCCEEDED, count, size};
}

bool MetaSyncListener::isReplicableMetaKey_(folly::StringPiece key) const {
  // Check key against the whitelist of replicable meta prefixes.
  // We use startsWith checks against known prefix strings.
  if (key.startsWith(kMetaSpacesPrefix)) {
    return true;
  }
  if (key.startsWith(kMetaTagsPrefix)) {
    return true;
  }
  if (key.startsWith(kMetaEdgesPrefix)) {
    return true;
  }
  if (key.startsWith(kMetaIndexesPrefix)) {
    return true;
  }
  if (key.startsWith(kMetaUsersPrefix)) {
    return true;
  }
  if (key.startsWith(kMetaRolesPrefix)) {
    return true;
  }
  return false;
}

std::string MetaSyncListener::encodeMetaPayload_(folly::StringPiece key,
                                                  folly::StringPiece value) {
  // Format: [keyLen (4 bytes)][key][valueLen (4 bytes)][value]
  uint32_t keyLen = static_cast<uint32_t>(key.size());
  uint32_t valueLen = static_cast<uint32_t>(value.size());
  std::string encoded;
  encoded.reserve(sizeof(uint32_t) + key.size() + sizeof(uint32_t) + value.size());
  encoded.append(reinterpret_cast<const char*>(&keyLen), sizeof(uint32_t));
  encoded.append(key.data(), key.size());
  encoded.append(reinterpret_cast<const char*>(&valueLen), sizeof(uint32_t));
  encoded.append(value.data(), value.size());
  return encoded;
}

void MetaSyncListener::persistLastSent_(LogID id) {
  // Re-use persist() to write the full checkpoint
  persist(id, 0, id);
}

LogID MetaSyncListener::loadLastSent_() {
  std::string checkpointFile = dumpPath_ + "/checkpoint";
  if (access(checkpointFile.c_str(), 0) != 0) {
    VLOG(3) << "No checkpoint file found at: " << checkpointFile;
    return 0;
  }
  int32_t fd = open(checkpointFile.c_str(), O_RDONLY);
  if (fd < 0) {
    LOG(ERROR) << "Failed to open checkpoint file \"" << checkpointFile << "\" (" << errno
               << "): " << strerror(errno);
    return 0;
  }
  // Read lastApplyLogId which is at offset sizeof(LogID) + sizeof(TermID)
  LogID logId;
  auto offset = sizeof(LogID) + sizeof(TermID);
  auto bytesRead = pread(fd, reinterpret_cast<char*>(&logId), sizeof(LogID), offset);
  close(fd);
  if (bytesRead != static_cast<ssize_t>(sizeof(LogID))) {
    LOG(ERROR) << "Failed to read lastApplyLogId from checkpoint file";
    return 0;
  }
  return logId;
}

}  // namespace meta
}  // namespace nebula
