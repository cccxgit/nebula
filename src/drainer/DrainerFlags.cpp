/* Copyright (c) 2024 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include <gflags/gflags.h>

DEFINE_int32(drainer_port, 9889, "RPC port for the drainer service");
DEFINE_string(drainer_data_path, "data/drainer", "Root data path for drainer persistent state");
DEFINE_string(drainer_secondary_meta_addrs,
              "",
              "Comma-separated list of backup cluster metad addresses (ip:port)");
DEFINE_int32(drainer_apply_workers, 16, "Number of worker threads for applying WAL entries");
DEFINE_int64(drainer_apply_batch_bytes,
             131072,
             "Max bytes per apply batch sent to the backup cluster (128KB)");
DEFINE_int32(drainer_max_inflight_per_part,
             16,
             "Max number of inflight apply batches per partition");
DEFINE_int32(drainer_schema_visibility_poll_ms,
             200,
             "Poll interval in ms when waiting for schema visibility on the backup cluster");
DEFINE_int32(drainer_schema_visibility_timeout_ms,
             30000,
             "Timeout in ms for schema visibility check before failing the batch");
DEFINE_int32(drainer_checkpoint_persist_interval_ms,
             1000,
             "Interval in ms to persist apply checkpoint to disk");
DEFINE_bool(drainer_dedup_window,
            true,
            "Enable deduplication window to skip already-applied WAL entries");
DEFINE_bool(drainer_tls_enabled, false, "Enable TLS for drainer RPC connections");
DEFINE_string(drainer_tls_cert, "", "Path to TLS certificate file for drainer");
DEFINE_string(drainer_tls_key, "", "Path to TLS private key file for drainer");
DEFINE_string(drainer_tls_ca, "", "Path to TLS CA certificate file for drainer");
DEFINE_bool(drainer_tls_require_client_cert,
            false,
            "Require client certificate for mutual TLS authentication");
DEFINE_int32(drainer_metrics_port, 19889, "HTTP port for drainer metrics/stats endpoint");
DEFINE_string(drainer_listener_token,
              "",
              "Authentication token that listeners must present. Empty means accept all.");
DEFINE_int32(drainer_apply_retry_times,
             3,
             "Max number of retries when applying WAL entries to the backup cluster fails");
DEFINE_int32(drainer_apply_retry_interval_ms,
             100,
             "Base retry interval in ms (exponential backoff: base, base*5, base*20)");
