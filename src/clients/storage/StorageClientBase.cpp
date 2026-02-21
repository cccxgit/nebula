/* Copyright (c) 2020 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include "clients/storage/StorageClientBase.h"

DEFINE_int32(storage_client_timeout_ms, 60 * 1000, "storage client timeout");
DEFINE_uint32(storage_client_retry_interval_ms,
              1000,
              "storage client sleep interval milliseconds between retry");
DEFINE_uint32(
    max_storage_inflight_per_query,
    8,
    "Max in-flight storage RPCs per query request in graph service, 0 means no limit");

namespace nebula {
namespace storage {}  // namespace storage
}  // namespace nebula
