// Copyright (c) 2024 vesoft inc. All rights reserved.
//
// This source code is licensed under Apache 2.0 License.

#ifndef GRAPH_EXECUTOR_ADMIN_SYNCEXECUTOR_H_
#define GRAPH_EXECUTOR_ADMIN_SYNCEXECUTOR_H_

#include "graph/executor/Executor.h"

namespace nebula {
namespace graph {

class ShowSyncStatusExecutor final : public Executor {
 public:
  ShowSyncStatusExecutor(const PlanNode *node, QueryContext *qctx)
      : Executor("ShowSyncStatusExecutor", node, qctx) {}

  folly::Future<Status> execute() override;
};

class ShowDrainerSyncStatusExecutor final : public Executor {
 public:
  ShowDrainerSyncStatusExecutor(const PlanNode *node, QueryContext *qctx)
      : Executor("ShowDrainerSyncStatusExecutor", node, qctx) {}

  folly::Future<Status> execute() override;
};

}  // namespace graph
}  // namespace nebula

#endif  // GRAPH_EXECUTOR_ADMIN_SYNCEXECUTOR_H_
