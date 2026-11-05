/* Copyright (c) 2022 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include "storage/admin/ClearSpaceProcessor.h"

#include "kvstore/NebulaStore.h"
#include "storage/StorageFlags.h"
#include "storage/fulltext/TantivyManager.h"

namespace nebula {
namespace storage {

void ClearSpaceProcessor::process(const cpp2::ClearSpaceReq& req) {
  auto spaceId = req.get_space_id();
  if (FLAGS_store_type != "nebula") {
    this->resp_.code_ref() = nebula::cpp2::ErrorCode::E_INVALID_STORE;
    onFinished();
    return;
  }
  auto* store = static_cast<kvstore::NebulaStore*>(env_->kvstore_);
  auto code = store->clearSpace(spaceId);
  if (code == nebula::cpp2::ErrorCode::SUCCEEDED && env_->tantivy_) {
    code = env_->tantivy_->rebuild(spaceId);
  }
  this->resp_.code_ref() = code;
  onFinished();
}

void ClearSpaceProcessor::onFinished() {
  this->promise_.setValue(std::move(resp_));
  delete this;
}

}  // namespace storage
}  // namespace nebula
