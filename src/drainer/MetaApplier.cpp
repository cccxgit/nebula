/* Copyright (c) 2024 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include "drainer/MetaApplier.h"

#include <glog/logging.h>

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

    // Apply each entry in the batch
    const auto& entries = *item.batch.entries_ref();
    for (const auto& entry : entries) {
      applyOne_(entry);
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

void MetaApplier::applyOne_(const sync::cpp2::SyncLogEntry& entry) {
  auto logId = *entry.logId_ref();
  auto op = *entry.op_ref();

  // TODO(P3): Apply meta operation to backup cluster's MetaClient.
  //   This will involve parsing the payload and calling the appropriate
  //   MetaClient methods (createTag, createEdge, alterTag, etc.) on the
  //   backup cluster.
  LOG(INFO) << "MetaApplier applyOne_ logId=" << logId
            << " op=" << static_cast<int>(op)
            << " payloadSize=" << entry.payload_ref()->size();

  // After successful meta application, mark schema as visible.
  // The schemaVer in the entry tells us which version was created/altered.
  if (entry.schemaVer_ref().has_value()) {
    auto schemaVer = *entry.schemaVer_ref();
    // For meta entries, the spaceId comes from the batch context.
    // We use a placeholder here; actual spaceId will be threaded through
    // from the batch in the full implementation.
    // TODO(P3): Extract spaceId from meta payload or pass from batch context.
    GraphSpaceID spaceId = 0;  // Placeholder - will be extracted from payload

    {
      std::lock_guard<std::mutex> lk(schemaLock_);
      schemaVisible_[std::make_pair(spaceId, schemaVer)] = true;
    }

    releaseParked_(spaceId, schemaVer);
  }
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
