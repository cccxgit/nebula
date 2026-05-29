/* Copyright (c) 2026 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include "meta/http/MetaHttpRenameSpaceHandler.h"

#include <folly/json.h>
#include <proxygen/httpserver/ResponseBuilder.h>
#include <proxygen/lib/http/HTTPHeaders.h>
#include <proxygen/lib/http/ProxygenErrorEnum.h>
#include <thrift/lib/cpp/util/EnumUtils.h>

#include "webservice/WebService.h"

DEFINE_bool(enable_space_rename_rest, false, "Enable internal REST API to hard rename spaces");
DEFINE_string(space_rename_rest_token, "", "Token for internal space hard rename REST API");

namespace nebula {
namespace meta {

using proxygen::HTTPMessage;
using proxygen::ProxygenError;
using proxygen::ResponseBuilder;
using proxygen::UpgradeProtocol;

void MetaHttpRenameSpaceHandler::init(nebula::kvstore::KVStore* kvstore) {
  kvstore_ = kvstore;
  CHECK_NOTNULL(kvstore_);
}

void MetaHttpRenameSpaceHandler::onRequest(std::unique_ptr<HTTPMessage> headers) noexcept {
  LOG(INFO) << __PRETTY_FUNCTION__;

  if (!FLAGS_enable_space_rename_rest) {
    status_ = HttpStatusCode::NOT_FOUND;
    errMsg_ = "space rename REST API is disabled";
    return;
  }

  auto token = headers->getHeaders().getSingleOrEmpty("X-Nebula-Admin-Token");
  if (FLAGS_space_rename_rest_token.empty() || token != FLAGS_space_rename_rest_token) {
    status_ = HttpStatusCode::FORBIDDEN;
    errMsg_ = "invalid admin token";
    return;
  }
}

void MetaHttpRenameSpaceHandler::onBody(std::unique_ptr<folly::IOBuf> body) noexcept {
  if (body) {
    body_.append(body->moveToFbString().toStdString());
  }
}

void MetaHttpRenameSpaceHandler::sendJson(HttpStatusCode status, const folly::dynamic& body) {
  ResponseBuilder(downstream_)
      .status(WebServiceUtils::to(status), WebServiceUtils::toString(status))
      .header("Content-Type", "application/json")
      .body(folly::toJson(body))
      .sendWithEOM();
}

bool MetaHttpRenameSpaceHandler::parseBody(RenameSpaceHardReq& req, folly::dynamic& root) {
  try {
    root = folly::parseJson(body_);
  } catch (const std::exception& e) {
    errMsg_ = folly::sformat("invalid json: {}", e.what());
    return false;
  }

  if (!root.isObject()) {
    errMsg_ = "request body must be a JSON object";
    return false;
  }
  if (!root.count("old_name") || !root["old_name"].isString()) {
    errMsg_ = "old_name is required and must be string";
    return false;
  }
  if (!root.count("new_name") || !root["new_name"].isString()) {
    errMsg_ = "new_name is required and must be string";
    return false;
  }
  if (!root.count("expected_space_id") || !root["expected_space_id"].isInt()) {
    errMsg_ = "expected_space_id is required and must be int";
    return false;
  }
  if (root.count("dry_run") && !root["dry_run"].isBool()) {
    errMsg_ = "dry_run must be bool";
    return false;
  }
  if (root.count("operator") && !root["operator"].isString()) {
    errMsg_ = "operator must be string";
    return false;
  }
  if (root.count("comment") && !root["comment"].isString()) {
    errMsg_ = "comment must be string";
    return false;
  }

  req.oldName = root["old_name"].asString();
  req.newName = root["new_name"].asString();
  req.expectedSpaceId = root["expected_space_id"].asInt();
  req.dryRun = root.count("dry_run") ? root["dry_run"].asBool() : true;
  req.operatorName = root.count("operator") ? root["operator"].asString() : "";
  req.comment = root.count("comment") ? root["comment"].asString() : "";
  return true;
}

void MetaHttpRenameSpaceHandler::onEOM() noexcept {
  if (status_ != HttpStatusCode::OK) {
    folly::dynamic response = folly::dynamic::object();
    response["code"] = -1;
    response["message"] = errMsg_;
    sendJson(status_, response);
    return;
  }

  RenameSpaceHardReq req;
  folly::dynamic root;
  if (!parseBody(req, root)) {
    folly::dynamic response = folly::dynamic::object();
    response["code"] = -1;
    response["message"] = errMsg_;
    sendJson(HttpStatusCode::BAD_REQUEST, response);
    return;
  }

  auto* processor = RenameSpaceHardProcessor::instance(kvstore_);
  auto f = processor->getFuture();
  processor->process(req);
  auto resp = std::move(f).get();
  auto code = resp.get_code();
  auto spaceId = resp.get_id().space_id_ref().has_value() ? resp.get_id().get_space_id() : -1;

  folly::dynamic response = folly::dynamic::object();
  response["code"] = static_cast<int32_t>(code);
  response["message"] = apache::thrift::util::enumNameSafe(code);
  response["operation"] = "hard_rename_space";
  response["dry_run"] = req.dryRun;
  response["old_name"] = req.oldName;
  response["new_name"] = req.newName;
  response["space_id"] = spaceId;
  if (code == nebula::cpp2::ErrorCode::SUCCEEDED) {
    if (req.dryRun) {
      response["message"] = "DRY_RUN_SUCCEEDED";
      auto willPut = folly::dynamic::array();
      willPut.push_back(folly::sformat("spaceNameKey({}) -> {}", req.newName, spaceId));
      willPut.push_back(folly::sformat("spaceKey({}) -> SpaceDesc.name={}", spaceId, req.newName));
      response["will_put"] = std::move(willPut);
      auto willRemove = folly::dynamic::array();
      willRemove.push_back(folly::sformat("spaceNameKey({})", req.oldName));
      response["will_remove"] = std::move(willRemove);
    } else {
      response["message"] = "RENAMED";
    }
    auto warnings = folly::dynamic::array();
    warnings.push_back("hard rename will make old_space unavailable immediately");
    warnings.push_back(
        "graphd may still need meta cache refresh or restart before new_name is visible");
    response["warnings"] = std::move(warnings);
    sendJson(HttpStatusCode::OK, response);
  } else if (code == nebula::cpp2::ErrorCode::E_LEADER_CHANGED) {
    sendJson(HttpStatusCode::FORBIDDEN, response);
  } else {
    sendJson(HttpStatusCode::BAD_REQUEST, response);
  }
}

void MetaHttpRenameSpaceHandler::onUpgrade(UpgradeProtocol) noexcept {}

void MetaHttpRenameSpaceHandler::requestComplete() noexcept {
  delete this;
}

void MetaHttpRenameSpaceHandler::onError(ProxygenError error) noexcept {
  LOG(INFO) << "Web Service MetaHttpRenameSpaceHandler got error : "
            << proxygen::getErrorString(error);
  delete this;
}

}  // namespace meta
}  // namespace nebula
