/* Copyright (c) 2024 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include "kvstore/listener/sync/WalRetentionHook.h"

namespace nebula {
namespace kvstore {

// Static member definitions
std::mutex WalRetentionHook::lock_;
std::unordered_map<std::pair<GraphSpaceID, PartitionID>, LogID, WalRetentionHook::PairHash>
    WalRetentionHook::progress_;

LogID WalRetentionHook::getMinRetainLogId(GraphSpaceID spaceId, PartitionID partId) {
  std::lock_guard<std::mutex> guard(lock_);
  auto key = std::make_pair(spaceId, partId);
  auto it = progress_.find(key);
  if (it == progress_.end()) {
    // No listener tracking this partition - no retention constraint
    return 0;
  }
  // The listener has forwarded up to this logId, so WAL entries with
  // ID >= (progress + 1) might still be needed. Return progress + 1
  // as the minimum to retain (i.e., don't delete anything at or after this).
  return it->second + 1;
}

void WalRetentionHook::updateListenerProgress(GraphSpaceID spaceId,
                                              PartitionID partId,
                                              LogID logId) {
  std::lock_guard<std::mutex> guard(lock_);
  auto key = std::make_pair(spaceId, partId);
  auto it = progress_.find(key);
  if (it == progress_.end()) {
    progress_.emplace(key, logId);
  } else {
    // Only advance forward; never regress
    if (logId > it->second) {
      it->second = logId;
    }
  }
}

bool WalRetentionHook::isListenerLagging(GraphSpaceID spaceId,
                                         PartitionID partId,
                                         LogID leaderCommitId,
                                         int64_t maxLagLogs) {
  std::lock_guard<std::mutex> guard(lock_);
  auto key = std::make_pair(spaceId, partId);
  auto it = progress_.find(key);
  if (it == progress_.end()) {
    // No progress recorded - if leader has committed logs, consider it lagging
    return leaderCommitId > 0;
  }
  int64_t lag = static_cast<int64_t>(leaderCommitId) - static_cast<int64_t>(it->second);
  return lag > maxLagLogs;
}

void WalRetentionHook::removeProgress(GraphSpaceID spaceId, PartitionID partId) {
  std::lock_guard<std::mutex> guard(lock_);
  auto key = std::make_pair(spaceId, partId);
  progress_.erase(key);
}

void WalRetentionHook::clear() {
  std::lock_guard<std::mutex> guard(lock_);
  progress_.clear();
}

}  // namespace kvstore
}  // namespace nebula
