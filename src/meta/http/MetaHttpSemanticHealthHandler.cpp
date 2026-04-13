/* Copyright (c) 2026 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include "meta/http/MetaHttpSemanticHealthHandler.h"

#include <proxygen/httpserver/ResponseBuilder.h>
#include <proxygen/lib/http/ProxygenErrorEnum.h>

#include "folly/json.h"
#include "webservice/WebService.h"

namespace nebula::meta {

void MetaHttpSemanticHealthHandler::onRequest(std::unique_ptr<proxygen::HTTPMessage>) noexcept {}

void MetaHttpSemanticHealthHandler::onBody(std::unique_ptr<folly::IOBuf>) noexcept {}

void MetaHttpSemanticHealthHandler::onEOM() noexcept {
  auto snap = manager_->snapshot();
  folly::dynamic body = folly::dynamic::object;
  body["service"] = "metad";
  body["role"] = snap.isLeader ? "LEADER" : "FOLLOWER";
  body["term"] = snap.term;
  body["state"] = toString(snap.state);
  body["since_ms"] = snap.stateSinceMs;
  body["latched"] = snap.latched;
  body["leader_grace"] = snap.leaderGrace;
  body["last_monitor_tick_ms"] = snap.lastMonitorTickMs;
  body["decision"] = snap.decision;
  auto listSpaces = folly::dynamic::object(
      "ok", snap.spacesProbe.ok)("last_ok_ms", snap.spacesProbe.lastSuccessMs)(
      "last_error", snap.spacesProbe.error)(
      "consecutive_failures", snap.spacesProbe.consecutiveFailures)(
      "latency_ms", snap.spacesProbe.latencyMs);
  auto getSession = folly::dynamic::object(
      "ok", snap.sessionProbe.ok)("last_ok_ms", snap.sessionProbe.lastSuccessMs)(
      "last_error", snap.sessionProbe.error)(
      "consecutive_failures", snap.sessionProbe.consecutiveFailures)(
      "latency_ms", snap.sessionProbe.latencyMs);
  body["checks"] = folly::dynamic::object("list_spaces", std::move(listSpaces))(
      "get_session", std::move(getSession));

  auto payload = folly::toJson(body);
  auto code = manager_->shouldFailLiveness() ? 500 : 200;
  auto reason = code == 500 ? "Internal Server Error" : "OK";
  proxygen::ResponseBuilder(downstream_)
      .status(code, reason)
      .header(proxygen::HTTP_HEADER_CONTENT_TYPE, "application/json")
      .body(payload)
      .sendWithEOM();
}

void MetaHttpSemanticHealthHandler::onUpgrade(proxygen::UpgradeProtocol) noexcept {}

void MetaHttpSemanticHealthHandler::requestComplete() noexcept {
  delete this;
}

void MetaHttpSemanticHealthHandler::onError(proxygen::ProxygenError error) noexcept {
  LOG(ERROR) << "MetaHttpSemanticHealthHandler on error: " << proxygen::getErrorString(error);
}

}  // namespace nebula::meta
