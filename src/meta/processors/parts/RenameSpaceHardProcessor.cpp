/* Copyright (c) 2026 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include "meta/processors/parts/RenameSpaceHardProcessor.h"

#include <thrift/lib/cpp/util/EnumUtils.h>

#include "kvstore/LogEncoder.h"
#include "kvstore/NebulaStore.h"

namespace nebula {
namespace meta {

nebula::cpp2::ErrorCode RenameSpaceHardProcessor::checkLeader() {
  auto* store = dynamic_cast<kvstore::NebulaStore*>(kvstore_);
  if (store == nullptr) {
    return nebula::cpp2::ErrorCode::SUCCEEDED;
  }
  return store->isLeader(kDefaultSpaceId, kDefaultPartId)
             ? nebula::cpp2::ErrorCode::SUCCEEDED
             : nebula::cpp2::ErrorCode::E_LEADER_CHANGED;
}

void RenameSpaceHardProcessor::process(const RenameSpaceHardReq& req) {
  folly::SharedMutex::WriteHolder holder(LockUtils::lock());

  auto code = checkLeader();
  if (code != nebula::cpp2::ErrorCode::SUCCEEDED) {
    LOG(INFO) << "Hard rename space rejected because this meta is not leader, old_space="
              << req.oldName << ", new_space=" << req.newName;
    handleErrorCode(code);
    onFinished();
    return;
  }

  if (req.oldName.empty() || req.newName.empty() || req.oldName == req.newName ||
      !req.expectedSpaceId.has_value()) {
    LOG(INFO) << "Hard rename space invalid arguments, old_space=" << req.oldName
              << ", new_space=" << req.newName
              << ", has_expected_space_id=" << req.expectedSpaceId.has_value();
    handleErrorCode(nebula::cpp2::ErrorCode::E_INVALID_PARM);
    onFinished();
    return;
  }

  auto oldSpaceRet = getSpaceId(req.oldName);
  if (!nebula::ok(oldSpaceRet)) {
    auto retCode = nebula::error(oldSpaceRet);
    LOG(INFO) << "Hard rename space failed to find old_space=" << req.oldName
              << ", error=" << apache::thrift::util::enumNameSafe(retCode);
    handleErrorCode(retCode);
    onFinished();
    return;
  }

  auto spaceId = nebula::value(oldSpaceRet);
  if (spaceId != req.expectedSpaceId.value()) {
    LOG(INFO) << "Hard rename space expected_space_id mismatch, old_space=" << req.oldName
              << ", actual_space_id=" << spaceId
              << ", expected_space_id=" << req.expectedSpaceId.value();
    handleErrorCode(nebula::cpp2::ErrorCode::E_INVALID_PARM);
    onFinished();
    return;
  }

  auto newSpaceRet = getSpaceId(req.newName);
  if (nebula::ok(newSpaceRet)) {
    LOG(INFO) << "Hard rename space failed because new_space already exists, new_space="
              << req.newName;
    handleErrorCode(nebula::cpp2::ErrorCode::E_EXISTED);
    onFinished();
    return;
  }
  auto newSpaceCode = nebula::error(newSpaceRet);
  if (newSpaceCode != nebula::cpp2::ErrorCode::E_SPACE_NOT_FOUND) {
    LOG(INFO) << "Hard rename space failed to check new_space=" << req.newName
              << ", error=" << apache::thrift::util::enumNameSafe(newSpaceCode);
    handleErrorCode(newSpaceCode);
    onFinished();
    return;
  }

  auto spaceKey = MetaKeyUtils::spaceKey(spaceId);
  auto spaceValRet = doGet(spaceKey);
  if (!nebula::ok(spaceValRet)) {
    auto retCode = nebula::error(spaceValRet);
    LOG(INFO) << "Hard rename space failed to read space desc, old_space=" << req.oldName
              << ", space_id=" << spaceId
              << ", error=" << apache::thrift::util::enumNameSafe(retCode);
    handleErrorCode(retCode);
    onFinished();
    return;
  }

  auto properties = MetaKeyUtils::parseSpace(nebula::value(spaceValRet));
  properties.space_name_ref() = req.newName;
  resp_.id_ref() = to(spaceId, EntryType::SPACE);

  if (req.dryRun) {
    LOG(INFO) << "DRY-RUN hard rename space succeeded, old_space=" << req.oldName
              << ", new_space=" << req.newName << ", space_id=" << spaceId
              << ", operator=" << req.operatorName << ", comment=" << req.comment;
    handleErrorCode(nebula::cpp2::ErrorCode::SUCCEEDED);
    onFinished();
    return;
  }

  auto batchHolder = std::make_unique<kvstore::BatchHolder>();
  batchHolder->put(MetaKeyUtils::indexSpaceKey(req.newName),
                   std::string(reinterpret_cast<const char*>(&spaceId), sizeof(spaceId)));
  batchHolder->put(std::move(spaceKey), MetaKeyUtils::spaceVal(properties));
  batchHolder->remove(MetaKeyUtils::indexSpaceKey(req.oldName));
  LastUpdateTimeMan::update(batchHolder.get(), time::WallClock::fastNowInMilliSec());

  LOG(INFO) << "AUDIT hard rename space committing, old_space=" << req.oldName
            << ", new_space=" << req.newName << ", space_id=" << spaceId
            << ", operator=" << req.operatorName << ", comment=" << req.comment;
  auto batch = encodeBatchValue(std::move(batchHolder)->getBatch());
  doBatchOperation(std::move(batch));
}

}  // namespace meta
}  // namespace nebula
