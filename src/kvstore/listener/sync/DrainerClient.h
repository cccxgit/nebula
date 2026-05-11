/* Copyright (c) 2024 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#ifndef KVSTORE_LISTENER_SYNC_DRAINERCLIENT_H_
#define KVSTORE_LISTENER_SYNC_DRAINERCLIENT_H_

#include <folly/executors/IOThreadPoolExecutor.h>
#include <folly/futures/Future.h>

#include "common/base/Base.h"
#include "common/base/StatusOr.h"
#include "common/datatypes/HostAddr.h"
#include "common/thrift/ThriftClientManager.h"
#include "interface/gen-cpp2/SyncServiceAsyncClient.h"
#include "interface/gen-cpp2/sync_types.h"

namespace nebula {
namespace kvstore {

/**
 * DrainerClient is a Thrift RPC client that bridges SyncListener (on storaged)
 * to the DrainerService (on nebula-drainerd).
 *
 * It provides asynchronous methods that correspond 1:1 to the SyncService
 * Thrift interface: appendLogs, heartbeat, and queryCheckpoint.
 *
 * Connection management uses ThriftClientManager, which maintains a per-thread
 * cache of RocketClientChannel connections, identical to the pattern in
 * MetaClient / StorageClient.
 *
 * Drainer selection is a simple hash: partId % drainerAddrs_.size().
 */
class DrainerClient {
 public:
  /**
   * @brief Construct a DrainerClient.
   *
   * @param ioPool  IO thread pool used to drive Thrift async calls.
   *                Caller retains ownership; must outlive this client.
   * @param drainerAddrs  List of drainer host:port addresses.
   */
  DrainerClient(folly::IOThreadPoolExecutor* ioPool, std::vector<HostAddr> drainerAddrs);

  ~DrainerClient() = default;

  // Non-copyable, movable.
  DrainerClient(const DrainerClient&) = delete;
  DrainerClient& operator=(const DrainerClient&) = delete;
  DrainerClient(DrainerClient&&) = default;
  DrainerClient& operator=(DrainerClient&&) = default;

  /**
   * @brief Send a batch of sync log entries to the drainer.
   *
   * The target drainer is selected by hashing the partId inside the batch.
   *
   * @param batch  The SyncLogBatch to send.
   * @param listenerToken  Authentication token from this listener.
   * @return folly::Future resolving to the drainer's AppendLogsResponse.
   */
  folly::Future<sync::cpp2::AppendLogsResponse> appendLogs(sync::cpp2::SyncLogBatch&& batch,
                                                            const std::string& listenerToken);

  /**
   * @brief Send a heartbeat to the drainer for liveness and lag tracking.
   *
   * @param req  Heartbeat request (spaceId, partId, committed log, etc.).
   * @return folly::Future resolving to HeartbeatResponse.
   */
  folly::Future<sync::cpp2::HeartbeatResponse> heartbeat(sync::cpp2::HeartbeatRequest&& req);

  /**
   * @brief Query the drainer's checkpoint (last-applied log) for a set of partitions.
   *
   * @param req  Checkpoint query request.
   * @return folly::Future resolving to CheckpointQueryResponse.
   */
  folly::Future<sync::cpp2::CheckpointQueryResponse> queryCheckpoint(
      sync::cpp2::CheckpointQueryRequest&& req);

 private:
  /**
   * @brief Pick a drainer address for the given partition.
   *
   * Uses a simple modulo hash: partId % drainerAddrs_.size().
   *
   * @param partId  Partition ID used for selection.
   * @return const HostAddr& of the chosen drainer.
   */
  const HostAddr& pickDrainer_(PartitionID partId) const;

  /**
   * @brief Generic helper to send a request to a specific drainer host.
   *
   * Obtains a Thrift client from the ThriftClientManager, dispatches the RPC
   * on an IO event base, and returns the future result.
   *
   * @tparam Request  Thrift request type.
   * @tparam RemoteFunc  Callable (shared_ptr<AsyncClient>, Request) -> Future<Response>.
   * @tparam Response  Thrift response type (deduced).
   *
   * @param host  Target drainer address.
   * @param req   The request object (moved in).
   * @param remoteFunc  Lambda invoking the desired RPC on the async client.
   * @return folly::Future<Response>
   */
  template <class Request,
            class RemoteFunc,
            class Response = typename std::result_of<RemoteFunc(
                std::shared_ptr<sync::cpp2::SyncServiceAsyncClient>,
                Request)>::type::value_type>
  folly::Future<Response> getResponse_(const HostAddr& host,
                                       Request&& req,
                                       RemoteFunc&& remoteFunc);

 private:
  folly::IOThreadPoolExecutor* ioPool_{nullptr};
  std::vector<HostAddr> drainerAddrs_;
  std::shared_ptr<thrift::ThriftClientManager<sync::cpp2::SyncServiceAsyncClient>> clientsMan_;
};

}  // namespace kvstore
}  // namespace nebula

#endif  // KVSTORE_LISTENER_SYNC_DRAINERCLIENT_H_
