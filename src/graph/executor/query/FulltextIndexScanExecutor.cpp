// Copyright (c) 2022 vesoft inc. All rights reserved.
//
// This source code is licensed under Apache 2.0 License.

#include "graph/executor/query/FulltextIndexScanExecutor.h"

#include <cmath>

#include "common/datatypes/DataSet.h"
#include "common/datatypes/Edge.h"
#include "graph/planner/plan/Query.h"
#include "graph/util/Constants.h"

using nebula::storage::StorageClient;
using nebula::storage::StorageRpcResponse;
using nebula::storage::cpp2::LookupIndexResp;

namespace nebula::graph {

folly::Future<Status> FulltextIndexScanExecutor::execute() {
  const auto* scan = asNode<FulltextIndexScan>(node());
  auto* search = scan->searchExpression();
  if (search == nullptr || search->kind() != Expression::Kind::kESQUERY) {
    return Status::SemanticError("Invalid TEXT_SEARCH expression");
  }
  const auto offset = scan->getValidOffset();
  const auto limit = scan->limit(qctx());
  if (limit >= 0 && limit <= offset) {
    return finish(
        ResultBuilder().value(Value(DataSet({"id", kScore}))).iter(Iterator::Kind::kProp).build());
  }
  storage::cpp2::IndexQueryContext context;
  context.fulltext_index_name_ref() = search->arg()->index();
  context.fulltext_field_ref() = "";
  context.fulltext_value_ref() = search->arg()->query();
  context.fulltext_mode_ref() = 0;
  const auto spaceId = qctx()->rctx()->session()->space().id;
  StorageClient::CommonRequestParam param(spaceId,
                                          qctx()->rctx()->session()->id(),
                                          qctx()->plan()->id(),
                                          qctx()->plan()->isProfileEnabled());
  std::vector<std::string> columns = scan->isEdge()
                                         ? std::vector<std::string>{kSrc, kDst, kRank, "_score"}
                                         : std::vector<std::string>{kVid, "_score"};
  // The storage fast path ranks each partition with the same score/VID ordering
  // as the merge below. Each partition needs at most the global end (offset +
  // count), never just count. INT64 and edge key byte order differs from Value.
  const auto& vidType = qctx()->rctx()->session()->space().spaceDesc.get_vid_type();
  const auto requestLimit =
      !scan->isEdge() && vidType.get_type() == nebula::cpp2::PropertyType::FIXED_STRING &&
              scan->filter() == nullptr && scan->orderBy().empty() && limit > 0 &&
              limit < std::numeric_limits<int64_t>::max()
          ? limit
          : std::numeric_limits<int64_t>::max();
  return qctx()
      ->getStorageClient()
      ->lookupIndex(
          param, {std::move(context)}, scan->isEdge(), scan->schemaId(), columns, {}, requestLimit)
      .via(runner())
      .thenValue([this](StorageRpcResponse<LookupIndexResp>&& response) {
        memory::MemoryCheckGuard guard;
        addStats(response);
        return handleResponse(std::move(response));
      });
}

Status FulltextIndexScanExecutor::handleResponse(StorageRpcResponse<LookupIndexResp>&& response) {
  auto completeness = handleCompleteness(response, false);
  NG_RETURN_IF_ERROR(completeness);
  const auto* scan = asNode<FulltextIndexScan>(node());
  DataSet result({"id", kScore});
  for (auto& partitionResponse : response.responses()) {
    if (!partitionResponse.data_ref().has_value()) {
      return Status::Error("TEXT_SEARCH received a storage response without result data");
    }
    for (auto& row : partitionResponse.data_ref()->rows) {
      const auto expectedColumns = scan->isEdge() ? 4u : 2u;
      if (row.values.size() != expectedColumns || !row.values.back().isNumeric()) {
        return Status::Error("TEXT_SEARCH received invalid identifier or score columns");
      }
      const auto score = row.values.back().isFloat()
                             ? row.values.back().getFloat()
                             : static_cast<double>(row.values.back().getInt());
      if (!std::isfinite(score)) {
        return Status::Error("TEXT_SEARCH received a non-finite relevance score");
      }
      if (scan->isEdge()) {
        if (!row.values[2].isInt()) {
          return Status::Error("TEXT_SEARCH received an invalid edge rank");
        }
        Edge edge;
        edge.src = std::move(row.values[0]);
        edge.dst = std::move(row.values[1]);
        edge.ranking = row.values[2].getInt();
        edge.type = scan->schemaId();
        result.emplace_back(Row({Value(std::move(edge)), Value(score)}));
      } else {
        // Storage already decodes both INT64 and FIXED_STRING VIDs.
        result.emplace_back(Row({std::move(row.values[0]), Value(score)}));
      }
    }
  }
  std::sort(result.rows.begin(), result.rows.end(), [](const Row& left, const Row& right) {
    const auto lhs = left.values[1].getFloat();
    const auto rhs = right.values[1].getFloat();
    return lhs != rhs ? lhs > rhs : left.values[0] < right.values[0];
  });
  std::unordered_set<Value> seen;
  auto last = std::remove_if(result.rows.begin(), result.rows.end(), [&](const Row& row) {
    return !seen.emplace(row.values[0]).second;
  });
  result.rows.erase(last, result.rows.end());
  const auto offset = static_cast<size_t>(scan->getValidOffset());
  const auto limit = scan->limit(qctx());
  const auto end =
      limit < 0 ? result.rows.size() : std::min(result.rows.size(), static_cast<size_t>(limit));
  const auto begin = std::min(offset, end);
  result.rows.erase(result.rows.begin() + end, result.rows.end());
  result.rows.erase(result.rows.begin(), result.rows.begin() + begin);
  return finish(
      ResultBuilder().value(Value(std::move(result))).iter(Iterator::Kind::kProp).build());
}

}  // namespace nebula::graph
