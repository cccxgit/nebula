/* Copyright (c) 2026 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#ifndef META_HEALTH_METASEMANTICPROBECLIENT_H_
#define META_HEALTH_METASEMANTICPROBECLIENT_H_

#include "common/base/Base.h"
#include "common/thrift/ThriftClientManager.h"
#include "interface/gen-cpp2/MetaServiceAsyncClient.h"
#include "meta/health/MetaHealthState.h"

namespace nebula::meta {

class MetaSemanticProbeClient {
 public:
  explicit MetaSemanticProbeClient(HostAddr localMetaAddr)
      : localMetaAddr_(std::move(localMetaAddr)) {}

  ProbeResult probeListSpaces(uint32_t timeoutMs);
  ProbeResult probeGetSession(uint32_t timeoutMs, SessionID sessionId);

 private:
  template <typename Req, typename Resp, typename Func>
  ProbeResult doProbe(const char* name, uint32_t timeoutMs, Req&& req, Func&& invoke);

 private:
  HostAddr localMetaAddr_;
  thrift::ThriftClientManager<cpp2::MetaServiceAsyncClient> clients_;
};

}  // namespace nebula::meta

#endif  // META_HEALTH_METASEMANTICPROBECLIENT_H_
