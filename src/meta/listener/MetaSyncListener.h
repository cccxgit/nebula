/* Copyright (c) 2024 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#ifndef META_LISTENER_META_SYNC_LISTENER_H_
#define META_LISTENER_META_SYNC_LISTENER_H_

#include <atomic>

#include "kvstore/listener/Listener.h"

namespace nebula {
namespace meta {

/**
 * MetaSyncListener is a listener that captures committed DDL WAL entries from metad's
 * Raft and writes them to a local dump file for cross-cluster schema synchronization.
 *
 * It replicates only schema-related meta operations:
 *   - Space operations (CREATE/DROP/ALTER SPACE)
 *   - Tag/Edge schema operations (CREATE/ALTER/DROP TAG/EDGE)
 *   - Index operations (CREATE/DROP INDEX)
 *   - User/Role operations (CREATE/ALTER/DROP USER, GRANT/REVOKE ROLE)
 *
 * Non-replicable operations (hosts, parts, leaders, zones, balance, listeners, etc.)
 * are filtered out.
 *
 * In P3 "dump-only" mode, it simply persists filtered WAL entries to a local file.
 * Future iterations will send entries to a drainer service.
 */
class MetaSyncListener : public kvstore::Listener {
 public:
  /**
   * @brief Construct a new MetaSyncListener
   *
   * @param spaceId       kDefaultSpaceId (0) for meta partition
   * @param partId        0 for meta partition
   * @param localAddr     Listener ip/addr
   * @param walPath       Listener's wal path
   * @param ioPool        IOThreadPool for listener
   * @param workers       Background thread for listener
   * @param handlers      Worker thread for listener
   * @param clusterId     Cluster ID for this listener
   */
  MetaSyncListener(GraphSpaceID spaceId,
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
   * @brief Process committed WAL entries: read, filter meta keys, encode, and write to dump file
   */
  void processLogs() override;

  std::tuple<nebula::cpp2::ErrorCode, int64_t, int64_t> commitSnapshot(
      const std::vector<std::string>& data,
      LogID committedLogId,
      TermID committedLogTerm,
      bool finished) override;

 private:
  /**
   * @brief Check if a meta key should be replicated to the target cluster.
   *
   * Replicable keys (whitelist):
   *   - __spaces__   (space definitions)
   *   - __tags__     (tag schema)
   *   - __edges__    (edge schema)
   *   - __indexes__  (index definitions)
   *   - __users__    (user accounts)
   *   - __roles__    (role assignments)
   *
   * Non-replicable (skipped):
   *   - __hosts__, __parts__, __leaders__, __leader_terms__, __zones__,
   *     __listener__, __balance_task__, __balance_plan__, __machines__,
   *     __host_dirs__, __disk_parts__, __job_mgr__, __stats__,
   *     __configs__, __snapshots__, __services__, __sessions__, etc.
   *
   * @param key The raw meta key from WAL
   * @return true if the key should be replicated
   */
  bool isReplicableMetaKey_(folly::StringPiece key) const;

  /**
   * @brief Encode a meta KV pair as a payload for the dump file
   *
   * Format: [keyLen (4 bytes)][key][valueLen (4 bytes)][value]
   *
   * @param key The meta key
   * @param value The meta value
   * @return std::string Encoded payload
   */
  std::string encodeMetaPayload_(folly::StringPiece key, folly::StringPiece value);

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

 private:
  int64_t clusterId_{0};
  std::string walPath_;
  LogID lastSentLogId_{0};
  std::string dumpPath_;
};

}  // namespace meta
}  // namespace nebula

#endif  // META_LISTENER_META_SYNC_LISTENER_H_
