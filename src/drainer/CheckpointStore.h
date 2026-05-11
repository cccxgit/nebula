/* Copyright (c) 2025 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#ifndef DRAINER_CHECKPOINTSTORE_H_
#define DRAINER_CHECKPOINTSTORE_H_

#include <rocksdb/db.h>

#include <memory>
#include <string>
#include <unordered_map>

#include "common/thrift/ThriftTypes.h"

namespace nebula {
namespace drainer {

/**
 * @brief CheckpointStore persists (space, part) -> lastAppliedLogId using an embedded RocksDB
 * instance. It also stores epoch and listener token per primary cluster.
 *
 * Key schema:
 *   "cp::{spaceId}::{partId}"          -> lastAppliedLogId (8 bytes, little-endian)
 *   "cp::epoch::{primaryClusterId}"    -> epoch (8 bytes, little-endian)
 *   "cp::token::{primaryClusterId}"    -> token string
 */
class CheckpointStore {
 public:
  explicit CheckpointStore(const std::string& dataPath);
  ~CheckpointStore();

  bool init();

  // Commit checkpoint for a (space, part)
  bool commit(GraphSpaceID spaceId, PartitionID partId, LogID lastAppliedLogId);

  // Get last applied log id for a (space, part). Returns 0 if not found.
  LogID getLastApplied(GraphSpaceID spaceId, PartitionID partId) const;

  // Get all checkpoints for a space (for SHOW DRAINER SYNC STATUS)
  std::unordered_map<PartitionID, LogID> getAllCheckpoints(GraphSpaceID spaceId) const;

  // Store/retrieve epoch
  bool storeEpoch(int64_t primaryClusterId, int64_t epoch);
  int64_t getEpoch(int64_t primaryClusterId) const;

  // Store/retrieve listener token
  bool storeToken(int64_t primaryClusterId, const std::string& token);
  std::string getToken(int64_t primaryClusterId) const;

 private:
  std::string makeCheckpointKey(GraphSpaceID spaceId, PartitionID partId) const;
  std::string makeEpochKey(int64_t primaryClusterId) const;
  std::string makeTokenKey(int64_t primaryClusterId) const;

  static void encodeInt64(int64_t value, std::string* buf);
  static int64_t decodeInt64(const std::string& buf);

  std::string dataPath_;
  std::unique_ptr<rocksdb::DB> db_;
};

}  // namespace drainer
}  // namespace nebula

#endif  // DRAINER_CHECKPOINTSTORE_H_
