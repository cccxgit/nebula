/* Copyright (c) 2021 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include "daemons/SetupBreakpad.h"

#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <ctime>
#include <thread>

#include "common/base/Base.h"
#include "common/fs/FileUtils.h"
#include "folly/ScopeGuard.h"

#if defined(ENABLE_BREAKPAD)
#include <breakpad/client/linux/handler/exception_handler.h>
#endif

DEFINE_bool(enable_breakpad_signal_minidump,
            false,
            "Whether to enable manual signal-triggered Breakpad minidump.");
DEFINE_int32(breakpad_signal_minidump_signal,
             SIGUSR2,
             "Signal number used to trigger manual Breakpad minidump.");
DEFINE_string(breakpad_signal_minidump_dir,
              "",
              "Directory to store signal-triggered minidumps. Empty means FLAGS_log_dir.");
DEFINE_int32(breakpad_signal_minidump_min_interval_sec,
             60,
             "Minimum interval between two signal-triggered minidumps.");
DEFINE_bool(breakpad_signal_minidump_sanitize_stacks,
            false,
            "Whether to sanitize stack memory in signal-triggered minidumps.");

DECLARE_string(log_dir);

using nebula::Status;
using nebula::fs::FileUtils;

namespace {

constexpr int kMaxSignalNumber = 64;

Status validateSignalForMinidump(int sig) {
  if (sig < 1 || sig > kMaxSignalNumber) {
    return Status::Error("Invalid breakpad signal minidump signal: %d", sig);
  }

  switch (sig) {
    case SIGINT:
    case SIGTERM:
    case SIGKILL:
    case SIGSTOP:
    case SIGSEGV:
    case SIGABRT:
    case SIGILL:
    case SIGFPE:
    case SIGBUS:
    case SIGPIPE:
    case SIGHUP:
    case SIGCHLD:
      return Status::Error(
          "Signal %d(%s) is not allowed for breakpad minidump trigger", sig, ::strsignal(sig));
    default:
      break;
  }

  if (FLAGS_breakpad_signal_minidump_min_interval_sec < 0) {
    return Status::Error("breakpad_signal_minidump_min_interval_sec should not be negative");
  }

  struct sigaction current;
  ::memset(&current, 0, sizeof(current));
  if (::sigaction(sig, nullptr, &current) != 0) {
    return Status::Error("Get signal %d action failed: %s", sig, ::strerror(errno));
  }
  if (current.sa_handler != SIG_DFL) {
    return Status::Error("Signal %d(%s) already has a non-default action", sig, ::strsignal(sig));
  }

  return Status::OK();
}

}  // namespace

#if defined(ENABLE_BREAKPAD)
namespace {

static std::unique_ptr<google_breakpad::ExceptionHandler> gExceptionHandler;
std::atomic<bool> gSignalMinidumpInProgress{false};
std::atomic<int64_t> gLastSignalMinidumpUnixSec{0};

void writeSignalMinidump(const std::string& dumpDir, pid_t senderPid, uid_t senderUid) {
  bool expected = false;
  if (!gSignalMinidumpInProgress.compare_exchange_strong(expected, true)) {
    return;
  }

  auto resetInProgress = folly::makeGuard([] { gSignalMinidumpInProgress.store(false); });

  auto now = static_cast<int64_t>(::time(nullptr));
  auto last = gLastSignalMinidumpUnixSec.load();
  if (last > 0 && now - last < FLAGS_breakpad_signal_minidump_min_interval_sec) {
    LOG(WARNING) << "Skip breakpad signal minidump because the last trigger was " << now - last
                 << " seconds ago, min interval is "
                 << FLAGS_breakpad_signal_minidump_min_interval_sec << " seconds";
    return;
  }

  google_breakpad::MinidumpDescriptor descriptor(dumpDir);
  descriptor.set_sanitize_stacks(FLAGS_breakpad_signal_minidump_sanitize_stacks);
  google_breakpad::ExceptionHandler handler(descriptor, nullptr, nullptr, nullptr, false, -1);

  bool ok = handler.WriteMinidump();
  gLastSignalMinidumpUnixSec.store(static_cast<int64_t>(::time(nullptr)));

  const char* path = handler.minidump_descriptor().path();
  LOG(INFO) << "Breakpad signal minidump finished, succeeded=" << ok << ", sender_pid=" << senderPid
            << ", sender_uid=" << senderUid << ", path=" << (path == nullptr ? "" : path);
}

void runSignalMinidumpThread(sigset_t signalSet, std::string dumpDir) {
  while (true) {
    siginfo_t info;
    ::memset(&info, 0, sizeof(info));

    int sig = ::sigwaitinfo(&signalSet, &info);
    if (sig < 0) {
      if (errno == EINTR) {
        continue;
      }
      LOG(WARNING) << "sigwaitinfo for breakpad signal minidump failed: " << ::strerror(errno);
      continue;
    }

    writeSignalMinidump(dumpDir, info.si_pid, info.si_uid);
  }
}

}  // namespace

Status setupBreakpad() {
  if (!FileUtils::exist(FLAGS_log_dir)) {
    return Status::Error("Log directory does not exist:`%s'", FLAGS_log_dir.c_str());
  }
  google_breakpad::MinidumpDescriptor descriptor(FLAGS_log_dir);
  gExceptionHandler = std::make_unique<google_breakpad::ExceptionHandler>(
      descriptor, nullptr, nullptr, nullptr, true, -1);
  return Status::OK();
}
#else
Status setupBreakpad() {
  return Status::OK();
}
#endif

Status setupBreakpadSignalMinidump() {
  if (!FLAGS_enable_breakpad_signal_minidump) {
    return Status::OK();
  }

#if !defined(ENABLE_BREAKPAD)
  LOG(WARNING) << "Breakpad signal minidump is requested but ENABLE_BREAKPAD is off";
  return Status::OK();
#else
  auto sig = FLAGS_breakpad_signal_minidump_signal;
  auto status = validateSignalForMinidump(sig);
  if (!status.ok()) {
    return status;
  }

  auto dumpDir = FLAGS_breakpad_signal_minidump_dir.empty() ? FLAGS_log_dir
                                                            : FLAGS_breakpad_signal_minidump_dir;
  if (!FileUtils::exist(dumpDir)) {
    return Status::Error("Breakpad signal minidump directory does not exist: `%s'",
                         dumpDir.c_str());
  }

  sigset_t signalSet;
  ::sigemptyset(&signalSet);
  ::sigaddset(&signalSet, sig);

  int ret = ::pthread_sigmask(SIG_BLOCK, &signalSet, nullptr);
  if (ret != 0) {
    return Status::Error("Block signal %d(%s) failed: %s", sig, ::strsignal(sig), ::strerror(ret));
  }

  try {
    std::thread(runSignalMinidumpThread, signalSet, dumpDir).detach();
  } catch (const std::exception& e) {
    return Status::Error("Start breakpad signal minidump thread failed: %s", e.what());
  }

  LOG(INFO) << "Breakpad signal minidump enabled, signal=" << sig << "(" << ::strsignal(sig)
            << "), dump_dir=" << dumpDir
            << ", min_interval_sec=" << FLAGS_breakpad_signal_minidump_min_interval_sec;
  return Status::OK();
#endif
}
