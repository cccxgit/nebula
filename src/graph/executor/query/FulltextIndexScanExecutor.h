// Copyright (c) 2022 vesoft inc. All rights reserved.
//
// This source code is licensed under Apache 2.0 License.

#ifndef GRAPH_EXECUTOR_QUERY_FULLTEXTINDEXSCANEXECUTOR_H_
#define GRAPH_EXECUTOR_QUERY_FULLTEXTINDEXSCANEXECUTOR_H_

#include "graph/executor/StorageAccessExecutor.h"
#include "graph/service/GraphFlags.h"

namespace nebula::graph {
class FulltextIndexScan;
class FulltextIndexScanExecutor final : public StorageAccessExecutor {
 public:
  FulltextIndexScanExecutor(const PlanNode* node, QueryContext* qctx)
      : StorageAccessExecutor("FulltextIndexScanExecutor", node, qctx) {}

  folly::Future<Status> execute() override;

 private:
  Status handleResponse(storage::StorageRpcResponse<storage::cpp2::LookupIndexResp>&& response);
};

}  // namespace nebula::graph

#endif
