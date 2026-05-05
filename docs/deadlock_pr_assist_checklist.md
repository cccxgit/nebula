# Leader Balance 与 Create Space 并发死锁问题复现与 PR 补充清单

> 适用场景：Nebula Graph 3 节点部署，`partition_num=20`，`replica_factor=3`，并发执行 leader balance 与 create space，偶现 storage offline + worker 线程卡死。

## 1. 当前代码快速结论（基于提交 `60df94c7d3ed2500086a763487c91b79da9ae22a`）

该修复将 `NebulaStore::newEngine()` 从调用 `newEngineAsync(...).get()` 改为直接同步构建 `RocksEngine`，避免在持有 store 内部写锁时阻塞等待异步线程执行造成循环等待风险。

核心推断链路：

1. `addSpace()` 在持有 `NebulaStore::lock_` 写锁期间调用 `newEngine()`。
2. 旧实现中 `newEngine()` 内部 `newEngineAsync(...).get()` 会阻塞当前线程，等待 IO 线程池任务执行。
3. 并发 leader balance/create space 场景下，IO 线程任务路径可能再次触达需要分片/空间相关锁的逻辑，形成锁顺序反转或“持锁等待异步完成”的死锁模式。
4. 结果表现为部分线程长期阻塞，storage 心跳异常，`SHOW HOSTS` 可能观察到 offline。

## 2. 你补 PR 时建议补齐的信息

### 必填背景

- 触发版本：请明确 first-found 分支（你提到 `release-3.6`）与验证分支（`master`）。
- 部署拓扑：3 meta + 3 graph + 3 storage，K8s 容器化。
- 空间参数：`partition_num=20`, `replica_factor=3`。

### 复现步骤（建议写成 deterministic 脚本）

- 并发启动：
  - 线程 A：反复触发 leader balance（全库或指定 space）。
  - 线程 B：反复 `CREATE SPACE`。
- 观察点：
  - `SHOW HOSTS` 出现 storage offline；
  - storage 日志出现长期无进展；
  - gdb `thread apply all bt` 显示多线程卡在锁等待。

### 根因描述建议（英文 PR 可直接复用）

- “`newEngine()` was blocking on `newEngineAsync(...).get()` while upper-layer code still held `NebulaStore::lock_` write lock. Under concurrent leader balancing and space creation, this could create a lock inversion / wait cycle and eventually deadlock worker threads.”

### 修复点说明

- 移除 `newEngine()` 中对异步 future 的阻塞等待；
- 改为同步构建 `RocksEngine`，不再引入跨线程等待；
- 说明行为不变（仅构建路径同步化，返回对象与参数一致）。

### 风险与回归说明

- 风险：低；逻辑等价，主要是并发模型简化。
- 建议回归：
  - 并发压测（leader balance + create space）；
  - 常规建库建边/点读写；
  - 重启恢复后 leader 分布与心跳健康。

## 3. 你接下来需要提供给我的最小材料

1. 复现脚本（或关键命令序列）。
2. 一次“问题现场”的 storage 日志片段（卡住前后 2~5 分钟）。
3. 一份 gdb 栈（`thread apply all bt`）。
4. 你的 PR 链接中 reviewer 的具体 comment（若有）。

拿到这 4 项后，我可以继续产出：

- 可直接粘贴到社区 PR 的英文/中文 Root Cause + Fix + Test Plan；
- 面向 reviewer 的逐条回复草稿；
- 若需要，补一个最小化并发回归测试思路（含伪代码）。
