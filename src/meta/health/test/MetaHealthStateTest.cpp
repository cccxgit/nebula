/* Copyright (c) 2026 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include <gtest/gtest.h>

#include "meta/health/MetaHealthState.h"

namespace nebula::meta {

TEST(MetaHealthStateTest, FollowerToHealthyFlow) {
  MetaHealthStateMachine machine(4, 2, 30);
  machine.enterFollower(1000, 1);
  EXPECT_EQ(machine.snapshot().state, SemanticHealthState::FOLLOWER_STANDBY);

  machine.enterLeaderGrace(2000, 2);
  EXPECT_EQ(machine.snapshot().state, SemanticHealthState::LEADER_GRACE);
  EXPECT_TRUE(machine.inGrace(2500));

  machine.updateByProbe(40000, 2, 0, 0, false);
  EXPECT_EQ(machine.snapshot().state, SemanticHealthState::LEADER_HEALTHY);
}

TEST(MetaHealthStateTest, SuspectAndUnhealthyFlow) {
  MetaHealthStateMachine machine(4, 2, 0);
  machine.enterLeaderGrace(1000, 1);
  machine.updateByProbe(2000, 1, 2, 0, false);
  EXPECT_EQ(machine.snapshot().state, SemanticHealthState::LEADER_SUSPECT);

  machine.updateByProbe(3000, 1, 4, 0, false);
  EXPECT_EQ(machine.snapshot().state, SemanticHealthState::LEADER_UNHEALTHY);
  EXPECT_TRUE(machine.snapshot().latched);

  machine.updateByProbe(4000, 1, 0, 0, false);
  EXPECT_EQ(machine.snapshot().state, SemanticHealthState::LEADER_UNHEALTHY);
}

TEST(MetaHealthStateTest, RecoverFromSuspect) {
  MetaHealthStateMachine machine(4, 2, 0);
  machine.enterLeaderGrace(1000, 1);
  machine.updateByProbe(2000, 1, 2, 0, false);
  EXPECT_EQ(machine.snapshot().state, SemanticHealthState::LEADER_SUSPECT);

  machine.updateByProbe(3000, 1, 0, 0, false);
  EXPECT_EQ(machine.snapshot().state, SemanticHealthState::LEADER_SUSPECT);

  machine.updateByProbe(4000, 1, 0, 0, false);
  EXPECT_EQ(machine.snapshot().state, SemanticHealthState::LEADER_HEALTHY);
}

}  // namespace nebula::meta
