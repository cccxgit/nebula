/* Copyright (c) 2025 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#ifndef DRAINER_BACKUPCLIENTCACHE_H_
#define DRAINER_BACKUPCLIENTCACHE_H_

#include <folly/executors/IOThreadPoolExecutor.h>

#include <memory>
#include <vector>

#include "clients/meta/MetaClient.h"
#include "clients/storage/StorageClient.h"
#include "common/base/Base.h"
#include "common/datatypes/HostAddr.h"

namespace nebula {
namespace drainer {

/**
 * @brief BackupClientCache manages connections to the backup cluster.
 * It holds a MetaClient connected to the backup meta servers and a
 * StorageClient for writing data to the backup storage nodes.
 */
class BackupClientCache {
 public:
  explicit BackupClientCache(std::vector<HostAddr> backupMetaHosts);
  ~BackupClientCache();

  bool init();

  meta::MetaClient* backupMetaClient() {
    return metaClient_.get();
  }

  storage::StorageClient* backupStorageClient() {
    return storageClient_.get();
  }

 private:
  std::vector<HostAddr> metaHosts_;
  std::unique_ptr<meta::MetaClient> metaClient_;
  std::unique_ptr<storage::StorageClient> storageClient_;
  std::shared_ptr<folly::IOThreadPoolExecutor> ioPool_;
};

}  // namespace drainer
}  // namespace nebula

#endif  // DRAINER_BACKUPCLIENTCACHE_H_
