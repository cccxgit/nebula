# Nebula Graph 3.6 空闲图空间 WAL 缓存加速老化方案

> 本文回答：在不改变 Heartbeat、周期空日志、Raft 复制和提交机制的前提下，能否通过识别长期无数据写入的图空间，加速缓存老化，解决 storaged 内存持续增长。
> 本轮只完成设计与源码审查，没有修改产品源码。源码行号以当前实验工作树为准；`AtomicLogBuffer.*` 含此前只读观测插桩，产品主逻辑未改变。

## 1. 结论

这个方向**可行，而且比改 Heartbeat/空日志协议的风险低很多**，但需要准确描述它能解决的范围：

1. 它能解决本案最大的主增长项——长期空闲 replica-part 的 `AtomicLogBuffer` Node 持续累积。正确实现后，该项会从“长期按天线性增长”变成“达到一个较小、可计算的保留窗口后平台化”。
2. 它不停止空日志的产生、磁盘 WAL 写入、网络复制，也不停止空日志提交时对 RocksDB commit key 的更新。因此它是**主路径内存治理**，不是消除上游根因。
3. 它不能承诺进程 RSS 立即按 Node 释放量下降。Node live bytes 会下降或平台化，但 jemalloc 可能保留已释放 page；RocksDB memtable 仍会增长、flush。
4. 不建议实现成“整个 space 一次性清空缓存”。最小且更精确的控制粒度应是 **replica-part 自识别空闲，space 只做汇总展示**。一个 space 的全部 part 都空闲时，效果等价于识别出空闲 space；若只有一个 part 活跃，不应阻止另外 19 个 part 老化。

推荐的第一版是：

> 每个 `FileBasedWal` 记录最后一次非空 Raft payload 成功写入本地 WAL 的时间；现有 600 秒 WAL 清理任务发现该 part 已长期没有非空 Raft WAL 活动时，在 `committedLogId` 安全边界内，从 `AtomicLogBuffer` 最旧端按完整 Node、限量、渐进地标脏；物理删除继续复用现有 reader-ref GC。始终保留最新的固定 Node 窗口，不修改磁盘 WAL。

不推荐第一版同时做 RocksDB 主动 flush、动态缩小 write buffer、整 buffer swap 或修改 Raft peer 状态机。它们应与 WAL 内存老化分开评估。

## 2. 为什么这个方向不改变 Raft 正确性

### 2.1 AtomicLogBuffer 是读缓存，不是日志权威副本

[`FileBasedWal::appendLogInternal()`](../src/kvstore/wal/FileBasedWal.cpp#L442-L500) 的顺序是：

1. 先编码并 `write()` 到本地 WAL 文件：`457-480`；
2. 更新 WAL 文件及首尾 LogID 元数据：`489-497`；
3. 最后才执行 `logBuffer_->push(...)`：`499`。

所以能够进入 `AtomicLogBuffer` 的记录，已经先写入本地 WAL 文件。默认 `wal_sync=false` 时不保证每条记录立即 `fsync` 到介质，但内存 cache 本来也不能提供进程崩溃后的持久性；淘汰 cache 不改变现有 durability 语义。

读取时，[`FileBasedWal::iterator()`](../src/kvstore/wal/FileBasedWal.cpp#L530-L536) 先尝试内存 iterator；若起始 LogID 不在内存，自动切换到 `WalFileIterator` 从磁盘读取。Leader 给落后 peer 组装 AppendLog 请求也使用这个统一 iterator：[`Host::prepareAppendLogRequest()`](../src/kvstore/raftex/Host.cpp#L287-L345)。

因此，只缩短内存缓存窗口而不删除磁盘 WAL，不改变：

- LogID、term、payload；
- Leader/Follower 复制协议；
- 多数派确认；
- committedLogId；
- 状态机 apply 结果；
- WAL 文件格式和 RocksDB 格式。

它改变的是“读取旧日志时先命中内存，还是回退到磁盘”的性能路径。

### 2.2 必须只从最旧端删除连续前缀

一个 [`Node`](../src/kvstore/wal/AtomicLogBuffer.h#L61-L128) 固定内嵌 64 个 `Record`。Atomic iterator 假定从 `firstLogId_` 开始的记录连续，且 [`FileBasedWal::iterator()`](../src/kvstore/wal/FileBasedWal.cpp#L530-L536) 是“起点在内存则整段走内存，否则整段走磁盘”。

所以不能：

- 在写入时跳过空日志的 `push`；
- 从链表中间单独删除空日志；
- 删除中间的空 Node，保留两侧 Node。

这些做法会制造 LogID 洞。正确操作只能从 `tail_` 指向的最旧有效 Node 开始，删除连续旧前缀，保留连续的最新 suffix。

### 2.3 空闲只决定“何时开始”，committed 水位决定“最多删到哪里”

“payload 为空”只是性能分类信号，不是正确性边界。真正的淘汰条件应是：

```text
该完整 Node 的最后一个 LogID <= 本地 committedLogId
```

Leader 只有在状态机 apply 成功后才推进 `committedLogId_`：[`RaftPart.cpp:1068-1083`](../src/kvstore/raftex/RaftPart.cpp#L1068-L1083)。Follower 同样只在 `commitLogs()` 成功后推进：[`RaftPart.cpp:1793-1804`](../src/kvstore/raftex/RaftPart.cpp#L1793-L1804)。

这个限制可避免：

- `E_WRITE_STALLED` 时淘汰尚未 apply 的日志；
- 把最新未提交 tail 当成普通冷缓存处理；
- rollback/reset 与主动老化之间扩大状态边界。

注意：当前 [`Node::lastLogId()`](../src/kvstore/wal/AtomicLogBuffer.h#L104-L111) 返回的是 `firstLogId + pos`，语义实际上是 exclusive end。判断完整 Node 的 inclusive last 时必须使用：

```text
firstLogId + pos - 1
```

不能直接拿现有 `lastLogId()` 与 committedLogId 做 `<=` 比较，否则会产生一条边界偏差。

## 3. 如何自识别“长期无数据写入”

### 3.1 不要观察 lastLogId 是否变化

空闲 Leader 仍由 [`RaftPart::statusPolling()`](../src/kvstore/raftex/RaftPart.cpp#L1401-L1434) 周期调用 `sendHeartbeat()`；复制空闲时，[`sendHeartbeat()`](../src/kvstore/raftex/RaftPart.cpp#L2041-L2049) 会追加空 `NORMAL` 日志。因此：

- lastLogId 一直变化；
- WAL 一直有 push；
- commit key 一直更新；
- “多久没有任何 Raft 日志”永远不能表示业务空闲。

### 3.2 最小、保守的活动判据

推荐在 [`FileBasedWal::appendLogInternal()`](../src/kvstore/wal/FileBasedWal.cpp#L442-L500) 成功写入后记录：

```text
lastNonEmptyLogTime = 当前 steady time，条件是 msg 非空
```

原因：

- 周期 no-op 的 `msg` 为空；
- [`Part::commitLogs()`](../src/kvstore/Part.cpp#L215-L358) 也明确在 `log.empty()` 时将其作为 heartbeat 跳过业务 payload；
- 常规 KV 写、删、批处理都由 [`LogEncoder.cpp:40-191`](../src/kvstore/LogEncoder.cpp#L40-L191) 编码时间戳、操作类型和数据，payload 非空；
- Leader 和 Follower 都经过 `FileBasedWal::appendLogInternal()`，三副本能各自独立得到同一类活动信号；
- membership、transfer leader 等非空管理日志也会重置空闲时间。这是安全的“误判为活跃”，只会少回收内存，不会把真实活动误判为空闲。

首次构造、rollback/reset 或 snapshot 重建后，应把时间初始化为“当前时间”，给予完整冷却期，不能使用 0 导致立即进入冷态。

这个字段应使用 atomic monotonic timestamp，或保证写入和 600 秒清理读取始终由同一把 `raftLock_` 串行；不能用普通整数在不同线程无同步读写。推荐使用单调时钟，避免系统时间校准使 idle 判断跳变。

还必须准确限定它的语义：`msg != empty` 证明的是“近期有非空 Raft WAL 活动”，不是完整的“近期有/无任何数据写入”。以下路径可以直接改变 RocksDB 数据而绕过普通 Raft WAL append：

- snapshot apply：[`Part::commitSnapshot()`，Part.cpp:366-400](../src/kvstore/Part.cpp#L366-L400)；
- SST ingest：[`NebulaStore.cpp:972-1012`](../src/kvstore/NebulaStore.cpp#L972-L1012)；
- ingest 管理任务：[`IngestTask.cpp:17-46`](../src/storage/admin/IngestTask.cpp#L17-L46)；
- restore ingest：[`NebulaStore.cpp:1368-1384`](../src/kvstore/NebulaStore.cpp#L1368-L1384)。

对“只淘汰已提交旧 WAL 内存 cache”而言，漏记这些数据路径不会造成一致性错误：它们不需要把旧 Atomic Node 保持为热缓存。接收 snapshot 已由现有状态检查跳过；snapshot/ingest/admin 的额外 busy gate 可作为后续可选的性能保护和运维观测，不列为第一版正确性前置。界面应显示 `COLD_WAL_CACHE` 或“长期无非空 Raft WAL 活动”，不能把它直接命名为整个业务 space 的 `IDLE`。

### 3.3 为什么推荐 per-part，而不是一个 space 共享布尔值

主内存对象的所有权粒度是 replica-part：

- 每个 Part 创建一个 `FileBasedWal`：[`RaftPart.cpp:330-370`](../src/kvstore/raftex/RaftPart.cpp#L330-L370)；
- 每个 `FileBasedWal` 创建一个 `AtomicLogBuffer`：[`FileBasedWal.cpp:40-60`](../src/kvstore/wal/FileBasedWal.cpp#L40-L60)。

所以最佳控制粒度也是 part：

```text
space 10:
  part 1 有写入       -> 保持热缓存
  part 2..20 无写入   -> 各自进入冷缓存
```

若按 space 聚合，只要一个 part 活跃，就会让其余 19 个空闲 buffer 继续增长。per-part 不会降低正确性，而且代码改动更小。对运维界面可以按 space 汇总：当该节点上某 space 的所有 replica-part 都冷时，显示为 `COLD_WAL_CACHE`，而不是宣称整个 space 没有任何形式的数据活动。

## 4. 推荐的缓存老化实现

### 4.1 接入现有 600 秒清理点

当前 `NebulaStore` 已每 600 秒执行一次 WAL 清理：

- 默认周期：[`NebulaStore.cpp:24`](../src/kvstore/NebulaStore.cpp#L24)；
- 首次调度：[`NebulaStore.cpp:72`](../src/kvstore/NebulaStore.cpp#L72)；
- 遍历 space/part：[`NebulaStore::cleanWAL()`，NebulaStore.cpp:1293-1321](../src/kvstore/NebulaStore.cpp#L1293-L1321)。

普通 Part 最终进入 [`RaftPart::cleanWal()`](../src/kvstore/raftex/RaftPart.cpp#L492-L495)，该函数持有 `raftLock_` 并能读取一致的 `committedLogId_`。[`needToCleanWal()`](../src/kvstore/raftex/RaftPart.cpp#L1452-L1462) 当前只跳过 STARTING、WAITING_SNAPSHOT 和观测到正在发送 snapshot 的状态；它不是完整的 trim 安全闸门，也不能单独证明 peer 已追平或没有在途请求。

`needToCleanWal()` 与随后 `cleanWal()` 之间还存在 TOCTOU，且现有 `sendingSnapshot_` 读取没有取得 Host 自身锁。因此 `cleanWal()` 内必须重新核对 status、activity generation 和 admin/snapshot busy 状态。若方案接受在途 iterator 由 ref 协议保护、落后 peer 转磁盘或 snapshot，则应把它明确归类为性能风险，不能宣称已经被 `needToCleanWal()` 排除。

trim 是 Atomic 链表的第二种 writer 操作，必须在这个 `raftLock_` 临界区内与 WAL append、rollback/reset 严格串行。600 秒任务只负责进入这条既有 WAL-writer 临界区，不能从另一个后台线程直接无锁修改 `AtomicLogBuffer` 链表；该类的设计前提本来就是 single writer / multi reader。接口注释、DCHECK 和并发测试都要固定这一前置条件，不能把它只当作当前调用方的偶然实现细节。

建议在这个调用链增加独立的内存 trim：

```text
RaftPart::cleanWal()
  1. 继续执行原磁盘 cleanWAL(committedLogId_)
  2. 若该 part 已超过 idle timeout
  3. 调用 AtomicLogBuffer::trimCommitted(
         committedLogId_,
         keepNewestNodes,
         maxNodesPerCall)
```

不要把 trim 简单塞到 `FileBasedWal::cleanWAL(id)` 函数末尾，因为磁盘清理在 WAL 文件为空或少于两个时会提前返回：[`FileBasedWal.cpp:676-706`](../src/kvstore/wal/FileBasedWal.cpp#L676-L706)。内存老化不应被磁盘文件数量意外短路。

### 4.2 trim API 的不变量

建议接口语义：

```cpp
trimCommitted(committedId, keepNewestNodes, maxNodesPerCall)
```

必须满足：

1. 只从最旧 `tail_` 向新方向移动；
2. 只处理完整 Node；
3. Node inclusive last 必须 `<= committedId`；
4. 永不标记当前 `head_`；
5. 至少保留 1 个 Node，生产建议保留远多于 1 个；
6. 单个 part 单次最多标记 `maxNodesPerCall`；进程级还必须有总 Node 预算、遍历游标和随机抖动，避免上千 part 同一轮集中释放；
7. reset 后 `firstLogId_ == 0` 或 head/tail 已标脏时直接 no-op；
8. 重复调用必须幂等，CAS 失败不得重复减 `size_` 或增加 `dirtyNodes_`。

`keepNewestNodes` 需要依据“当前有效、未标脏 Node 数”判断。此前观测补丁中的 `nodes_` 统计的是尚未物理 delete 的全部 live Node，包含 dirty Node，不能直接拿来作为保留窗口。产品实现应单独维护 valid-node 计数（新 head 时加一、tail 成功标脏时减一、reset 时清零），或在 trim 中受限遍历有效区间；前者更适合 1000 parts 的周期任务。

每次成功标脏必须严格复用当前 [`AtomicLogBuffer::push()`](../src/kvstore/wal/AtomicLogBuffer.cpp#L144-L167) 的顺序：

```text
读取 tail.prev
  -> firstLogId = prev.firstLogId
  -> release-store tail = prev
  -> size 减旧 tail.size
  -> dirtyNodes 加 1
```

最好抽成一个共享的 `markOldestNodeDeleted()`，避免 push 淘汰和 idle trim 维护两套略有差异的链表操作。

但批量 trim 不能只是连续调用这个 helper。`tail_.store(prev)` 发布后，最后一个旧 reader 可能在 trim 尚未完成访问旧 tail 时进入 [`releaseRef()`](../src/kvstore/wal/AtomicLogBuffer.cpp#L220-L268) 并删除 dirty 链，造成 UAF 或 dirty 计数下溢。最小协调方式是在**整个批次**开始前增加一个 synthetic buffer ref，以 RAII guard 持有；完成最后一次旧 Node 访问和所有计数更新后再 release。这样：

- 批次中间的真实 reader 析构不会成为“最后一个旧 reader”并启动 GC；
- 批次结束释放 synthetic ref 时，可以安全触发现有 GC；
- 若期间有新 reader，最终仍由最后一个真实 reader 触发；
- 不需要直接 delete，也不改变 reader epoch 语义。

必须增加确定性交错测试：开始时已经有 6 个 dirty Node，最后一个真实 reader 在 trim 批次中间 release；测试不得出现旧 tail UAF、dirty 下溢或重复 delete。

### 4.3 只标脏，不直接 delete

现有 Atomic buffer 是 single writer / multi reader。Iterator 构造时增加引用，析构时调用 [`releaseRef()`](../src/kvstore/wal/AtomicLogBuffer.cpp#L220-L268)。旧 reader 可以继续访问 trim 前已经看到的 Node；新 reader 看到更新后的 tail，旧 LogID 内存 miss 后走磁盘。

因此 trim 只能：

- 标记 `markDeleted_`；
- 推进 tail/firstLogId；
- 更新 size/dirty 计数。

不能：

- 看到某一瞬间 `refs_ == 0` 就直接 delete；
- 修改 dirty Node 的 next/prev 链；
- 并发调用当前 `reset()`；
- 调用 `FileBasedWal::reset()`，因为后者还会删除全部磁盘 WAL：[`FileBasedWal.cpp:622-L638`](../src/kvstore/wal/FileBasedWal.cpp#L622-L638)。

物理删除继续由既有 GC 完成。当前 GC 只有在旧 reader 离开，且 `dirtyNodes > 5` 或逻辑 size 超过 16MiB 时执行：[`AtomicLogBuffer.cpp:220-268`](../src/kvstore/wal/AtomicLogBuffer.cpp#L220-L268)。空日志会持续产生 iterator，所以标脏后通常会在后续复制/提交 iterator 释放时回收；长期 reader 仍可能延迟 GC，这是既有语义。

### 4.4 不推荐在线修改 capacity_

当前 `capacity_` 是普通 `int32_t`：[`AtomicLogBuffer.h:380-388`](../src/kvstore/wal/AtomicLogBuffer.h#L380-L388)。`wal_buffer_size` 只在 Part 构造时复制进 `FileBasedWalPolicy`：[`RaftPart.cpp:357-365`](../src/kvstore/raftex/RaftPart.cpp#L357-L365)。在线改 gflag 对既有 buffer 无效。

即使新增 setter，也存在这些问题：

- 与 push/观测线程并发访问时需新增同步；
- 当前每次 push 最多标记一个旧 Node；
- 创建新 head 的分支在容量检查前提前 return：[`AtomicLogBuffer.cpp:124-143`](../src/kvstore/wal/AtomicLogBuffer.cpp#L124-L143)；
- capacity 不知道 committed 水位；
- 业务恢复后，低 capacity 会继续作用，不能自然恢复热窗口；
- 空记录逻辑只计 16B，无法直接表达物理 Node 预算。

显式的 `trimCommitted()` 更容易证明：它是一次有 committed 上界的旧缓存老化，原来的热容量和写入行为完全保留。

## 5. 建议参数与本案定量结果

建议增加四个参数；`timeout=0` 时整项功能默认关闭：

```text
idle_wal_cache_timeout_secs = 0       # 0 表示关闭
idle_wal_cache_keep_nodes = 256       # 每 replica-part 保留最新 256 个有效 Node
idle_wal_cache_trim_nodes_per_run = 64
idle_wal_cache_max_total_trim_nodes_per_run = 4096
```

参数命名可在实现时调整。第一版推荐按 Node 数配置，而不是按逻辑 byte 配置，因为空日志的逻辑计费与物理对象差异很大：

- [`Record::size()`](../src/kvstore/wal/AtomicLogBuffer.h#L40-L56) 对空 payload 只计 16B；
- 一个 Node 固定包含 64 个 Record：[`AtomicLogBuffer.h:61-128`](../src/kvstore/wal/AtomicLogBuffer.h#L61-L128)；
- 本次 Debug ABI 实测 `sizeof(Node)=3200B`，jemalloc usable 为 3584B；该数值依赖现网编译器 ABI 与 allocator。

按发布配置 `raft_heartbeat_interval_secs=30`，忽略执行/排队开销的名义调度均值约为 10.2495 秒。对本次每台 1000 replica-parts 的拓扑：

| 保留 Node/part | 可容纳空日志 | 名义内存时间窗 | 本机 jemalloc usable，1000 parts |
|---:|---:|---:|---:|
| 64 | 4,096 | 约 11.7 小时 | 约 218.8MiB |
| 128 | 8,192 | 约 23.3 小时 | 约 437.5MiB |
| 256 | 16,384 | 约 46.6 小时 | 约 875MiB |

当前 8MiB 逻辑容量对空日志约等于 8,192 个满 Node；本机 jemalloc 下，1000 parts 的理论平台约 27.3GiB。**仅当这 1000 个 parts 全部进入 cold 状态**时，改为 256 Node 冷窗口才会把该主缓存平台量级降约 96.9%，同时仍保留接近两天的空日志内存窗口。

更一般地，只有 `N_cold` 个 part 受冷窗口约束。任何在 idle timeout 内持续出现非空日志的 part 都保留原有 8MiB 逻辑容量和原增长过程；本方案不会压缩它。实际平台必须按 cold/hot 两组分别计算，不能把 875MiB 无条件外推到整台 storaged。

这里有三个重要边界：

1. `keep_nodes` 决定主缓存平台，不决定进入冷态前的增长斜率；冷却期内空日志仍按原速产生。
2. `trim_nodes_per_run=64` 只是 per-part 上限，不足以控制进程峰值：1000 parts 同轮最多可标脏 64,000 Nodes，本机 usable 约 218.8MiB。还要用全局 4096 Node 预算、round-robin 跨轮游标和 per-part 抖动。单个 part 若每轮都能拿满 64 Node，从接近 8MiB 老缓存缩到 256 Node 约需 21 小时；但 1000 parts 都接近上限时，4096 的全局预算完成全量最坏收缩名义上约需 13.5 天。滚动部署会重启进程，初始 buffer 为空，通常不会经历这个最坏过程。
3. 256 不是普适最优值。`keep_nodes` 只决定内存命中窗口，应按可接受的磁盘回退 IO/延迟选择；它不能延长磁盘 WAL 已清理后的可恢复期限。最大离线恢复 SLO 由磁盘 WAL 文件保留、TTL/rollover 和 snapshot 能力决定，不能为追求“72 小时可恢复”而只增大 Atomic cache。

## 6. 它不能消除的 RocksDB 次路径

空日志在 [`Part::commitLogs()`](../src/kvstore/Part.cpp#L215-L358) 中虽然于 `229-233` 跳过业务 KV 操作，但最终仍执行：

- `putCommitMsg()`：`349-358`；
- 写每个 Part 各自固定的 `systemCommitKey(partId)`：[`Part.cpp:403-410`](../src/kvstore/Part.cpp#L403-L410)、[`NebulaKeyUtils.cpp:101-108`](../src/common/utils/NebulaKeyUtils.cpp#L101-L108)；
- `db_->Write()`：[`RocksEngine.cpp:120-140`](../src/kvstore/RocksEngine.cpp#L120-L140)。

而 RocksEngine 的粒度是每个 `space × data_path`：[`NebulaStore.cpp:354-370`](../src/kvstore/NebulaStore.cpp#L354-L370)、[`395-421`](../src/kvstore/NebulaStore.cpp#L395-L421)。本实验每台 50 个 space、一个 data path，即 50 个 RocksDB 实例。

所以 WAL cache 老化后仍会出现：

- active memtable entries/bytes 增长；
- memtable 达阈值后的 flush；
- allocator/page cache 导致 RSS 不完全回落；
- 空日志对应的磁盘 WAL、网络、CPU 消耗。

短跑证据已证明 active-memtable property 随空日志 commit 增长；当前实验未完整覆盖每个 DB 的长期 flush 后 RSS 形态。因而验收标准不能写成“RSS 必须完全水平且立即下降”，而应首先观察：

```text
进入 cold 的 Atomic live nodes/live bytes 在冷窗口附近平台化
这些 part 的 live bytes 净增长斜率接近 0，但 Node 分配/释放 churn 仍持续
RocksDB active memtable 仍按独立路径变化
在 H=30、1000 parts 全 cold、且 ABI/allocator 与本实验一致时，移除条件估算约 450MiB/日的 Atomic live-usable 净增长分量；RSS 只作次级观测
```

### 为什么第一版不建议对 idle space 主动 flush

对空闲 space 定期 `engine->flush()` 可以压低 active memtable，但会把原本留在内存中的大量“每个 Part 固定 commit key 的新 sequence 版本”更频繁变成小 SST，增加：

- 磁盘写放大；
- compaction；
- IO 抖动；
- 文件数量和后台任务；
- 多 space 同时 flush 的峰值。

它解决的是次路径，且把内存成本转换成 IO/compaction 成本。为最小化回归风险，不应和 Atomic 老化在第一批补丁同时上线。只有在主路径平台化后，若 RocksDB 残余仍超预算，才单独设计“按 active memtable bytes 阈值、最短间隔、全局并发限制”的 idle flush 实验。

这里说的是“不新增 idle-specific flush”。若生产设置 `rocksdb_disable_wal=true`，当前 [`NebulaStore::cleanWAL()`](../src/kvstore/NebulaStore.cpp#L1293-L1303) 已在每个清理周期对所有 space 的全部 engine 执行 `flush()`；这种配置下，RocksDB 残余会表现为更频繁的 memtable/SST/IO 循环，验收必须把该既有行为单独记录。

## 7. 仍需接受的风险

### 7.1 磁盘读取与 snapshot 风险

cache miss 会尝试转磁盘 WAL；如果请求起点早于 `FileBasedWal::firstLogId()`，Leader 会在 [`Host.cpp:306-309`](../src/kvstore/raftex/Host.cpp#L306-L309) 转 snapshot。这不会改变 Raft safety，但可能增加追赶延迟和 IO。若文件元数据声称范围存在、实际 iterator 却因损坏/短读而无效，[`Host.cpp:323-345`](../src/kvstore/raftex/Host.cpp#L323-L345) 可能返回 `E_RAFT_NO_WAL_FOUND`，不能把所有 miss 都无条件描述为自动 snapshot。

本文伪代码先执行原磁盘 `cleanWAL(committedId)` 再做内存 trim；同一轮可能同时删除旧磁盘文件并淘汰对应内存副本。实现必须在 trim 决策中记录候选范围是否仍有磁盘覆盖，并明确选择：

- 接受无增量历史时转 snapshot，同时确认该 Part 有可用 snapshot provider，并监控 snapshot 和持续 `E_RAFT_NO_WAL_FOUND`；或
- 若目标是“不增加 snapshot”，还要按 peer/learner/listener 的 match/apply 水位限制 trim，不能只看本地 committedId。

本设计选择第一种作为一致性语义，承认性能路径可能变化；生产 canary 用 snapshot/IO/p99 闸门决定该窗口是否可接受。

普通磁盘 WAL 清理只删除：

- 末尾 LogID 小于 committed 水位的旧文件；
- 超过 TTL 的文件；
- 并且至少保留最后两个 WAL 文件。

源码见 [`FileBasedWal::cleanWAL(LogID)`](../src/kvstore/wal/FileBasedWal.cpp#L676-L706)。空闲空间的 32B 空记录需要很久才滚满 16MiB 文件，因此最后两个文件通常能覆盖远大于 `wal_ttl=4h` 的日志范围，但这不是所有业务负载下的硬保证。

### 7.2 现有磁盘 iterator/rollback 并发边界

动态缩短内存窗口会提高 `WalFileIterator` 的使用频率。当前 [`rollbackToLog()`](../src/kvstore/wal/FileBasedWal.cpp#L569-L619) 持 `rollbackLock_` 写锁，但 [`WalFileIterator`](../src/kvstore/wal/WalFileIterator.cpp#L15-L105) 生命周期没有持对应读锁。磁盘 iterator 与并发 rollback/ftruncate 的窗口是既有薄弱点；过去较高内存命中率可能降低了暴露概率。

这不能留到混沌测试触发后再决定。主动提高磁盘 iterator 使用率以前，必须先完成 disk iterator 与 rollback/ftruncate 的并发闭环。简单给 iterator 增加生命周期读锁也不够：[`ESListener::processLogs()`](../src/kvstore/listener/elasticsearch/ESListener.cpp#L232-L329) 持有 iterator 后又取得 `raftLock_`，而 snapshot/rollback 可按 `raftLock_ -> WAL write lock` 的顺序执行，直接增加 read holder 可能形成反向锁序死锁。

发布前必须选择并验证一种完整方案，例如统一锁序并确保进入 `raftLock_` 前释放 disk iterator，或采用不会原地 ftruncate 已被 reader 打开的版本化/不可变文件方案。第一版可以不对 Listener 做 idle trim，但只要给所有 `WalFileIterator` 增加读锁，就仍需审计 Listener 锁序。这个并发闭环是启用特性的前置条件，不是可选优化。

### 7.3 Node 释放不等于 RSS 同步归还

现有 GC 删除 Node 后，jemalloc 可能把 page 留在 arena。验收必须同时看：

- live Node 数；
- jemalloc allocated/active/resident；
- RSS/anonymous RSS；
- WAL 磁盘读次数和延迟；
- snapshot 数；
- RocksDB memtable properties。

只看 RSS 可能把“对象已经释放、allocator 尚未归还页”误判为方案无效。

## 8. 最小代码改动面

第一版产品逻辑预计涉及：

1. `src/kvstore/wal/AtomicLogBuffer.h/.cpp`
   - 新增 committed-aware、整 Node、限量 trim；
   - 抽取共享 tail 标脏 helper；
   - 用 synthetic ref/RAII 阻止 trim 批次中途 GC；
   - 增加 valid Node、trim/保留 Node 指标。
2. `src/kvstore/wal/FileBasedWal.h/.cpp`
   - 记录最后一次非空 payload 成功写入时间；
   - 暴露 idle 判断与 memory trim 转发；
   - reset/rollback 后重新进入冷却期。
3. `src/kvstore/raftex/RaftPart.cpp`
   - 在现有 `cleanWal()` 的 `raftLock_` 临界区，以本地 committed 水位调用 trim。
4. `src/kvstore/NebulaStore.*`
   - 为每轮 trim 提供进程级总预算、跨轮游标和抖动，避免 1000 parts 集中处理。
5. `src/kvstore/wal/WalFileIterator.*` 及相关锁序调用点
   - 在启用前闭环 disk iterator 与 rollback/ftruncate 竞态；
   - 若使用生命周期读锁，先消除 Listener 等路径的反向锁序。
6. 配置模板及单元/集成测试。

不需要修改：

- `sendHeartbeat()`；
- Heartbeat/AppendLog Thrift；
- Host 请求合并；
- follower commit 逻辑；
- WAL 磁盘格式；
- RocksDB schema；
- Meta/Graph 服务；
- 线上 space 元数据。

如果包含 Listener，其 [`Listener::cleanWal()`](../src/kvstore/listener/Listener.cpp#L125-L128) 也必须以 `lastApplyLogId_` 为安全水位做对应处理，不能套用普通 Part 的 committed 水位。

## 9. 必须完成的测试

### 9.1 AtomicLogBuffer 单元测试

- 3 个以上 Node，committed 边界分别落在第 63/64/65 条；
- Node 横跨 committed 边界时不删除；
- 始终保留 head 和配置的最新 Node 数；
- 多次 trim 幂等，`size/firstLogId/dirtyNodes` 不双减；
- 旧 iterator 先创建，trim 后仍能读完；新 iterator 对旧 LogID miss；旧 reader 销毁后安全 GC；
- dirty 已为 6、最后 reader 在批量 trim 中途 release 的确定性交错测试；
- reset 后 trim、trim 后 reset、rollback 后新 term 重写；
- single writer + multi reader + 周期 trimmer 的 ASan/TSan 长跑。

### 9.2 FileBasedWal 测试

- 非空 payload 重置 idle 时间，空 payload 不重置；
- constructor/reset/rollback 后不会立即进入冷态；
- Atomic miss 后磁盘 iterator 返回相同的 LogID/term/source/msg；
- trim 绝不改变 FileBasedWal 的 first/last LogID 和磁盘文件；
- disk iterator 与 rollback/ftruncate 并发测试。

### 9.3 三副本集成测试

- `heartbeat=1` 的空载加速实验中，WAL LogID、push、empty_push 继续按原速增加，但 live nodes 在冷窗口附近平台；
- 冷态后恢复业务写，三副本读写、term、commit、leader lease 正常；
- leader transfer；
- follower 短时掉线后从 WAL 追赶；
- 超过内存/磁盘窗口后通过 snapshot 恢复；
- follower `E_WRITE_STALLED` 时不淘汰未提交 Node；
- backup、balance、rebuild、snapshot 与 trim 并发；
- 1000 replica-parts 同时进入冷态时无明显 CPU/IO/allocator 峰值。

## 10. 发布顺序与验收门槛

1. 所有新参数默认关闭。先在**不向配置文件加入新 flag**的情况下，把含新功能但默认关闭的新二进制滚动部署到全部目标 storaged；确认全量版本一致后，才给 canary 配置增加新参数。旧二进制不认识新 flag，回滚旧二进制前必须从配置中删除这些 flag，仅把值写成 `0` 仍可能因 unknown flag 启动失败。
2. 选择一台 storaged canary，建议先用：

   ```text
   idle timeout = 24h
   keep nodes = 256
   trim nodes/run = 64
   max total trim nodes/run = 4096
   ```

3. 保持 Heartbeat、`wal_buffer_size`、`wal_ttl`、RocksDB 参数全部不变，避免多变量实验。
4. 至少观察一个完整冷却期和 48 小时冷态；重点验证 Node 平台、磁盘 iterator、snapshot、p99 和 IO，而不是只看重启后 RSS 下降。
5. 三台 storaged 逐台启用；不要让多台同时重启或同时进入激进 trim。
6. 若 snapshot 数、持续 `E_RAFT_NO_WAL_FOUND`、WAL 磁盘读延迟、业务 p99 或 IO 明显劣化，停止后续节点。若这些 flag 未接入经过验证的动态配置，关闭特性需要改配置并逐台重启，不能假设在线写 `0` 立即生效。已经淘汰的内存记录不会因关开关自动回填，后续由磁盘 WAL/snapshot 路径恢复；不能通过删除 data/WAL 回滚。

## 11. 最终判断

### 能否解决当前问题

按本案已验证的根因，答案是：

- **能使进入 cold 状态的 replica-parts 的 AtomicLogBuffer live 内存从线性增长转为小窗口平台；**
- **不能彻底停止所有由空日志引起的内存、IO 和 CPU 活动；**
- **不会让 RSS 必然立即下降，但应使这些 cold parts 的 Atomic live bytes 从线性增长转为小窗口平台。**

### 是否优于修改 Heartbeat/空日志机制

如果当前优先级是“最小化一致性风险”，这个方案更优：它不改变 Raft wire、日志生成、commit 传播，以及 follower catch-up/snapshot 的协议与一致性语义。但它会把一部分追赶从内存改成磁盘，甚至提高 snapshot 概率，因此追赶延迟、IO 和可用性表现会变化。其主要新增风险是性能退化，而不是提交错误或副本状态机分歧；前提是前述 trim/GC 与 disk iterator/rollback 并发闭环已经完成。

### 推荐选择

第一版采用：

```text
per-part 非空日志活动识别
  + committed 水位保护
  + 整 Node 连续旧前缀老化
  + 保留固定最新窗口
  + 600 秒周期、单次限量
  + 默认关闭、canary 启用
```

不要第一版同时做 RocksDB 主动 flush。先验证 Atomic 主斜率被切断，再决定是否值得用额外 IO/compaction 换取 RocksDB 次路径的更低内存。

## 12. 一个完整例子：space `shop` 如何从约 560MiB 降到约 17.5MiB

### 12.1 拓扑

假设 `shop` 的配置是：

```text
PARTITION_NUM = 20
replica_factor = 3
storaged = S1、S2、S3
```

三个 storaged 都保存 20 个 replica-parts，因此每台有：

```text
shop-part-1  -> 一个 FileBasedWal -> 一个 AtomicLogBuffer
...
shop-part-20 -> 一个 FileBasedWal -> 一个 AtomicLogBuffer
```

创建关系见 [`RaftPart.cpp:330-370`](../src/kvstore/raftex/RaftPart.cpp#L330-L370) 和 [`FileBasedWal.cpp:40-60`](../src/kvstore/wal/FileBasedWal.cpp#L40-L60)。本方案不会删除 `shop` 的点、边、RocksDB block cache 或 memtable，只处理上面 20 个 Atomic WAL 内存 buffer 中已经提交的旧 Node。

### 12.2 完全无业务写时，为什么 buffer 仍增长

假设发布配置 H=30。每个 Raft group 的 Leader 名义上约每 10.2495 秒产生一条空日志，并复制给另外两个副本。对 `shop-part-1`：

```text
S1 是 Leader：生成空日志 #1001
S2 是 Follower：收到并写入空日志 #1001
S3 是 Follower：收到并写入空日志 #1001
```

三台的 `shop-part-1` 都先写本地 WAL 文件，再执行 `AtomicLogBuffer::push()`：[`FileBasedWal.cpp:442-500`](../src/kvstore/wal/FileBasedWal.cpp#L442-L500)。其余 19 个 part 发生相同过程。

一个 Node 放 64 条日志。每个 part 每天约收到 8429.7 条空日志，因此约新增：

```text
8429.7 / 64 = 131.7 个 Node/天/replica-part
```

### 12.3 冷态参数

采用示例参数：

```text
idle timeout = 24 小时
keep nodes = 256
trim nodes/part/run = 64
clean interval = 10 分钟
```

`idle timeout=24h` 表示“24 小时没有非空 Raft WAL 后允许老化”，不表示在第 24 小时把缓存清零。`keep nodes=256` 表示始终保留最新 256 个有效 Node。

若进程刚启动时 buffer 为空：

```text
0h：0 Node
24h：约 132 Node，已具备 cold 资格，但小于 256，所以不删除
46.6h：约 256 Node，达到冷窗口
48h：约 263 Node，下一轮 clean 把最旧约 7 个已提交 Node 标脏
以后：新 Node 继续产生，旧 Node 以近似相同速度删除，live Node 在 256 附近波动
```

所以该方案没有停止 `new Node`；它让“新增”和“删除”达到平衡，使 live 内存不再长期线性增加。

### 12.4 一次实际 trim 发生了什么

假设 `shop-part-1` 当前有 263 个有效 Node：

下面只按时间从旧到新示意，不代表源码中 `next_/prev_` 指针的实际方向：

```text
N1 -> N2 -> ... -> N256 -> ... -> N263
^最旧                                  ^最新 head
```

本地 `committedLogId=16820`，并且 N1 到 N7 都是完整 Node，其 inclusive last LogID 不超过 16820。10 分钟清理任务执行：

1. [`RaftPart::cleanWal()`](../src/kvstore/raftex/RaftPart.cpp#L492-L495) 在 `raftLock_` 内取得本地 committed 水位。
2. 确认距离最后一条非空 WAL 已超过 24 小时。
3. 发现 263 - 256 = 7 个多余有效 Node。
4. synthetic ref 在整个批次中阻止并发 GC 删除正在处理的旧 Node。
5. 依次把 N1 至 N7 标为 deleted，并把有效 tail 移到 N8；若某个 Node 跨过 committed 水位，则立即停止，绝不继续删除。
6. 批次结束释放 synthetic ref。旧 iterator 若仍在读取 N1 至 N7，它可以安全读完；最后一个旧 reader 释放后，[`AtomicLogBuffer::releaseRef()`](../src/kvstore/wal/AtomicLogBuffer.cpp#L220-L268) 才真正 `delete` 这些 Node。

删除后有效内存链是：

```text
N8 -> N9 -> ... -> N263
共 256 个有效 Node
```

磁盘 `.wal`、`FileBasedWal::first/lastLogId`、term、commit 水位和 RocksDB 数据均未被这次内存 trim 修改。

### 12.5 如果后来需要一条已淘汰的旧日志

例如 Leader 需要从 #16000 给刚恢复的 Follower 补日志：

1. [`FileBasedWal::iterator()`](../src/kvstore/wal/FileBasedWal.cpp#L530-L536) 先查 Atomic buffer；
2. #16000 早于新的内存 `firstLogId`，内存 iterator 无效；
3. 改用 `WalFileIterator` 读取磁盘 WAL；
4. 磁盘历史也不存在时，可能转 snapshot；文件异常时也可能返回 `E_RAFT_NO_WAL_FOUND`。

所以清理内存不会制造错误日志，但可能把追赶从内存路径变成磁盘或 snapshot 路径。这就是必须监控 IO、追赶延迟和 snapshot 的原因。

### 12.6 一个 space 实际释放多少

本机实验 jemalloc 对一个满 Node 的 usable size 是 3584B，即 3.5KiB。

默认 8MiB 逻辑容量对空日志约可积累 8192 个 Node：

```text
单 part：8192 × 3.5KiB = 28MiB
shop 每台 20 parts：20 × 28MiB = 560MiB/storaged
```

进入 256 Node 冷窗口后：

```text
单 part：256 × 3.5KiB = 0.875MiB
shop 每台 20 parts：20 × 0.875MiB = 17.5MiB/storaged
```

因此，当 `shop` 的 20 个本地 replica-parts 全部进入 cold 时，本机 ABI/allocator 条件下，每台 storaged 的该 space Atomic 主缓存理论平台约从 560MiB 降到 17.5MiB。三台合计约从 1.64GiB 降到 52.5MiB，但生产应按每台进程分别做预算。

### 12.7 业务写恢复时

假设第 10 天写入一个落到 `shop-part-3` 的点：

1. 该数据操作被编码为非空 payload；
2. part-3 的 Leader/Follower 在写 WAL 时都更新 `lastNonEmptyLogTime`；
3. 三台的 `shop-part-3` 立即退出 cold，不再执行 idle trim；
4. 它以后可按原有 `wal_buffer_size=8MiB` 继续扩大热缓存；已经删除的旧 Node 不会为了“恢复热态”重新从磁盘预加载；
5. `shop-part-1、2、4...20` 若仍没有非空 WAL，则继续维持 256 Node 冷窗口。

这也是采用 per-part 而不是整个 space 一个 idle 开关的原因：一个热点分片不应让其他 19 个空闲分片重新增长。

### 12.8 它没有清理什么

即使上述 Atomic 内存已经降到平台，下面这些仍存在：

- RocksDB block cache；
- RocksDB active/immutable memtable；
- OS page cache；
- Raft/Part/FileBasedWal 对象本身；
- 空日志对应的磁盘 WAL、网络和 CPU；
- jemalloc 已释放但暂未归还操作系统的 page。

普通图查询读取的是 RocksDB，不是 AtomicLogBuffer。因此本方案不驱逐图查询的 block cache；主要性能代价发生在落后副本追赶、日志重放和 snapshot，而不是正常点边读取。
