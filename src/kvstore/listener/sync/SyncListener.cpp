/* Copyright (c) 2024 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include "kvstore/listener/sync/SyncListener.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/fs/FileUtils.h"
#include "kvstore/LogEncoder.h"
#include "kvstore/listener/sync/SyncListenerFlags.h"

namespace nebula {
namespace kvstore {

void SyncListener::init() {
  // Determine dump path
  if (FLAGS_sync_listener_dump_path.empty()) {
    dumpPath_ = folly::stringPrintf("%s/sync_dump_%d_%d", walPath_.c_str(), spaceId_, partId_);
  } else {
    dumpPath_ = folly::stringPrintf(
        "%s/sync_dump_%d_%d", FLAGS_sync_listener_dump_path.c_str(), spaceId_, partId_);
  }

  // Create dump directory if it does not exist
  if (!fs::FileUtils::exist(dumpPath_)) {
    if (!fs::FileUtils::makeDir(dumpPath_)) {
      LOG(FATAL) << "Failed to create sync dump directory: " << dumpPath_;
    }
  }

  // Load last sent log id from checkpoint
  lastSentLogId_ = loadLastSent_();
  LOG(INFO) << idStr_ << "SyncListener initialized, dumpPath=" << dumpPath_
            << ", lastSentLogId=" << lastSentLogId_ << ", clusterId=" << clusterId_;
}

LogID SyncListener::lastApplyLogId() {
  return loadLastSent_();
}

std::pair<LogID, TermID> SyncListener::lastCommittedLogId() {
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

bool SyncListener::persist(LogID commitLogId, TermID commitLogTerm, LogID lastApplyLogId) {
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

void SyncListener::processLogs() {
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
  int64_t batchBytes = 0;
  int64_t batchCount = 0;

  while (iter->valid()) {
    lastApplyId = iter->logId();
    auto log = iter->logMsg();

    if (log.empty()) {
      // Skip heartbeat
      ++(*iter);
      continue;
    }

    // Skip Raft membership command WALs
    if (isSkippableCommandWal_(log)) {
      ++(*iter);
      continue;
    }

    // Encode the WAL entry
    std::string encoded = encodeOne_(iter->logId(), iter->logTerm(), log);
    batchBytes += encoded.size();
    batchCount++;
    buffer.append(std::move(encoded));

    // Check batch limits
    if (batchBytes >= FLAGS_sync_listener_batch_bytes ||
        batchCount >= FLAGS_sync_listener_batch_count) {
      break;
    }
    ++(*iter);
  }

  if (lastApplyId == -1 || buffer.empty()) {
    return;
  }

  // Write to dump file (append mode)
  std::string dumpFile = dumpPath_ + "/sync_dump.log";
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
  VLOG(2) << idStr_ << "SyncListener dumped logs up to " << lastApplyId
          << ", batchCount=" << batchCount << ", batchBytes=" << batchBytes;
}

std::tuple<nebula::cpp2::ErrorCode, int64_t, int64_t> SyncListener::commitSnapshot(
    const std::vector<std::string>& data,
    LogID committedLogId,
    TermID committedLogTerm,
    bool finished) {
  VLOG(2) << idStr_ << "SyncListener committing snapshot.";
  int64_t count = 0;
  int64_t size = 0;

  std::string buffer;
  for (const auto& row : data) {
    count++;
    size += row.size();
    // Encode snapshot data as a WAL entry with the committed log id/term
    std::string encoded = encodeOne_(committedLogId, committedLogTerm, folly::StringPiece(row));
    buffer.append(std::move(encoded));
  }

  if (!buffer.empty()) {
    std::string dumpFile = dumpPath_ + "/sync_dump.log";
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
    LOG(INFO) << idStr_ << "SyncListener snapshot committed: committedLogId=" << committedLogId
              << ", committedLogTerm=" << committedLogTerm;
  }
  return {nebula::cpp2::ErrorCode::SUCCEEDED, count, size};
}

bool SyncListener::isSkippableCommandWal_(folly::StringPiece log) {
  if (log.size() < static_cast<size_t>(sizeof(int64_t) + 1)) {
    return false;
  }
  // WAL format: first sizeof(int64_t) bytes are the clusterId/timestamp,
  // then the next byte is the operation type
  char opType = log[sizeof(int64_t)];
  switch (opType) {
    case OP_TRANS_LEADER:
    case OP_ADD_LEARNER:
    case OP_ADD_PEER:
    case OP_REMOVE_PEER:
      return true;
    default:
      return false;
  }
}

std::string SyncListener::encodeOne_(LogID id, TermID term, folly::StringPiece raw) {
  // Format: [logId (8 bytes)][term (8 bytes)][dataLen (4 bytes)][raw data]
  uint32_t dataLen = static_cast<uint32_t>(raw.size());
  std::string encoded;
  encoded.reserve(sizeof(LogID) + sizeof(TermID) + sizeof(uint32_t) + raw.size());
  encoded.append(reinterpret_cast<const char*>(&id), sizeof(LogID));
  encoded.append(reinterpret_cast<const char*>(&term), sizeof(TermID));
  encoded.append(reinterpret_cast<const char*>(&dataLen), sizeof(uint32_t));
  encoded.append(raw.data(), raw.size());
  return encoded;
}

void SyncListener::persistLastSent_(LogID id) {
  // Re-use persist() to write the full checkpoint
  persist(id, 0, id);
}

LogID SyncListener::loadLastSent_() {
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

}  // namespace kvstore
}  // namespace nebula
