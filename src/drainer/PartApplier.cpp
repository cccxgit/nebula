/* Copyright (c) 2024 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include "drainer/PartApplier.h"

#include <glog/logging.h>

#include "drainer/DrainerEnv.h"

namespace nebula {
namespace drainer {

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
}

void PartApplier::applyPut_(const sync::cpp2::SyncLogEntry& entry) {
  // TODO(P2): Route put operation to backup cluster via
  //   env_->backupClientCache()->backupStorageClient()->put(...)
  VLOG(3) << "applyPut_ space=" << spaceId_ << " part=" << partId_
           << " logId=" << *entry.logId_ref()
           << " payloadSize=" << entry.payload_ref()->size();
}

void PartApplier::applyMultiPut_(const sync::cpp2::SyncLogEntry& entry) {
  // TODO(P2): Route multi-put operation to backup cluster via
  //   env_->backupClientCache()->backupStorageClient()->multiPut(...)
  VLOG(3) << "applyMultiPut_ space=" << spaceId_ << " part=" << partId_
           << " logId=" << *entry.logId_ref()
           << " payloadSize=" << entry.payload_ref()->size();
}

void PartApplier::applyRemove_(const sync::cpp2::SyncLogEntry& entry) {
  // TODO(P2): Route remove operation to backup cluster via
  //   env_->backupClientCache()->backupStorageClient()->remove(...)
  VLOG(3) << "applyRemove_ space=" << spaceId_ << " part=" << partId_
           << " logId=" << *entry.logId_ref()
           << " payloadSize=" << entry.payload_ref()->size();
}

void PartApplier::applyMultiRemove_(const sync::cpp2::SyncLogEntry& entry) {
  // TODO(P2): Route multi-remove operation to backup cluster via
  //   env_->backupClientCache()->backupStorageClient()->multiRemove(...)
  VLOG(3) << "applyMultiRemove_ space=" << spaceId_ << " part=" << partId_
           << " logId=" << *entry.logId_ref()
           << " payloadSize=" << entry.payload_ref()->size();
}

void PartApplier::applyRemoveRange_(const sync::cpp2::SyncLogEntry& entry) {
  // TODO(P2): Route remove-range operation to backup cluster via
  //   env_->backupClientCache()->backupStorageClient()->removeRange(...)
  VLOG(3) << "applyRemoveRange_ space=" << spaceId_ << " part=" << partId_
           << " logId=" << *entry.logId_ref()
           << " payloadSize=" << entry.payload_ref()->size();
}

void PartApplier::applyBatchWrite_(const sync::cpp2::SyncLogEntry& entry) {
  // TODO(P2): Route batch-write operation to backup cluster via
  //   env_->backupClientCache()->backupStorageClient()->batchWrite(...)
  VLOG(3) << "applyBatchWrite_ space=" << spaceId_ << " part=" << partId_
           << " logId=" << *entry.logId_ref()
           << " payloadSize=" << entry.payload_ref()->size();
}

}  // namespace drainer
}  // namespace nebula
