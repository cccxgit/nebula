/* Copyright (c) 2026 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "graph/service/RunningSlowQueryLogger.h"

DEFINE_bool(enable_running_slow_query_log,
            false,
            "Whether to periodically scan local running queries and write running slow query log");
DEFINE_string(running_slow_query_log_dir,
              "",
              "Directory for running slow query log, empty means using log_dir");
DEFINE_string(running_slow_query_log_filename,
              "nebula-running-slow-query.log",
              "Running slow query log filename under running_slow_query_log_dir or log_dir");
DEFINE_int32(slow_query_log_max_query_len,
             4096,
             "Maximum query length for one slow query log line, query will be truncated if exceeds");

namespace nebula {
namespace graph {
namespace {

class ScopedTempDir final {
 public:
  ScopedTempDir() {
    char path[] = "/tmp/running_slow_query_logger_test.XXXXXX";
    auto* dir = ::mkdtemp(path);
    CHECK(dir != nullptr);
    path_ = dir;
  }

  ~ScopedTempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  const std::string& path() const {
    return path_;
  }

 private:
  std::string path_;
};

std::vector<std::string> readAllLines(const std::string& path) {
  std::ifstream ifs(path);
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(ifs, line)) {
    lines.emplace_back(std::move(line));
  }
  return lines;
}

RunningSlowQueryLogRecord makeRecord(const std::string& query = "MATCH (v) RETURN v",
                                     int64_t sessionId = 10,
                                     int64_t planId = 99) {
  RunningSlowQueryLogRecord record;
  record.elapsedUs = 812340;
  record.thresholdUs = 200000;
  record.space = "test_space";
  record.user = "root";
  record.sessionId = sessionId;
  record.planId = planId;
  record.startTimeUs = 1700000000123456;
  record.status = "RUNNING";
  record.query = query;
  return record;
}

TEST(RunningSlowQueryLoggerTest, WriteOneLine) {
  ScopedTempDir tempDir;
  FLAGS_log_dir = tempDir.path();
  FLAGS_enable_running_slow_query_log = true;
  FLAGS_running_slow_query_log_dir = tempDir.path();
  FLAGS_running_slow_query_log_filename = "running-slow.log";
  FLAGS_slow_query_log_max_query_len = 4096;

  RunningSlowQueryLogger::instance().logRunningSlowQuery(makeRecord("MATCH (v)\nRETURN v"));

  auto logPath = tempDir.path() + "/running-slow.log";
  auto lines = readAllLines(logPath);
  ASSERT_EQ(1, lines.size());
  EXPECT_NE(std::string::npos, lines[0].find("elapsed_us=812340"));
  EXPECT_NE(std::string::npos, lines[0].find("threshold_us=200000"));
  EXPECT_NE(std::string::npos, lines[0].find("space=\"test_space\""));
  EXPECT_NE(std::string::npos, lines[0].find("user=\"root\""));
  EXPECT_NE(std::string::npos, lines[0].find("session_id=10"));
  EXPECT_NE(std::string::npos, lines[0].find("plan_id=99"));
  EXPECT_NE(std::string::npos, lines[0].find("status=\"RUNNING\""));
  EXPECT_NE(std::string::npos, lines[0].find("query=\"MATCH (v)\\nRETURN v\""));
}

TEST(RunningSlowQueryLoggerTest, DisableLogByFlag) {
  ScopedTempDir tempDir;
  FLAGS_log_dir = tempDir.path();
  FLAGS_enable_running_slow_query_log = false;
  FLAGS_running_slow_query_log_dir = tempDir.path();
  FLAGS_running_slow_query_log_filename = "running-slow.log";

  RunningSlowQueryLogger::instance().logRunningSlowQuery(makeRecord());

  auto logPath = tempDir.path() + "/running-slow.log";
  EXPECT_FALSE(std::filesystem::exists(logPath));
}

TEST(RunningSlowQueryLoggerTest, TruncateLongQuery) {
  ScopedTempDir tempDir;
  FLAGS_log_dir = tempDir.path();
  FLAGS_enable_running_slow_query_log = true;
  FLAGS_running_slow_query_log_dir = tempDir.path();
  FLAGS_running_slow_query_log_filename = "running-slow.log";
  FLAGS_slow_query_log_max_query_len = 5;

  RunningSlowQueryLogger::instance().logRunningSlowQuery(makeRecord("0123456789"));

  auto logPath = tempDir.path() + "/running-slow.log";
  auto lines = readAllLines(logPath);
  ASSERT_EQ(1, lines.size());
  EXPECT_NE(std::string::npos, lines[0].find("truncated=true"));
  EXPECT_NE(std::string::npos, lines[0].find("query=\"01234\""));
}

}  // namespace
}  // namespace graph
}  // namespace nebula
