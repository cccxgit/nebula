/* Copyright (c) 2026 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#ifndef GRAPH_SERVICE_SLOWQUERYLOGGER_H_
#define GRAPH_SERVICE_SLOWQUERYLOGGER_H_

#include "common/base/Base.h"

namespace nebula {
namespace graph {

struct SlowQueryLogRecord {
  uint64_t latencyUs{0};
  int32_t thresholdUs{0};
  std::string space;
  std::string user;
  int64_t sessionId{0};
  int64_t planId{0};
  int32_t statusCode{0};
  std::string query;
};

class SlowQueryLogger final {
 public:
  static SlowQueryLogger& instance();

  void logSlowQuery(const SlowQueryLogRecord& record);

 private:
  SlowQueryLogger() = default;
  ~SlowQueryLogger();

  bool ensureLogFdUnlocked();
  std::string buildLogPath() const;
  void resetLogFdUnlocked();

 private:
  mutable std::mutex lock_;
  int logFd_{-1};
  std::string openedPath_;
  bool warnedOpenFailed_{false};
  bool warnedWriteFailed_{false};
};

}  // namespace graph
}  // namespace nebula

#endif  // GRAPH_SERVICE_SLOWQUERYLOGGER_H_
