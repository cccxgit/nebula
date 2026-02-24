/* Copyright (c) 2026 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include <gtest/gtest.h>

#include <filesystem>

#include "graph/service/SlowQueryLogger.h"

DEFINE_bool(enable_slow_query_log, true, "Whether to write slow query nGQL to dedicated log file");
DEFINE_string(slow_query_log_filename,
              "nebula-slow-query.log",
              "Slow query log filename under log_dir");
DEFINE_int32(slow_query_log_max_query_len,
             4096,
             "Maximum query length for one slow query log line, query will be truncated if exceeds");

namespace nebula {
namespace graph {
namespace {

class ScopedTempDir final {
 public:
  ScopedTempDir() {
    char path[] = "/tmp/slow_query_logger_test.XXXXXX";
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

int findOpenedFdByPath(const std::string& targetPath) {
  std::error_code ec;
  std::filesystem::path procFd("/proc/self/fd");
  if (!std::filesystem::exists(procFd, ec)) {
    return -1;
  }

  for (const auto& entry : std::filesystem::directory_iterator(procFd, ec)) {
    if (ec) {
      return -1;
    }
    std::error_code linkEc;
    auto link = std::filesystem::read_symlink(entry.path(), linkEc);
    if (linkEc || link.string() != targetPath) {
      continue;
    }
    auto fdName = entry.path().filename().string();
    try {
      return std::stoi(fdName);
    } catch (...) {
      return -1;
    }
  }
  return -1;
}

SlowQueryLogRecord makeRecord(const std::string& query = "MATCH (v) RETURN v",
                              int64_t sessionId = 17,
                              int64_t planId = 103) {
  SlowQueryLogRecord record;
  record.latencyUs = 512340;
  record.thresholdUs = 200000;
  record.space = "test_space";
  record.user = "root";
  record.sessionId = sessionId;
  record.planId = planId;
  record.statusCode = 0;
  record.query = query;
  return record;
}

TEST(SlowQueryLoggerTest, WriteOneLine) {
  ScopedTempDir tempDir;
  FLAGS_log_dir = tempDir.path();
  FLAGS_enable_slow_query_log = true;
  FLAGS_slow_query_log_filename = "slow.log";
  FLAGS_slow_query_log_max_query_len = 4096;

  auto record = makeRecord("MATCH (v)\nRETURN v");
  SlowQueryLogger::instance().logSlowQuery(record);

  auto logPath = tempDir.path() + "/slow.log";
  auto lines = readAllLines(logPath);
  ASSERT_EQ(1, lines.size());
  EXPECT_NE(std::string::npos, lines[0].find("latency_us=512340"));
  EXPECT_NE(std::string::npos, lines[0].find("threshold_us=200000"));
  EXPECT_NE(std::string::npos, lines[0].find("space=\"test_space\""));
  EXPECT_NE(std::string::npos, lines[0].find("user=\"root\""));
  EXPECT_NE(std::string::npos, lines[0].find("session_id=17"));
  EXPECT_NE(std::string::npos, lines[0].find("plan_id=103"));
  EXPECT_NE(std::string::npos, lines[0].find("status_code=0"));
  EXPECT_NE(std::string::npos, lines[0].find("truncated=false"));
  EXPECT_NE(std::string::npos, lines[0].find("query=\"MATCH (v)\\nRETURN v\""));
}

TEST(SlowQueryLoggerTest, DisableLogByFlag) {
  ScopedTempDir tempDir;
  FLAGS_log_dir = tempDir.path();
  FLAGS_enable_slow_query_log = false;
  FLAGS_slow_query_log_filename = "slow.log";

  SlowQueryLogger::instance().logSlowQuery(makeRecord());

  auto logPath = tempDir.path() + "/slow.log";
  EXPECT_FALSE(std::filesystem::exists(logPath));
}

TEST(SlowQueryLoggerTest, TruncateLongQuery) {
  ScopedTempDir tempDir;
  FLAGS_log_dir = tempDir.path();
  FLAGS_enable_slow_query_log = true;
  FLAGS_slow_query_log_filename = "slow.log";
  FLAGS_slow_query_log_max_query_len = 5;

  SlowQueryLogger::instance().logSlowQuery(makeRecord("0123456789"));

  auto logPath = tempDir.path() + "/slow.log";
  auto lines = readAllLines(logPath);
  ASSERT_EQ(1, lines.size());
  EXPECT_NE(std::string::npos, lines[0].find("truncated=true"));
  EXPECT_NE(std::string::npos, lines[0].find("query=\"01234\""));
}

TEST(SlowQueryLoggerTest, FallbackWhenMaxLenIsNonPositive) {
  ScopedTempDir tempDir;
  FLAGS_log_dir = tempDir.path();
  FLAGS_enable_slow_query_log = true;
  FLAGS_slow_query_log_filename = "slow.log";
  FLAGS_slow_query_log_max_query_len = 0;

  SlowQueryLogger::instance().logSlowQuery(makeRecord("0123456789"));

  auto logPath = tempDir.path() + "/slow.log";
  auto lines = readAllLines(logPath);
  ASSERT_EQ(1, lines.size());
  EXPECT_NE(std::string::npos, lines[0].find("truncated=false"));
  EXPECT_NE(std::string::npos, lines[0].find("query=\"0123456789\""));
}

TEST(SlowQueryLoggerTest, DegradeWhenWriteFails) {
  ScopedTempDir tempDir;
  FLAGS_enable_slow_query_log = true;
  FLAGS_log_dir = tempDir.path();
  FLAGS_slow_query_log_filename = "slow.log";
  FLAGS_slow_query_log_max_query_len = 4096;

  auto logPath = tempDir.path() + "/slow.log";
  SlowQueryLogger::instance().logSlowQuery(makeRecord("RETURN first"));

  auto fd = findOpenedFdByPath(logPath);
  if (fd < 0) {
    GTEST_SKIP() << "cannot resolve opened fd for log path";
  }
  ::close(fd);

  // The logger now holds a stale fd and should hit write failure path without crashing.
  SlowQueryLogger::instance().logSlowQuery(makeRecord("RETURN fail"));

  // After one write failure, logger should still work for later normal writes.
  FLAGS_slow_query_log_filename = "slow2.log";
  SlowQueryLogger::instance().logSlowQuery(makeRecord("RETURN recovered"));

  auto recoveredLogPath = tempDir.path() + "/slow2.log";
  auto lines = readAllLines(recoveredLogPath);
  ASSERT_EQ(1, lines.size());
  EXPECT_NE(std::string::npos, lines[0].find("query=\"RETURN recovered\""));
}

TEST(SlowQueryLoggerTest, ConcurrentWrite) {
  ScopedTempDir tempDir;
  FLAGS_log_dir = tempDir.path();
  FLAGS_enable_slow_query_log = true;
  FLAGS_slow_query_log_filename = "slow.log";
  FLAGS_slow_query_log_max_query_len = 4096;

  constexpr int32_t kThreads = 8;
  constexpr int32_t kPerThreadWrites = 50;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int32_t i = 0; i < kThreads; ++i) {
    threads.emplace_back([i]() {
      for (int32_t j = 0; j < kPerThreadWrites; ++j) {
        SlowQueryLogger::instance().logSlowQuery(makeRecord("RETURN " + std::to_string(j), i, j));
      }
    });
  }
  for (auto& t : threads) {
    t.join();
  }

  auto logPath = tempDir.path() + "/slow.log";
  auto lines = readAllLines(logPath);
  EXPECT_EQ(kThreads * kPerThreadWrites, lines.size());
}

}  // namespace
}  // namespace graph
}  // namespace nebula
