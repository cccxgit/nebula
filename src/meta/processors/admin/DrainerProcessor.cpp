/* Copyright (c) 2024 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include "meta/processors/admin/DrainerProcessor.h"

#include <thrift/lib/cpp2/protocol/CompactProtocol.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

namespace nebula {
namespace meta {

static const std::string kDrainerServiceKey = "__drainer_service__";  // NOLINT
static const std::string kDrainerNodesPrefix = "__drainer_nodes__";   // NOLINT
static const std::string kSyncProgressPrefix = "__sync_progress__";   // NOLINT

static std::string drainerNodesKey(GraphSpaceID spaceId) {
  std::string key;
  key.reserve(kDrainerNodesPrefix.size() + sizeof(GraphSpaceID));
  key.append(kDrainerNodesPrefix.data(), kDrainerNodesPrefix.size())
      .append(reinterpret_cast<const char*>(&spaceId), sizeof(GraphSpaceID));
  return key;
}

static std::string syncProgressKey(GraphSpaceID spaceId) {
  std::string key;
  key.reserve(kSyncProgressPrefix.size() + sizeof(GraphSpaceID));
  key.append(kSyncProgressPrefix.data(), kSyncProgressPrefix.size())
      .append(reinterpret_cast<const char*>(&spaceId), sizeof(GraphSpaceID));
  return key;
}

static std::string serializeHostAddrs(const std::vector<HostAddr>& hosts) {
  std::string val;
  val.reserve(hosts.size() * sizeof(HostAddr));
  for (const auto& host : hosts) {
    auto hostStr = MetaKeyUtils::serializeHostAddr(host);
    int32_t len = hostStr.size();
    val.append(reinterpret_cast<const char*>(&len), sizeof(int32_t));
    val.append(hostStr);
  }
  return val;
}

static std::vector<HostAddr> deserializeHostAddrs(folly::StringPiece rawData) {
  std::vector<HostAddr> hosts;
  size_t offset = 0;
  while (offset < rawData.size()) {
    if (offset + sizeof(int32_t) > rawData.size()) {
      break;
    }
    int32_t len = *reinterpret_cast<const int32_t*>(rawData.data() + offset);
    offset += sizeof(int32_t);
    if (offset + len > rawData.size()) {
      break;
    }
    auto host = MetaKeyUtils::deserializeHostAddr(
        folly::StringPiece(rawData.data() + offset, len));
    hosts.emplace_back(std::move(host));
    offset += len;
  }
  return hosts;
}

void SignInDrainerProcessor::process(const cpp2::SignInDrainerReq& req) {
  folly::SharedMutex::WriteHolder holder(LockUtils::lock());
  const auto& hosts = req.get_hosts();
  if (hosts.empty()) {
    LOG(INFO) << "Sign in drainer failed, hosts list is empty.";
    handleErrorCode(nebula::cpp2::ErrorCode::E_INVALID_PARM);
    onFinished();
    return;
  }

  std::vector<kvstore::KV> data;
  data.emplace_back(kDrainerServiceKey, serializeHostAddrs(hosts));
  auto result = doSyncPut(std::move(data));
  handleErrorCode(result);
  onFinished();
}

void SignOutDrainerProcessor::process(const cpp2::SignOutDrainerReq& req) {
  UNUSED(req);
  folly::SharedMutex::WriteHolder holder(LockUtils::lock());

  // Check if drainer service is registered
  auto ret = doGet(kDrainerServiceKey);
  if (!nebula::ok(ret)) {
    auto code = nebula::error(ret);
    if (code == nebula::cpp2::ErrorCode::E_KEY_NOT_FOUND) {
      LOG(INFO) << "Sign out drainer failed, drainer service not found.";
      handleErrorCode(nebula::cpp2::ErrorCode::E_KEY_NOT_FOUND);
    } else {
      LOG(INFO) << "Sign out drainer failed, error: "
                << apache::thrift::util::enumNameSafe(code);
      handleErrorCode(code);
    }
    onFinished();
    return;
  }

  doRemove(kDrainerServiceKey);
}

void ListDrainerClientsProcessor::process(const cpp2::ListDrainerClientsReq& req) {
  UNUSED(req);
  folly::SharedMutex::ReadHolder holder(LockUtils::lock());

  auto ret = doGet(kDrainerServiceKey);
  if (!nebula::ok(ret)) {
    auto code = nebula::error(ret);
    if (code == nebula::cpp2::ErrorCode::E_KEY_NOT_FOUND) {
      // No drainer clients registered, return empty list
      handleErrorCode(nebula::cpp2::ErrorCode::SUCCEEDED);
      onFinished();
      return;
    }
    LOG(INFO) << "List drainer clients failed, error: "
              << apache::thrift::util::enumNameSafe(code);
    handleErrorCode(code);
    onFinished();
    return;
  }

  auto hosts = deserializeHostAddrs(nebula::value(ret));
  resp_.clients_ref() = std::move(hosts);
  handleErrorCode(nebula::cpp2::ErrorCode::SUCCEEDED);
  onFinished();
}

void AddDrainerProcessor::process(const cpp2::AddDrainerReq& req) {
  auto space = req.get_space_id();
  CHECK_SPACE_ID_AND_RETURN(space);
  folly::SharedMutex::WriteHolder holder(LockUtils::lock());
  const auto& hosts = req.get_hosts();
  if (hosts.empty()) {
    LOG(INFO) << "Add drainer failed, hosts list is empty.";
    handleErrorCode(nebula::cpp2::ErrorCode::E_INVALID_PARM);
    onFinished();
    return;
  }

  // Check if drainer already exists for this space
  auto key = drainerNodesKey(space);
  auto ret = doGet(key);
  if (nebula::ok(ret)) {
    LOG(INFO) << "Add drainer failed, drainer already exists for space " << space;
    handleErrorCode(nebula::cpp2::ErrorCode::E_EXISTED);
    onFinished();
    return;
  }

  std::vector<kvstore::KV> data;
  data.emplace_back(std::move(key), serializeHostAddrs(hosts));
  auto result = doSyncPut(std::move(data));
  handleErrorCode(result);
  onFinished();
}

void RemoveDrainerProcessor::process(const cpp2::RemoveDrainerReq& req) {
  auto space = req.get_space_id();
  CHECK_SPACE_ID_AND_RETURN(space);
  folly::SharedMutex::WriteHolder holder(LockUtils::lock());

  auto key = drainerNodesKey(space);
  auto ret = doGet(key);
  if (!nebula::ok(ret)) {
    auto code = nebula::error(ret);
    if (code == nebula::cpp2::ErrorCode::E_KEY_NOT_FOUND) {
      LOG(INFO) << "Remove drainer failed, drainer not found for space " << space;
      handleErrorCode(nebula::cpp2::ErrorCode::E_KEY_NOT_FOUND);
    } else {
      LOG(INFO) << "Remove drainer failed, error: "
                << apache::thrift::util::enumNameSafe(code);
      handleErrorCode(code);
    }
    onFinished();
    return;
  }

  doRemove(key);
}

void ListDrainersProcessor::process(const cpp2::ListDrainersReq& req) {
  auto space = req.get_space_id();
  CHECK_SPACE_ID_AND_RETURN(space);
  folly::SharedMutex::ReadHolder holder(LockUtils::lock());

  auto key = drainerNodesKey(space);
  auto ret = doGet(key);
  if (!nebula::ok(ret)) {
    auto code = nebula::error(ret);
    if (code == nebula::cpp2::ErrorCode::E_KEY_NOT_FOUND) {
      // No drainers for this space, return empty list
      handleErrorCode(nebula::cpp2::ErrorCode::SUCCEEDED);
      onFinished();
      return;
    }
    LOG(INFO) << "List drainers failed, error: "
              << apache::thrift::util::enumNameSafe(code);
    handleErrorCode(code);
    onFinished();
    return;
  }

  auto hosts = deserializeHostAddrs(nebula::value(ret));
  resp_.drainers_ref() = std::move(hosts);
  handleErrorCode(nebula::cpp2::ErrorCode::SUCCEEDED);
  onFinished();
}

void GetSyncStatusProcessor::process(const cpp2::GetSyncStatusReq& req) {
  auto space = req.get_space_id();
  CHECK_SPACE_ID_AND_RETURN(space);
  folly::SharedMutex::ReadHolder holder(LockUtils::lock());

  // In the full implementation, this aggregates real-time progress from listeners/drainers.
  // For now, read stored progress and convert to SyncStatusItem list.
  auto key = syncProgressKey(space);
  auto ret = doGet(key);
  if (!nebula::ok(ret)) {
    auto code = nebula::error(ret);
    if (code == nebula::cpp2::ErrorCode::E_KEY_NOT_FOUND) {
      // No progress data yet, return empty lists
      handleErrorCode(nebula::cpp2::ErrorCode::SUCCEEDED);
      onFinished();
      return;
    }
    LOG(INFO) << "Get sync status failed, error: "
              << apache::thrift::util::enumNameSafe(code);
    handleErrorCode(code);
    onFinished();
    return;
  }

  // Deserialize progress and convert to SyncStatusItem list
  // Format: repeated [PartitionID (4 bytes) + logId (8 bytes)]
  std::vector<cpp2::SyncStatusItem> items;
  auto rawData = nebula::value(ret);
  size_t offset = 0;
  while (offset + sizeof(PartitionID) + sizeof(int64_t) <= rawData.size()) {
    auto partId = *reinterpret_cast<const PartitionID*>(rawData.data() + offset);
    offset += sizeof(PartitionID);
    auto logId = *reinterpret_cast<const int64_t*>(rawData.data() + offset);
    offset += sizeof(int64_t);

    cpp2::SyncStatusItem item;
    item.part_id_ref() = partId;
    item.status_ref() = "SYNCING";
    item.log_id_lag_ref() = 0;
    item.time_latency_ms_ref() = 0;
    items.emplace_back(std::move(item));
    UNUSED(logId);
  }
  resp_.items_ref() = std::move(items);
  handleErrorCode(nebula::cpp2::ErrorCode::SUCCEEDED);
  onFinished();
}

void StopSyncProcessor::process(const cpp2::StopSyncReq& req) {
  auto space = req.get_space_id();
  CHECK_SPACE_ID_AND_RETURN(space);
  folly::SharedMutex::WriteHolder holder(LockUtils::lock());

  LOG(INFO) << "StopSyncProcessor: stopping sync for space " << space;
  // TODO(sync): In the full implementation, this will persist a "sync paused"
  // flag for the space and notify SyncListeners to stop processing WAL logs.
  // For now, just return SUCCEEDED.
  handleErrorCode(nebula::cpp2::ErrorCode::SUCCEEDED);
  onFinished();
}

void RestartSyncProcessor::process(const cpp2::RestartSyncReq& req) {
  auto space = req.get_space_id();
  CHECK_SPACE_ID_AND_RETURN(space);
  folly::SharedMutex::WriteHolder holder(LockUtils::lock());

  LOG(INFO) << "RestartSyncProcessor: restarting sync for space " << space;
  // TODO(sync): In the full implementation, this will clear the "sync paused"
  // flag for the space and notify SyncListeners to resume processing WAL logs.
  // For now, just return SUCCEEDED.
  handleErrorCode(nebula::cpp2::ErrorCode::SUCCEEDED);
  onFinished();
}

}  // namespace meta
}  // namespace nebula
