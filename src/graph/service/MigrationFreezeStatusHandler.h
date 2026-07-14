/* Copyright (c) 2026 vesoft inc. All rights reserved.
 *
 * This source code is licensed under the Apache License, Version 2.0.
 */

#ifndef GRAPH_SERVICE_MIGRATIONFREEZESTATUSHANDLER_H_
#define GRAPH_SERVICE_MIGRATIONFREEZESTATUSHANDLER_H_

#include <proxygen/httpserver/RequestHandler.h>

#include "webservice/Common.h"

namespace nebula {
namespace graph {

class MigrationFreezeStatusHandler final : public proxygen::RequestHandler {
 public:
  void onRequest(std::unique_ptr<proxygen::HTTPMessage> headers) noexcept override;
  void onBody(std::unique_ptr<folly::IOBuf>) noexcept override;
  void onEOM() noexcept override;
  void onUpgrade(proxygen::UpgradeProtocol protocol) noexcept override;
  void requestComplete() noexcept override;
  void onError(proxygen::ProxygenError error) noexcept override;

 private:
  HttpCode err_{HttpCode::SUCCEEDED};
};

}  // namespace graph
}  // namespace nebula

#endif  // GRAPH_SERVICE_MIGRATIONFREEZESTATUSHANDLER_H_
