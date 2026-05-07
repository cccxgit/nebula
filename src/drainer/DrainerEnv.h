/* Copyright (c) 2024 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#ifndef DRAINER_DRAINERENV_H_
#define DRAINER_DRAINERENV_H_

#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

#include "common/thrift/ThriftTypes.h"

namespace nebula {
namespace drainer {

class MetaApplier;
class PartApplier;
class BackupClientCache;
class CheckpointStore;

class DrainerEnv {
 public:
  DrainerEnv();
  ~DrainerEnv();

  bool init();

  bool isSelfClusterId(int64_t id) const { return id == selfClusterId_; }

  int64_t currentEpochFor(int64_t clusterId) const;

  MetaApplier* metaApplier() { return metaApplier_.get(); }

  PartApplier* getOrCreatePartApplier(GraphSpaceID spaceId, PartitionID partId);

  BackupClientCache* backupClientCache() { return clientCache_.get(); }

  CheckpointStore* checkpointStore() { return checkpointStore_.get(); }

 private:
  int64_t selfClusterId_{0};
  std::unordered_map<int64_t, int64_t> epochMap_;
  mutable std::mutex epochLock_;

  std::unique_ptr<MetaApplier> metaApplier_;
  struct PairHash {
    std::size_t operator()(const std::pair<GraphSpaceID, PartitionID>& p) const {
      auto h1 = std::hash<GraphSpaceID>{}(p.first);
      auto h2 = std::hash<PartitionID>{}(p.second);
      return h1 ^ (h2 << 32);
    }
  };
  std::unordered_map<std::pair<GraphSpaceID, PartitionID>,
                     std::shared_ptr<PartApplier>,
                     PairHash>
      partAppliers_;
  std::mutex partApplierLock_;

  std::unique_ptr<BackupClientCache> clientCache_;
  std::unique_ptr<CheckpointStore> checkpointStore_;
};

}  // namespace drainer
}  // namespace nebula

#endif  // DRAINER_DRAINERENV_H_
