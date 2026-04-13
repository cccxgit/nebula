/* Copyright (c) 2026 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include "meta/health/MetaSemanticHealthManager.h"

#include <thread>

#include "common/stats/StatsManager.h"
#include "common/time/WallClock.h"

DEFINE_bool(enable_semantic_health_check, false, "Enable metad semantic health self-check");
DEFINE_string(semantic_health_mode,
              "observe_only",
              "observe_only / report_only / fail_liveness / direct_exit");
DEFINE_int32(semantic_health_probe_interval_secs, 5, "Semantic probe interval in seconds");
DEFINE_int32(semantic_health_probe_timeout_ms, 1500, "Semantic probe timeout in milliseconds");
DEFINE_int32(semantic_health_leader_grace_secs, 30, "Grace period after becoming leader");
DEFINE_int32(semantic_health_consecutive_failures, 4, "Consecutive failures threshold");
DEFINE_int32(semantic_health_recover_successes, 2, "Successes required to recover from suspect");
DEFINE_int64(semantic_health_session_probe_id, -1, "Session id used by semantic probe");
DEFINE_int32(semantic_health_monitor_stall_timeout_ms, 17000, "Monitor tick stall timeout");

namespace nebula::meta {

namespace {
auto kStateGauge =
    stats::StatsManager::registerStats("metad_semantic_health_state", "avg");
auto kUnhealthyTransitions =
    stats::StatsManager::registerStats("metad_semantic_unhealthy_transitions_total", "sum");
auto kProbeLatencyUs =
    stats::StatsManager::registerHisto(
        "metad_semantic_probe_latency_us", 500, 0, 5000000, "p95,p99");
auto kProbeTotal = stats::StatsManager::registerStats("metad_semantic_probe_total", "sum");

bool enabledFailingMode() {
  return FLAGS_semantic_health_mode == "fail_liveness" ||
         FLAGS_semantic_health_mode == "direct_exit";
}
}  // namespace

MetaSemanticHealthManager::MetaSemanticHealthManager(kvstore::KVStore* kv, HostAddr localMetaAddr)
    : kv_(kv),
      localMetaAddr_(std::move(localMetaAddr)),
      probeClient_(localMetaAddr_),
      machine_(FLAGS_semantic_health_consecutive_failures,
               FLAGS_semantic_health_recover_successes,
               FLAGS_semantic_health_leader_grace_secs) {}

MetaSemanticHealthManager::~MetaSemanticHealthManager() {
  stop();
}

void MetaSemanticHealthManager::start() {
  if (!FLAGS_enable_semantic_health_check) {
    return;
  }
  if (running_.exchange(true)) {
    return;
  }
  worker_ = std::thread([this] { runLoop(); });
}

void MetaSemanticHealthManager::stop() {
  if (!running_.exchange(false)) {
    return;
  }
  if (worker_.joinable()) {
    worker_.join();
  }
}

HealthSnapshot MetaSemanticHealthManager::snapshot() const {
  return *snapshot_.rlock();
}

bool MetaSemanticHealthManager::shouldFailLiveness() const {
  if (!FLAGS_enable_semantic_health_check) {
    return false;
  }
  if (!enabledFailingMode()) {
    return false;
  }
  auto snap = snapshot();
  auto now = time::WallClock::fastNowInMilliSec();
  auto stalled = now - snap.lastMonitorTickMs > FLAGS_semantic_health_monitor_stall_timeout_ms;
  return stalled || snap.state == SemanticHealthState::LEADER_UNHEALTHY;
}

void MetaSemanticHealthManager::runLoop() {
  while (running_.load()) {
    runOnce();
    std::this_thread::sleep_for(std::chrono::seconds(FLAGS_semantic_health_probe_interval_secs));
  }
}

void MetaSemanticHealthManager::updateProbe(ProbeResult* target, ProbeResult&& current) {
  target->probeName = current.probeName;
  target->latencyMs = current.latencyMs;
  target->code = current.code;
  target->ok = current.ok;
  target->error = std::move(current.error);
  if (target->ok) {
    target->consecutiveFailures = 0;
    target->lastSuccessMs = time::WallClock::fastNowInMilliSec();
  } else {
    target->consecutiveFailures += 1;
  }
  stats::StatsManager::addValue(kProbeTotal);
  stats::StatsManager::addValue(kProbeLatencyUs, target->latencyMs * 1000);
}

void MetaSemanticHealthManager::runOnce() {
  auto nowMs = time::WallClock::fastNowInMilliSec();
  auto partLeaderRet = kv_->partLeader(kDefaultSpaceId, kDefaultPartId);

  auto guard = snapshot_.wlock();
  guard->lastMonitorTickMs = nowMs;

  if (!partLeaderRet.ok()) {
    machine_.enterFollower(nowMs, lastLeaderTerm_);
    *guard = machine_.snapshot();
    guard->decision = "leader unknown, fail-open";
    return;
  }

  auto leader = partLeaderRet.value();
  bool isLeader = (leader == localMetaAddr_);
  if (!isLeader) {
    machine_.enterFollower(nowMs, lastLeaderTerm_);
    *guard = machine_.snapshot();
    return;
  }


  if (guard->state == SemanticHealthState::FOLLOWER_STANDBY) {
    ++lastLeaderTerm_;
  }

  if (guard->state == SemanticHealthState::FOLLOWER_STANDBY ||
      guard->state == SemanticHealthState::LEADER_GRACE ||
      guard->term != lastLeaderTerm_) {
    machine_.enterLeaderGrace(nowMs, lastLeaderTerm_);
  }

  if (machine_.inGrace(nowMs)) {
    *guard = machine_.snapshot();
    return;
  }

  auto spacesProbe = probeClient_.probeListSpaces(FLAGS_semantic_health_probe_timeout_ms);
  auto sessionProbe =
      probeClient_.probeGetSession(FLAGS_semantic_health_probe_timeout_ms,
                                   static_cast<SessionID>(FLAGS_semantic_health_session_probe_id));

  updateProbe(&guard->spacesProbe, std::move(spacesProbe));
  updateProbe(&guard->sessionProbe, std::move(sessionProbe));

  machine_.updateByProbe(nowMs,
                         lastLeaderTerm_,
                         guard->spacesProbe.consecutiveFailures,
                         guard->sessionProbe.consecutiveFailures,
                         false);

  auto before = guard->state;
  auto merged = machine_.snapshot();
  merged.lastMonitorTickMs = guard->lastMonitorTickMs;
  merged.spacesProbe = guard->spacesProbe;
  merged.sessionProbe = guard->sessionProbe;
  *guard = std::move(merged);

  stats::StatsManager::addValue(kStateGauge, static_cast<int32_t>(guard->state));
  if (before != SemanticHealthState::LEADER_UNHEALTHY &&
      guard->state == SemanticHealthState::LEADER_UNHEALTHY) {
    stats::StatsManager::addValue(kUnhealthyTransitions);
  }

  if (FLAGS_semantic_health_mode == "direct_exit" &&
      guard->state == SemanticHealthState::LEADER_UNHEALTHY) {
    LOG(FATAL) << "semantic health entered unhealthy in direct_exit mode";
  }
}

}  // namespace nebula::meta
