/* Copyright (c) 2026 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#ifndef META_HEALTH_METAHEALTHSTATE_H_
#define META_HEALTH_METAHEALTHSTATE_H_

#include <cstdint>
#include <string>

#include "common/base/Base.h"
#include "interface/gen-cpp2/common_types.h"

namespace nebula::meta {

enum class SemanticHealthState : int32_t {
  FOLLOWER_STANDBY = 0,
  LEADER_GRACE = 1,
  LEADER_HEALTHY = 2,
  LEADER_SUSPECT = 3,
  LEADER_UNHEALTHY = 4,
};

struct ProbeResult {
  bool ok{false};
  std::string probeName;
  std::string error;
  int64_t latencyMs{0};
  int64_t lastSuccessMs{0};
  int32_t consecutiveFailures{0};
  nebula::cpp2::ErrorCode code{nebula::cpp2::ErrorCode::E_UNKNOWN};
};

struct HealthSnapshot {
  bool isLeader{false};
  int64_t term{0};
  SemanticHealthState state{SemanticHealthState::FOLLOWER_STANDBY};
  int64_t stateSinceMs{0};
  bool latched{false};
  bool leaderGrace{false};
  int64_t lastMonitorTickMs{0};
  ProbeResult spacesProbe;
  ProbeResult sessionProbe;
  std::string decision{"init"};
};

class MetaHealthStateMachine {
 public:
  MetaHealthStateMachine(int32_t failThreshold, int32_t recoverSuccesses, int64_t graceSecs);

  void enterFollower(int64_t nowMs, int64_t term);
  void enterLeaderGrace(int64_t nowMs, int64_t term);
  bool inGrace(int64_t nowMs) const;
  void updateByProbe(int64_t nowMs,
                     int64_t term,
                     int32_t spacesFailures,
                     int32_t sessionFailures,
                     bool monitorStalled);

  const HealthSnapshot& snapshot() const {
    return snapshot_;
  }

 private:
  void transit(SemanticHealthState state, int64_t nowMs, std::string reason);
  void latchUnhealthy(int64_t nowMs, std::string reason);

 private:
  HealthSnapshot snapshot_;
  int32_t failThreshold_{4};
  int32_t recoverSuccesses_{2};
  int64_t graceMs_{30000};
  int32_t healthyStreak_{0};
};

std::string toString(SemanticHealthState state);

}  // namespace nebula::meta

#endif  // META_HEALTH_METAHEALTHSTATE_H_
