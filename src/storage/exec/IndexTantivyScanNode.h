/* Copyright (c) 2026. Licensed under Apache 2.0. */
#ifndef STORAGE_EXEC_INDEXTANTIVYSCANNODE_H_
#define STORAGE_EXEC_INDEXTANTIVYSCANNODE_H_
#include "common/plugin/fulltext/tantivy/TantivyIndex.h"
#include "storage/exec/IndexNode.h"
#include "storage/fulltext/TantivyTopK.h"
namespace nebula::storage {
class IndexTantivyScanNode final : public IndexNode {
 public:
  IndexTantivyScanNode(RuntimeContext* context,
                       const cpp2::IndexQueryContext& query,
                       size_t limit = 0);
  IndexTantivyScanNode(const IndexTantivyScanNode& node);
  nebula::cpp2::ErrorCode init(InitContext& ctx) override;
  std::unique_ptr<IndexNode> copy() override;
  std::string identify() override;

 protected:
  nebula::cpp2::ErrorCode doExecute(PartitionID part) override;
  Result doNext() override;

 private:
  Result readRow(const plugin::TantivyIndex::Hit& hit, const std::string& data);
  cpp2::IndexQueryContext query_;
  std::vector<std::string> columns_;
  std::vector<plugin::TantivyIndex::Hit> hits_;
  size_t limit_{0};
  std::vector<fulltext::TopKCandidate> candidates_;
  bool fallback_{false};
  size_t cursor_{0};
  PartitionID part_{0};
  nebula::cpp2::ErrorCode error_{nebula::cpp2::ErrorCode::SUCCEEDED};
};
}  // namespace nebula::storage
#endif
