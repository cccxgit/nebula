// Copyright (c) 2024 vesoft inc. All rights reserved.
//
// This source code is licensed under Apache 2.0 License.

#include "graph/executor/admin/SyncExecutor.h"

#include "graph/planner/plan/Admin.h"

namespace nebula {
namespace graph {

folly::Future<Status> ShowSyncStatusExecutor::execute() {
  SCOPED_TIMER(&execTime_);
  auto spaceId = qctx()->rctx()->session()->space().id;
  return qctx()->getMetaClient()->getSyncStatus(spaceId).via(runner()).thenValue(
      [this](StatusOr<std::vector<meta::cpp2::SyncStatusItem>> resp) {
        SCOPED_TIMER(&execTime_);
        if (!resp.ok()) {
          LOG(WARNING) << "Show sync status failed: " << resp.status();
          return resp.status();
        }

        auto statusItems = std::move(resp).value();
        std::sort(statusItems.begin(), statusItems.end(), [](const auto& a, const auto& b) {
          return a.get_part_id() < b.get_part_id();
        });

        DataSet result({"PartId", "SyncStatus", "LogIdLag", "TimeLatencyMs"});
        for (const auto& item : statusItems) {
          Row row;
          row.values.emplace_back(item.get_part_id());
          row.values.emplace_back(item.get_status());
          row.values.emplace_back(item.get_log_id_lag());
          row.values.emplace_back(item.get_time_latency_ms());
          result.emplace_back(std::move(row));
        }

        return finish(std::move(result));
      });
}

folly::Future<Status> ShowDrainerSyncStatusExecutor::execute() {
  SCOPED_TIMER(&execTime_);
  auto spaceId = qctx()->rctx()->session()->space().id;
  return qctx()->getMetaClient()->getDrainerSyncStatus(spaceId).via(runner()).thenValue(
      [this](StatusOr<std::vector<meta::cpp2::DrainerSyncStatusItem>> resp) {
        SCOPED_TIMER(&execTime_);
        if (!resp.ok()) {
          LOG(WARNING) << "Show drainer sync status failed: " << resp.status();
          return resp.status();
        }

        auto statusItems = std::move(resp).value();
        std::sort(statusItems.begin(), statusItems.end(), [](const auto& a, const auto& b) {
          if (a.get_drainer_host() != b.get_drainer_host()) {
            return a.get_drainer_host() < b.get_drainer_host();
          }
          return a.get_part_id() < b.get_part_id();
        });

        DataSet result({"DrainerHost", "PartId", "SyncStatus", "LogIdLag",
                        "TimeLatencyMs", "Epoch", "LastAppliedLogId"});
        for (const auto& item : statusItems) {
          Row row;
          row.values.emplace_back(item.get_drainer_host());
          row.values.emplace_back(item.get_part_id());
          row.values.emplace_back(item.get_status());
          row.values.emplace_back(item.get_log_id_lag());
          row.values.emplace_back(item.get_time_latency_ms());
          row.values.emplace_back(item.get_epoch());
          row.values.emplace_back(item.get_last_applied_log_id());
          result.emplace_back(std::move(row));
        }

        return finish(std::move(result));
      });
}

}  // namespace graph
}  // namespace nebula
