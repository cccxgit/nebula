/* Copyright (c) 2026 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#ifndef META_HTTP_METAHTTPRENAMESPACEHANDLER_H_
#define META_HTTP_METAHTTPRENAMESPACEHANDLER_H_

#include <proxygen/httpserver/RequestHandler.h>

#include "common/base/Base.h"
#include "kvstore/KVStore.h"
#include "meta/processors/parts/RenameSpaceHardProcessor.h"
#include "webservice/Common.h"

namespace nebula {
namespace meta {

class MetaHttpRenameSpaceHandler : public proxygen::RequestHandler {
 public:
  MetaHttpRenameSpaceHandler() = default;

  void init(nebula::kvstore::KVStore* kvstore);

  void onRequest(std::unique_ptr<proxygen::HTTPMessage> headers) noexcept override;
  void onBody(std::unique_ptr<folly::IOBuf> body) noexcept override;
  void onEOM() noexcept override;
  void onUpgrade(proxygen::UpgradeProtocol protocol) noexcept override;
  void requestComplete() noexcept override;
  void onError(proxygen::ProxygenError error) noexcept override;

 private:
  void sendJson(HttpStatusCode status, const folly::dynamic& body);
  bool parseBody(RenameSpaceHardReq& req, folly::dynamic& body);

 private:
  nebula::kvstore::KVStore* kvstore_{nullptr};
  std::string body_;
  HttpStatusCode status_{HttpStatusCode::OK};
  std::string errMsg_;
};

}  // namespace meta
}  // namespace nebula

#endif  // META_HTTP_METAHTTPRENAMESPACEHANDLER_H_
