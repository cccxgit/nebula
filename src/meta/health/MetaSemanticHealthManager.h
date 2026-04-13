/* Copyright (c) 2026 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#ifndef META_HEALTH_METASEMANTICHEALTHMANAGER_H_
#define META_HEALTH_METASEMANTICHEALTHMANAGER_H_

#include <folly/Synchronized.h>

#include <atomic>
#include <thread>

#include "common/base/Base.h"
#include "kvstore/KVStore.h"
#include "meta/health/MetaHealthState.h"
#include "meta/health/MetaSemanticProbeClient.h"

namespace nebula::meta {

class MetaSemanticHealthManager {
 public:
  MetaSemanticHealthManager(kvstore::KVStore* kv, HostAddr localMetaAddr);
  ~MetaSemanticHealthManager();

  void start();
  void stop();

  HealthSnapshot snapshot() const;
  bool shouldFailLiveness() const;

 private:
  void runLoop();
  void runOnce();
  void updateProbe(ProbeResult* target, ProbeResult&& current);

 private:
  kvstore::KVStore* kv_{nullptr};
  HostAddr localMetaAddr_;
  MetaSemanticProbeClient probeClient_;
  MetaHealthStateMachine machine_;
  folly::Synchronized<HealthSnapshot> snapshot_;
  std::thread worker_;
  std::atomic<bool> running_{false};
  int64_t lastLeaderTerm_{0};
};

}  // namespace nebula::meta

#endif  // META_HEALTH_METASEMANTICHEALTHMANAGER_H_
