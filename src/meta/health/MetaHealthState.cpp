/* Copyright (c) 2026 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include "meta/health/MetaHealthState.h"

namespace nebula::meta {

MetaHealthStateMachine::MetaHealthStateMachine(int32_t failThreshold,
                                               int32_t recoverSuccesses,
                                               int64_t graceSecs)
    : failThreshold_(std::max(1, failThreshold)),
      recoverSuccesses_(std::max(1, recoverSuccesses)),
      graceMs_(std::max<int64_t>(0, graceSecs) * 1000) {
  snapshot_.spacesProbe.probeName = "list_spaces";
  snapshot_.sessionProbe.probeName = "get_session";
}

void MetaHealthStateMachine::transit(SemanticHealthState state, int64_t nowMs, std::string reason) {
  if (snapshot_.state != state) {
    snapshot_.stateSinceMs = nowMs;
  }
  snapshot_.state = state;
  snapshot_.decision = std::move(reason);
}

void MetaHealthStateMachine::enterFollower(int64_t nowMs, int64_t term) {
  snapshot_.isLeader = false;
  snapshot_.term = term;
  snapshot_.latched = false;
  snapshot_.leaderGrace = false;
  healthyStreak_ = 0;
  transit(SemanticHealthState::FOLLOWER_STANDBY, nowMs, "follower standby");
}

void MetaHealthStateMachine::enterLeaderGrace(int64_t nowMs, int64_t term) {
  snapshot_.isLeader = true;
  snapshot_.term = term;
  snapshot_.latched = false;
  snapshot_.leaderGrace = true;
  healthyStreak_ = 0;
  transit(SemanticHealthState::LEADER_GRACE, nowMs, "leader grace");
}

bool MetaHealthStateMachine::inGrace(int64_t nowMs) const {
  return snapshot_.state == SemanticHealthState::LEADER_GRACE &&
         (nowMs - snapshot_.stateSinceMs < graceMs_);
}

void MetaHealthStateMachine::latchUnhealthy(int64_t nowMs, std::string reason) {
  snapshot_.latched = true;
  snapshot_.leaderGrace = false;
  transit(SemanticHealthState::LEADER_UNHEALTHY, nowMs, std::move(reason));
}

void MetaHealthStateMachine::updateByProbe(int64_t nowMs,
                                           int64_t term,
                                           int32_t spacesFailures,
                                           int32_t sessionFailures,
                                           bool monitorStalled) {
  snapshot_.term = term;
  snapshot_.isLeader = true;
  snapshot_.leaderGrace = false;

  if (snapshot_.latched) {
    transit(SemanticHealthState::LEADER_UNHEALTHY, nowMs, "latched unhealthy");
    return;
  }

  if (monitorStalled) {
    latchUnhealthy(nowMs, "monitor stalled");
    return;
  }

  if (spacesFailures >= failThreshold_) {
    latchUnhealthy(nowMs, "list_spaces failed continuously");
    return;
  }
  if (sessionFailures >= failThreshold_) {
    latchUnhealthy(nowMs, "get_session failed continuously");
    return;
  }

  auto suspectThreshold = std::max(1, failThreshold_ / 2);
  if (spacesFailures >= suspectThreshold || sessionFailures >= suspectThreshold) {
    healthyStreak_ = 0;
    transit(SemanticHealthState::LEADER_SUSPECT, nowMs, "partial probe failures");
    return;
  }

  healthyStreak_ += 1;
  if (snapshot_.state == SemanticHealthState::LEADER_SUSPECT &&
      healthyStreak_ < recoverSuccesses_) {
    transit(SemanticHealthState::LEADER_SUSPECT, nowMs, "recovering from suspect");
    return;
  }
  transit(SemanticHealthState::LEADER_HEALTHY, nowMs, "all probes passed");
}

std::string toString(SemanticHealthState state) {
  switch (state) {
    case SemanticHealthState::FOLLOWER_STANDBY:
      return "FOLLOWER_STANDBY";
    case SemanticHealthState::LEADER_GRACE:
      return "LEADER_GRACE";
    case SemanticHealthState::LEADER_HEALTHY:
      return "LEADER_HEALTHY";
    case SemanticHealthState::LEADER_SUSPECT:
      return "LEADER_SUSPECT";
    case SemanticHealthState::LEADER_UNHEALTHY:
      return "LEADER_UNHEALTHY";
  }
  return "UNKNOWN";
}

}  // namespace nebula::meta
