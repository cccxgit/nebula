/* Copyright (c) 2026 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include "meta/health/MetaSemanticProbeClient.h"

#include <folly/Conv.h>
#include <thrift/lib/cpp/util/EnumUtils.h>

#include "common/time/WallClock.h"

namespace nebula::meta {

template <typename Req, typename Resp, typename Func>
ProbeResult MetaSemanticProbeClient::doProbe(const char* name,
                                             uint32_t timeoutMs,
                                             Req&& req,
                                             Func&& invoke) {
  ProbeResult result;
  result.probeName = name;
  auto startMs = time::WallClock::fastNowInMilliSec();
  try {
    auto* evb = folly::EventBaseManager::get()->getEventBase();
    auto client = clients_.client(localMetaAddr_, evb, false, timeoutMs);
    if (!client) {
      result.error = "client not available";
      result.ok = false;
    } else {
      Resp resp = invoke(client.get(), req).via(evb).get();
      result.code = resp.get_code();
      result.ok = (resp.get_code() == cpp2::ErrorCode::SUCCEEDED ||
                   resp.get_code() == cpp2::ErrorCode::E_SESSION_NOT_FOUND);
      if (!result.ok) {
        result.error = apache::thrift::util::enumNameSafe(resp.get_code());
      }
    }
  } catch (const std::exception& ex) {
    result.ok = false;
    result.error = ex.what();
  }

  result.latencyMs = time::WallClock::fastNowInMilliSec() - startMs;
  return result;
}

ProbeResult MetaSemanticProbeClient::probeListSpaces(uint32_t timeoutMs) {
  cpp2::ListSpacesReq req;
  return doProbe<cpp2::ListSpacesReq, cpp2::ListSpacesResp>(
      "list_spaces", timeoutMs, std::move(req), [](auto* client, auto& r) {
        return client->future_listSpaces(r);
      });
}

ProbeResult MetaSemanticProbeClient::probeGetSession(uint32_t timeoutMs, SessionID sessionId) {
  cpp2::GetSessionReq req;
  req.session_id_ref() = sessionId;
  return doProbe<cpp2::GetSessionReq, cpp2::GetSessionResp>(
      "get_session", timeoutMs, std::move(req), [](auto* client, auto& r) {
        return client->future_getSession(r);
      });
}

}  // namespace nebula::meta
