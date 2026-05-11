/* Copyright (c) 2024 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#ifndef DRAINER_DRAINERFLAGS_H_
#define DRAINER_DRAINERFLAGS_H_

#include <gflags/gflags.h>

DECLARE_int32(drainer_port);
DECLARE_string(drainer_data_path);
DECLARE_string(drainer_secondary_meta_addrs);
DECLARE_int32(drainer_apply_workers);
DECLARE_int64(drainer_apply_batch_bytes);
DECLARE_int32(drainer_max_inflight_per_part);
DECLARE_int32(drainer_schema_visibility_poll_ms);
DECLARE_int32(drainer_schema_visibility_timeout_ms);
DECLARE_int32(drainer_checkpoint_persist_interval_ms);
DECLARE_bool(drainer_dedup_window);
DECLARE_bool(drainer_tls_enabled);
DECLARE_string(drainer_tls_cert);
DECLARE_string(drainer_tls_key);
DECLARE_string(drainer_tls_ca);
DECLARE_bool(drainer_tls_require_client_cert);
DECLARE_int32(drainer_metrics_port);
DECLARE_string(drainer_listener_token);
DECLARE_int32(drainer_apply_retry_times);
DECLARE_int32(drainer_apply_retry_interval_ms);

#endif  // DRAINER_DRAINERFLAGS_H_
