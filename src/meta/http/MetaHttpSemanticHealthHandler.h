/* Copyright (c) 2026 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#ifndef META_HTTP_METAHTTPSEMANTICHEALTHHANDLER_H_
#define META_HTTP_METAHTTPSEMANTICHEALTHHANDLER_H_

#include <proxygen/httpserver/RequestHandler.h>

#include "meta/health/MetaSemanticHealthManager.h"

namespace nebula::meta {

class MetaHttpSemanticHealthHandler : public proxygen::RequestHandler {
 public:
  explicit MetaHttpSemanticHealthHandler(std::shared_ptr<MetaSemanticHealthManager> manager)
      : manager_(std::move(manager)) {}

  void onRequest(std::unique_ptr<proxygen::HTTPMessage> headers) noexcept override;
  void onBody(std::unique_ptr<folly::IOBuf> body) noexcept override;
  void onEOM() noexcept override;
  void onUpgrade(proxygen::UpgradeProtocol protocol) noexcept override;
  void requestComplete() noexcept override;
  void onError(proxygen::ProxygenError error) noexcept override;

 private:
  std::shared_ptr<MetaSemanticHealthManager> manager_;
};

}  // namespace nebula::meta

#endif  // META_HTTP_METAHTTPSEMANTICHEALTHHANDLER_H_
