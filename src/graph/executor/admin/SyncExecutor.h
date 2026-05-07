// Copyright (c) 2024 vesoft inc. All rights reserved.
//
// This source code is licensed under Apache 2.0 License.

#ifndef GRAPH_EXECUTOR_ADMIN_SYNCEXECUTOR_H_
#define GRAPH_EXECUTOR_ADMIN_SYNCEXECUTOR_H_

#include "graph/executor/Executor.h"

namespace nebula {
namespace graph {

class AddSyncListenerExecutor final : public Executor {
 public:
  AddSyncListenerExecutor(const PlanNode *node, QueryContext *qctx)
      : Executor("AddSyncListenerExecutor", node, qctx) {}

  folly::Future<Status> execute() override;
};

class RemoveSyncListenerExecutor final : public Executor {
 public:
  RemoveSyncListenerExecutor(const PlanNode *node, QueryContext *qctx)
      : Executor("RemoveSyncListenerExecutor", node, qctx) {}

  folly::Future<Status> execute() override;
};

class ShowSyncListenerExecutor final : public Executor {
 public:
  ShowSyncListenerExecutor(const PlanNode *node, QueryContext *qctx)
      : Executor("ShowSyncListenerExecutor", node, qctx) {}

  folly::Future<Status> execute() override;
};

class SignInDrainerServiceExecutor final : public Executor {
 public:
  SignInDrainerServiceExecutor(const PlanNode *node, QueryContext *qctx)
      : Executor("SignInDrainerServiceExecutor", node, qctx) {}

  folly::Future<Status> execute() override;
};

class SignOutDrainerServiceExecutor final : public Executor {
 public:
  SignOutDrainerServiceExecutor(const PlanNode *node, QueryContext *qctx)
      : Executor("SignOutDrainerServiceExecutor", node, qctx) {}

  folly::Future<Status> execute() override;
};

class ShowDrainerClientsExecutor final : public Executor {
 public:
  ShowDrainerClientsExecutor(const PlanNode *node, QueryContext *qctx)
      : Executor("ShowDrainerClientsExecutor", node, qctx) {}

  folly::Future<Status> execute() override;
};

class AddDrainerExecutor final : public Executor {
 public:
  AddDrainerExecutor(const PlanNode *node, QueryContext *qctx)
      : Executor("AddDrainerExecutor", node, qctx) {}

  folly::Future<Status> execute() override;
};

class RemoveDrainerExecutor final : public Executor {
 public:
  RemoveDrainerExecutor(const PlanNode *node, QueryContext *qctx)
      : Executor("RemoveDrainerExecutor", node, qctx) {}

  folly::Future<Status> execute() override;
};

class ShowDrainersExecutor final : public Executor {
 public:
  ShowDrainersExecutor(const PlanNode *node, QueryContext *qctx)
      : Executor("ShowDrainersExecutor", node, qctx) {}

  folly::Future<Status> execute() override;
};

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

class StopSyncExecutor final : public Executor {
 public:
  StopSyncExecutor(const PlanNode *node, QueryContext *qctx)
      : Executor("StopSyncExecutor", node, qctx) {}

  folly::Future<Status> execute() override;
};

class RestartSyncExecutor final : public Executor {
 public:
  RestartSyncExecutor(const PlanNode *node, QueryContext *qctx)
      : Executor("RestartSyncExecutor", node, qctx) {}

  folly::Future<Status> execute() override;
};

}  // namespace graph
}  // namespace nebula

#endif  // GRAPH_EXECUTOR_ADMIN_SYNCEXECUTOR_H_
