/* Copyright (c) 2024 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include <gflags/gflags.h>

DEFINE_int64(sync_listener_batch_bytes, 65536, "Max bytes per batch for sync listener");
DEFINE_int64(sync_listener_batch_count, 256, "Max log count per batch for sync listener");
DEFINE_int32(sync_listener_batch_ms, 50, "Max milliseconds to wait before flushing a batch");
DEFINE_int64(sync_listener_max_inflight_bytes,
             33554432,
             "Max inflight bytes for backpressure (32MB)");
DEFINE_bool(sync_listener_dump_only,
            true,
            "P1 MVP mode: dump WAL entries to local file only, do not send to drainer");
DEFINE_string(sync_listener_dump_path,
              "",
              "Path for dump files. If empty, uses the listener WAL path");
DEFINE_string(sync_listener_drainer_addrs,
              "",
              "Comma-separated list of drainer addresses (ip:port) for non-dump mode");
DEFINE_string(sync_listener_token,
              "",
              "Authentication token sent with each AppendLogs RPC to the drainer");
DEFINE_bool(sync_listener_compression,
            false,
            "Enable zstd compression for sync batches sent to drainer");
DEFINE_int32(sync_listener_compression_level,
             3,
             "Zstd compression level (1-22) for sync batches");
