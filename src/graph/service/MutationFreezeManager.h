/* Copyright (c) 2026 vesoft inc. All rights reserved.
 *
 * This source code is licensed under the Apache License, Version 2.0.
 */

#ifndef GRAPH_SERVICE_MUTATIONFREEZEMANAGER_H_
#define GRAPH_SERVICE_MUTATIONFREEZEMANAGER_H_

#include <atomic>
#include <cstdint>
#include <memory>

#include "common/base/Status.h"

namespace nebula {
class Sentence;

namespace graph {

class MutationFreezeManager;

class MutationLease final {
 public:
  explicit MutationLease(MutationFreezeManager* manager);
  ~MutationLease();

  MutationLease(const MutationLease&) = delete;
  MutationLease& operator=(const MutationLease&) = delete;

 private:
  MutationFreezeManager* manager_{nullptr};
};

// Coordinates mutation admission with the graphd freeze flag. A lease is held by a QueryInstance
// from successful validation until the query completes or is discarded.
class MutationFreezeManager final {
 public:
  static MutationFreezeManager& instance();

  Status checkMutationAllowed(const Sentence* sentence) const;
  StatusOr<std::unique_ptr<MutationLease>> acquire(const Sentence* sentence);

  bool isMutating(const Sentence* sentence) const;
  uint64_t activeMutationCount() const;
  bool isFrozen() const;

  void release();

 private:
  MutationFreezeManager() = default;

  std::atomic<uint64_t> activeMutationCount_{0};
};

}  // namespace graph
}  // namespace nebula

#endif  // GRAPH_SERVICE_MUTATIONFREEZEMANAGER_H_
