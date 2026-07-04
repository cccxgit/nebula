/* Copyright (c) 2026 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#ifndef STORAGE_HTTP_STORAGEHTTPWALSTATSHANDLER_H_
#define STORAGE_HTTP_STORAGEHTTPWALSTATSHANDLER_H_

#include <optional>

#include <proxygen/httpserver/RequestHandler.h>

#include "common/base/Base.h"
#include "kvstore/KVStore.h"
#include "webservice/Common.h"

namespace nebula {
namespace storage {

class StorageHttpWalStatsHandler : public proxygen::RequestHandler {
 public:
  explicit StorageHttpWalStatsHandler(kvstore::KVStore* kv) : kv_(kv) {}

  void onRequest(std::unique_ptr<proxygen::HTTPMessage> headers) noexcept override;

  void onBody(std::unique_ptr<folly::IOBuf> body) noexcept override;

  void onEOM() noexcept override;

  void onUpgrade(proxygen::UpgradeProtocol protocol) noexcept override;

  void requestComplete() noexcept override;

  void onError(proxygen::ProxygenError error) noexcept override;

 private:
  HttpCode err_{HttpCode::SUCCEEDED};
  kvstore::KVStore* kv_{nullptr};
  std::optional<GraphSpaceID> spaceId_;
  std::optional<PartitionID> partId_;
  std::string resp_;
};

}  // namespace storage
}  // namespace nebula

#endif  // STORAGE_HTTP_STORAGEHTTPWALSTATSHANDLER_H_
