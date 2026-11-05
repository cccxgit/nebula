// Copyright (c) 2021 vesoft inc. All rights reserved.
//
// This source code is licensed under Apache 2.0 License.

#ifndef GRAPH_UTIL_FT_INDEXUTIL_H_
#define GRAPH_UTIL_FT_INDEXUTIL_H_

#include "clients/meta/MetaClient.h"
#include "common/base/StatusOr.h"
#include "common/plugin/fulltext/elasticsearch/ESAdapter.h"
#include "graph/util/SchemaUtil.h"
#include "interface/gen-cpp2/storage_types.h"
#include "parser/MaintainSentences.h"

namespace nebula {
namespace graph {

class FTIndexUtils final {
 public:
  FTIndexUtils() = delete;
  // Checks if the filter expression is full-text search related
  static bool needTextSearch(const Expression* expr);

  // Select an indexed positive string predicate, including one conjunct of an AND.
  // Never extract a single branch of OR/NOT: that would produce incomplete results.
  static bool buildNativeIndexContext(meta::MetaClient* client,
                                      GraphSpaceID spaceId,
                                      int32_t schemaId,
                                      bool isEdge,
                                      const Expression* filter,
                                      storage::cpp2::IndexQueryContext* context);

  static bool buildNativeIndexContext(
      const std::unordered_map<std::string, meta::cpp2::FTIndex>& indexes,
      GraphSpaceID spaceId,
      int32_t schemaId,
      bool isEdge,
      const Expression* filter,
      storage::cpp2::IndexQueryContext* context);

  static StatusOr<::nebula::plugin::ESAdapter> getESAdapter(meta::MetaClient* client);

  // Converts TextSearchExpression into a relational expression that could be pushed down
  static StatusOr<Expression*> rewriteTSFilter(ObjectPool* pool,
                                               bool isEdge,
                                               Expression* expr,
                                               const std::string& index,
                                               ::nebula::plugin::ESAdapter& esAdapter);

  // Performs full-text search using elastic search adapter
  // Search type is defined by the expression kind
  static StatusOr<nebula::plugin::ESQueryResult> textSearch(Expression* expr,
                                                            const std::string& index,
                                                            ::nebula::plugin::ESAdapter& esAdapter);
};

}  // namespace graph
}  // namespace nebula
#endif  // GRAPH_UTIL_FT_INDEXUTIL_H_
