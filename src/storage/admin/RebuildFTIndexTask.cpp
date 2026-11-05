/* Copyright (c) 2026. Licensed under Apache 2.0. */
#include "storage/admin/RebuildFTIndexTask.h"

#include "storage/fulltext/TantivyManager.h"
namespace nebula::storage {
bool RebuildFTIndexTask::check() {
  return env_->kvstore_ && env_->tantivy_;
}
ErrorOr<nebula::cpp2::ErrorCode, std::vector<AdminSubTask>> RebuildFTIndexTask::genSubTasks() {
  if (!env_->tantivy_) return nebula::cpp2::ErrorCode::E_UNSUPPORTED;
  std::vector<AdminSubTask> tasks;
  auto space = *ctx_.parameters_.space_id_ref();
  tasks.emplace_back([this, space] { return env_->tantivy_->rebuild(space); });
  return tasks;
}
}  // namespace nebula::storage
