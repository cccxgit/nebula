/* Copyright (c) 2026 vesoft inc. All rights reserved.
 *
 * This source code is licensed under the Apache License, Version 2.0.
 */

#include "graph/service/MigrationFreezeStatusHandler.h"

#include <proxygen/httpserver/ResponseBuilder.h>
#include <proxygen/lib/http/ProxygenErrorEnum.h>

#include "graph/service/GraphFlags.h"
#include "graph/service/MutationFreezeManager.h"

namespace nebula {
namespace graph {

using proxygen::HTTPMessage;
using proxygen::HTTPMethod;
using proxygen::ProxygenError;
using proxygen::ResponseBuilder;
using proxygen::UpgradeProtocol;

void MigrationFreezeStatusHandler::onRequest(std::unique_ptr<HTTPMessage> headers) noexcept {
  if (!headers->getMethod() || headers->getMethod().value() != HTTPMethod::GET) {
    err_ = HttpCode::E_UNSUPPORTED_METHOD;
  }
}

void MigrationFreezeStatusHandler::onBody(std::unique_ptr<folly::IOBuf>) noexcept {}

void MigrationFreezeStatusHandler::onEOM() noexcept {
  if (err_ == HttpCode::E_UNSUPPORTED_METHOD) {
    ResponseBuilder(downstream_)
        .status(WebServiceUtils::to(HttpStatusCode::METHOD_NOT_ALLOWED),
                WebServiceUtils::toString(HttpStatusCode::METHOD_NOT_ALLOWED))
        .sendWithEOM();
    return;
  }

  const auto& manager = MutationFreezeManager::instance();
  folly::dynamic json = folly::dynamic::object();
  json["enableGraphMutation"] = FLAGS_enable_graph_mutation;
  json["activeMutationCount"] = manager.activeMutationCount();
  json["state"] = FLAGS_enable_graph_mutation
                      ? "WRITABLE"
                      : (manager.isFrozen() ? "FROZEN" : "DRAINING");
  ResponseBuilder(downstream_)
      .status(WebServiceUtils::to(HttpStatusCode::OK),
              WebServiceUtils::toString(HttpStatusCode::OK))
      .body(folly::toJson(json))
      .sendWithEOM();
}

void MigrationFreezeStatusHandler::onUpgrade(UpgradeProtocol) noexcept {}

void MigrationFreezeStatusHandler::requestComplete() noexcept {
  delete this;
}

void MigrationFreezeStatusHandler::onError(ProxygenError error) noexcept {
  LOG(ERROR) << "Migration freeze status request failed: " << proxygen::getErrorString(error);
}

}  // namespace graph
}  // namespace nebula
