/* Copyright (c) 2024 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include "kvstore/listener/sync/DrainerClient.h"

#include <folly/io/async/EventBaseManager.h>

DECLARE_int32(conn_timeout_ms);

namespace nebula {
namespace kvstore {

DrainerClient::DrainerClient(folly::IOThreadPoolExecutor* ioPool,
                             std::vector<HostAddr> drainerAddrs)
    : ioPool_(ioPool), drainerAddrs_(std::move(drainerAddrs)) {
  CHECK(ioPool_ != nullptr) << "IOThreadPool is required for DrainerClient";
  CHECK(!drainerAddrs_.empty()) << "At least one drainer address is required";
  clientsMan_ =
      std::make_shared<thrift::ThriftClientManager<sync::cpp2::SyncServiceAsyncClient>>();
  LOG(INFO) << "DrainerClient created with " << drainerAddrs_.size() << " drainer address(es)";
}

const HostAddr& DrainerClient::pickDrainer_(PartitionID partId) const {
  auto idx = static_cast<size_t>(partId) % drainerAddrs_.size();
  return drainerAddrs_[idx];
}

folly::Future<sync::cpp2::AppendLogsResponse> DrainerClient::appendLogs(
    sync::cpp2::SyncLogBatch&& batch, const std::string& listenerToken) {
  auto partId = batch.get_partId();
  auto& host = pickDrainer_(partId);

  sync::cpp2::AppendLogsRequest req;
  req.batch_ref() = std::move(batch);
  req.listenerToken_ref() = listenerToken;

  return getResponse_(
      host, std::move(req), [](auto client, auto request) {
        return client->future_appendLogs(request);
      });
}

folly::Future<sync::cpp2::HeartbeatResponse> DrainerClient::heartbeat(
    sync::cpp2::HeartbeatRequest&& req) {
  auto partId = req.get_partId();
  auto& host = pickDrainer_(partId);

  return getResponse_(
      host, std::move(req), [](auto client, auto request) {
        return client->future_heartbeat(request);
      });
}

folly::Future<sync::cpp2::CheckpointQueryResponse> DrainerClient::queryCheckpoint(
    sync::cpp2::CheckpointQueryRequest&& req) {
  // For checkpoint queries that span multiple partitions, pick based on spaceId
  // (deterministic but doesn't matter much since all drainers should respond).
  // If there's at least one part, use that; otherwise fall back to spaceId hash.
  PartitionID pickId = 0;
  auto& parts = req.get_parts();
  if (!parts.empty()) {
    pickId = parts.front();
  } else {
    pickId = static_cast<PartitionID>(req.get_spaceId());
  }
  auto& host = pickDrainer_(pickId);

  return getResponse_(
      host, std::move(req), [](auto client, auto request) {
        return client->future_queryCheckpoint(request);
      });
}

template <class Request, class RemoteFunc, class Response>
folly::Future<Response> DrainerClient::getResponse_(const HostAddr& host,
                                                    Request&& req,
                                                    RemoteFunc&& remoteFunc) {
  auto* evb = ioPool_->getEventBase();
  folly::Promise<Response> promise;
  auto future = promise.getFuture();

  folly::via(
      evb,
      [host,
       evb,
       req = std::move(req),
       remoteFunc = std::forward<RemoteFunc>(remoteFunc),
       pro = std::move(promise),
       clientsMan = clientsMan_]() mutable {
        auto client = clientsMan->client(host, evb);
        VLOG(1) << "Sending request to drainer " << host;
        remoteFunc(client, req)
            .via(evb)
            .then([pro = std::move(pro),
                   host](folly::Try<Response>&& t) mutable {
              if (t.hasException()) {
                LOG(ERROR) << "RPC to drainer " << host
                           << " failed: " << t.exception().what().c_str();
                // Return a response with error code rather than throwing.
                // The caller can inspect the code field.
                Response resp;
                resp.code_ref() = nebula::cpp2::ErrorCode::E_RPC_FAILURE;
                pro.setValue(std::move(resp));
                return;
              }
              pro.setValue(std::move(t.value()));
            });
      });

  return future;
}

}  // namespace kvstore
}  // namespace nebula
