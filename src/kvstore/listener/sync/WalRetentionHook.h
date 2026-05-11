/* Copyright (c) 2024 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#ifndef KVSTORE_LISTENER_SYNC_WAL_RETENTION_HOOK_H_
#define KVSTORE_LISTENER_SYNC_WAL_RETENTION_HOOK_H_

#include <mutex>
#include <unordered_map>
#include <utility>

#include "common/base/Base.h"
#include "common/utils/Types.h"

namespace nebula {
namespace kvstore {

/**
 * WalRetentionHook coordinates between the SyncListener's progress and the WAL
 * garbage collection mechanism.
 *
 * The SyncListener reads committed WAL entries and forwards them to the drainer.
 * WAL GC must not delete log files that contain entries the listener has not yet
 * read and forwarded. This class maintains the minimum log ID that must be retained
 * per (space, partition) pair.
 *
 * Usage:
 *   - SyncListener calls updateListenerProgress() after successfully forwarding logs.
 *   - WAL GC (FileBasedWal::rollbackToLog / TTL cleanup) calls getMinRetainLogId()
 *     to determine the safe lower bound for deletion.
 *   - Operators or monitoring can call isListenerLagging() to detect if the listener
 *     has fallen too far behind the leader's committed position.
 *
 * Thread safety: All methods are thread-safe (protected by internal mutex).
 */
class WalRetentionHook {
 public:
  /**
   * @brief Get the minimum log ID that must be retained for sync.
   *
   * WAL GC should not delete logs with ID >= this value.
   * Returns 0 if no progress is tracked for the given (spaceId, partId),
   * which means no retention constraint from sync.
   *
   * @param spaceId Graph space ID
   * @param partId Partition ID
   * @return LogID Minimum log ID to retain
   */
  static LogID getMinRetainLogId(GraphSpaceID spaceId, PartitionID partId);

  /**
   * @brief Update the listener's progress after successful forwarding.
   *
   * Called by SyncListener after it has successfully sent log entries
   * to the drainer (or dumped them in P1 mode). The WAL GC is then
   * safe to delete entries with ID < logId.
   *
   * @param spaceId Graph space ID
   * @param partId Partition ID
   * @param logId The last log ID that has been successfully forwarded
   */
  static void updateListenerProgress(GraphSpaceID spaceId, PartitionID partId, LogID logId);

  /**
   * @brief Check if the sync listener is lagging beyond a threshold.
   *
   * Useful for alerting and for deciding whether to trigger a snapshot-based
   * gap recovery instead of waiting for WAL replay to catch up.
   *
   * @param spaceId Graph space ID
   * @param partId Partition ID
   * @param leaderCommitId The leader's current committed log ID
   * @param maxLagLogs Maximum acceptable lag in number of log entries
   * @return true if listener is lagging beyond threshold (or no progress recorded)
   */
  static bool isListenerLagging(GraphSpaceID spaceId,
                                PartitionID partId,
                                LogID leaderCommitId,
                                int64_t maxLagLogs);

  /**
   * @brief Remove tracking for a (space, partition) pair.
   *
   * Called when the sync listener is removed or the space is dropped.
   *
   * @param spaceId Graph space ID
   * @param partId Partition ID
   */
  static void removeProgress(GraphSpaceID spaceId, PartitionID partId);

  /**
   * @brief Clear all tracked progress. Used in tests.
   */
  static void clear();

 private:
  struct PairHash {
    std::size_t operator()(const std::pair<GraphSpaceID, PartitionID>& p) const {
      auto h1 = std::hash<GraphSpaceID>{}(p.first);
      auto h2 = std::hash<PartitionID>{}(p.second);
      return h1 ^ (h2 << 32);
    }
  };

  static std::mutex lock_;
  static std::unordered_map<std::pair<GraphSpaceID, PartitionID>, LogID, PairHash> progress_;
};

}  // namespace kvstore
}  // namespace nebula

#endif  // KVSTORE_LISTENER_SYNC_WAL_RETENTION_HOOK_H_
