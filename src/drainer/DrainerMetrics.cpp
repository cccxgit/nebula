/* Copyright (c) 2024 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include "drainer/DrainerMetrics.h"

#include "common/base/Base.h"
#include "common/stats/StatsManager.h"

namespace nebula {
namespace drainer {

// Per-partition apply metrics
stats::CounterId kSyncBatchesReceived;
stats::CounterId kSyncBatchesApplied;
stats::CounterId kSyncBatchesFailed;
stats::CounterId kSyncEntriesApplied;
stats::CounterId kSyncBytesApplied;

// Latency histograms
stats::CounterId kSyncApplyLatencyUs;
stats::CounterId kSyncTransportLatencyMs;

// Schema barrier
stats::CounterId kSchemaBarrierWaits;
stats::CounterId kSchemaBarrierTimeouts;

// Epoch fencing
stats::CounterId kEpochMismatchTotal;
stats::CounterId kClusterLoopTotal;
stats::CounterId kAuthFailedTotal;

// Lag gauge
stats::CounterId kSyncLogIdLag;

void initDrainerMetrics() {
  // Per-partition apply counters
  kSyncBatchesReceived =
      stats::StatsManager::registerStats("sync_batches_received", "rate, sum");
  kSyncBatchesApplied =
      stats::StatsManager::registerStats("sync_batches_applied", "rate, sum");
  kSyncBatchesFailed =
      stats::StatsManager::registerStats("sync_batches_failed", "rate, sum");
  kSyncEntriesApplied =
      stats::StatsManager::registerStats("sync_entries_applied", "rate, sum");
  kSyncBytesApplied =
      stats::StatsManager::registerStats("sync_bytes_applied", "rate, sum");

  // Latency histograms (microseconds for apply, milliseconds for transport)
  kSyncApplyLatencyUs = stats::StatsManager::registerHisto(
      "sync_apply_latency_us", 1000, 0, 20000, "avg, p75, p95, p99, p999");
  kSyncTransportLatencyMs = stats::StatsManager::registerHisto(
      "sync_transport_latency_ms", 10, 0, 5000, "avg, p50, p75, p95, p99, p999");

  // Schema barrier metrics
  kSchemaBarrierWaits =
      stats::StatsManager::registerStats("schema_barrier_waits", "rate, sum");
  kSchemaBarrierTimeouts =
      stats::StatsManager::registerStats("schema_barrier_timeouts", "rate, sum");

  // Epoch fencing metrics
  kEpochMismatchTotal =
      stats::StatsManager::registerStats("epoch_mismatch_total", "rate, sum");
  kClusterLoopTotal =
      stats::StatsManager::registerStats("cluster_loop_total", "rate, sum");
  kAuthFailedTotal =
      stats::StatsManager::registerStats("auth_failed_total", "rate, sum");

  // Lag gauge - represents the logId gap between source and drainer
  kSyncLogIdLag =
      stats::StatsManager::registerStats("sync_log_id_lag", "sum");
}

}  // namespace drainer
}  // namespace nebula
