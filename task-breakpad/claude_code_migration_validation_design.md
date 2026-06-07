# Claude Code 搬迁与端到端验证设计文档

## 1. 背景和目标

本设计文档用于指导在另一套生产环境中，使用 Claude Code 将当前仓库已提交的 Breakpad 信号触发 minidump 方案搬迁过去，并完成端到端编译、部署、触发、解析和风险验证。

当前源提交：

```text
cec9593e3b4e2705df81671287f2c83c4da81d8e
Add signal-triggered Breakpad minidump support
```

目标：

1. 将信号触发 Breakpad minidump 的代码能力搬迁到目标环境代码仓库。
2. 确认默认关闭时不影响 Nebula Graph 原有功能。
3. 在隔离测试实例中构造死锁，验证 `SIGUSR2` 能生成 minidump。
4. 验证 dump 可通过 `minidump_stackwalk` 解析出 dump 触发栈和死锁线程栈。
5. 验证触发 dump 后进程不退出、不重启，死锁现场仍保持，便于后续定位。

## 2. 搬迁范围

### 2.1 必须搬迁的代码文件

```text
src/daemons/SetupBreakpad.cpp
src/daemons/SetupBreakpad.h
src/daemons/GraphDaemon.cpp
src/daemons/MetaDaemon.cpp
src/daemons/StorageDaemon.cpp
src/daemons/StandAloneDaemon.cpp
```

### 2.2 必须搬迁的设计和验证材料

```text
task-breakpad/breakpad_signal_minidump_design.md
task-breakpad/breakpad_signal_minidump_task_summary.md
task-breakpad/nebula_graph_breakpad_usage_guide.md
task-breakpad/graphd_breakpad_smoke.conf
task-breakpad/graphd_signal_deadlock.conf
task-breakpad/metad_signal_deadlock.conf
task-breakpad/storaged_signal_deadlock.conf
task-breakpad/task-breakpad.md
```

### 2.3 可选参考材料

```text
task-breakpad/nebula_breakpad_signal_minidump_best_practice_design.md
```

该文件是对比方案参考，不是当前实现的主设计来源。

### 2.4 不需要搬迁的本地运行产物

```text
build-breakpad/
task-breakpad/logs/
task-breakpad/runtime/
task-breakpad/parser-generated-backup/
task-breakpad/run.log
*.dmp
*.sym
*stackwalk*.txt
```

这些文件属于本地编译、dump、日志、符号和解析输出。目标环境需要重新生成自己的验证证据。

## 3. 安全边界

### 3.1 严禁在真实业务进程中启用测试死锁 fixture

以下两个 flag 只允许用于隔离测试实例：

```text
--breakpad_test_enable_deadlock=true
--breakpad_test_deadlock_only=true
```

它们会故意制造死锁。不能在承载真实流量的 graphd、metad、storaged 上打开。

生产业务实例只能启用信号 dump 能力：

```text
--enable_breakpad_signal_minidump=true
```

### 3.2 功能默认关闭

当前实现默认关闭：

```text
--enable_breakpad_signal_minidump=false
```

默认关闭时不注册信号等待线程，不改变运行行为。

### 3.3 触发信号选择

推荐使用：

```text
SIGUSR2 = 12
```

禁止使用或不应使用：

```text
SIGINT, SIGTERM, SIGKILL, SIGSTOP, SIGSEGV, SIGABRT, SIGILL,
SIGFPE, SIGBUS, SIGPIPE, SIGHUP, SIGCHLD
```

原因：

- 避免覆盖正常停止信号。
- 避免覆盖 fatal crash dump 信号。
- 避免使用系统不可捕获或具有特殊语义的信号。

### 3.4 生产环境验证方式

推荐顺序：

1. 目标机器先做源码编译验证。
2. 使用独立端口、独立 pid/log/data/dump 目录启动隔离测试实例。
3. 在隔离测试实例中启用测试死锁 fixture。
4. 解析测试 dump，确认定位能力。
5. 再选择一台 canary 业务节点，只启用信号 dump，不启用测试死锁 fixture。
6. 在低峰或维护窗口对 canary 业务进程触发一次 `SIGUSR2`。
7. 验证 canary 进程不退出、不重启、业务指标无异常。

## 4. 目标环境前置条件

Claude Code 在目标环境执行前，需要确认：

1. 当前 Nebula Graph 源码版本和本仓库版本兼容，最好同为 3.6 发布版本。
2. 目标仓库有干净工作区，或明确哪些本地改动不能触碰。
3. Breakpad 编译依赖可用。
4. 可使用 `dump_syms` 和 `minidump_stackwalk`。
5. 安装目录明确，默认按 `/usr/local/nebula` 处理。
6. 运行 daemon 前执行：

```bash
ulimit -n 65536
```

7. 目标环境有足够磁盘空间存放 dump。
8. dump 目录对 Nebula 进程运行用户可写。

## 5. 搬迁方式

### 5.1 优先方式：git cherry-pick

如果目标环境可以访问当前提交所在仓库：

```bash
git fetch <source-remote> 3.6-breakpad
git checkout -b breakpad-signal-minidump-validation
git cherry-pick cec9593e3b4e2705df81671287f2c83c4da81d8e
```

冲突处理原则：

1. 优先保留目标环境已有业务改动。
2. 只搬迁 Breakpad 信号 dump 所需代码。
3. 不引入 `src/tools`、`docs` 或其他无关改动。
4. 冲突解决后先运行格式化，再编译。

### 5.2 备选方式：format-patch

在源环境生成 patch：

```bash
git format-patch -1 cec9593e3b4e2705df81671287f2c83c4da81d8e
```

在目标环境应用：

```bash
git checkout -b breakpad-signal-minidump-validation
git am < 0001-Add-signal-triggered-Breakpad-minidump-support.patch
```

如有冲突：

```bash
git am --abort
git apply --3way 0001-Add-signal-triggered-Breakpad-minidump-support.patch
```

然后人工处理冲突并提交。

### 5.3 兜底方式：按文件手工搬迁

如果目标环境不能直接应用 patch，Claude Code 可按本设计文档的文件清单逐个对照迁移。

手工搬迁后必须执行：

```bash
rg -n "setupBreakpadSignalMinidump|enable_breakpad_signal_minidump|breakpad_test_enable_deadlock" src/daemons
```

确认 graphd、metad、storaged、standalone 都有正确接入。

## 6. 目标环境配置调整

当前已提交的测试 flagfile 使用了源环境绝对路径：

```text
/home/sch/nebula/nebula-release-3.6/task-breakpad/...
```

目标环境必须替换为目标 repo 路径，例如：

```text
<TARGET_REPO>/task-breakpad/runtime/...
```

建议 Claude Code 在目标环境新增或修改以下配置：

```text
task-breakpad/graphd_signal_deadlock.conf
task-breakpad/metad_signal_deadlock.conf
task-breakpad/storaged_signal_deadlock.conf
```

路径替换项：

```text
--pid_file
--log_dir
--data_path
--breakpad_signal_minidump_dir
```

端口要求：

- graphd 使用独立测试端口，避免和真实业务 graphd 冲突。
- metad 使用独立测试端口。
- storaged 指向测试 metad 地址。
- HTTP 端口也必须避开真实服务。

## 7. 编译设计

建议在目标环境使用独立构建目录：

```bash
mkdir -p build-breakpad
cd build-breakpad
cmake -DCMAKE_INSTALL_PREFIX=/usr/local/nebula \
      -DENABLE_TESTING=OFF \
      -DENABLE_BREAKPAD=ON \
      -DCMAKE_BUILD_TYPE=Debug \
      ..
make -j10
make install
```

要求：

1. 编译日志写入 `task-breakpad/logs/`。
2. 如果安装需要 sudo，记录 sudo 安装日志。
3. 如果 `ENABLE_BREAKPAD` 未打开，手动信号 dump 只会记录 warning，不会生成 dump。

建议记录：

```text
task-breakpad/logs/target_01_cmake.log
task-breakpad/logs/target_02_make.log
task-breakpad/logs/target_03_make_install.log
```

## 8. 端到端验证设计

### 8.1 默认关闭验证

目的：

确认未配置 `--enable_breakpad_signal_minidump=true` 时，原 daemon 能正常启动。

验证点：

1. daemon 正常启动。
2. 没有新增 `do_sigtimedwait` 信号等待线程。
3. 原有服务状态正常。

命令示例：

```bash
ps -T -p <pid> -o pid,tid,stat,wchan:32,comm
```

### 8.2 graphd 隔离死锁验证

启动前准备：

```bash
mkdir -p task-breakpad/logs task-breakpad/runtime
ulimit -n 65536
```

启动 graphd 测试实例：

```bash
cd /usr/local/nebula
/usr/local/nebula/bin/nebula-graphd --flagfile <TARGET_REPO>/task-breakpad/graphd_signal_deadlock.conf
```

检查：

```bash
pid=$(cat <graphd_pid_file>)
ps -T -p "$pid" -o pid,tid,stat,wchan:32,comm
```

期望看到：

```text
do_sigtimedwait
bp-deadlock-a  futex_wait_queue
bp-deadlock-b  futex_wait_queue
```

触发 dump：

```bash
kill -USR2 "$pid"
```

验收：

1. dump 目录出现 `.dmp` 文件。
2. PID 不变。
3. `bp-deadlock-a/b` 仍在 `futex_wait_queue`。
4. 进程未退出、未重启。

### 8.3 metad 隔离死锁验证

启动 metad 测试实例：

```bash
cd /usr/local/nebula
ulimit -n 65536
/usr/local/nebula/bin/nebula-metad --flagfile <TARGET_REPO>/task-breakpad/metad_signal_deadlock.conf
```

后续检查和触发方式同 graphd。

验收栈应包含：

```text
writeSignalMinidump [SetupBreakpad.cpp]
runSignalMinidumpThread [SetupBreakpad.cpp]
breakpadTestDeadlockThreadA [MetaDaemon.cpp]
breakpadTestDeadlockThreadB [MetaDaemon.cpp]
std::mutex::lock()
```

### 8.4 storaged 隔离死锁验证

启动 storaged 测试实例：

```bash
cd /usr/local/nebula
ulimit -n 65536
/usr/local/nebula/bin/nebula-storaged --flagfile <TARGET_REPO>/task-breakpad/storaged_signal_deadlock.conf
```

后续检查和触发方式同 graphd。

验收栈应包含：

```text
writeSignalMinidump [SetupBreakpad.cpp]
runSignalMinidumpThread [SetupBreakpad.cpp]
breakpadTestDeadlockThreadA [StorageDaemon.cpp]
breakpadTestDeadlockThreadB [StorageDaemon.cpp]
std::mutex::lock()
```

## 9. dump 解析设计

目标环境需要定位 `dump_syms` 和 `minidump_stackwalk`：

```bash
which dump_syms
which minidump_stackwalk
```

如果不在 `PATH` 中，按目标环境实际路径指定，例如：

```text
/opt/vesoft/third-party/3.3/bin/dump_syms
/opt/vesoft/third-party/3.3/bin/minidump_stackwalk
```

生成符号：

```bash
dump_syms /usr/local/nebula/bin/nebula-graphd > nebula-graphd.sym
```

读取模块信息：

```bash
awk '/^MODULE / {print $4, $5; exit}' nebula-graphd.sym
```

按 Breakpad 规范组织：

```text
symbols/<module_name>/<module_id>/<module_name>.sym
```

解析：

```bash
minidump_stackwalk <dump_file> <symbols_dir> > stackwalk.txt 2> stackwalk.stderr
```

验收字段：

```text
Crash reason: DUMP_REQUESTED
writeSignalMinidump [SetupBreakpad.cpp]
runSignalMinidumpThread [SetupBreakpad.cpp]
breakpadTestDeadlockThreadA
breakpadTestDeadlockThreadB
std::mutex::lock()
```

## 10. canary 业务验证设计

隔离测试通过后，才允许进入 canary 业务验证。

canary 配置只允许加入：

```text
--enable_breakpad_signal_minidump=true
--breakpad_signal_minidump_signal=12
--breakpad_signal_minidump_dir=<writable_dump_dir>
--breakpad_signal_minidump_min_interval_sec=60
```

严禁加入：

```text
--breakpad_test_enable_deadlock=true
--breakpad_test_deadlock_only=true
```

canary 验证步骤：

1. 重启或滚动替换一台 canary daemon。
2. 确认业务健康。
3. 记录 PID。
4. 执行一次：

```bash
kill -USR2 <pid>
```

5. 确认生成 dump。
6. 确认 PID 不变。
7. 确认服务健康检查、日志、QPS、延迟、错误率无异常。
8. 解析 dump，确认 `DUMP_REQUESTED` 和 `SetupBreakpad.cpp` 栈存在。

## 11. 回滚设计

### 11.1 配置级回滚

关闭：

```text
--enable_breakpad_signal_minidump=false
```

或删除该 flag 后重启 daemon。

### 11.2 二进制级回滚

恢复目标环境原二进制：

```bash
/usr/local/nebula/scripts/nebula.service stop <daemon>
cp <backup_binary> /usr/local/nebula/bin/<daemon_binary>
/usr/local/nebula/scripts/nebula.service start <daemon>
```

### 11.3 代码级回滚

如果目标环境使用 cherry-pick：

```bash
git revert <target_commit>
```

如果还未合入主分支，直接丢弃验证分支。

## 12. 验收标准

端到端完成必须满足：

1. 目标环境源码编译通过。
2. 安装后 graphd、metad、storaged 均可启动隔离测试实例。
3. 三个测试实例均能通过 `SIGUSR2` 生成 minidump。
4. 三个测试实例触发 dump 后 PID 不变。
5. 三个测试实例触发 dump 后死锁线程仍处于 `futex_wait_queue`。
6. 三个 dump 均可解析，且显示 `Crash reason: DUMP_REQUESTED`。
7. stackwalk 能定位到 `SetupBreakpad.cpp` 和对应 daemon 的测试死锁线程。
8. canary 业务实例只启用信号 dump，不启用测试死锁 fixture。
9. canary 触发一次 dump 后服务不重启，健康检查无异常。
10. 所有关键日志记录到 `task-breakpad/logs/`。

## 13. Claude Code 执行要求

在目标环境使用 Claude Code 时，建议使用以下约束：

1. 先读目标环境 `AGENTS.md` 或等效说明。
2. 每一步执行前说明要操作的文件和命令。
3. 不使用 `git reset --hard` 或 `git checkout --` 覆盖用户改动。
4. 发现无关工作区改动时，不纳入本次提交。
5. 所有关键命令输出写入 `task-breakpad/logs/`。
6. 只提交源码、设计文档、验证配置，不提交 dump、runtime、build 目录。
7. 如果目标环境是生产机器，先确认测试实例端口、pid、data、log、dump 目录不会和真实服务冲突。
8. 不在真实业务进程上启用测试死锁 fixture。

## 14. Claude Code 推荐提示词

可在目标环境给 Claude Code 使用以下提示词：

```text
请基于提交 cec9593e3b4e2705df81671287f2c83c4da81d8e 的内容，
将 Nebula Graph 信号触发 Breakpad minidump 方案搬迁到当前仓库。

要求：
1. 先阅读 task-breakpad/claude_code_migration_validation_design.md。
2. 只搬迁 Breakpad 信号 dump 相关代码和验证配置，不引入无关改动。
3. 构建时启用 ENABLE_BREAKPAD=ON。
4. 运行前执行 ulimit -n 65536。
5. 修改 task-breakpad/*_signal_deadlock.conf 中的绝对路径为当前环境路径。
6. 使用独立 pid/log/data/dump 目录和独立端口启动 graphd、metad、storaged 测试实例。
7. 只在隔离测试实例中启用 breakpad_test_enable_deadlock 和 breakpad_test_deadlock_only。
8. 对三个测试实例分别执行 kill -USR2 <pid>，验证 dump 生成、PID 不变、死锁线程仍在 futex_wait_queue。
9. 使用 dump_syms 和 minidump_stackwalk 解析 dump，确认 DUMP_REQUESTED、SetupBreakpad.cpp 和对应 daemon 死锁栈。
10. 全部关键日志写入 task-breakpad/logs/。
11. 不提交 build-breakpad、task-breakpad/runtime、task-breakpad/logs、minidump、symbols、stackwalk 运行产物。
12. 最后输出验证结论、dump 路径、stackwalk 路径、保留现场 PID 和清理命令。
```

## 15. 风险清单

| 风险 | 影响 | 控制措施 |
| --- | --- | --- |
| 在真实业务进程启用测试死锁 fixture | 业务进程被人为死锁 | 只允许隔离测试实例开启 |
| dump 目录不可写 | 无法生成 dump | 启动前检查目录权限 |
| 未启用 `ENABLE_BREAKPAD` | 不会生成 dump | 编译命令显式开启 |
| 信号选择冲突 | 影响停服或 crash 语义 | 使用 `SIGUSR2` |
| dump 写入造成 IO 压力 | 短时性能波动 | canary、低峰、限频 |
| 目标代码版本差异导致冲突 | 编译失败或行为不一致 | 冲突处按目标代码结构人工适配 |
| 提交运行产物 | 仓库污染、泄露敏感信息 | dump/log/runtime 不提交 |

## 16. 产出物要求

目标环境最终应产出：

```text
task-breakpad/logs/target_01_cmake.log
task-breakpad/logs/target_02_make.log
task-breakpad/logs/target_03_make_install.log
task-breakpad/logs/target_graphd_sigusr2_check.log
task-breakpad/logs/target_metad_sigusr2_check.log
task-breakpad/logs/target_storaged_sigusr2_check.log
task-breakpad/logs/target_three_daemon_state.log
task-breakpad/breakpad_signal_minidump_target_summary.md
```

其中 summary 需要包含：

1. 目标环境 commit。
2. 编译参数。
3. 三个测试进程 PID。
4. 三个 dump 路径。
5. 三个 stackwalk 路径。
6. `DUMP_REQUESTED` 证据。
7. PID 不变证据。
8. 死锁线程仍在 `futex_wait_queue` 证据。
9. canary 验证结果。
10. 清理命令和回滚方式。
