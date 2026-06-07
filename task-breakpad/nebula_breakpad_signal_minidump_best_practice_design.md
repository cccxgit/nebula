# Nebula Graph 手动信号触发 Breakpad 生成 Minidump 方案设计指导书

> 适用对象：Nebula Graph `graphd` / `metad` / `storaged`  
> 适用场景：生产环境死锁、卡死、无进展、线程池阻塞、Raft/RocksDB/Executor 长时间等待  
> 设计目标：通过手动信号触发 Breakpad 生成 minidump，不主动崩溃进程，不改变原死锁现场，提升线下定位效率  
> 文档日期：2026-06-07

---

## 1. 背景与问题

Nebula Graph 生产环境偶发死锁或卡死时，常见现象包括：

1. `graphd` 请求无响应，但进程仍存在；
2. `storaged` 查询、写入、Raft 任务、RocksDB 后台任务长时间无进展；
3. `metad` 请求卡住，客户端或其他服务访问 meta 超时；
4. K8s livenessProbe 可能最终重启容器，导致现场丢失；
5. 线上环境不方便直接 `gdb attach`，也不一定允许生成巨大 core dump。

传统日志只能说明“卡住了”，但很难回答以下关键问题：

1. 哪些线程在等待？
2. 等待发生在 `mutex`、`condition_variable`、`future`、`folly::Baton`、`pthread_cond_wait`、`futex`、RocksDB 内部锁，还是 RPC 阻塞？
3. Raft 线程、Storage executor、RocksDB background thread、Meta client thread 是否存在互相等待？
4. 卡死前后各服务节点是否处在一致状态？

因此需要一种**低侵入、手动触发、不会主动退出进程、可离线分析线程栈**的现场采集能力。

---

## 2. 官方依据与业界实践结论

### 2.1 Breakpad 官方能力

Breakpad 官方文档说明，Breakpad 可以在应用去除调试信息后记录紧凑的 minidump，并离线生成 C/C++ 调用栈；它也支持对**未崩溃程序按需写 minidump**。官方文档还说明 Breakpad 被 Google Chrome、Firefox、Google Earth 等大型 C++ 项目使用。

官方文档中对 minidump 内容的描述包括：

1. 当前进程加载的可执行文件和共享库列表；
2. 进程线程列表；
3. 每个线程的寄存器状态；
4. 每个线程的栈内存；
5. 处理器、操作系统版本和 dump 原因等信息。

这些内容正好适合定位 Nebula Graph 死锁、卡死、线程池阻塞和服务无进展问题。

### 2.2 Breakpad 支持应用主动请求 dump

Breakpad client design 文档说明，handler 可以因异常触发 dump，也可以由应用主动请求触发 dump；后者适合调试断言或其他“非崩溃但想知道程序如何进入某个状态”的问题。

Linux `ExceptionHandler` 接口也明确说明：`ExceptionHandler` 可以在异常发生时写 minidump，也可以在程序显式调用 `WriteMinidump()` 时写 minidump；如果不希望安装崩溃处理器，可以用 `install_handler=false` 创建 `ExceptionHandler`，之后手动调用 `WriteMinidump()`。

### 2.3 业界优秀实践抽象

公开资料能确认的是：

1. Breakpad/minidump 是成熟工业组件；
2. Breakpad 官方支持非崩溃状态主动 dump；
3. Linux 信号触发诊断动作是常见运维模式；
4. `signalfd` / `sigwait` 比在异步 signal handler 中执行复杂逻辑更稳；
5. K8s livenessProbe 可以用于 deadlock 恢复，但探针配置不当可能导致现场还没采集完就被重启。

未找到强公开证据表明“某个大型数据库开源项目直接采用 `SIGUSR2 -> Breakpad WriteMinidump()` 的同款实现”。因此，本方案的定位不是“数据库业界标准方案”，而是：

> 基于成熟 Breakpad 能力、成熟 Linux 信号机制和 Nebula Graph 自研封装的生产 DFX 现场采集能力。

---

## 3. 总体设计结论

推荐方案：

```text
运维人员 / liveness 脚本
        |
        | kill -USR2 <nebula_pid>
        v
Nebula 进程收到 SIGUSR2
        |
        | 由启动早期 block 的信号进入 pending 状态
        v
BreakpadSignalThread 通过 signalfd/sigwait 同步接收信号
        |
        | 普通线程上下文，不是异步 signal handler
        v
BreakpadManager 调用 ExceptionHandler::WriteMinidump()
        |
        +--> 写入 xxx.dmp
        +--> 写入 xxx.dmp.json sidecar 元信息
        +--> 打点 metrics / 输出轻量日志
        |
        v
进程继续运行，不 exit，不 abort，不主动重启
```

核心红线：

```text
禁止在 signal handler 中直接调用 Breakpad / glog / gflags / malloc / new / std::string / mutex / 文件流。
```

原因：

1. Linux signal handler 中只能安全调用 async-signal-safe 函数；
2. Breakpad 官方文档也提醒异常上下文需要避免 heap、复杂库函数和不安全操作；
3. `WriteMinidump()` 虽然适合应用主动调用，但不适合直接放在异步 signal handler 中执行；
4. 死锁场景下，如果业务线程正持有 malloc、glog、mutex 相关锁，错误的 signal handler 可能制造二次死锁。

---

## 4. 设计目标与非目标

### 4.1 设计目标

1. 支持 `graphd` / `metad` / `storaged` 通过手动信号生成 minidump；
2. 默认信号使用 `SIGUSR2`，允许通过 gflags 配置；
3. 触发 dump 后默认不退出进程；
4. 支持 dump 限频，避免误操作打爆磁盘；
5. 支持 dump 文件数量和空间清理；
6. dump 附带 sidecar 元信息，方便关联 Pod、Host、Space、进程、版本、时间；
7. 支持离线符号化，输出可读线程调用栈；
8. 与 K8s liveness/readiness 配合，避免现场采集前被重启；
9. 允许生产默认关闭，问题集群灰度开启。

### 4.2 非目标

1. 第一阶段不做自动上传 crash server；
2. 第一阶段不接管 `SIGSEGV` / `SIGABRT` 等崩溃信号；
3. 第一阶段不替代 core dump；
4. 第一阶段不在 dump 过程中做复杂诊断、锁分析、远程网络上传；
5. 第一阶段不自动 kill 或重启进程。

---

## 5. 推荐参数设计

建议新增 gflags：

```cpp
DEFINE_bool(enable_breakpad_minidump, false,
            "Enable Breakpad minidump infrastructure.");

DEFINE_bool(enable_signal_minidump, false,
            "Enable manual signal triggered minidump.");

DEFINE_string(signal_minidump_dir, "/data/nebula/minidump",
              "Directory used to store Breakpad minidump files.");

DEFINE_string(signal_minidump_signal, "SIGUSR2",
              "Signal used to trigger manual minidump. Recommended: SIGUSR2.");

DEFINE_int32(signal_minidump_min_interval_secs, 300,
             "Minimum interval between two manual minidumps.");

DEFINE_int32(signal_minidump_max_files, 20,
             "Maximum number of minidump files retained.");

DEFINE_int64(signal_minidump_max_dir_mb, 2048,
             "Maximum total size of minidump directory in MB.");

DEFINE_bool(signal_minidump_write_sidecar, true,
            "Whether to write sidecar json metadata for each minidump.");

DEFINE_bool(signal_minidump_install_crash_handler, false,
            "Whether Breakpad installs crash signal handlers. For manual dump only, keep false.");

DEFINE_bool(signal_minidump_exit_after_dump, false,
            "Whether to exit after manual minidump. Production default must be false.");
```

生产建议：

```text
--enable_breakpad_minidump=true
--enable_signal_minidump=true
--signal_minidump_signal=SIGUSR2
--signal_minidump_dir=/data/nebula/minidump
--signal_minidump_min_interval_secs=300
--signal_minidump_max_files=10
--signal_minidump_max_dir_mb=1024
--signal_minidump_write_sidecar=true
--signal_minidump_install_crash_handler=false
--signal_minidump_exit_after_dump=false
```

---

## 6. 模块设计

### 6.1 推荐目录

```text
src/common/diagnostics/
  BreakpadManager.h
  BreakpadManager.cpp
  SignalMinidumpTrigger.h
  SignalMinidumpTrigger.cpp
  MinidumpMetadata.h
  MinidumpMetadata.cpp
  MinidumpCleaner.h
  MinidumpCleaner.cpp

src/daemons/
  Main.cpp 或各服务 main 初始化处接入
```

### 6.2 模块职责

| 模块 | 职责 |
|---|---|
| `BreakpadManager` | 封装 Breakpad `ExceptionHandler`，提供 `Init()`、`WriteMinidump()`、`Shutdown()` |
| `SignalMinidumpTrigger` | block 指定信号，启动专用信号线程，通过 `signalfd` 或 `sigwait` 接收信号 |
| `MinidumpMetadata` | 生成 sidecar json，记录服务名、pid、hostname、pod、version、trigger signal、时间 |
| `MinidumpCleaner` | 限制文件数量和目录大小，防止磁盘被打满 |
| `metrics` | 记录 dump 触发次数、成功次数、失败次数、最近一次时间、最近一次耗时 |

---

## 7. 初始化时序设计

### 7.1 正确初始化顺序

在 Nebula 服务启动早期执行：

```text
main()
  |
  |-- 初始化 gflags
  |-- 初始化基础日志
  |-- 解析 enable_breakpad_minidump / enable_signal_minidump
  |
  |-- SignalMinidumpTrigger::BlockSignalBeforeThreads()
  |      必须尽早执行，最好在大量 worker 线程启动前执行
  |
  |-- BreakpadManager::Init()
  |      创建 ExceptionHandler，install_handler=false
  |
  |-- SignalMinidumpTrigger::Start()
  |      启动 BreakpadSignalThread
  |
  |-- 启动 graphd/metad/storaged 业务线程
```

### 7.2 为什么必须早期 block 信号

多线程程序中，进程级信号可能投递给任意未屏蔽该信号的线程。如果业务线程没有屏蔽 `SIGUSR2`，可能出现：

1. 某个业务线程直接收到 `SIGUSR2`；
2. 如果没有 handler，进程按默认动作退出；
3. 如果有 handler，则可能在业务线程上下文执行不安全逻辑；
4. dump 线程收不到信号。

因此要在大量业务线程创建前，在主线程执行：

```cpp
sigset_t mask;
sigemptyset(&mask);
sigaddset(&mask, SIGUSR2);
pthread_sigmask(SIG_BLOCK, &mask, nullptr);
```

由于新线程会继承创建者的 signal mask，因此后续业务线程也会默认 block `SIGUSR2`。专用 `BreakpadSignalThread` 再通过 `signalfd` 或 `sigwait` 同步接收该信号。

---

## 8. 信号接收方式设计

### 8.1 推荐方式：Linux 使用 signalfd

优点：

1. 不使用异步 signal handler；
2. 信号变成 fd 事件，逻辑清晰；
3. 能拿到发送者 pid、uid 等信息；
4. 可扩展到 epoll；
5. 对复杂 C++ 服务更安全。

伪代码：

```cpp
class SignalMinidumpTrigger {
 public:
  bool BlockSignalBeforeThreads(int signo) {
    sigemptyset(&mask_);
    sigaddset(&mask_, signo);
    int ret = pthread_sigmask(SIG_BLOCK, &mask_, nullptr);
    return ret == 0;
  }

  bool Start(int signo, BreakpadManager* manager) {
    manager_ = manager;
    signo_ = signo;
    running_.store(true);
    thread_ = std::thread([this] { this->RunSignalfdLoop(); });
    return true;
  }

  void Stop() {
    running_.store(false);
    if (signalFd_ >= 0) {
      close(signalFd_);
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }

 private:
  void RunSignalfdLoop() {
    signalFd_ = signalfd(-1, &mask_, SFD_CLOEXEC);
    if (signalFd_ < 0) {
      // 启动期日志即可，运行期不要反复刷屏
      return;
    }

    while (running_.load()) {
      signalfd_siginfo info;
      ssize_t n = read(signalFd_, &info, sizeof(info));
      if (n != sizeof(info)) {
        continue;
      }

      if (info.ssi_signo == static_cast<uint32_t>(signo_)) {
        manager_->WriteManualMinidump("signal", info.ssi_pid, info.ssi_uid);
      }
    }
  }

 private:
  int signo_{SIGUSR2};
  int signalFd_{-1};
  sigset_t mask_;
  std::atomic<bool> running_{false};
  std::thread thread_;
  BreakpadManager* manager_{nullptr};
};
```

### 8.2 备选方式：sigwait

如果工程不希望引入 `signalfd`，也可以使用 `sigwait`：

```cpp
void RunSigwaitLoop() {
  while (running_.load()) {
    int sig = 0;
    int ret = sigwait(&mask_, &sig);
    if (ret == 0 && sig == signo_) {
      manager_->WriteManualMinidump("signal", -1, -1);
    }
  }
}
```

两种方式都比在 `sigaction` handler 中直接调用 `WriteMinidump()` 更合适。

---

## 9. BreakpadManager 设计

### 9.1 类定义草案

```cpp
#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>

#include "client/linux/handler/exception_handler.h"
#include "client/linux/handler/minidump_descriptor.h"

namespace nebula {
namespace diagnostics {

class BreakpadManager final {
 public:
  struct Options {
    bool enabled{false};
    bool installCrashHandler{false};
    bool exitAfterDump{false};
    bool writeSidecar{true};
    std::string serviceName;
    std::string dumpDir;
    int minIntervalSecs{300};
    int maxFiles{20};
    int64_t maxDirMb{2048};
  };

  static BreakpadManager& Instance();

  bool Init(const Options& options);
  bool WriteManualMinidump(const std::string& trigger,
                           int senderPid,
                           int senderUid);
  void Shutdown();

 private:
  BreakpadManager() = default;

  static bool MinidumpCallback(
      const google_breakpad::MinidumpDescriptor& descriptor,
      void* context,
      bool succeeded);

  bool ShouldRateLimit();
  void WriteSidecarFile(const std::string& dumpPath,
                        const std::string& trigger,
                        int senderPid,
                        int senderUid,
                        bool succeeded,
                        int64_t elapsedMs);
  void CleanOldDumps();

 private:
  Options options_;
  std::unique_ptr<google_breakpad::ExceptionHandler> handler_;
  std::mutex dumpMutex_;
  std::atomic<int64_t> lastDumpUnixSec_{0};
  std::atomic<bool> initialized_{false};
};

}  // namespace diagnostics
}  // namespace nebula
```

### 9.2 初始化实现草案

```cpp
bool BreakpadManager::Init(const Options& options) {
  if (!options.enabled) {
    return true;
  }

  options_ = options;

  // 启动期创建目录。运行期 dump 路径不要反复做复杂准备。
  if (!EnsureDirectory(options_.dumpDir)) {
    return false;
  }

  google_breakpad::MinidumpDescriptor descriptor(options_.dumpDir);

  handler_ = std::make_unique<google_breakpad::ExceptionHandler>(
      descriptor,
      nullptr,                    // filter callback
      &BreakpadManager::MinidumpCallback,
      this,                       // callback context
      options_.installCrashHandler,
      -1);                        // server fd, -1 means unused

  initialized_.store(true);
  return true;
}
```

第一阶段建议 `installCrashHandler=false`，只支持手动 dump，避免改变原有崩溃处理行为。

### 9.3 手动 dump 实现草案

```cpp
bool BreakpadManager::WriteManualMinidump(const std::string& trigger,
                                          int senderPid,
                                          int senderUid) {
  if (!initialized_.load() || handler_ == nullptr) {
    return false;
  }

  std::unique_lock<std::mutex> guard(dumpMutex_, std::try_to_lock);
  if (!guard.owns_lock()) {
    // 已有 dump 正在进行，避免重入
    return false;
  }

  if (ShouldRateLimit()) {
    return false;
  }

  auto start = std::chrono::steady_clock::now();

  // 注意：这里是在普通线程上下文调用，不是 signal handler。
  bool ok = handler_->WriteMinidump();

  auto end = std::chrono::steady_clock::now();
  int64_t elapsedMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

  lastDumpUnixSec_.store(::time(nullptr));

  // callback 里拿到 dump path 更自然；这里也可以只记录 metrics。
  // 如果无法在 callback 写 sidecar，可在这里扫描最新 dump 文件补写。
  CleanOldDumps();

  return ok;
}
```

### 9.4 回调设计原则

Breakpad 回调里不要做复杂事情：

允许：

1. 保存 dump 成功/失败状态；
2. 保存 dump path；
3. 写很小的 sidecar json；
4. 记录轻量 metrics。

禁止：

1. 访问 Nebula 复杂对象；
2. 加业务锁；
3. 遍历图空间、partition、session、raft 状态；
4. 发 RPC；
5. 上传网络；
6. 压缩 dump；
7. 执行外部命令。

原因：回调可能处在异常/诊断路径，复杂逻辑容易二次卡死。

---

## 10. Sidecar 元信息设计

每个 dump 旁边生成一个同名 `.json` 文件：

```text
nebula-storaged.20260607T132500.12345.dmp
nebula-storaged.20260607T132500.12345.dmp.json
```

示例：

```json
{
  "service": "storaged",
  "pid": 1,
  "ppid": 0,
  "hostname": "nebula-storaged-0",
  "pod_name": "nebula-storaged-0",
  "namespace": "prod-graph",
  "node_name": "node-10-1-2-3",
  "container_name": "storaged",
  "nebula_version": "3.x-custom",
  "git_sha": "unknown",
  "build_type": "RelWithDebInfo",
  "trigger": "SIGUSR2",
  "sender_pid": 23456,
  "sender_uid": 0,
  "dump_path": "/data/nebula/minidump/nebula-storaged.xxx.dmp",
  "dump_succeeded": true,
  "elapsed_ms": 534,
  "timestamp_unix": 1780819500,
  "timestamp_local": "2026-06-07T13:25:00+09:00",
  "rate_limit_secs": 300,
  "exit_after_dump": false
}
```

sidecar 的价值：

1. dump 文件本身命名可能只包含 uuid，不方便关联环境；
2. 多 Pod、多节点同时采集时，需要快速识别来源；
3. 离线分析时可以判断二进制版本和符号文件是否匹配；
4. 可以记录触发方式和耗时，辅助判断 dump 是否扰动现场。

---

## 11. Dump 文件命名规范

推荐格式：

```text
<service>.<hostname>.<pid>.<timestamp>.<uuid>.dmp
```

例如：

```text
storaged.nebula-storaged-0.1.20260607T132500.6b4d9f10.dmp
```

如果 Breakpad 自己生成 uuid 命名，可保留 Breakpad 原始文件名，并在 sidecar 中记录映射关系。

---

## 12. 符号文件生成与离线解析

### 12.1 编译要求

生产用于 dump 分析的二进制建议保留独立符号文件：

```bash
# 示例：不要在生产二进制里保留巨大 debug 信息，可拆分符号
objcopy --only-keep-debug nebula-storaged nebula-storaged.debug
strip --strip-debug --strip-unneeded nebula-storaged
objcopy --add-gnu-debuglink=nebula-storaged.debug nebula-storaged
```

如果你们生产二进制已经是 `RelWithDebInfo` 或带 debug 包，需要确保最终能拿到**与线上二进制完全匹配**的符号。

### 12.2 Breakpad symbol 生成

```bash
dump_syms nebula-storaged.debug > nebula-storaged.sym
dump_syms nebula-graphd.debug > nebula-graphd.sym
dump_syms nebula-metad.debug > nebula-metad.sym
```

Breakpad symbol 目录通常需要按照 `.sym` 文件第一行的模块名和 ID 建目录。

示例：

```bash
head -1 nebula-storaged.sym
# MODULE Linux x86_64 ABCDEF1234567890 nebula-storaged

mkdir -p symbols/nebula-storaged/ABCDEF1234567890
mv nebula-storaged.sym symbols/nebula-storaged/ABCDEF1234567890/
```

### 12.3 minidump_stackwalk 解析

```bash
minidump_stackwalk \
  /data/nebula/minidump/storaged.xxx.dmp \
  /data/nebula/symbols \
  > storaged.xxx.stack.txt
```

重点关注：

```text
1. Crashed / requesting thread
2. All threads
3. pthread_mutex_lock / futex / pthread_cond_wait
4. folly::Future / folly::Baton / std::condition_variable
5. nebula::storage / nebula::raftex / rocksdb namespace
6. EventBase / Thrift / brpc / thread pool worker
7. 是否多个线程构成循环等待
```

---

## 13. Nebula Graph 接入点建议

### 13.1 graphd

目标定位：

1. 查询执行线程卡死；
2. session / plan / executor 等路径等待；
3. RPC client 到 storage/meta 阻塞；
4. 慢查询与线程池耗尽。

建议启动日志：

```text
Breakpad manual minidump enabled for graphd:
  signal=SIGUSR2
  dump_dir=/data/nebula/minidump
  install_crash_handler=false
```

### 13.2 storaged

目标定位：

1. Storage executor 卡死；
2. Raft worker 卡死；
3. RocksDB 写入/compact/flush 相关等待；
4. partition lock、kvstore lock、listener、balance 等路径卡住。

优先级最高，建议第一阶段先接入 `storaged`。

### 13.3 metad

目标定位：

1. meta service 线程池阻塞；
2. leader/balance/job 相关逻辑卡住；
3. graph/storage 访问 meta 长时间等待。

---

## 14. Kubernetes 生产操作设计

### 14.1 手动触发命令

```bash
# 进入目标 Pod
kubectl exec -n <namespace> -it <pod-name> -c <container-name> -- bash

# 确认进程
ps -ef | grep nebula-storaged

# 通常容器主进程 pid 为 1
kill -USR2 1

# 查看 dump
ls -lh /data/nebula/minidump
```

也可以不进入容器：

```bash
kubectl exec -n <namespace> <pod-name> -c <container-name> -- kill -USR2 1
```

### 14.2 多节点同时采集

```bash
for pod in $(kubectl get pod -n <namespace> -l app=nebula-storaged -o name); do
  kubectl exec -n <namespace> ${pod#pod/} -c storaged -- kill -USR2 1
done
```

### 14.3 拉取 dump

```bash
mkdir -p ./dumps

kubectl cp \
  <namespace>/<pod-name>:/data/nebula/minidump \
  ./dumps/<pod-name> \
  -c <container-name>
```

### 14.4 与 livenessProbe 配合

如果生产问题是“死锁后 livenessProbe 很快重启”，需要保证 dump 有窗口生成。

建议：

1. readinessProbe 失败只摘流量，不杀容器；
2. livenessProbe 的 `failureThreshold * periodSeconds` 至少给 dump 留出 30～60 秒窗口；
3. 自动化场景下可以在 liveness 脚本判定卡死后先发 `SIGUSR2`，再返回失败让 kubelet 后续重启；
4. liveness 脚本需要通过 marker 文件避免每次探测都触发 dump。

示例：

```bash
#!/usr/bin/env bash
set -euo pipefail

SERVICE=$1
MARKER="/tmp/${SERVICE}_minidump_triggered"
PID=1

if detect_deadlock_or_no_progress "$SERVICE"; then
  if [ ! -f "$MARKER" ]; then
    touch "$MARKER"
    kill -USR2 "$PID" || true
    sleep 10
  fi
  exit 1
fi

rm -f "$MARKER"
exit 0
```

注意：第一阶段建议只做**手动触发**，不要马上接入 liveness 自动触发，避免误判导致生产频繁 dump。

---

## 15. 风险分析与规避措施

| 风险 | 原因 | 规避 |
|---|---|---|
| 误发 SIGUSR2 导致进程退出 | SIGUSR2 默认动作是终止进程 | 启动早期 block，dump 线程启动成功后日志确认 |
| 二次死锁 | signal handler 中调用复杂逻辑 | 不使用异步 handler，使用 signalfd/sigwait |
| dump 卡住 | malloc、文件系统、线程状态采集阻塞 | 限制操作复杂度，dump 目录独立，必要时由 liveness 后续重启 |
| 磁盘打满 | 连续触发 dump | 限频、限文件数、限目录大小 |
| liveness 抢先重启 | 探针阈值太小 | 提高 failureThreshold，先 readiness 摘流量 |
| dump 无法解析 | 符号不匹配 | 建立符号归档，以 git sha/build id 关联 |
| dump 泄露敏感信息 | 栈内存可能包含查询、token、业务数据 | dump 访问权限控制，导出脱敏流程 |
| 性能影响 | dump 时枚举线程、写文件 | 默认关闭，问题集群开启；触发频率低 |
| 多次并发 dump | 多人同时 kill | `try_lock` 防重入，rate limit |
| sidecar 写入失败 | 目录权限或磁盘满 | dump 成功不依赖 sidecar，记录失败 metrics |

---

## 16. 安全与权限设计

1. dump 目录权限建议 `0700` 或至少只允许 Nebula 运行用户和运维用户访问；
2. dump 可能包含 NGQL、属性值、token、内存片段，不能随意外传；
3. `kubectl cp` 导出的 dump 应进入受控故障分析目录；
4. dump 保留周期建议 7～30 天；
5. 上传到缺陷平台前应确认是否包含敏感业务数据；
6. 生产环境不要开放普通业务用户执行 `kill -USR2` 的权限。

---

## 17. 测试方案

### 17.1 单元测试

| 用例 | 预期 |
|---|---|
| 参数默认关闭 | 不启动 dump 线程 |
| 参数开启但目录不存在 | 自动创建或启动失败有明确日志 |
| 无效 signal 名称 | 启动失败或回退失败，日志明确 |
| 连续触发两次 | 第二次被 rate limit |
| 并发触发 | 只有一个 dump 执行 |
| sidecar 开关关闭 | 只生成 dmp |
| max_files 生效 | 老 dump 被清理 |
| max_dir_mb 生效 | 超限后清理老文件 |

### 17.2 集成测试

启动 `nebula-storaged`：

```bash
./bin/nebula-storaged \
  --enable_breakpad_minidump=true \
  --enable_signal_minidump=true \
  --signal_minidump_dir=/tmp/nebula_minidump \
  --signal_minidump_signal=SIGUSR2 \
  --signal_minidump_min_interval_secs=1
```

触发：

```bash
kill -USR2 $(pidof nebula-storaged)
```

验证：

```bash
ls -lh /tmp/nebula_minidump
```

预期：

1. 生成 `.dmp`；
2. 生成 `.json`；
3. 进程仍然存活；
4. 日志中有 dump 成功记录；
5. `minidump_stackwalk` 能解析出线程列表。

### 17.3 死锁模拟测试

增加测试开关，仅测试环境使用：

```cpp
DEFINE_bool(test_enable_deadlock_endpoint, false,
            "Only for test. Enable endpoint to create intentional deadlock.");
```

模拟：

```cpp
std::mutex m1;
std::mutex m2;

void DeadlockThreadA() {
  std::lock_guard<std::mutex> l1(m1);
  std::this_thread::sleep_for(std::chrono::seconds(1));
  std::lock_guard<std::mutex> l2(m2);
}

void DeadlockThreadB() {
  std::lock_guard<std::mutex> l2(m2);
  std::this_thread::sleep_for(std::chrono::seconds(1));
  std::lock_guard<std::mutex> l1(m1);
}
```

触发死锁后执行：

```bash
kill -USR2 <pid>
```

验收标准：

1. dump 中能看到两个测试线程；
2. 两个线程分别卡在 `pthread_mutex_lock` / `futex`；
3. 进程没有因为 `SIGUSR2` 退出；
4. dump 线程没有卡住；
5. 第二次触发受 rate limit 控制。

### 17.4 K8s 测试

1. 部署开启参数的 storaged；
2. `kubectl exec` 触发 `kill -USR2 1`；
3. 验证 Pod 不重启；
4. 验证 dump 文件落盘到持久化目录；
5. 验证 `kubectl cp` 可以拉取；
6. 验证 livenessProbe 不会在 dump 前抢先杀容器。

---

## 18. 验收标准

### 18.1 功能验收

1. `graphd` / `metad` / `storaged` 均可配置开启；
2. `kill -USR2 1` 后生成 minidump；
3. 生成 sidecar 元信息；
4. 进程不退出；
5. 支持限频；
6. 支持文件清理；
7. dump 可离线解析；
8. 解析结果能看到所有线程栈。

### 18.2 稳定性验收

1. 连续触发 100 次，不发生崩溃；
2. 并发触发，不发生重入；
3. dump 目录不可写时，服务启动或运行行为符合预期；
4. 磁盘满场景不导致服务异常退出；
5. 死锁场景下，dump 尽可能生成，即使失败也不能主动 abort；
6. K8s 场景下，手动 dump 不导致容器重启。

### 18.3 性能验收

1. 未开启时无额外线程和可感知性能损耗；
2. 开启但未触发时只增加一个轻量等待线程；
3. 触发 dump 时允许短暂 IO 和线程枚举开销；
4. dump 耗时应记录 metrics，便于评估。

---

## 19. 代码实现任务拆解

### 任务 1：引入 Breakpad 编译依赖

1. 检查三方件是否已有 Breakpad；
2. 如无，补充 third-party 构建；
3. CMake 增加 include/library；
4. 确认 `dump_syms` 和 `minidump_stackwalk` 工具可构建或可随诊断工具包发布。

### 任务 2：实现 BreakpadManager

1. 封装 `ExceptionHandler`；
2. 支持 `install_handler=false`；
3. 支持 `WriteManualMinidump()`；
4. 实现 callback；
5. 实现 sidecar；
6. 实现 metrics。

### 任务 3：实现 SignalMinidumpTrigger

1. 解析 signal 名称；
2. 启动早期 block signal；
3. Linux 使用 signalfd；
4. 不使用异步 signal handler；
5. 线程命名为 `BreakpadSignalThread`；
6. 实现安全停止。

### 任务 4：接入 graphd/metad/storaged

1. 在服务 main 初始化早期调用；
2. 确保在大量业务线程启动前 block signal；
3. 输出启动日志；
4. 配置默认关闭；
5. 文档说明生产开启方式。

### 任务 5：增加测试

1. 单元测试；
2. 集成测试；
3. 死锁模拟测试；
4. K8s 手动触发测试；
5. 符号化测试。

---

## 20. 推荐 Codex / Claude Code 任务提示词

```markdown
你是 Nebula Graph C++ 数据库内核开发工程师。请在当前 Nebula Graph 工程中实现“手动信号触发 Breakpad 生成 minidump”的生产诊断能力。

目标：
1. 支持 graphd/metad/storaged 在运行时通过 SIGUSR2 手动触发 Breakpad WriteMinidump。
2. 不允许在异步 signal handler 中调用 Breakpad、glog、malloc、std::string、mutex 或文件流。
3. 必须使用 signalfd 或 sigwait 专用线程同步接收 SIGUSR2。
4. 必须在服务启动早期 block SIGUSR2，确保后续业务线程继承 signal mask。
5. Breakpad ExceptionHandler 第一阶段使用 install_handler=false，只做手动 dump，不接管崩溃信号。
6. 触发 dump 后默认不 exit、不 abort、不主动重启进程。
7. dump 文件旁边生成 sidecar json，记录 service、pid、hostname、pod、version、git_sha、trigger、sender_pid、timestamp、dump_path、elapsed_ms、succeeded。
8. 增加限频、文件数量限制、目录大小限制。
9. 增加单元测试、集成测试和死锁模拟测试。
10. 输出实现总结、编译命令、测试命令和风险说明。

建议新增模块：
src/common/diagnostics/BreakpadManager.h
src/common/diagnostics/BreakpadManager.cpp
src/common/diagnostics/SignalMinidumpTrigger.h
src/common/diagnostics/SignalMinidumpTrigger.cpp
src/common/diagnostics/MinidumpMetadata.h
src/common/diagnostics/MinidumpMetadata.cpp
src/common/diagnostics/MinidumpCleaner.h
src/common/diagnostics/MinidumpCleaner.cpp

新增参数：
--enable_breakpad_minidump
--enable_signal_minidump
--signal_minidump_dir
--signal_minidump_signal
--signal_minidump_min_interval_secs
--signal_minidump_max_files
--signal_minidump_max_dir_mb
--signal_minidump_write_sidecar
--signal_minidump_install_crash_handler=false
--signal_minidump_exit_after_dump=false

验收：
1. kill -USR2 <pid> 后生成 .dmp 和 .json。
2. 进程仍存活。
3. 连续触发受 rate limit 控制。
4. 死锁模拟场景下 dump 可解析所有线程栈。
5. K8s 容器内 kill -USR2 1 不导致 Pod 重启。
6. 文档中明确官方依据、风险和生产操作步骤。
```

---

## 21. 生产落地建议

分阶段实施：

```text
阶段 1：storaged 手动 SIGUSR2 dump
  - 默认关闭
  - 问题集群开启
  - 只支持手动触发
  - 不接入 liveness 自动触发

阶段 2：graphd/metad/storaged 全量接入
  - 增加 sidecar
  - 增加符号化脚本
  - 增加批量采集脚本

阶段 3：与 liveness/readiness 结合
  - readiness 先摘流量
  - liveness 判死锁前先触发一次 dump
  - marker 防止重复 dump

阶段 4：形成 DFX 工具链
  - 一键采集多 Pod dump
  - 一键拉取 dump
  - 一键 stackwalk
  - 自动汇总可疑等待线程
```

优先级建议：

```text
P0：storaged + 手动 SIGUSR2 + WriteMinidump + 不退出
P1：sidecar + 符号化工具链 + 文件清理
P2：graphd/metad 接入
P3：liveness 自动触发 dump
P4：dump 上传和自动栈聚合
```

---

## 22. 最终结论

本方案可以作为 Nebula Graph 生产死锁/卡死问题的低侵入现场采集能力。

最关键的设计点不是“能不能调用 Breakpad”，而是：

```text
1. 信号只做触发，不直接做 dump；
2. dump 在专用线程普通上下文执行；
3. Breakpad 第一阶段只手动触发，不接管崩溃信号；
4. 触发后不退出进程；
5. 配合 K8s 探针，避免现场采集前被重启；
6. 建立符号归档，否则 dump 价值会大幅降低。
```

一句话：

> `SIGUSR2 + signalfd/sigwait + Breakpad WriteMinidump + sidecar + offline stackwalk` 是一个适合 Nebula Graph 生产死锁定位的工程化方案，但必须坚持“非异步 handler 直接 dump、默认不退出、限频限量、符号匹配”这几条红线。

---

## 23. 参考资料

1. Breakpad Getting Started：`https://chromium.googlesource.com/breakpad/breakpad/+/master/docs/getting_started_with_breakpad.md`
2. Breakpad Client Design：`https://chromium.googlesource.com/breakpad/breakpad/+/master/docs/client_design.md`
3. Breakpad Linux ExceptionHandler：`https://chromium.googlesource.com/breakpad/breakpad/+/master/src/client/linux/handler/exception_handler.h`
4. Linux signalfd manual：`https://man7.org/linux/man-pages/man2/signalfd.2.html`
5. Linux signal-safety manual：`https://man7.org/linux/man-pages/man7/signal-safety.7.html`
6. Kubernetes Liveness, Readiness, Startup Probes：`https://kubernetes.io/docs/concepts/workloads/pods/probes/`
