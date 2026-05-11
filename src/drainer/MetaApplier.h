/* Copyright (c) 2024 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#ifndef DRAINER_METAAPPLIER_H_
#define DRAINER_METAAPPLIER_H_

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include <folly/futures/Future.h>
#include <folly/futures/Promise.h>

#include "common/thrift/ThriftTypes.h"
#include "interface/gen-cpp2/sync_types.h"

namespace nebula {
namespace meta {
class MetaClient;
}  // namespace meta
}  // namespace nebula

namespace nebula {
namespace drainer {

class DrainerEnv;

/**
 * MetaApplier handles meta (DDL) sync log batches. It applies schema changes
 * to the backup cluster's meta service and tracks schema visibility for the
 * schema barrier protocol (PartAppliers wait until dependent schemas are
 * visible before applying DML that references them).
 */
class MetaApplier {
 public:
  explicit MetaApplier(DrainerEnv* env);
  ~MetaApplier();

  void start();
  void stop();

  /**
   * Enqueue a meta batch for processing. Returns a future that resolves
   * with the last applied log id when the batch is applied.
   */
  folly::Future<LogID> enqueue(sync::cpp2::SyncLogBatch&& batch);

  /**
   * Check if a schema version is visible (applied) on the backup cluster.
   * Used by PartAppliers to implement the schema barrier.
   */
  bool schemaVisible(GraphSpaceID spaceId, int64_t schemaVer) const;

  /**
   * Park a callback that will be invoked once the given schema version
   * becomes visible. If the schema is already visible, the callback
   * is invoked immediately.
   */
  void parkUntilSchemaReady(GraphSpaceID spaceId,
                            int64_t schemaVer,
                            std::function<void()> callback);

 private:
  void run_();
  void applyOne_(const sync::cpp2::SyncLogEntry& entry, GraphSpaceID spaceId);
  void markVisible_(GraphSpaceID spaceId, int64_t schemaVer);
  void releaseParked_(GraphSpaceID spaceId, int64_t schemaVer);

  /**
   * Decode a single meta payload encoded by MetaSyncListener::encodeMetaPayload_().
   * Format: [keyLen (4 bytes)][key][valueLen (4 bytes)][value]
   * Returns (key, value) pairs extracted from the payload buffer.
   */
  static std::vector<std::pair<std::string, std::string>>
  decodeMetaPayload_(folly::StringPiece payload);

  /**
   * Determine the meta key type from its prefix and return a human-readable name.
   */
  static std::string classifyMetaKey_(folly::StringPiece key);

  DrainerEnv* env_;
  std::atomic<bool> stop_{false};
  std::thread worker_;

  struct QueueItem {
    sync::cpp2::SyncLogBatch batch;
    folly::Promise<LogID> promise;
  };
  std::deque<QueueItem> queue_;
  std::mutex queueLock_;
  std::condition_variable cv_;

  // Schema visibility tracking: (spaceId, schemaVer) -> visible
  std::map<std::pair<GraphSpaceID, int64_t>, bool> schemaVisible_;
  mutable std::mutex schemaLock_;

  // Parked callbacks waiting for a schema to become visible
  std::map<std::pair<GraphSpaceID, int64_t>, std::vector<std::function<void()>>> parked_;
  std::mutex parkedLock_;
};

}  // namespace drainer
}  // namespace nebula

#endif  // DRAINER_METAAPPLIER_H_
