/* Copyright (c) 2024 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#ifndef DRAINER_PARTAPPLIER_H_
#define DRAINER_PARTAPPLIER_H_

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#include <folly/futures/Future.h>
#include <folly/futures/Promise.h>

#include "common/thrift/ThriftTypes.h"
#include "interface/gen-cpp2/sync_types.h"

namespace nebula {
namespace drainer {

class DrainerEnv;

/**
 * PartApplier is responsible for applying sync log batches for a specific
 * (space, partition) pair. It runs a dedicated worker thread that consumes
 * batches from its internal queue and applies them sequentially.
 */
class PartApplier {
 public:
  PartApplier(GraphSpaceID spaceId, PartitionID partId, DrainerEnv* env);
  ~PartApplier();

  void start();
  void stop();

  /**
   * Enqueue a batch and return a future that resolves when the batch
   * has been successfully applied. The future carries the lastLogId
   * of the applied batch.
   */
  folly::Future<LogID> enqueue(sync::cpp2::SyncLogBatch&& batch);

  /**
   * Get last applied log id.
   */
  LogID lastApplied() const {
    return lastApplied_.load(std::memory_order_acquire);
  }

 private:
  void run_();
  void applyBatch_(const sync::cpp2::SyncLogBatch& batch);
  void applyPut_(const sync::cpp2::SyncLogEntry& entry);
  void applyMultiPut_(const sync::cpp2::SyncLogEntry& entry);
  void applyRemove_(const sync::cpp2::SyncLogEntry& entry);
  void applyMultiRemove_(const sync::cpp2::SyncLogEntry& entry);
  void applyRemoveRange_(const sync::cpp2::SyncLogEntry& entry);
  void applyBatchWrite_(const sync::cpp2::SyncLogEntry& entry);

  /**
   * Retry a storage operation with exponential backoff.
   * Retries up to FLAGS_drainer_apply_retry_times times with increasing delays
   * (base, base*5, base*20 where base = FLAGS_drainer_apply_retry_interval_ms).
   * Throws std::runtime_error if all retries are exhausted.
   *
   * @param opName   Name of the operation (for logging)
   * @param logId    Log ID of the entry being applied (for logging)
   * @param func     Lambda that returns true on success, false on failure
   */
  void retryOnFailure_(const std::string& opName,
                       LogID logId,
                       std::function<bool()> func);

  /**
   * Wait until the schema version referenced by the entry becomes visible
   * on the backup cluster. Polls with FLAGS_drainer_schema_visibility_poll_ms
   * interval up to FLAGS_drainer_schema_visibility_timeout_ms.
   *
   * @param entry The sync log entry that may reference a schema version
   * @return true if the schema is visible (or entry has no schemaVer); false on timeout
   */
  bool waitForSchema_(const sync::cpp2::SyncLogEntry& entry);

  /**
   * Check if a key is an in-edge key that should be skipped during replication.
   * In-edges have a negative edgeType. The backup cluster's storage layer will
   * auto-generate in-edges when out-edges are inserted, so replicating them
   * would cause duplicates.
   *
   * Derives vIdLen from the key length using the edge key structure:
   *   keyLen = kEdgeLen + 2 * vIdLen
   *
   * @param key The raw key to check
   * @return true if the key is an in-edge and should be skipped
   */
  static bool isInEdgeKey_(folly::StringPiece key);

  GraphSpaceID spaceId_;
  PartitionID partId_;
  DrainerEnv* env_;
  std::atomic<LogID> lastApplied_{0};
  std::atomic<bool> stop_{false};
  std::thread worker_;

  struct QueueItem {
    sync::cpp2::SyncLogBatch batch;
    folly::Promise<LogID> promise;
  };
  std::deque<QueueItem> queue_;
  std::mutex queueLock_;
  std::condition_variable cv_;
};

}  // namespace drainer
}  // namespace nebula

#endif  // DRAINER_PARTAPPLIER_H_
