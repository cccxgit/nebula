/* Copyright (c) 2024 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#ifndef KVSTORE_LISTENER_SYNC_LISTENER_FLAGS_H_
#define KVSTORE_LISTENER_SYNC_LISTENER_FLAGS_H_

#include <gflags/gflags.h>

DECLARE_int64(sync_listener_batch_bytes);
DECLARE_int64(sync_listener_batch_count);
DECLARE_int32(sync_listener_batch_ms);
DECLARE_int64(sync_listener_max_inflight_bytes);
DECLARE_bool(sync_listener_dump_only);
DECLARE_string(sync_listener_dump_path);
DECLARE_string(sync_listener_drainer_addrs);
DECLARE_string(sync_listener_token);
DECLARE_bool(sync_listener_compression);
DECLARE_int32(sync_listener_compression_level);

#endif  // KVSTORE_LISTENER_SYNC_LISTENER_FLAGS_H_
