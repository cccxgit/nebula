# Nebula Graph 信号触发 Breakpad Minidump 设计指导书

## 1. 背景与目标

生产环境偶发死锁时，进程通常没有崩溃，现有 Breakpad 只能在段错误、abort 等 fatal signal 下自动生成 minidump。为了提升现场定位效率，需要支持运维人员向 graphd、metad、storaged 发送一个诊断信号，在不杀进程、不触发重启、不主动修改业务状态的前提下生成 minidump。

目标：

- 支持通过 Linux signal 触发 Breakpad 写 minidump。
- 默认关闭，新功能未开启时不改变 Nebula Graph 原行为。
- 开启后不复用 SIGINT/SIGTERM 等停服信号，不制造 SIGSEGV/SIGABRT 等崩溃信号。
- dump 触发链路不持有业务锁，不调用 stop/shutdown，不改变死锁现场的锁状态。
- 生成 dump 后进程保持原 PID 和原运行状态；如果现场已死锁，应继续保持该现场，便于后续继续用 gdb、pstack 等手段定位。

非目标：

- 不保证解决所有类型死锁。如果进程级别死锁影响 malloc、文件系统、ptrace 或 Breakpad 内部依赖，in-process minidump 仍可能失败或卡住。
- 不替代 core dump、gdb attach、pstack、perf 等手段。该功能是低侵入的第一现场快照工具。

## 2. 当前仓库现状

已确认的现有实现：

- `src/daemons/SetupBreakpad.cpp` 仅在 `ENABLE_BREAKPAD` 下编译 Breakpad 逻辑，创建全局 `google_breakpad::ExceptionHandler`，dump 目录使用 `FLAGS_log_dir`。
- `ExceptionHandler` 构造参数当前为 `install_handler=true`、`server_fd=-1`，即 fatal signal 自动写 dump，且使用 in-process dump generation。
- graphd、metad、storaged 只安装 SIGINT、SIGTERM 作为停服信号，未占用 SIGUSR1/SIGUSR2。
- `common/base/SignalHandler` 默认忽略 SIGPIPE、SIGHUP，并通过 `std::function` 分发信号处理逻辑。它适合现有停服逻辑，但不适合作为本功能的 dump 触发通道。
- Breakpad 头文件说明 `WriteMinidump()` 可显式写 dump，但该方法使用堆内存，不应在异步 signal handler 上下文直接调用。
- `/opt/vesoft/third-party/3.3/bin` 下有 `dump_syms`、`minidump_stackwalk`、`minidump_dump` 等工具，可用于 dump 可用性验证。

## 3. 设计原则

1. 默认不启用

   所有新 flag 默认关闭。不开启时不注册额外信号，不创建后台线程，不改变 SIGUSR2 默认语义，不影响原有启动、停服、崩溃 dump 行为。

2. 诊断信号独立

   默认使用 `SIGUSR2`。禁止配置 SIGINT、SIGTERM、SIGKILL、SIGSTOP、SIGSEGV、SIGABRT、SIGILL、SIGFPE、SIGBUS、SIGPIPE、SIGHUP、SIGCHLD 等已有语义或高风险信号。

3. 不在 signal handler 中写 dump

   不允许在 POSIX signal handler 中调用 `WriteMinidump()`、`LOG/FLOG`、加锁、分配内存或访问复杂对象。推荐方案使用 `pthread_sigmask + sigwaitinfo` 的专用线程消费信号，完全避免异步 signal handler。

4. 不触发服务退出

   收到诊断信号后只执行 dump 生成流程，不调用 `notifyStop()`、`ThriftServer::stop()`、`raise()`、`abort()`、`exit()`。

5. 不持有业务锁

   dump 线程不得进入 graph/meta/storage 业务对象，不读取 KVStore、Session、RocksDB、JobManager 等状态。minidump 本身通过 Breakpad 采集进程快照。

6. 限流与防重入

   dump 生成期间忽略新的触发信号；增加最小触发间隔，避免生产环境误操作造成 IO 风暴。

7. 失败不影响服务

   诊断功能初始化失败时记录 warning 并自动降级为不启用，daemon 不因此退出。

## 4. 推荐架构

推荐链路：

```text
kill -USR2 <pid>
        |
        v
内核投递 SIGUSR2
        |
        v
专用 sigwaitinfo 线程收到信号
        |
        v
检查 enable / interval / in_progress
        |
        v
google_breakpad::ExceptionHandler::WriteMinidump(...)
        |
        v
写入 minidump 文件，记录成功或失败日志
```

关键点：

- 使用 `pthread_sigmask(SIG_BLOCK, {SIGUSR2})` 在 daemon 主线程中屏蔽诊断信号。
- 屏蔽动作必须发生在服务业务线程启动前，使后续创建的 worker/http/io 线程继承该 signal mask。
- 创建一个 detached 诊断线程，线程内调用 `sigwaitinfo()` 等待诊断信号。
- `sigwaitinfo()` 返回后处于普通线程上下文，可以调用 Breakpad `WriteMinidump()`。
- dump 前尽量不做 glog 输出，避免如果死锁现场持有 glog/malloc 相关锁时，诊断线程在真正写 dump 前被日志阻塞。

## 5. 新增运行参数

建议新增以下 gflags，命名可按团队规范微调：

```text
--enable_breakpad_signal_minidump=false
--breakpad_signal_minidump_signal=12
--breakpad_signal_minidump_dir=
--breakpad_signal_minidump_min_interval_sec=60
--breakpad_signal_minidump_sanitize_stacks=false
```

参数说明：

- `enable_breakpad_signal_minidump`：是否启用信号触发 minidump。默认 false。
- `breakpad_signal_minidump_signal`：触发信号号。Linux x86_64 上 SIGUSR2 为 12。默认 SIGUSR2。
- `breakpad_signal_minidump_dir`：dump 输出目录。为空时使用 `FLAGS_log_dir`；生产建议使用 `${log_dir}/minidump` 并确保目录存在、权限受控、磁盘空间充足。
- `breakpad_signal_minidump_min_interval_sec`：两次 dump 最小间隔。默认 60 秒。
- `breakpad_signal_minidump_sanitize_stacks`：是否启用 Breakpad stack sanitize。默认 false，因为死锁分析通常需要较完整的栈和寄存器信息；如涉及敏感数据合规，可按需开启。

flag 建议在 `SetupBreakpad.cpp` 中无条件定义。非 `ENABLE_BREAKPAD` 构建下如果用户误开该 flag，应只记录 warning 并禁用该功能，避免 flagfile 在不同构建之间不兼容。

## 6. 代码改造建议

### 6.1 新增头文件

新增 `src/daemons/SetupBreakpad.h`：

```cpp
#pragma once

#include "common/base/Status.h"

nebula::Status setupBreakpad();
nebula::Status setupBreakpadSignalMinidump();
```

daemon 不再使用 `extern Status setupBreakpad();`，统一 include 该头文件。

### 6.2 改造 SetupBreakpad.cpp

职责拆分：

- `setupBreakpad()`：保持现有 fatal signal 自动 dump 能力。
- `setupBreakpadSignalMinidump()`：只负责诊断信号触发能力。
- `validateSignalForMinidump()`：校验信号是否合法且未与停服/fatal 信号冲突。
- `runSignalMinidumpThread()`：专用线程循环 `sigwaitinfo()` 并触发 dump。
- `writeSignalMinidump()`：执行 Breakpad dump，带防重入和限流。

核心伪代码：

```cpp
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

  auto dumpDir = FLAGS_breakpad_signal_minidump_dir.empty()
                     ? FLAGS_log_dir
                     : FLAGS_breakpad_signal_minidump_dir;
  if (!FileUtils::exist(dumpDir)) {
    return Status::Error("Breakpad signal minidump dir does not exist: %s", dumpDir.c_str());
  }

  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, sig);

  sigset_t oldSet;
  if (pthread_sigmask(SIG_BLOCK, &set, &oldSet) != 0) {
    return Status::Error("Block signal %d failed: %s", sig, strerror(errno));
  }

  try {
    std::thread([set, dumpDir] {
      runSignalMinidumpThread(set, dumpDir);
    }).detach();
  } catch (const std::exception& e) {
    pthread_sigmask(SIG_SETMASK, &oldSet, nullptr);
    return Status::Error("Start breakpad signal minidump thread failed: %s", e.what());
  }

  return Status::OK();
#endif
}
```

触发 dump 伪代码：

```cpp
void runSignalMinidumpThread(sigset_t set, std::string dumpDir) {
  while (true) {
    siginfo_t info;
    memset(&info, 0, sizeof(info));
    int sig = sigwaitinfo(&set, &info);
    if (sig < 0) {
      if (errno == EINTR) {
        continue;
      }
      continue;
    }
    writeSignalMinidump(dumpDir, info.si_pid, info.si_uid);
  }
}

void writeSignalMinidump(const std::string& dumpDir, pid_t senderPid, uid_t senderUid) {
  static std::atomic<bool> inProgress{false};
  if (inProgress.exchange(true)) {
    return;
  }

  auto guard = folly::makeGuard([] { inProgress.store(false); });

  if (!passIntervalLimit()) {
    LOG(WARNING) << "Skip breakpad signal minidump because of interval limit";
    return;
  }

  google_breakpad::MinidumpDescriptor descriptor(dumpDir);
  descriptor.set_sanitize_stacks(FLAGS_breakpad_signal_minidump_sanitize_stacks);

  google_breakpad::ExceptionHandler handler(
      descriptor, nullptr, nullptr, nullptr, false, -1);
  bool ok = handler.WriteMinidump();

  const char* path = handler.minidump_descriptor().path();
  LOG(INFO) << "Breakpad signal minidump finished, ok=" << ok
            << ", sender_pid=" << senderPid
            << ", sender_uid=" << senderUid
            << ", path=" << (path == nullptr ? "" : path);
}
```

说明：

- `install_handler=false` 的临时 `ExceptionHandler` 不改变现有 fatal crash handler。
- 也可以调用 `google_breakpad::ExceptionHandler::WriteMinidump(dumpDir, callback, context)`；使用临时 handler 的好处是可以设置 `sanitize_stacks` 等 descriptor 参数。
- 不建议直接复用全局 `gExceptionHandler->WriteMinidump()`，避免和 fatal crash handler 共享状态产生额外耦合。
- 不建议调用 `ExceptionHandler::HandleSignal(SIGUSR2, ...)`，因为本需求不是崩溃处理，也不能让诊断信号具备 crash signal 语义。

### 6.3 daemon 调用位置

必须在 daemonize 之后、业务线程启动之前调用 `setupBreakpadSignalMinidump()`。

原因：

- 如果在 daemonize 前创建诊断线程，fork 后只有调用 fork 的线程存在，容易留下不一致状态。
- 如果业务线程已启动，后续 `pthread_sigmask` 无法保证已存在的线程都屏蔽 SIGUSR2，信号可能落到业务线程。

建议位置：

- graphd：`ProcessUtils::daemonize()` 或 `makePidFile()` 成功之后，`WebService::start()` 之前。
- metad：`ProcessUtils::daemonize()` 或 `makePidFile()` 成功之后，HDFS helper、thread pool、web service、ThriftServer 启动之前。
- storaged：`ProcessUtils::daemonize()` 或 `makePidFile()` 成功之后，StorageServer 构造和启动之前。
- standalone：同样放在 daemonize/makePidFile 后，服务线程启动前。

daemon 调用方式：

```cpp
auto signalDumpStatus = setupBreakpadSignalMinidump();
if (!signalDumpStatus.ok()) {
  LOG(WARNING) << "Breakpad signal minidump is disabled: " << signalDumpStatus;
}
```

注意这里不要 `return EXIT_FAILURE`，诊断功能失败不能阻塞服务启动。

### 6.4 不使用 common/base/SignalHandler 的原因

`SignalHandler` 当前会在 signal handler 上下文中构造 `GeneralSignalInfo` 并调用 `std::function`。现有停服路径已经这样使用，但本功能面向死锁现场，应该进一步降低干扰：

- 不在异步 signal handler 中分配内存。
- 不在异步 signal handler 中执行 glog。
- 不让诊断信号中断业务线程的系统调用。
- 不覆盖 SIGINT/SIGTERM 既有停服逻辑。

因此本功能应独立使用 `pthread_sigmask + sigwaitinfo`。

## 7. 生产使用流程

编译安装：

```bash
cmake -DCMAKE_INSTALL_PREFIX=/usr/local/nebula \
      -DENABLE_TESTING=OFF \
      -DENABLE_BREAKPAD=ON \
      -DCMAKE_BUILD_TYPE=Debug ..
make -j10
make install
```

配置示例：

```text
--enable_breakpad_signal_minidump=true
--breakpad_signal_minidump_signal=12
--breakpad_signal_minidump_dir=/usr/local/nebula/logs/minidump
--breakpad_signal_minidump_min_interval_sec=60
```

启动前：

```bash
ulimit -n 65536
mkdir -p /usr/local/nebula/logs/minidump
```

触发 graphd dump：

```bash
pid=$(cat /usr/local/nebula/pids/nebula-graphd.pid)
kill -USR2 "${pid}"
```

验证服务未退出：

```bash
/usr/local/nebula/scripts/nebula.service status graphd
test "${pid}" = "$(cat /usr/local/nebula/pids/nebula-graphd.pid)"
ls -ltr /usr/local/nebula/logs/minidump/*.dmp
```

解析 dump：

```bash
/opt/vesoft/third-party/3.3/bin/minidump_stackwalk \
  /usr/local/nebula/logs/minidump/<dump>.dmp \
  /path/to/breakpad-symbols \
  > /tmp/nebula-minidump-stackwalk.txt
```

符号准备：

- `package/package.sh` 已使用 `dump_syms` 为 graphd、storaged、metad 生成 Breakpad 符号。
- `minidump_stackwalk` 通常需要按 `MODULE` 行中的 debug id 整理符号目录，例如 `symbols/nebula-graphd/<debug-id>/nebula-graphd.sym`。

## 8. 测试设计

### 8.1 单元测试

建议将 signal 等待和 dump 写入抽象为可替换接口，避免单元测试依赖真实 Breakpad 文件：

- `SignalMinidumpConfigTest`
  - 默认关闭。
  - SIGUSR2 合法。
  - SIGINT/SIGTERM/fatal signals/SIGKILL/SIGSTOP 非法。
  - 非法目录时返回错误但 daemon 侧不退出。

- `SignalMinidumpRateLimitTest`
  - 连续触发只执行一次 fake writer。
  - 超过 interval 后允许再次执行。
  - `inProgress=true` 时跳过重复触发。

- `SignalMinidumpThreadTest`
  - 使用测试信号和 fake writer。
  - `pthread_kill` 或 `kill(getpid(), sig)` 后 fake writer 被调用。
  - 测试结束后不影响 SIGTERM/SIGINT 行为。

### 8.2 Breakpad smoke test

仅在 `ENABLE_BREAKPAD=ON` 时启用：

- 启动一个小型测试进程，启用 `--enable_breakpad_signal_minidump=true`。
- 发送 SIGUSR2。
- 断言生成 `.dmp` 文件。
- 使用 `minidump_dump` 或 `minidump_stackwalk` 验证 dump 可解析。
- 断言测试进程没有退出，仍可响应正常退出信号。

### 8.3 daemon 端到端测试

分别覆盖 graphd、metad、storaged：

1. `ulimit -n 65536`。
2. 启动服务。
3. 记录 PID。
4. `kill -USR2 <pid>`。
5. 检查 minidump 文件生成。
6. 检查 PID 未变化，service status 仍为 running。
7. 再次发送 SIGUSR2，验证 interval 内不会生成大量 dump。
8. 发送 SIGTERM，验证原停服流程仍正常。

### 8.4 死锁现场模拟测试

新增或临时编译一个测试程序：

- 主线程启动两个 worker。
- worker A 持有 mutex1 后等待 mutex2。
- worker B 持有 mutex2 后等待 mutex1。
- 诊断线程启用 signal minidump。
- 发送 SIGUSR2。
- 断言 dump 生成，进程仍未退出，两个 worker 仍处于 deadlock。
- stackwalk 输出中可以看到两个 worker 的互相等待栈。

该测试用于证明本功能不会通过崩溃或停服破坏死锁现场。

## 9. 风险与控制

| 风险 | 影响 | 控制措施 |
| --- | --- | --- |
| `WriteMinidump()` 使用堆内存 | 如果死锁涉及 malloc 或 Breakpad 依赖，dump 线程可能卡住 | dump 前不做 glog/业务访问；必要时后续扩展 out-of-process crash generation |
| dump IO 较大 | 磁盘压力、短时 CPU/IO 抖动 | 默认关闭；显式目录；interval 限流；生产只在问题现场触发 |
| 信号冲突 | 覆盖原有语义或导致退出 | 默认 SIGUSR2；校验禁止停服/fatal 信号；如已有 handler 则拒绝启用 |
| daemonize 前启动线程 | fork 后线程状态异常 | 必须在 daemonize/makePidFile 后启动诊断线程 |
| 已有业务线程未屏蔽 SIGUSR2 | 信号可能落到业务线程 | setup 必须早于业务线程创建；端到端测试覆盖 |
| dump 包含敏感数据 | 合规风险 | dump 目录权限 0700；按需开启 sanitize；生产归档脱敏流程 |
| 无符号或符号不匹配 | stackwalk 不可读 | 构建时生成对应版本 `.sym`；符号与二进制版本一一对应 |

## 10. 验收标准

功能验收：

- 未开启 flag 时，Nebula Graph 行为与原版本一致。
- 开启 flag 后，SIGUSR2 可生成 minidump。
- 触发后服务不退出、不重启、PID 不变化。
- SIGINT/SIGTERM 停服逻辑保持不变。
- fatal crash 下原 Breakpad 自动 dump 行为保持不变。
- interval 内重复触发不会产生 dump 风暴。
- minidump 可被 `minidump_stackwalk` 或 `minidump_dump` 解析。

生产安全验收：

- dump 目录可控，权限和磁盘空间满足生产要求。
- 错误配置不会导致 daemon 启动失败，只会禁用该诊断功能并记录 warning。
- 死锁模拟场景下，触发 dump 不会打破或释放死锁现场。

## 11. 结论

按本设计实现后，信号触发 dump 不会通过 fatal signal、停服 signal 或退出流程影响 Nebula Graph 原功能，也不会主动改变死锁现场。它的不可避免影响是一次 minidump 采集带来的短时 CPU/IO 和 Breakpad 内部观测开销；这是现场快照工具的正常成本。对于极端的进程级死锁，如果 in-process `WriteMinidump()` 自身无法运行，应将下一阶段增强设计为 Breakpad out-of-process dump helper。
