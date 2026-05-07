/* Copyright (c) 2024 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#ifndef DRAINER_DRAINERMETRICS_H_
#define DRAINER_DRAINERMETRICS_H_

#include "common/base/Base.h"
#include "common/stats/StatsManager.h"

namespace nebula {
namespace drainer {

// Per-partition apply metrics
extern stats::CounterId kSyncBatchesReceived;
extern stats::CounterId kSyncBatchesApplied;
extern stats::CounterId kSyncBatchesFailed;
extern stats::CounterId kSyncEntriesApplied;
extern stats::CounterId kSyncBytesApplied;

// Latency histograms
extern stats::CounterId kSyncApplyLatencyUs;
extern stats::CounterId kSyncTransportLatencyMs;

// Schema barrier
extern stats::CounterId kSchemaBarrierWaits;
extern stats::CounterId kSchemaBarrierTimeouts;

// Epoch fencing
extern stats::CounterId kEpochMismatchTotal;
extern stats::CounterId kClusterLoopTotal;
extern stats::CounterId kAuthFailedTotal;

// Lag gauge (simplified)
extern stats::CounterId kSyncLogIdLag;

void initDrainerMetrics();

}  // namespace drainer
}  // namespace nebula

#endif  // DRAINER_DRAINERMETRICS_H_
