/* Copyright (c) 2024 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include "drainer/DrainerEnv.h"

#include "common/base/Base.h"
#include "common/network/NetworkUtils.h"
#include "drainer/BackupClientCache.h"
#include "drainer/CheckpointStore.h"
#include "drainer/DrainerFlags.h"
#include "drainer/MetaApplier.h"
#include "drainer/PartApplier.h"

namespace nebula {
namespace drainer {

DrainerEnv::DrainerEnv() {}

DrainerEnv::~DrainerEnv() {}

bool DrainerEnv::init() {
  // Parse secondary meta addresses
  if (FLAGS_drainer_secondary_meta_addrs.empty()) {
    LOG(ERROR) << "drainer_secondary_meta_addrs is empty, cannot connect to backup cluster";
    return false;
  }

  auto backupMetaHostsRet =
      nebula::network::NetworkUtils::toHosts(FLAGS_drainer_secondary_meta_addrs);
  if (!backupMetaHostsRet.ok() || backupMetaHostsRet.value().empty()) {
    LOG(ERROR) << "Failed to parse backup meta addresses: " << backupMetaHostsRet.status();
    return false;
  }
  auto backupMetaHosts = std::move(backupMetaHostsRet).value();

  // Create BackupClientCache for connections to the backup cluster
  clientCache_ = std::make_unique<BackupClientCache>(std::move(backupMetaHosts));

  // Create CheckpointStore for persisting apply progress
  checkpointStore_ = std::make_unique<CheckpointStore>(FLAGS_drainer_data_path);

  // Create MetaApplier for DDL replay
  metaApplier_ = std::make_unique<MetaApplier>(this);

  LOG(INFO) << "DrainerEnv initialized successfully with secondary meta addrs: "
            << FLAGS_drainer_secondary_meta_addrs;
  return true;
}

int64_t DrainerEnv::currentEpochFor(int64_t clusterId) const {
  std::lock_guard<std::mutex> lk(epochLock_);
  auto it = epochMap_.find(clusterId);
  if (it == epochMap_.end()) {
    return 0;
  }
  return it->second;
}

PartApplier* DrainerEnv::getOrCreatePartApplier(GraphSpaceID spaceId, PartitionID partId) {
  auto key = std::make_pair(spaceId, partId);
  std::lock_guard<std::mutex> lk(partApplierLock_);
  auto it = partAppliers_.find(key);
  if (it != partAppliers_.end()) {
    return it->second.get();
  }
  auto applier = std::make_shared<PartApplier>(spaceId, partId, this);
  partAppliers_.emplace(key, applier);
  return applier.get();
}

}  // namespace drainer
}  // namespace nebula
