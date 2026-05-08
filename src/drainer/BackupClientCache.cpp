/* Copyright (c) 2025 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include "drainer/BackupClientCache.h"

#include "common/base/Base.h"

namespace nebula {
namespace drainer {

BackupClientCache::BackupClientCache(std::vector<HostAddr> backupMetaHosts)
    : metaHosts_(std::move(backupMetaHosts)) {}

BackupClientCache::~BackupClientCache() {
  if (storageClient_) {
    storageClient_.reset();
  }
  if (metaClient_) {
    metaClient_->notifyStop();
    metaClient_->stop();
    metaClient_.reset();
  }
  if (ioPool_) {
    ioPool_->stop();
    ioPool_.reset();
  }
}

bool BackupClientCache::init() {
  // Create IO thread pool for async thrift communication
  ioPool_ = std::make_shared<folly::IOThreadPoolExecutor>(3);

  // Create MetaClient options for the backup cluster.
  // Use UNKNOWN role so that the MetaClient does NOT send heartbeats or try
  // to register as a storage host.  The drainer is a pure client that reads
  // schema information; it should never appear in the host registry.
  meta::MetaClientOptions options;
  options.serviceName_ = "drainer";
  options.skipConfig_ = true;
  options.role_ = meta::cpp2::HostRole::UNKNOWN;

  // Create MetaClient pointing to backup meta servers
  metaClient_ = std::make_unique<meta::MetaClient>(ioPool_, metaHosts_, options);

  // Wait for metadata to be ready
  if (!metaClient_->waitForMetadReady(3)) {
    LOG(ERROR) << "Failed to connect to backup meta servers";
    return false;
  }
  LOG(INFO) << "Connected to backup meta cluster successfully";

  // Create StorageClient using the MetaClient
  storageClient_ = std::make_unique<storage::StorageClient>(ioPool_, metaClient_.get());
  LOG(INFO) << "Backup storage client created";

  return true;
}

}  // namespace drainer
}  // namespace nebula
