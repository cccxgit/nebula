/* Copyright (c) 2024 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include <folly/ssl/Init.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

#include "common/base/Base.h"
#include "common/base/SignalHandler.h"
#include "common/fs/FileUtils.h"
#include "common/network/NetworkUtils.h"
#include "common/process/ProcessUtils.h"
#include "daemons/SetupLogging.h"
#include "drainer/DrainerEnv.h"
#include "drainer/DrainerFlags.h"
#include "drainer/DrainerService.h"
#include "version/Version.h"
#include "webservice/WebService.h"

DEFINE_string(local_ip, "", "IP address which is used to identify this server");
DEFINE_bool(daemonize, true, "Whether to run the process as a daemon");
DEFINE_string(pid_file, "pids/nebula-drainerd.pid", "File to hold the process id");

using nebula::ProcessUtils;
using nebula::Status;
using nebula::network::NetworkUtils;

static std::unique_ptr<nebula::drainer::DrainerEnv> gDrainerEnv;

static void signalHandler(apache::thrift::ThriftServer* drainerServer, int sig);
static void waitForStop();
static Status setupSignalHandler(apache::thrift::ThriftServer* drainerServer);
#if defined(ENABLE_BREAKPAD)
extern Status setupBreakpad();
#endif

int main(int argc, char* argv[]) {
  google::SetVersionString(nebula::versionString());
  google::SetUsageMessage("Usage: " + std::string(argv[0]) + " [options]");

  // Detect if the server has already been started
  // Check pid before glog init, in case of user may start daemon twice
  // the 2nd will make the 1st failed to output log anymore
  gflags::ParseCommandLineFlags(&argc, &argv, false);

  Status status;

  auto pidPath = FLAGS_pid_file;
  status = ProcessUtils::isPidAvailable(pidPath);
  if (!status.ok()) {
    LOG(ERROR) << status;
    return EXIT_FAILURE;
  }

  folly::init(&argc, &argv, true);
  if (FLAGS_drainer_tls_enabled) {
    folly::ssl::init();
  }

  // Setup logging
  status = setupLogging(argv[0]);
  if (!status.ok()) {
    LOG(ERROR) << status;
    return EXIT_FAILURE;
  }

  if (FLAGS_daemonize) {
    google::SetStderrLogging(google::FATAL);
  } else {
    google::SetStderrLogging(google::INFO);
  }

#if defined(ENABLE_BREAKPAD)
  status = setupBreakpad();
  if (!status.ok()) {
    LOG(ERROR) << status;
    return EXIT_FAILURE;
  }
#endif

  if (FLAGS_daemonize) {
    status = ProcessUtils::daemonize(pidPath);
    if (!status.ok()) {
      LOG(ERROR) << status;
      return EXIT_FAILURE;
    }
  } else {
    // Write the current pid into the pid file
    status = ProcessUtils::makePidFile(pidPath);
    if (!status.ok()) {
      LOG(ERROR) << status;
      return EXIT_FAILURE;
    }
  }

  std::string hostName;
  if (FLAGS_local_ip.empty()) {
    hostName = nebula::network::NetworkUtils::getHostname();
  } else {
    status = NetworkUtils::validateHostOrIp(FLAGS_local_ip);
    if (!status.ok()) {
      LOG(ERROR) << status;
      return EXIT_FAILURE;
    }
    hostName = FLAGS_local_ip;
  }
  nebula::HostAddr localhost{hostName, FLAGS_drainer_port};
  LOG(INFO) << "Drainer localhost = " << localhost;

  // Initialize DrainerEnv
  gDrainerEnv = std::make_unique<nebula::drainer::DrainerEnv>();
  if (!gDrainerEnv->init()) {
    LOG(ERROR) << "Failed to initialize DrainerEnv";
    return EXIT_FAILURE;
  }

  // Start web service for metrics
  auto webSvc = std::make_unique<nebula::WebService>();
  status = webSvc->start(FLAGS_drainer_metrics_port);
  if (!status.ok()) {
    LOG(WARNING) << "Failed to start web service on port " << FLAGS_drainer_metrics_port
                 << ": " << status;
    // Non-fatal: drainer can run without metrics endpoint
  }

  // Create and configure the thrift server
  auto drainerServer = std::make_unique<apache::thrift::ThriftServer>();

  // Setup the signal handlers
  status = setupSignalHandler(drainerServer.get());
  if (!status.ok()) {
    LOG(ERROR) << status;
    return EXIT_FAILURE;
  }

  auto handler = std::make_shared<nebula::drainer::DrainerService>(gDrainerEnv.get());
  LOG(INFO) << "The drainer daemon starting on " << localhost;

  try {
    drainerServer->setPort(FLAGS_drainer_port);
    drainerServer->setIdleTimeout(std::chrono::seconds(0));  // No idle timeout on client connection
    drainerServer->setInterface(std::move(handler));
    if (FLAGS_drainer_tls_enabled) {
      // TODO(sync): Configure SSL context from drainer TLS flags
      // drainerServer->setSSLConfig(...);
    }
    drainerServer->serve();  // Will wait until the server shuts down
    waitForStop();
  } catch (const std::exception& e) {
    LOG(ERROR) << "Exception thrown: " << e.what();
    return EXIT_FAILURE;
  }

  LOG(INFO) << "The drainer daemon stopped";
  return EXIT_SUCCESS;
}

Status setupSignalHandler(apache::thrift::ThriftServer* drainerServer) {
  return nebula::SignalHandler::install(
      {SIGINT, SIGTERM}, [drainerServer](nebula::SignalHandler::GeneralSignalInfo* info) {
        signalHandler(drainerServer, info->sig());
      });
}

void signalHandler(apache::thrift::ThriftServer* drainerServer, int sig) {
  switch (sig) {
    case SIGINT:
    case SIGTERM:
      FLOG_INFO("Signal %d(%s) received, stopping this server", sig, ::strsignal(sig));
      if (drainerServer) {
        drainerServer->stop();
      }
      break;
    default:
      FLOG_ERROR("Signal %d(%s) received but ignored", sig, ::strsignal(sig));
  }
}

void waitForStop() {
  if (gDrainerEnv) {
    gDrainerEnv.reset();
  }
}
