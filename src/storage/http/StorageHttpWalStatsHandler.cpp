/* Copyright (c) 2026 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include "storage/http/StorageHttpWalStatsHandler.h"

#include <folly/json.h>
#include <proxygen/httpserver/ResponseBuilder.h>
#include <proxygen/lib/http/ProxygenErrorEnum.h>

#include "kvstore/NebulaStore.h"

namespace nebula {
namespace storage {

using proxygen::HTTPMessage;
using proxygen::HTTPMethod;
using proxygen::ProxygenError;
using proxygen::ResponseBuilder;
using proxygen::UpgradeProtocol;

void StorageHttpWalStatsHandler::onRequest(std::unique_ptr<HTTPMessage> headers) noexcept {
  if (!headers->getMethod() || headers->getMethod().value() != HTTPMethod::GET) {
    err_ = HttpCode::E_UNSUPPORTED_METHOD;
    return;
  }

  try {
    auto* space = headers->getQueryParamPtr("space");
    if (space != nullptr && !space->empty()) {
      spaceId_ = folly::to<GraphSpaceID>(*space);
    }
    auto* part = headers->getQueryParamPtr("part");
    if (part != nullptr && !part->empty()) {
      partId_ = folly::to<PartitionID>(*part);
    }
  } catch (const std::exception& ex) {
    err_ = HttpCode::E_ILLEGAL_ARGUMENT;
    resp_ = folly::stringPrintf("Bad wal stats query: %s", ex.what());
  }
}

void StorageHttpWalStatsHandler::onBody(std::unique_ptr<folly::IOBuf>) noexcept {}

void StorageHttpWalStatsHandler::onEOM() noexcept {
  switch (err_) {
    case HttpCode::E_UNSUPPORTED_METHOD:
      ResponseBuilder(downstream_).status(405, "Method Not Allowed").sendWithEOM();
      return;
    case HttpCode::E_ILLEGAL_ARGUMENT:
      ResponseBuilder(downstream_).status(400, "Bad Request").body(resp_).sendWithEOM();
      return;
    default:
      break;
  }

  auto* nebulaStore = dynamic_cast<kvstore::NebulaStore*>(kv_);
  if (nebulaStore == nullptr) {
    ResponseBuilder(downstream_)
        .status(500, "Internal Server Error")
        .body("wal stats only supports NebulaStore")
        .sendWithEOM();
    return;
  }

  auto stats = nebulaStore->walStats(spaceId_, partId_);
  ResponseBuilder(downstream_)
      .status(200, "OK")
      .body(folly::toPrettyJson(stats))
      .sendWithEOM();
}

void StorageHttpWalStatsHandler::onUpgrade(UpgradeProtocol) noexcept {}

void StorageHttpWalStatsHandler::requestComplete() noexcept {
  delete this;
}

void StorageHttpWalStatsHandler::onError(ProxygenError error) noexcept {
  LOG(ERROR) << "Web service StorageHttpWalStatsHandler got error: "
             << proxygen::getErrorString(error);
  delete this;
}

}  // namespace storage
}  // namespace nebula
