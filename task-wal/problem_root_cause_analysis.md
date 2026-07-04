# storage 内存缓慢上涨问题根因分析文档

## 1. 问题背景

当前任务分析的是 Nebula Graph 3.6 storage 进程长期运行后 RSS 缓慢上涨的问题。现场现象包括：

- 图空间数量越多，内存上涨越快。
- partition 数量越多，内存上涨越快。
- 即使很多图空间没有业务写入，storage 内存仍然持续上涨。
- 内存上涨一段时间后可能出现阶段性下降。

这些现象说明问题不完全由业务写入量触发，而是与每个 partition 都会周期性执行的后台路径有关。结合源码排查，最符合这些特征的路径是 Raft heartbeat 写 WAL 以及 WAL 内存 buffer 的释放机制。

## 2. 关键源码链路

### 2.1 每个 partition 都有独立 WAL buffer

源码链路：

1. `NebulaStore::newPart()` 为每个 partition 创建 `Part`。
2. `Part` 继承自 `RaftPart`。
3. `RaftPart` 构造时通过 `FileBasedWal::getWal(...)` 创建 WAL。
4. `FileBasedWal` 内部创建 `AtomicLogBuffer`。

因此，每个本地 partition 都持有一份独立的 WAL 内存 buffer。partition 数量越多，后台 heartbeat 和 WAL buffer 数量越多。

### 2.2 leader heartbeat 会进入 RaftPart::sendHeartbeat()

Raft leader 会周期性执行 `statusPolling()`，leader 分支会调用 `sendHeartbeat()`。默认 heartbeat 周期约为：

```text
FLAGS_raft_heartbeat_interval_secs * 1000 / 3 + random(500)
```

如果 `raft_heartbeat_interval_secs` 为 5 秒，则每个 leader partition 大约 1.6 到 2.1 秒执行一次 heartbeat。

### 2.3 修复前每轮 heartbeat 都可能追加空 WAL log

修复前，`RaftPart::sendHeartbeat()` 的关键逻辑是：

```cpp
if (!replicatingLogs_.load(std::memory_order_acquire)) {
  folly::via(executor_.get(), [this] {
    std::string log = "";
    appendLogAsync(clusterId_, LogType::NORMAL, std::move(log));
  });
}
```

该逻辑表示：只要当前没有正在复制日志，leader heartbeat 就会追加一个空 NORMAL log。

这个空 log 的原始目的，是让 leader 在当前任期提交一条日志，从而满足 Raft 的当前任期提交语义。但是修复前没有判断当前任期是否已经提交过日志，导致稳定 leader 任期内每轮 heartbeat 都可能持续追加空 log。

### 2.4 空 log 虽然不写业务数据，但已经进入 WAL 和内存 buffer

空 log 后续链路：

1. `sendHeartbeat()` 调用 `appendLogAsync(...)`。
2. `appendLogAsync(...)` 进入 Raft append 流程。
3. `appendLogsInternal()` 调用 `wal_->appendLogs(iter)`。
4. `FileBasedWal::appendLogInternal()` 写入磁盘 WAL，同时调用 `logBuffer_->push(...)` 写入 `AtomicLogBuffer`。
5. `Part::commitLogs()` 在 state machine 层看到空 log 后跳过业务写入。

关键点是：`Part::commitLogs()` 跳过空 log 时，空 log 已经写入 WAL 文件，也已经进入 `AtomicLogBuffer` 内存 buffer。

因此，“没有业务写入”不代表“没有 WAL 内存增长”。Raft heartbeat 自身就会产生空 WAL log。

## 3. 根因结论

根因是：修复前 Raft leader heartbeat 在稳定任期内仍持续追加空 NORMAL log，这些空 log 会进入 `FileBasedWal` 和 `AtomicLogBuffer`，导致 storage RSS 随 partition 数量和运行时间缓慢上涨。

这个根因解释了全部关键现象：

- 与图空间和 partition 数强相关：每个 leader partition 都会周期性产生空 WAL log。
- 空闲图空间也增长：触发条件是 Raft heartbeat，不依赖业务写入。
- 3 副本下增长更明显：leader 追加空 log 后会复制到 follower，follower 也会写 WAL 和 WAL buffer。
- 阶段性下降：`AtomicLogBuffer` 超过阈值后旧 node 被标记为 dirty，最终在 GC 时释放。

## 4. WAL buffer 为什么会表现为缓慢上涨后回落

`AtomicLogBuffer` 的释放不是每条 log 立即释放，而是分阶段完成：

1. 新 log 进入 active node，`size_` 增长。
2. buffer 超过容量或调用 reset 时，旧 node 被标记为 deleted。
3. 被标记 deleted 的 node 进入 dirty 状态。
4. 只有当 iterator 引用释放并满足 GC 条件时，dirty node 才会真正 delete。

因此 RSS 表现通常是：

- 空 log 持续进入 buffer，内存缓慢上涨。
- 部分 node 被标记 dirty，但如果 reader 引用未释放或 GC 尚未触发，RSS 不会马上下降。
- GC 真正执行后，RSS 才可能阶段性下降。

## 5. 根因定位证据

本次排查记录和验证日志保存在任务目录：

- 过程记录：`task-wal/memor_analysis_reading_log.md`
- 根因分析原始记录：`task-wal/memor_analysis_root_cause.md`
- 构建日志：`task-wal/build_20260704_raft_wal_fix.log`
- 指标验证日志：`task-wal/runtime_stats_20260704_raft_wal_fix.log`
- WAL 分片指标验证日志：`task-wal/runtime_wal_stats_20260704.log`

修复后验证观察到：

- `num_raft_heartbeat_empty_log.sum.60=0`
- `num_raft_heartbeat_without_empty_log.sum.60` 与 `num_raft_heartbeat.sum.60` 一致

这说明稳定 leader 任期内 heartbeat 已不再持续追加空 WAL log。

## 6. 风险判断

本次根因修复不完全取消空 heartbeat log，而是只在 leader 当前任期尚未提交日志时追加一次必要空 log。这样保留了 Raft 当前任期提交语义，同时去掉稳定任期内的重复空 WAL 写入。

需要重点关注的风险是：如果某些隐含逻辑依赖“每次 heartbeat 都推进 lastLogId”，该行为会被改变。但从当前源码看，lease 有效性依赖 `commitInThisTerm_` 和 heartbeat response 更新时间，稳定 leader 的 heartbeat RPC 仍然会发送，因此 leader lease 维护不应受影响。
