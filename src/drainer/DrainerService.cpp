/* Copyright (c) 2024 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include "drainer/DrainerService.h"

#include "common/base/Base.h"
#include "common/time/WallClock.h"
#include "drainer/CheckpointStore.h"
#include "drainer/DrainerEnv.h"
#include "drainer/MetaApplier.h"
#include "drainer/PartApplier.h"

namespace nebula {
namespace drainer {

DrainerService::DrainerService(DrainerEnv* env) : env_(env) {
  CHECK_NOTNULL(env_);
}

folly::Future<sync::cpp2::AppendLogsResponse> DrainerService::future_appendLogs(
    const sync::cpp2::AppendLogsRequest& req) {
  const auto& batch = req.get_batch();

  sync::cpp2::AppendLogsResponse resp;

  // 1) Auth: verify listenerToken (stub - accept all for now)
  // TODO(sync): Validate req.get_listenerToken() against
  //             env_->checkpointStore()->getToken(batch.get_clusterId())

  // 2) Loop guard: if batch.clusterId == self, return E_SYNC_CLUSTER_LOOP
  if (env_->isSelfClusterId(batch.get_clusterId())) {
    LOG(WARNING) << "Detected cluster loop, dropping batch from self cluster "
                 << batch.get_clusterId();
    resp.code_ref() = nebula::cpp2::ErrorCode::E_SYNC_CLUSTER_LOOP;
    return resp;
  }

  // 3) Epoch fencing: if batch.epoch < currentEpoch, return E_SYNC_EPOCH_MISMATCH
  auto currentEpoch = env_->currentEpochFor(batch.get_clusterId());
  if (batch.get_epoch() < currentEpoch) {
    LOG(WARNING) << "Epoch mismatch for cluster " << batch.get_clusterId()
                 << ": batch epoch=" << batch.get_epoch()
                 << " < current epoch=" << currentEpoch;
    resp.code_ref() = nebula::cpp2::ErrorCode::E_SYNC_EPOCH_MISMATCH;
    return resp;
  }

  auto spaceId = batch.get_spaceId();
  auto partId = batch.get_partId();

  // 4) Route: if partId == 0, route to MetaApplier; else route to PartApplier
  // 5) Dedup: if batch.lastLogId <= applier.lastApplied(), return OK (already applied)
  // 6) Gap check: if batch.firstLogId > lastApplied + 1, return E_LOG_GAP with requestResendFrom
  LogID lastApplied = env_->checkpointStore()->getLastApplied(spaceId, partId);

  if (batch.get_lastLogId() <= lastApplied) {
    LOG(INFO) << "Batch already applied for space=" << spaceId << " part=" << partId
              << " batch.lastLogId=" << batch.get_lastLogId()
              << " <= lastApplied=" << lastApplied;
    resp.code_ref() = nebula::cpp2::ErrorCode::SUCCEEDED;
    resp.ackedLogId_ref() = lastApplied;
    return resp;
  }

  if (batch.get_firstLogId() > lastApplied + 1) {
    LOG(WARNING) << "Log gap detected for space=" << spaceId << " part=" << partId
                 << " batch.firstLogId=" << batch.get_firstLogId()
                 << " > lastApplied+1=" << (lastApplied + 1);
    resp.code_ref() = nebula::cpp2::ErrorCode::E_LOG_GAP;
    resp.requestResendFrom_ref() = lastApplied + 1;
    return resp;
  }

  // 7) Enqueue to applier, return future
  folly::Future<LogID> appliedFuture = folly::makeFuture<LogID>(0);
  if (partId == 0) {
    // Meta partition routes to MetaApplier
    appliedFuture =
        env_->metaApplier()->enqueue(sync::cpp2::SyncLogBatch(batch));
  } else {
    // Data partition routes to PartApplier
    auto* applier = env_->getOrCreatePartApplier(spaceId, partId);
    appliedFuture = applier->enqueue(sync::cpp2::SyncLogBatch(batch));
  }

  return std::move(appliedFuture)
      .thenValue([](LogID ackedId) {
        sync::cpp2::AppendLogsResponse r;
        r.code_ref() = nebula::cpp2::ErrorCode::SUCCEEDED;
        r.ackedLogId_ref() = ackedId;
        return r;
      })
      .thenError([spaceId, partId](folly::exception_wrapper ew) {
        LOG(ERROR) << "Failed to apply batch for space=" << spaceId << " part=" << partId
                   << ": " << ew.what();
        sync::cpp2::AppendLogsResponse r;
        r.code_ref() = nebula::cpp2::ErrorCode::E_UNKNOWN;
        return r;
      });
}

folly::Future<sync::cpp2::HeartbeatResponse> DrainerService::future_heartbeat(
    const sync::cpp2::HeartbeatRequest& req) {
  sync::cpp2::HeartbeatResponse resp;

  auto spaceId = req.get_spaceId();
  auto partId = req.get_partId();

  // Return drainer's lastApplied and wallclock for the requested (space, part)
  auto lastApplied = env_->checkpointStore()->getLastApplied(spaceId, partId);

  resp.code_ref() = nebula::cpp2::ErrorCode::SUCCEEDED;
  resp.drainerLastAppliedLogId_ref() = lastApplied;
  resp.drainerWallClockMs_ref() = time::WallClock::fastNowInMilliSec();
  return resp;
}

folly::Future<sync::cpp2::CheckpointQueryResponse> DrainerService::future_queryCheckpoint(
    const sync::cpp2::CheckpointQueryRequest& req) {
  sync::cpp2::CheckpointQueryResponse resp;

  auto spaceId = req.get_spaceId();
  const auto& parts = req.get_parts();

  // Return map of partId -> lastApplied from CheckpointStore
  std::map<PartitionID, LogID> lastAppliedMap;
  for (auto partId : parts) {
    lastAppliedMap[partId] = env_->checkpointStore()->getLastApplied(spaceId, partId);
  }

  resp.code_ref() = nebula::cpp2::ErrorCode::SUCCEEDED;
  resp.lastApplied_ref() = std::move(lastAppliedMap);
  return resp;
}

}  // namespace drainer
}  // namespace nebula
