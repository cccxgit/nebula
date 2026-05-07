/* Copyright (c) 2024 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include <gflags/gflags.h>

DEFINE_string(meta_sync_listener_dump_path,
              "",
              "Path for meta sync dump files. If empty, uses the listener WAL path");
