# Nebula Graph 信号触发 Breakpad Minidump 任务总结

生成时间：2026-06-07 22:46:00 +0800

## 1. 总体结论

本次任务已经完成信号触发 Breakpad 生成 minidump 的设计、实现、构建安装和三类 daemon 的死锁现场验证。

验证覆盖：

| 进程 | 验证结果 | PID 是否保持 | 死锁现场是否保持 | dump 是否可解析定位 |
| --- | --- | --- | --- | --- |
| nebula-graphd | 通过 | 是，PID 39663 | 是，`bp-deadlock-a/b` 仍在 `futex_wait_queue` | 是 |
| nebula-metad | 通过 | 是，PID 43705 | 是，`bp-deadlock-a/b` 仍在 `futex_wait_queue` | 是 |
| nebula-storaged | 通过 | 是，PID 43969 | 是，`bp-deadlock-a/b` 仍在 `futex_wait_queue` | 是 |

关键结论：

1. `SIGUSR2` 可以在进程死锁场景下触发 Breakpad 写出 minidump。
2. dump 原因是 `DUMP_REQUESTED`，不是 crash，不会触发异常退出。
3. 触发 dump 后进程 PID 不变，服务没有异常重启。
4. 触发 dump 后人工构造的死锁线程仍保持阻塞，未破坏后续定位现场。
5. `minidump_stackwalk` 能解析出 dump 触发线程和死锁线程栈，具备辅助定位价值。

## 2. 目标约束与当前实现映射

### 2.1 不影响 Nebula Graph 原本功能

当前功能默认关闭：

```text
--enable_breakpad_signal_minidump=false
```

默认关闭时，daemon 仅调用一次初始化函数并立即返回，不注册信号等待线程，不改变正常业务路径。

原有 fatal crash Breakpad 行为保留：

```text
setupBreakpad()
```

新的手动 dump 能力是独立函数：

```text
setupBreakpadSignalMinidump()
```

正式业务入口只新增默认关闭的信号 dump 初始化，不改变原有启动、运行、停止流程。

### 2.2 不影响原死锁环境和后续定位

当前方案没有在异步 signal handler 中直接写 dump，而是：

1. daemon 初始化时通过 `pthread_sigmask(SIG_BLOCK, ...)` 阻塞指定信号。
2. 启动独立后台线程。
3. 后台线程通过 `sigwaitinfo()` 同步等待信号。
4. 收到信号后在线程上下文调用 Breakpad `WriteMinidump()`。

这避免了传统 signal handler 的 async-signal-unsafe 风险，也避免在业务线程中执行 dump 逻辑。

验证结果证明：

- 触发 dump 后 PID 保持不变。
- 死锁线程仍在 `futex_wait_queue`。
- dump 解析结果仍能看到死锁线程的互斥锁等待栈。

### 2.3 避免服务异常重启或信号冲突

当前实现禁止使用以下信号作为手动 dump 触发信号：

```text
SIGINT, SIGTERM, SIGKILL, SIGSTOP, SIGSEGV, SIGABRT, SIGILL,
SIGFPE, SIGBUS, SIGPIPE, SIGHUP, SIGCHLD
```

设计目的：

- 不覆盖 Nebula 原有停止信号。
- 不覆盖原有 crash dump 信号。
- 不使用不可捕获或不可等待的信号。
- 不影响子进程回收和常见系统语义。

本次验证使用：

```text
SIGUSR2 = 12
```

## 3. 代码变更概要

### 3.1 新增 Breakpad 统一声明头文件

文件：

```text
src/daemons/SetupBreakpad.h
```

声明：

```cpp
nebula::Status setupBreakpad();
nebula::Status setupBreakpadSignalMinidump();
```

目的：

- 替代各 daemon 中 `extern Status setupBreakpad();` 的散落声明。
- 给新增信号 dump 初始化提供统一入口。

### 3.2 扩展 Breakpad 初始化实现

文件：

```text
src/daemons/SetupBreakpad.cpp
```

新增 gflags：

| flag | 默认值 | 说明 |
| --- | --- | --- |
| `enable_breakpad_signal_minidump` | `false` | 是否启用信号触发 minidump |
| `breakpad_signal_minidump_signal` | `SIGUSR2` | 触发信号，Linux 上为 12 |
| `breakpad_signal_minidump_dir` | 空 | dump 目录；空表示使用 `FLAGS_log_dir` |
| `breakpad_signal_minidump_min_interval_sec` | `60` | 两次手动 dump 的最小间隔 |
| `breakpad_signal_minidump_sanitize_stacks` | `false` | 是否清理栈内存内容 |

关键逻辑：

- 未启用时直接返回 `Status::OK()`。
- `ENABLE_BREAKPAD` 未打开时记录 warning 并返回，不影响进程启动。
- 先校验 signal、dump 目录和参数，再阻塞信号。
- 通过 `sigwaitinfo()` 等待触发信号。
- 使用临时 Breakpad `ExceptionHandler` 写 dump，`install_handler=false`，不覆盖原 fatal crash handler。
- 使用 `gSignalMinidumpInProgress` 防止并发 dump。
- 使用 `gLastSignalMinidumpUnixSec` 做最小触发间隔保护。

### 3.3 daemon 接入点

已接入：

```text
src/daemons/GraphDaemon.cpp
src/daemons/MetaDaemon.cpp
src/daemons/StorageDaemon.cpp
src/daemons/StandAloneDaemon.cpp
```

接入方式：

```cpp
status = setupBreakpadSignalMinidump();
if (!status.ok()) {
  LOG(WARNING) << "Breakpad signal minidump is disabled: " << status;
}
```

说明：

- 启动失败不退出 daemon，只禁用信号触发 dump。
- 原有 `setupBreakpad()` 仍用于 fatal crash dump。
- 本次死锁验证覆盖 graphd、metad、storaged；standalone 已接入但未做本轮死锁验证。

### 3.4 验证用死锁 fixture

为验证 dump 是否破坏死锁现场，在 graphd/metad/storaged 中临时加入测试开关：

| flag | 默认值 | 说明 |
| --- | --- | --- |
| `breakpad_test_enable_deadlock` | `false` | 启动两个反向加锁线程制造死锁 |
| `breakpad_test_deadlock_only` | `false` | 构造死锁后仅保活，不进入完整业务启动 |

测试线程名：

```text
bp-deadlock-a
bp-deadlock-b
```

这部分是验证 fixture，不是信号 dump 正式能力本身。正式合入前建议评审是否移除、转为测试专用代码，或通过编译宏限制在 Debug/测试构建中。

## 4. 构建和安装记录

构建目录：

```text
build-breakpad/
```

安装目录：

```text
/usr/local/nebula
```

关键编译安装命令：

```bash
cmake -DCMAKE_INSTALL_PREFIX=/usr/local/nebula -DENABLE_TESTING=OFF -DCMAKE_BUILD_TYPE=Debug ..
make -j10
make install
```

补充构建和安装日志：

```text
task-breakpad/logs/29_make_after_signal_block_fix.log
task-breakpad/logs/30_make_install_after_signal_block_fix.log
task-breakpad/logs/35_make_metad_storaged_deadlock_test.log
task-breakpad/logs/36_make_install_metad_storaged_deadlock_test.log
```

其中 metad/storaged 最后一次安装确认：

```text
-- Installing: /usr/local/nebula/bin/nebula-storaged
-- Installing: /usr/local/nebula/bin/nebula-metad
```

## 5. 验证配置

graphd：

```text
task-breakpad/graphd_signal_deadlock.conf
```

metad：

```text
task-breakpad/metad_signal_deadlock.conf
```

storaged：

```text
task-breakpad/storaged_signal_deadlock.conf
```

共同关键配置：

```text
--enable_breakpad_signal_minidump=true
--breakpad_signal_minidump_signal=12
--breakpad_signal_minidump_min_interval_sec=1
--breakpad_test_enable_deadlock=true
--breakpad_test_deadlock_only=true
```

各进程使用独立 pid、log、data、dump 目录，避免和默认 Nebula 服务冲突。

## 6. 三进程验证结果

### 6.1 graphd

进程：

```text
PID 39663
/usr/local/nebula/bin/nebula-graphd --flagfile task-breakpad/graphd_signal_deadlock.conf
```

dump：

```text
task-breakpad/runtime/signal-minidumps/e7790921-2855-4181-31202d84-651f3c05.dmp
```

stackwalk：

```text
task-breakpad/runtime/final_graphd_signal_deadlock_stackwalk.txt
```

关键解析结果：

```text
Crash reason:  DUMP_REQUESTED
nebula-graphd!writeSignalMinidump [SetupBreakpad.cpp : 121]
nebula-graphd!runSignalMinidumpThread [SetupBreakpad.cpp : 143]
nebula-graphd!std::mutex::lock() [std_mutex.h : 100]
nebula-graphd!breakpadTestDeadlockThreadA [GraphDaemon.cpp : 63]
nebula-graphd!breakpadTestDeadlockThreadB [GraphDaemon.cpp : 70]
```

现场保持：

```text
do_sigtimedwait                  nebula-graphd
futex_wait_queue                 bp-deadlock-a
futex_wait_queue                 bp-deadlock-b
```

### 6.2 metad

进程：

```text
PID 43705
/usr/local/nebula/bin/nebula-metad --flagfile task-breakpad/metad_signal_deadlock.conf
```

dump：

```text
task-breakpad/runtime/metad-signal-minidumps/37b268d4-cf5e-47cb-2b605d8b-9e8963d7.dmp
```

stackwalk：

```text
task-breakpad/runtime/final_metad_signal_deadlock_stackwalk.txt
```

关键解析结果：

```text
Crash reason:  DUMP_REQUESTED
nebula-metad!writeSignalMinidump [SetupBreakpad.cpp : 121]
nebula-metad!runSignalMinidumpThread [SetupBreakpad.cpp : 143]
nebula-metad!std::mutex::lock() [std_mutex.h : 100]
nebula-metad!breakpadTestDeadlockThreadA [MetaDaemon.cpp : 82]
nebula-metad!breakpadTestDeadlockThreadB [MetaDaemon.cpp : 89]
```

现场保持：

```text
do_sigtimedwait                  nebula-metad
futex_wait_queue                 bp-deadlock-a
futex_wait_queue                 bp-deadlock-b
```

### 6.3 storaged

进程：

```text
PID 43969
/usr/local/nebula/bin/nebula-storaged --flagfile task-breakpad/storaged_signal_deadlock.conf
```

dump：

```text
task-breakpad/runtime/storaged-signal-minidumps/adc5cddf-1d36-474b-dbc32092-74750624.dmp
```

stackwalk：

```text
task-breakpad/runtime/final_storaged_signal_deadlock_stackwalk.txt
```

关键解析结果：

```text
Crash reason:  DUMP_REQUESTED
nebula-storaged!writeSignalMinidump [SetupBreakpad.cpp : 121]
nebula-storaged!runSignalMinidumpThread [SetupBreakpad.cpp : 143]
nebula-storaged!std::mutex::lock() [std_mutex.h : 100]
nebula-storaged!breakpadTestDeadlockThreadA [StorageDaemon.cpp : 75]
nebula-storaged!breakpadTestDeadlockThreadB [StorageDaemon.cpp : 82]
```

现场保持：

```text
do_sigtimedwait                  nebula-storaged
futex_wait_queue                 bp-deadlock-a
futex_wait_queue                 bp-deadlock-b
```

## 7. 关键日志索引

graphd：

```text
task-breakpad/logs/31_final_graphd_start_stdout.log
task-breakpad/logs/32_final_sigusr2_check.log
task-breakpad/logs/33_final_stackwalk_summary.log
task-breakpad/logs/34_final_process_left_for_debug.log
```

metad：

```text
task-breakpad/logs/37_metad_deadlock_start.log
task-breakpad/logs/38_metad_sigusr2_minidump_check.log
task-breakpad/logs/39_metad_stackwalk_summary.log
```

storaged：

```text
task-breakpad/logs/40_storaged_deadlock_start.log
task-breakpad/logs/41_storaged_sigusr2_minidump_check.log
task-breakpad/logs/42_storaged_stackwalk_summary.log
```

三进程最终现场汇总：

```text
task-breakpad/logs/43_three_daemon_deadlock_state.log
```

## 8. 当前保留现场

当前三个测试进程仍在运行，便于继续分析：

```text
graphd   PID 39663
metad    PID 43705
storaged PID 43969
```

清理命令：

```bash
kill -KILL 39663 43705 43969
```

说明：

- 这三个进程是测试进程，使用 `task-breakpad/*_signal_deadlock.conf` 启动。
- 进程处于人工构造的 deadlock-only 保活状态。
- 清理前如需继续分析，可以直接查看上述 dump、stackwalk 和日志。

## 9. dump 解析方法

生成符号：

```bash
/opt/vesoft/third-party/3.3/bin/dump_syms /usr/local/nebula/bin/nebula-metad > nebula-metad.sym
```

按 Breakpad 规范组织符号目录：

```text
symbols/<module_name>/<module_id>/<module_name>.sym
```

解析 dump：

```bash
/opt/vesoft/third-party/3.3/bin/minidump_stackwalk <dump_file> <symbol_tree> > stackwalk.txt 2> stackwalk.stderr
```

本次实际输出：

```text
task-breakpad/runtime/final_graphd_signal_deadlock_stackwalk.txt
task-breakpad/runtime/final_metad_signal_deadlock_stackwalk.txt
task-breakpad/runtime/final_storaged_signal_deadlock_stackwalk.txt
```

## 10. 生产使用建议

推荐生产配置示例：

```text
--enable_breakpad_signal_minidump=true
--breakpad_signal_minidump_signal=12
--breakpad_signal_minidump_dir=/path/to/minidumps
--breakpad_signal_minidump_min_interval_sec=60
--breakpad_signal_minidump_sanitize_stacks=false
```

触发方式：

```bash
kill -USR2 <nebula_pid>
```

建议：

1. dump 目录使用独立目录，确保 daemon 运行用户有写权限。
2. dump 目录所在磁盘要预留空间，避免定位时写满日志盘或数据盘。
3. 保持默认限频，不建议把 `breakpad_signal_minidump_min_interval_sec` 设置过低。
4. 不要选择 `SIGTERM`、`SIGINT`、`SIGSEGV`、`SIGABRT` 等已有语义信号。
5. 生产中不要开启 `breakpad_test_enable_deadlock` 和 `breakpad_test_deadlock_only`。
6. dump 中可能包含线程栈、寄存器和部分内存内容，导出前需要按公司安全规范处理。

## 11. 风险和注意事项

1. 手动 dump 本身会带来短时间 IO 和线程枚举成本，尤其是大进程或磁盘压力高时。
2. Breakpad 写 dump 时会采集线程上下文，理论上会短暂影响进程调度，但本次验证中未破坏死锁现场。
3. 如果进程处于更极端的全局资源死锁，例如 allocator 内部锁或文件系统写卡死，dump 线程仍可能受到环境影响。
4. 当前触发权限依赖 Linux `kill` 权限模型，通常同用户或 root 可以发送信号；如需更强控制，可以后续增加 sender pid/uid 审计或外部运维流程限制。
5. 本次验证 fixture 是为了构造可控死锁，不建议作为正式发布功能保留，除非用编译宏或测试配置隔离。

## 12. 后续建议

建议下一步按优先级处理：

1. 将验证用死锁 fixture 从正式代码中移除，或用测试编译宏保护。
2. 保留 `setupBreakpadSignalMinidump()` 正式能力，并补充配置文档。
3. 对 standalone 做一次 smoke 验证，确认接入路径没有问题。
4. 增加一份运维 runbook：如何开启、如何触发、如何收集 dump、如何解析符号。
5. 在预发环境验证真实 Nebula 负载下触发 dump 的耗时、dump 大小和日志表现。

## 13. 相关文档

设计文档：

```text
task-breakpad/breakpad_signal_minidump_design.md
```

ChatGPT 生成的对比方案：

```text
task-breakpad/nebula_breakpad_signal_minidump_best_practice_design.md
```

使用指导草稿：

```text
task-breakpad/nebula_graph_breakpad_usage_guide.md
```

本总结：

```text
task-breakpad/breakpad_signal_minidump_task_summary.md
```
