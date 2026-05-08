/* Copyright (c) 2024 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#ifndef KVSTORE_LISTENER_SYNC_LISTENER_H_
#define KVSTORE_LISTENER_SYNC_LISTENER_H_

#include <atomic>

#include "kvstore/listener/Listener.h"

namespace nebula {
namespace kvstore {

class DrainerClient;

/**
 * SyncListener is a listener that captures committed WAL entries and writes them
 * to a local dump file for cross-cluster synchronization.
 *
 * In P1 "dump-only" mode, it simply persists WAL entries to a local file.
 * Future iterations will send entries to a drainer service.
 */
class SyncListener : public Listener {
 public:
  /**
   * @brief Construct a new SyncListener
   *
   * @param spaceId
   * @param partId
   * @param localAddr Listener ip/addr
   * @param walPath Listener's wal path
   * @param ioPool IOThreadPool for listener
   * @param workers Background thread for listener
   * @param handlers Worker thread for listener
   * @param clusterId Cluster ID for this listener
   */
  SyncListener(GraphSpaceID spaceId,
               PartitionID partId,
               HostAddr localAddr,
               const std::string& walPath,
               std::shared_ptr<folly::IOThreadPoolExecutor> ioPool,
               std::shared_ptr<thread::GenericThreadPool> workers,
               std::shared_ptr<folly::Executor> handlers,
               int64_t clusterId)
      : Listener(spaceId, partId, std::move(localAddr), walPath, ioPool, workers, handlers),
        clusterId_(clusterId),
        walPath_(walPath) {}

 protected:
  /**
   * @brief Init work: set up dump path, load last sent log id
   */
  void init() override;

  /**
   * @brief Get last apply id from persistence storage
   *
   * @return LogID Last apply log id (in dump-only mode, this is the last dumped id)
   */
  LogID lastApplyLogId() override;

  /**
   * @brief Persist commitLogId, commitLogTerm, and lastApplyLogId to checkpoint file
   */
  bool persist(LogID commitLogId, TermID commitLogTerm, LogID lastApplyLogId) override;

  /**
   * @brief Get commit log id and commit log term from persistence storage
   *
   * @return std::pair<LogID, TermID>
   */
  std::pair<LogID, TermID> lastCommittedLogId() override;

  /**
   * @brief Process committed WAL entries: read, filter, encode, and write to dump file
   */
  void processLogs() override;

  std::tuple<nebula::cpp2::ErrorCode, int64_t, int64_t> commitSnapshot(
      const std::vector<std::string>& data,
      LogID committedLogId,
      TermID committedLogTerm,
      bool finished) override;

 private:
  /**
   * @brief Persist the last sent log id to checkpoint file
   *
   * @param id The log id to persist
   */
  void persistLastSent_(LogID id);

  /**
   * @brief Load last sent log id from checkpoint file
   *
   * @return LogID The last sent/dumped log id
   */
  LogID loadLastSent_();

  /**
   * @brief Check if a WAL entry is a skippable command (Raft membership operations)
   *
   * Filters out: OP_TRANS_LEADER, OP_ADD_LEARNER, OP_ADD_PEER, OP_REMOVE_PEER
   *
   * @param log The raw WAL log entry
   * @return true if the log should be skipped
   */
  bool isSkippableCommandWal_(folly::StringPiece log);

  /**
   * @brief Encode a single WAL entry for the dump file
   *
   * Format: [logId (8 bytes)][term (8 bytes)][dataLen (4 bytes)][raw data]
   *
   * @param id Log ID
   * @param term Log term
   * @param raw Raw log data
   * @return std::string Encoded entry
   */
  std::string encodeOne_(LogID id, TermID term, folly::StringPiece raw);

  bool sendBatchToDrainer_(GraphSpaceID spaceId,
                           PartitionID partId,
                           LogID firstLogId,
                           LogID lastLogId,
                           std::vector<std::pair<LogID, std::string>>&& entries);

 private:
  int64_t clusterId_{0};
  std::string walPath_;
  LogID lastSentLogId_{0};
  std::atomic<int64_t> inflightBytes_{0};
  std::string dumpPath_;
  std::shared_ptr<DrainerClient> drainerClient_;
};

}  // namespace kvstore
}  // namespace nebula
#endif  // KVSTORE_LISTENER_SYNC_LISTENER_H_
