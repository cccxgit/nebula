/* Copyright (c) 2024 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include "drainer/QueueManager.h"

#include <glog/logging.h>

namespace nebula {
namespace drainer {

QueueManager::QueueManager(int32_t maxInflightPerPart) : maxInflight_(maxInflightPerPart) {
  CHECK_GT(maxInflight_, 0) << "maxInflightPerPart must be positive";
}

QueueManager::~QueueManager() = default;

bool QueueManager::enqueue(GraphSpaceID spaceId,
                           PartitionID partId,
                           sync::cpp2::SyncLogBatch&& batch,
                           folly::Promise<LogID>&& promise) {
  auto& pq = getOrCreate_(spaceId, partId);
  std::lock_guard<std::mutex> lk(pq.lock);
  if (static_cast<int32_t>(pq.items.size()) >= maxInflight_) {
    VLOG(2) << "Queue full for space=" << spaceId << " part=" << partId
             << " depth=" << pq.items.size() << " max=" << maxInflight_;
    return false;
  }
  pq.items.emplace_back(QueueItem{std::move(batch), std::move(promise)});
  return true;
}

int32_t QueueManager::queueDepth(GraphSpaceID spaceId, PartitionID partId) const {
  auto key = std::make_pair(spaceId, partId);
  std::lock_guard<std::mutex> lk(globalLock_);
  auto it = queues_.find(key);
  if (it == queues_.end()) {
    return 0;
  }
  std::lock_guard<std::mutex> qlk(it->second->lock);
  return static_cast<int32_t>(it->second->items.size());
}

std::optional<QueueManager::QueueItem> QueueManager::pop(GraphSpaceID spaceId,
                                                         PartitionID partId) {
  auto& pq = getOrCreate_(spaceId, partId);
  std::lock_guard<std::mutex> lk(pq.lock);
  if (pq.items.empty()) {
    return std::nullopt;
  }
  auto item = std::move(pq.items.front());
  pq.items.pop_front();
  return item;
}

QueueManager::PartQueue& QueueManager::getOrCreate_(GraphSpaceID spaceId, PartitionID partId) {
  auto key = std::make_pair(spaceId, partId);
  std::lock_guard<std::mutex> lk(globalLock_);
  auto it = queues_.find(key);
  if (it == queues_.end()) {
    auto [inserted, success] = queues_.emplace(key, std::make_unique<PartQueue>());
    return *inserted->second;
  }
  return *it->second;
}

}  // namespace drainer
}  // namespace nebula
