/* Copyright (c) 2018 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#ifndef CLIENTS_STORAGE_STORAGECLIENTBASE_INL_H
#define CLIENTS_STORAGE_STORAGECLIENTBASE_INL_H

#include <folly/ExceptionWrapper.h>
#include <folly/Try.h>
#include <folly/futures/Future.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <unordered_map>

#include "clients/storage/stats/StorageClientStats.h"
#include "common/base/Logging.h"
#include "common/base/StatusOr.h"
#include "common/datatypes/HostAddr.h"
#include "common/memory/MemoryTracker.h"
#include "common/memory/MemoryUtils.h"
#include "common/ssl/SSLConfig.h"
#include "common/stats/StatsManager.h"
#include "common/thrift/ThriftTypes.h"
#include "common/time/WallClock.h"
#include "interface/gen-cpp2/common_types.h"

namespace nebula {
namespace storage {

template <typename ClientType, typename ClientManagerType>
StorageClientBase<ClientType, ClientManagerType>::StorageClientBase(
    std::shared_ptr<folly::IOThreadPoolExecutor> threadPool, meta::MetaClient* metaClient)
    : metaClient_(metaClient), ioThreadPool_(threadPool) {
  clientsMan_ = std::make_unique<ClientManagerType>(FLAGS_enable_ssl);
}

template <typename ClientType, typename ClientManagerType>
StorageClientBase<ClientType, ClientManagerType>::~StorageClientBase() {
  VLOG(3) << "Destructing StorageClientBase";
  if (nullptr != metaClient_) {
    metaClient_ = nullptr;
  }
}

template <typename ClientType, typename ClientManagerType>
StatusOr<HostAddr> StorageClientBase<ClientType, ClientManagerType>::getLeader(
    GraphSpaceID spaceId, PartitionID partId) const {
  return metaClient_->getStorageLeaderFromCache(spaceId, partId);
}

template <typename ClientType, typename ClientManagerType>
void StorageClientBase<ClientType, ClientManagerType>::updateLeader(GraphSpaceID spaceId,
                                                                    PartitionID partId,
                                                                    const HostAddr& leader) {
  metaClient_->updateStorageLeader(spaceId, partId, leader);
}

template <typename ClientType, typename ClientManagerType>
void StorageClientBase<ClientType, ClientManagerType>::invalidLeader(GraphSpaceID spaceId,
                                                                     PartitionID partId) {
  metaClient_->invalidStorageLeader(spaceId, partId);
}

template <typename ClientType, typename ClientManagerType>
void StorageClientBase<ClientType, ClientManagerType>::invalidLeader(
    GraphSpaceID spaceId, std::vector<PartitionID>& partsId) {
  for (const auto& partId : partsId) {
    invalidLeader(spaceId, partId);
  }
}

template <typename ClientType, typename ClientManagerType>
template <class Request, class RemoteFunc, class Response>
folly::SemiFuture<StorageRpcResponse<Response>>
StorageClientBase<ClientType, ClientManagerType>::collectResponse(
    folly::EventBase* evb,
    std::unordered_map<HostAddr, Request> requests,
    RemoteFunc&& remoteFunc) {
  memory::MemoryCheckOffGuard offGuard;
  using RequestItem = std::pair<HostAddr, Request>;
  std::vector<RequestItem> requestItems;
  requestItems.reserve(requests.size());
  for (auto& req : requests) {
    requestItems.emplace_back(req.first, std::move(req.second));
  }

  if (requestItems.empty()) {
    return folly::makeSemiFuture(StorageRpcResponse<Response>(0));
  }

  auto markMemoryExceededFailure = [this](StorageRpcResponse<Response>& rpcResp,
                                          const Request& req) {
    rpcResp.markFailure();
    const auto& parts = this->getReqPartsId(req);
    rpcResp.appendFailedParts(parts, nebula::cpp2::ErrorCode::E_GRAPH_MEMORY_EXCEEDED);
  };

  if (memory::MemoryUtils::hitsOneQueryMemoryLimit()) {
    LOG(WARNING) << "===============in collectResponse 1";
    StorageRpcResponse<Response> rpcResp(requestItems.size());
    for (const auto& req : requestItems) {
      markMemoryExceededFailure(rpcResp, req.second);
    }
    return folly::makeSemiFuture(std::move(rpcResp));
  }

  size_t inflightLimit = requestItems.size();
  if (FLAGS_max_storage_inflight_per_query > 0) {
    inflightLimit = std::min(
        inflightLimit, static_cast<size_t>(FLAGS_max_storage_inflight_per_query));
  }

  auto remoteFuncPtr =
      std::make_shared<std::decay_t<RemoteFunc>>(std::forward<RemoteFunc>(remoteFunc));

  struct CollectState final {
    explicit CollectState(std::vector<RequestItem>&& reqs)
        : requestItems(std::move(reqs)), rpcResp(requestItems.size()) {}

    std::vector<RequestItem> requestItems;
    StorageRpcResponse<Response> rpcResp;
    size_t nextToLaunch{0};
    size_t inFlight{0};
    bool aborted{false};
    bool fulfilled{false};
    std::mutex lock;
    folly::Promise<StorageRpcResponse<Response>> promise;
  };

  auto state = std::make_shared<CollectState>(std::move(requestItems));
  auto resultFuture = state->promise.getSemiFuture();
  using RemoteFuncType = std::decay_t<RemoteFunc>;
  struct CollectController final : public std::enable_shared_from_this<CollectController> {
    StorageClientBase* self{nullptr};
    folly::EventBase* evb{nullptr};
    std::shared_ptr<RemoteFuncType> remoteFuncPtr;
    std::shared_ptr<CollectState> state;
    size_t inflightLimit{0};

    void markRequestFailureLocked(size_t requestIdx, nebula::cpp2::ErrorCode code) {
      state->rpcResp.markFailure();
      const auto& parts = self->getReqPartsId(state->requestItems[requestIdx].second);
      state->rpcResp.appendFailedParts(parts, code);
    }

    void markAbortAndSkipUnlaunchedLocked() {
      if (state->aborted) {
        return;
      }
      state->aborted = true;
      for (size_t i = state->nextToLaunch; i < state->requestItems.size(); ++i) {
        markRequestFailureLocked(i, nebula::cpp2::ErrorCode::E_GRAPH_MEMORY_EXCEEDED);
      }
      state->nextToLaunch = state->requestItems.size();
    }

    void launchRequest(size_t idx) {
      auto start = time::WallClock::fastNowInMicroSec();
      HostAddr host = state->requestItems[idx].first;
      const auto& req = state->requestItems[idx].second;
      auto selfKeepAlive = this->shared_from_this();
      self->getResponse(
              evb,
              host,
              req,
              [remoteFuncPtr = remoteFuncPtr](ClientType* client, const Request& request) {
                return (*remoteFuncPtr)(client, request);
              })
          .thenTry([selfKeepAlive, idx, host, start](
                       folly::Try<StatusOr<Response>>&& tryResp) mutable {
            selfKeepAlive->handleOneResponse(idx, host, start, std::move(tryResp));
          });
    }

    void handleOneResponse(size_t idx,
                           const HostAddr& host,
                           int64_t start,
                           folly::Try<StatusOr<Response>>&& tryResp) {
      // throw in MemoryCheckGuard verified
      memory::MemoryCheckGuard guard;
      std::optional<StorageRpcResponse<Response>> finalResp;
      {
        std::lock_guard<std::mutex> lg(state->lock);
        if (state->aborted || memory::MemoryUtils::hitsOneQueryMemoryLimit()) {
          LOG(WARNING) << "===============in collectResponse 2";
          markAbortAndSkipUnlaunchedLocked();
          markRequestFailureLocked(idx, nebula::cpp2::ErrorCode::E_GRAPH_MEMORY_EXCEEDED);
        } else if (tryResp.hasException()) {
          std::string errMsg = tryResp.exception().what().toStdString();
          LOG(ERROR) << "There some RPC errors: " << errMsg;
          markRequestFailureLocked(idx, nebula::cpp2::ErrorCode::E_RPC_FAILURE);
        } else {
          StatusOr<Response> status = std::move(tryResp).value();
          if (status.ok()) {
            auto resp = std::move(status).value();
            const auto& result = resp.get_result();

            if (!result.get_failed_parts().empty()) {
              state->rpcResp.markFailure();
              for (auto& part : result.get_failed_parts()) {
                state->rpcResp.emplaceFailedPart(part.get_part_id(), part.get_code());
              }
            }

            auto latency = result.get_latency_in_us();
            auto e2eLatency = time::WallClock::fastNowInMicroSec() - start;
            state->rpcResp.setLatency(host, latency, e2eLatency);
            state->rpcResp.addResponse(std::move(resp));
          } else {
            state->rpcResp.markFailure();
            Status s = std::move(status).status();
            nebula::cpp2::ErrorCode errorCode =
                s.code() == Status::Code::kGraphMemoryExceeded
                    ? nebula::cpp2::ErrorCode::E_GRAPH_MEMORY_EXCEEDED
                    : nebula::cpp2::ErrorCode::E_RPC_FAILURE;
            LOG(ERROR) << "There some RPC errors: " << s.message();
            const auto& parts = self->getReqPartsId(state->requestItems[idx].second);
            state->rpcResp.appendFailedParts(parts, errorCode);
          }

          if (memory::MemoryUtils::hitsOneQueryMemoryLimit()) {
            LOG(WARNING) << "===============in collectResponse 3";
            markAbortAndSkipUnlaunchedLocked();
          }
        }

        DCHECK_GT(state->inFlight, 0);
        --state->inFlight;
        if (!state->fulfilled && state->nextToLaunch == state->requestItems.size() &&
            state->inFlight == 0) {
          state->fulfilled = true;
          finalResp.emplace(std::move(state->rpcResp));
        }
      }
      if (finalResp.has_value()) {
        state->promise.setValue(std::move(*finalResp));
      } else {
        launchMore();
      }
    }

    void launchMore() {
      std::vector<size_t> tasks;
      std::optional<StorageRpcResponse<Response>> finalResp;
      {
        std::lock_guard<std::mutex> lg(state->lock);
        if (state->fulfilled) {
          return;
        }

        while (!state->aborted && state->inFlight < inflightLimit &&
               state->nextToLaunch < state->requestItems.size()) {
          tasks.emplace_back(state->nextToLaunch++);
          ++state->inFlight;
        }

        if (state->nextToLaunch == state->requestItems.size() && state->inFlight == 0) {
          state->fulfilled = true;
          finalResp.emplace(std::move(state->rpcResp));
        }
      }

      if (finalResp.has_value()) {
        state->promise.setValue(std::move(*finalResp));
        return;
      }

      for (auto task : tasks) {
        launchRequest(task);
      }
    }
  };

  auto controller = std::make_shared<CollectController>();
  controller->self = this;
  controller->evb = evb;
  controller->remoteFuncPtr = std::move(remoteFuncPtr);
  controller->state = state;
  controller->inflightLimit = inflightLimit;
  controller->launchMore();
  return resultFuture;
}

template <typename ClientType, typename ClientManagerType>
template <class Request, class RemoteFunc, class Response>
folly::Future<StatusOr<Response>> StorageClientBase<ClientType, ClientManagerType>::getResponse(
    folly::EventBase* evb, const HostAddr& host, const Request& request, RemoteFunc&& remoteFunc) {
  static_assert(
      folly::isFuture<std::invoke_result_t<RemoteFunc, ClientType*, const Request&>>::value);

  stats::StatsManager::addValue(kNumRpcSentToStoraged);
  if (evb == nullptr) {
    evb = DCHECK_NOTNULL(ioThreadPool_)->getEventBase();
  }

  auto spaceId = request.get_space_id();
  return folly::via(evb)
      .thenValue([remoteFunc = std::move(remoteFunc), request, evb, host, this](auto&&) {
        // MemoryTrackerVerified
        memory::MemoryCheckGuard guard;
        // NOTE: Create new channel on each thread to avoid TIMEOUT RPC error
        auto client = clientsMan_->client(host, evb, false, FLAGS_storage_client_timeout_ms);
        // Encoding invoke Cpp2Ops::write the request to protocol is in current thread,
        // do not need to turn on in Cpp2Ops::write
        return remoteFunc(client.get(), request);
      })
      .thenValue([spaceId, this](Response&& resp) mutable -> StatusOr<Response> {
        // MemoryTrackerVerified
        memory::MemoryCheckGuard guard;
        auto& result = resp.get_result();
        for (auto& part : result.get_failed_parts()) {
          auto partId = part.get_part_id();
          auto code = part.get_code();

          VLOG(3) << "Failure! Failed part " << partId << ", failed part "
                  << static_cast<int32_t>(code);

          switch (code) {
            case nebula::cpp2::ErrorCode::E_LEADER_CHANGED: {
              auto* leader = part.get_leader();
              if (isValidHostPtr(leader)) {
                updateLeader(spaceId, partId, *leader);
              } else {
                invalidLeader(spaceId, partId);
              }
              break;
            }
            case nebula::cpp2::ErrorCode::E_PART_NOT_FOUND:
            case nebula::cpp2::ErrorCode::E_SPACE_NOT_FOUND: {
              invalidLeader(spaceId, partId);
              break;
            }
            default:
              break;
          }
        }
        return std::move(resp);
      })
      .thenError(
          folly::tag_t<std::bad_alloc>{},
          [](const std::bad_alloc&) {
            return folly::makeFuture<StatusOr<Response>>(Status::GraphMemoryExceeded(
                "(%d)", static_cast<int32_t>(nebula::cpp2::ErrorCode::E_GRAPH_MEMORY_EXCEEDED)));
          })
      .thenError([request, host, spaceId, this](
                     folly::exception_wrapper&& exWrapper) mutable -> StatusOr<Response> {
        stats::StatsManager::addValue(kNumRpcSentToStoragedFailed);

        using TransportException = apache::thrift::transport::TTransportException;
        auto ex = exWrapper.get_exception<TransportException>();
        if (ex) {
          if (ex->getType() == TransportException::TIMED_OUT) {
            LOG(ERROR) << "Request to " << host << " time out: " << ex->what();
            return Status::Error("RPC failure in StorageClient with timeout: %s", ex->what());
          } else {
            LOG(ERROR) << "Request to " << host << " failed: " << ex->what();
            return Status::Error("RPC failure in StorageClient: %s", ex->what());
          }
        } else {
          auto partsId = getReqPartsId(request);
          invalidLeader(spaceId, partsId);
          LOG(ERROR) << "Request to " << host << " failed.";
          return Status::Error("RPC failure in StorageClient.");
        }
      });
}

template <typename ClientType, typename ClientManagerType>
template <class Container, class GetIdFunc>
StatusOr<std::unordered_map<
    HostAddr,
    std::unordered_map<PartitionID, std::vector<typename Container::value_type>>>>
StorageClientBase<ClientType, ClientManagerType>::clusterIdsToHosts(GraphSpaceID spaceId,
                                                                    const Container& ids,
                                                                    GetIdFunc f) const {
  std::unordered_map<HostAddr,
                     std::unordered_map<PartitionID, std::vector<typename Container::value_type>>>
      clusters;

  auto status = metaClient_->partsNum(spaceId);
  if (!status.ok()) {
    return Status::Error("Space not found, spaceid: %d", spaceId);
  }
  auto numParts = status.value();
  std::unordered_map<PartitionID, HostAddr> leaders;
  for (int32_t partId = 1; partId <= numParts; ++partId) {
    auto leader = getLeader(spaceId, partId);
    if (!leader.ok()) {
      return leader.status();
    }
    leaders[partId] = std::move(leader).value();
  }
  for (auto& id : ids) {
    CHECK(!!metaClient_);
    status = metaClient_->partId(numParts, f(id));
    if (!status.ok()) {
      return status.status();
    }

    auto part = status.value();
    const auto& leader = leaders[part];
    clusters[leader][part].emplace_back(std::move(id));
  }
  return clusters;
}

template <typename ClientType, typename ClientManagerType>
StatusOr<std::unordered_map<HostAddr, std::vector<PartitionID>>>
StorageClientBase<ClientType, ClientManagerType>::getHostParts(GraphSpaceID spaceId) const {
  std::unordered_map<HostAddr, std::vector<PartitionID>> hostParts;
  auto status = metaClient_->partsNum(spaceId);
  if (!status.ok()) {
    return Status::Error("Space not found, spaceid: %d", spaceId);
  }

  auto parts = status.value();
  for (auto partId = 1; partId <= parts; partId++) {
    auto leader = getLeader(spaceId, partId);
    if (!leader.ok()) {
      return leader.status();
    }
    hostParts[leader.value()].emplace_back(partId);
  }
  return hostParts;
}

template <typename ClientType, typename ClientManagerType>
StatusOr<std::unordered_map<HostAddr, std::unordered_map<PartitionID, cpp2::ScanCursor>>>
StorageClientBase<ClientType, ClientManagerType>::getHostPartsWithCursor(
    GraphSpaceID spaceId) const {
  std::unordered_map<HostAddr, std::unordered_map<PartitionID, cpp2::ScanCursor>> hostParts;
  auto status = metaClient_->partsNum(spaceId);
  if (!status.ok()) {
    return Status::Error("Space not found, spaceid: %d", spaceId);
  }

  // TODO support cursor
  cpp2::ScanCursor c;
  auto parts = status.value();
  for (auto partId = 1; partId <= parts; partId++) {
    auto leader = getLeader(spaceId, partId);
    if (!leader.ok()) {
      return leader.status();
    }
    hostParts[leader.value()].emplace(partId, c);
  }
  return hostParts;
}

}  // namespace storage
}  // namespace nebula
#endif
