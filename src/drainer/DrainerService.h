/* Copyright (c) 2024 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#ifndef DRAINER_DRAINERSERVICE_H_
#define DRAINER_DRAINERSERVICE_H_

#include "common/base/Base.h"
#include "interface/gen-cpp2/SyncService.h"

namespace nebula {
namespace drainer {

class DrainerEnv;

class DrainerService final : public sync::cpp2::SyncServiceSvIf {
 public:
  explicit DrainerService(DrainerEnv* env);

  folly::Future<sync::cpp2::AppendLogsResponse> future_appendLogs(
      const sync::cpp2::AppendLogsRequest& req) override;

  folly::Future<sync::cpp2::HeartbeatResponse> future_heartbeat(
      const sync::cpp2::HeartbeatRequest& req) override;

  folly::Future<sync::cpp2::CheckpointQueryResponse> future_queryCheckpoint(
      const sync::cpp2::CheckpointQueryRequest& req) override;

 private:
  DrainerEnv* env_{nullptr};
};

}  // namespace drainer
}  // namespace nebula

#endif  // DRAINER_DRAINERSERVICE_H_
