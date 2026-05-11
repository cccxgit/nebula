/* Copyright (c) 2024 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#ifndef DRAINER_QUEUEMANAGER_H_
#define DRAINER_QUEUEMANAGER_H_

#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

#include <folly/futures/Future.h>
#include <folly/futures/Promise.h>

#include "common/thrift/ThriftTypes.h"
#include "interface/gen-cpp2/sync_types.h"

namespace nebula {
namespace drainer {

/**
 * QueueManager manages per-(space, part) bounded queues and dispatches
 * batches to workers. It provides backpressure by rejecting enqueue when
 * the per-partition queue depth exceeds the configured maximum.
 */
class QueueManager {
 public:
  explicit QueueManager(int32_t maxInflightPerPart);
  ~QueueManager();

  /**
   * Enqueue a batch for processing.
   * Returns false if queue is full (backpressure signal to the caller).
   */
  bool enqueue(GraphSpaceID spaceId,
               PartitionID partId,
               sync::cpp2::SyncLogBatch&& batch,
               folly::Promise<LogID>&& promise);

  /**
   * Get queue depth for a (space, part).
   */
  int32_t queueDepth(GraphSpaceID spaceId, PartitionID partId) const;

  struct QueueItem {
    sync::cpp2::SyncLogBatch batch;
    folly::Promise<LogID> promise;
  };

  /**
   * Pop a batch for processing (called by worker threads).
   * Returns std::nullopt if the queue is empty.
   */
  std::optional<QueueItem> pop(GraphSpaceID spaceId, PartitionID partId);

 private:
  struct PartQueue {
    std::deque<QueueItem> items;
    mutable std::mutex lock;
  };

  struct PairHash {
    std::size_t operator()(const std::pair<GraphSpaceID, PartitionID>& p) const {
      auto h1 = std::hash<GraphSpaceID>{}(p.first);
      auto h2 = std::hash<PartitionID>{}(p.second);
      return h1 ^ (h2 << 32);
    }
  };

  int32_t maxInflight_;
  std::unordered_map<std::pair<GraphSpaceID, PartitionID>,
                     std::unique_ptr<PartQueue>,
                     PairHash>
      queues_;
  mutable std::mutex globalLock_;

  PartQueue& getOrCreate_(GraphSpaceID spaceId, PartitionID partId);
};

}  // namespace drainer
}  // namespace nebula

#endif  // DRAINER_QUEUEMANAGER_H_
