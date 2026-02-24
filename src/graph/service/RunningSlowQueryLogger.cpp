/* Copyright (c) 2026 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include "graph/service/RunningSlowQueryLogger.h"

#include <algorithm>
#include <chrono>

#include "graph/stats/GraphStats.h"

DECLARE_string(log_dir);

namespace nebula {
namespace graph {

namespace {

constexpr int32_t kDefaultSlowQueryLogMaxQueryLen = 4096;

std::string escapeForLog(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (char c : value) {
    switch (c) {
      case '\\':
        escaped.append("\\\\");
        break;
      case '\n':
        escaped.append("\\n");
        break;
      case '\r':
        escaped.append("\\r");
        break;
      case '\t':
        escaped.append("\\t");
        break;
      case '"':
        escaped.append("\\\"");
        break;
      default:
        escaped.push_back(c);
        break;
    }
  }
  return escaped;
}

std::pair<std::string, bool> formatEscapedQuery(const std::string& query) {
  auto maxLen = FLAGS_slow_query_log_max_query_len;
  if (maxLen <= 0) {
    maxLen = kDefaultSlowQueryLogMaxQueryLen;
  }
  size_t rawSize = query.size();
  size_t actualLen = std::min(rawSize, static_cast<size_t>(maxLen));
  bool truncated = rawSize > actualLen;
  return {escapeForLog(query.substr(0, actualLen)), truncated};
}

std::string formatNowAsIso8601Local() {
  auto now = std::chrono::system_clock::now();
  auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch());
  auto nowUs = micros.count();
  auto microsPart = static_cast<int32_t>(nowUs % 1000000);
  auto nowSec = static_cast<time_t>(nowUs / 1000000);

  struct tm localTm;
  if (::localtime_r(&nowSec, &localTm) == nullptr) {
    return "1970-01-01T00:00:00.000000+00:00";
  }

  char timeBuffer[32];
  char zoneBuffer[8];
  if (std::strftime(timeBuffer, sizeof(timeBuffer), "%Y-%m-%dT%H:%M:%S", &localTm) == 0 ||
      std::strftime(zoneBuffer, sizeof(zoneBuffer), "%z", &localTm) == 0) {
    return "1970-01-01T00:00:00.000000+00:00";
  }

  std::string zone(zoneBuffer);
  if (zone.size() == 5) {
    zone.insert(3, ":");
  }

  return folly::stringPrintf("%s.%06d%s", timeBuffer, microsPart, zone.c_str());
}

}  // namespace

RunningSlowQueryLogger& RunningSlowQueryLogger::instance() {
  static RunningSlowQueryLogger logger;
  return logger;
}

RunningSlowQueryLogger::~RunningSlowQueryLogger() {
  std::lock_guard<std::mutex> g(lock_);
  resetLogFdUnlocked();
}

void RunningSlowQueryLogger::logRunningSlowQuery(const RunningSlowQueryLogRecord& record) {
  if (!FLAGS_enable_running_slow_query_log) {
    return;
  }

  auto queryAndTruncated = formatEscapedQuery(record.query);
  auto escapedSpace = escapeForLog(record.space);
  auto escapedUser = escapeForLog(record.user);
  auto escapedStatus = escapeForLog(record.status);
  auto line = folly::stringPrintf(
      "ts=%s elapsed_us=%lu threshold_us=%d space=\"%s\" user=\"%s\" session_id=%ld plan_id=%ld "
      "start_time_us=%ld status=\"%s\" truncated=%s query=\"%s\"\n",
      formatNowAsIso8601Local().c_str(),
      record.elapsedUs,
      record.thresholdUs,
      escapedSpace.c_str(),
      escapedUser.c_str(),
      record.sessionId,
      record.planId,
      record.startTimeUs,
      escapedStatus.c_str(),
      queryAndTruncated.second ? "true" : "false",
      queryAndTruncated.first.c_str());

  std::lock_guard<std::mutex> g(lock_);
  if (!ensureLogFdUnlocked()) {
    return;
  }

  auto* pos = line.data();
  auto left = line.size();
  while (left > 0) {
    auto written = ::write(logFd_, pos, left);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (!warnedWriteFailed_) {
        warnedWriteFailed_ = true;
        LOG(WARNING) << "Failed to write running slow query log `" << openedPath_
                     << "': " << ::strerror(errno);
      }
      return;
    }
    if (written == 0) {
      if (!warnedWriteFailed_) {
        warnedWriteFailed_ = true;
        LOG(WARNING) << "Failed to write running slow query log `" << openedPath_
                     << "': write returned 0";
      }
      return;
    }
    left -= static_cast<size_t>(written);
    pos += written;
  }
  warnedWriteFailed_ = false;
}

bool RunningSlowQueryLogger::ensureLogFdUnlocked() {
  auto targetPath = buildLogPath();
  if (logFd_ >= 0 && openedPath_ == targetPath) {
    return true;
  }

  resetLogFdUnlocked();
  openedPath_ = std::move(targetPath);
  logFd_ = ::open(openedPath_.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
  if (logFd_ < 0) {
    if (!warnedOpenFailed_) {
      warnedOpenFailed_ = true;
      LOG(WARNING) << "Failed to open running slow query log `" << openedPath_
                   << "': " << ::strerror(errno);
    }
    return false;
  }
  warnedOpenFailed_ = false;
  warnedWriteFailed_ = false;
  return true;
}

std::string RunningSlowQueryLogger::buildLogPath() const {
  auto filename = FLAGS_running_slow_query_log_filename.empty()
                      ? "nebula-running-slow-query.log"
                      : FLAGS_running_slow_query_log_filename;
  auto dir = FLAGS_running_slow_query_log_dir.empty() ? FLAGS_log_dir : FLAGS_running_slow_query_log_dir;
  if (dir.empty()) {
    return filename;
  }
  if (dir.back() == '/') {
    return dir + filename;
  }
  return dir + "/" + filename;
}

void RunningSlowQueryLogger::resetLogFdUnlocked() {
  if (logFd_ >= 0) {
    ::close(logFd_);
    logFd_ = -1;
  }
}

}  // namespace graph
}  // namespace nebula
