/* Copyright (c) 2025 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include "drainer/CheckpointStore.h"

#include <rocksdb/options.h>
#include <rocksdb/slice.h>

#include "common/base/Base.h"

namespace nebula {
namespace drainer {

static const char* kCheckpointPrefix = "cp::";
static const char* kEpochPrefix = "cp::epoch::";
static const char* kTokenPrefix = "cp::token::";

CheckpointStore::CheckpointStore(const std::string& dataPath) : dataPath_(dataPath) {}

CheckpointStore::~CheckpointStore() {
  if (db_) {
    db_.reset();
  }
}

bool CheckpointStore::init() {
  rocksdb::Options options;
  options.create_if_missing = true;

  std::string dbPath = dataPath_ + "/checkpoint.db";
  rocksdb::DB* db = nullptr;
  auto status = rocksdb::DB::Open(options, dbPath, &db);
  if (!status.ok()) {
    LOG(ERROR) << "Failed to open checkpoint store at " << dbPath << ": " << status.ToString();
    return false;
  }
  db_.reset(db);
  LOG(INFO) << "Checkpoint store opened at " << dbPath;
  return true;
}

bool CheckpointStore::commit(GraphSpaceID spaceId, PartitionID partId, LogID lastAppliedLogId) {
  auto key = makeCheckpointKey(spaceId, partId);
  std::string value;
  encodeInt64(lastAppliedLogId, &value);

  auto status = db_->Put(rocksdb::WriteOptions(), key, value);
  if (!status.ok()) {
    LOG(ERROR) << "Failed to commit checkpoint for space " << spaceId << " part " << partId
               << ": " << status.ToString();
    return false;
  }
  return true;
}

LogID CheckpointStore::getLastApplied(GraphSpaceID spaceId, PartitionID partId) const {
  auto key = makeCheckpointKey(spaceId, partId);
  std::string value;
  auto status = db_->Get(rocksdb::ReadOptions(), key, &value);
  if (!status.ok()) {
    return 0;
  }
  return decodeInt64(value);
}

std::unordered_map<PartitionID, LogID> CheckpointStore::getAllCheckpoints(
    GraphSpaceID spaceId) const {
  std::unordered_map<PartitionID, LogID> result;

  // Build prefix: "cp::{spaceId}::"
  std::string prefix = std::string(kCheckpointPrefix) + std::to_string(spaceId) + "::";

  std::unique_ptr<rocksdb::Iterator> iter(db_->NewIterator(rocksdb::ReadOptions()));
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    auto key = iter->key().ToString();
    if (key.find(prefix) != 0) {
      break;
    }
    // Extract partId from key: "cp::{spaceId}::{partId}"
    auto partIdStr = key.substr(prefix.size());
    PartitionID partId = 0;
    try {
      partId = std::stoi(partIdStr);
    } catch (const std::exception& e) {
      LOG(WARNING) << "Invalid partition id in checkpoint key: " << key;
      continue;
    }
    auto logId = decodeInt64(iter->value().ToString());
    result.emplace(partId, logId);
  }
  return result;
}

bool CheckpointStore::storeEpoch(int64_t primaryClusterId, int64_t epoch) {
  auto key = makeEpochKey(primaryClusterId);
  std::string value;
  encodeInt64(epoch, &value);

  auto status = db_->Put(rocksdb::WriteOptions(), key, value);
  if (!status.ok()) {
    LOG(ERROR) << "Failed to store epoch for cluster " << primaryClusterId << ": "
               << status.ToString();
    return false;
  }
  return true;
}

int64_t CheckpointStore::getEpoch(int64_t primaryClusterId) const {
  auto key = makeEpochKey(primaryClusterId);
  std::string value;
  auto status = db_->Get(rocksdb::ReadOptions(), key, &value);
  if (!status.ok()) {
    return 0;
  }
  return decodeInt64(value);
}

bool CheckpointStore::storeToken(int64_t primaryClusterId, const std::string& token) {
  auto key = makeTokenKey(primaryClusterId);
  auto status = db_->Put(rocksdb::WriteOptions(), key, token);
  if (!status.ok()) {
    LOG(ERROR) << "Failed to store token for cluster " << primaryClusterId << ": "
               << status.ToString();
    return false;
  }
  return true;
}

std::string CheckpointStore::getToken(int64_t primaryClusterId) const {
  auto key = makeTokenKey(primaryClusterId);
  std::string value;
  auto status = db_->Get(rocksdb::ReadOptions(), key, &value);
  if (!status.ok()) {
    return "";
  }
  return value;
}

std::string CheckpointStore::makeCheckpointKey(GraphSpaceID spaceId, PartitionID partId) const {
  return std::string(kCheckpointPrefix) + std::to_string(spaceId) + "::" + std::to_string(partId);
}

std::string CheckpointStore::makeEpochKey(int64_t primaryClusterId) const {
  return std::string(kEpochPrefix) + std::to_string(primaryClusterId);
}

std::string CheckpointStore::makeTokenKey(int64_t primaryClusterId) const {
  return std::string(kTokenPrefix) + std::to_string(primaryClusterId);
}

void CheckpointStore::encodeInt64(int64_t value, std::string* buf) {
  buf->resize(sizeof(int64_t));
  // Little-endian encoding
  for (size_t i = 0; i < sizeof(int64_t); ++i) {
    (*buf)[i] = static_cast<char>((value >> (i * 8)) & 0xFF);
  }
}

int64_t CheckpointStore::decodeInt64(const std::string& buf) {
  if (buf.size() < sizeof(int64_t)) {
    return 0;
  }
  int64_t value = 0;
  for (size_t i = 0; i < sizeof(int64_t); ++i) {
    value |= (static_cast<int64_t>(static_cast<uint8_t>(buf[i])) << (i * 8));
  }
  return value;
}

}  // namespace drainer
}  // namespace nebula
