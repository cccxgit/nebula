# Nebula Graph 3.6 storaged 空闲场景内存持续上涨根因分析

> 分析范围：`tasks/task-wal.md` 的任务 1，仅分析根因，不给修复方案。
>
> 代码基线：本仓库 `de9b3ed800a6627d9845e9289b6bbc5b6faf460a`（3.6 分支）。
>
> 证据优先级：本地 3.6 源码与本机长跑日志 > RocksDB 官方文档 > NebulaGraph 官方论坛经验贴。

## 1. 结论摘要

现有证据不支持把本题简单定性为“没有业务写入时发生的神秘内存泄漏”。Nebula Graph 3.6 的空闲 Raft group 实际上仍在持续写入：

1. 每个稳定 leader 的 `statusPolling()` 都会调用 `sendHeartbeat()`。
2. `sendHeartbeat()` 除了发送真正的 Heartbeat RPC，还会在没有日志正在复制时追加一条 payload 为空的 `NORMAL` Raft log。
3. 这条空日志复制到 leader 和 follower 的每个本地副本，进入每 partition 一个的 `AtomicLogBuffer`。
4. 空 payload 在状态机应用时虽然被称为 heartbeat 并跳过业务数据，但仍会更新该 partition 的 RocksDB commit key，执行一次 RocksDB `Write()`。
5. RocksDB memtable 因此保存大量“同一个 commit key 的不同 sequence 版本”；达到每 space 默认约 64 MiB 后 flush，造成 RSS 阶梯式回落。

完整链路如下：

```text
RaftPart::statusPolling()
        |
        v
RaftPart::sendHeartbeat()
        |
        +-- 真正 Heartbeat RPC --------------------> follower 不追加日志
        |
        +-- appendLogAsync(NORMAL, "")
                 |
                 +-- leader/follower FileBasedWal
                 |          |
                 |          +-- 每 partition 的 AtomicLogBuffer 持续增加 Node
                 |
                 +-- Part::commitLogs()
                            |
                            +-- 空 payload 仅跳过业务操作
                            +-- putCommitMsg(systemCommitKey(partId))
                                       |
                                       v
                              每 space 的 RocksDB memtable
                                       |
                                       +-- Write Buffer Full -> flush -> 部分 RSS 回落
```

因此需要区分三个层次：

- **共同上游根因**：`RaftPart::sendHeartbeat()` 周期性追加空 `NORMAL` log，而不是只发送不落盘的 Heartbeat RPC。
- **约数天后回落的主导机制**：空日志的 commit key 版本填满 RocksDB memtable，随后 `Write Buffer Full` flush；现有证据对此为高置信度归因，精确生产占比仍需同步 RSS/flush 时间线确认。
- **长期上涨的叠加项**：每 partition 的 `AtomicLogBuffer` 在达到逻辑容量前持续分配 Node；它的周期远长于本题所述约 5 天，不能单独解释该回落点。

结论置信度：空日志触发链和两个内存持有路径为高置信度；现网“恰好 5 天”的精确周期仍取决于实际生效的 Raft heartbeat 参数及少量其他写入，不能仅凭仓库默认值断言。

## 2. 为什么空闲 partition 仍在持续产生写入

### 2.1 实际调度周期不是配置值本身

`src/kvstore/raftex/RaftPart.cpp:1401-1433` 中，每个本地 `RaftPart` 都运行 `statusPolling()`。下一次调度延迟为：

```cpp
FLAGS_raft_heartbeat_interval_secs * 1000 / 3
    + folly::Random::rand32(500)
```

稳定 leader 的 `needToSendHeartbeat()` 在 `RaftPart.cpp:1138-1141` 只检查当前角色和状态，所以每轮都会进入 `sendHeartbeat()`。

仓库发布配置 `conf/nebula-storaged.conf.default:46` 和 `conf/nebula-storaged.conf.production:46` 都设置：

```text
raft_heartbeat_interval_secs = 30
```

其实际平均 polling 周期不是 30 秒，而是：

```text
floor(30 * 1000 / 3) + mean([0, 499])
= 10000 + 249.5 ms
= 10.2495 秒
```

| `H=raft_heartbeat_interval_secs` | 实际间隔范围 | 平均间隔 |
|---:|---:|---:|
| 1（本地加速验证） | 0.333～0.832 秒 | 0.5825 秒 |
| 5（源码 gflag 默认值） | 1.666～2.165 秒 | 1.9155 秒 |
| 20 | 6.666～7.165 秒 | 6.9155 秒 |
| 30（发布配置） | 10～10.499 秒 | 10.2495 秒 |

如果上一批日志仍在复制，`replicatingLogs_` 会使该轮空日志跳过，因此以上是健康、低延迟 Raft group 的名义频率。

### 2.2 `sendHeartbeat()` 每轮追加空 NORMAL log

根源代码位于 `src/kvstore/raftex/RaftPart.cpp:2041-2048`：

```cpp
// If leader has not commit any logs in this term, it must commit all logs in
// previous term, so heartbeat is send by appending one empty log.
if (!replicatingLogs_.load(std::memory_order_acquire)) {
  folly::via(executor_.get(), [this] {
    std::string log = "";
    appendLogAsync(clusterId_, LogType::NORMAL, std::move(log));
  });
}
```

注释描述的是“本 term 尚未 commit 时用一条空日志推进 commit”，但实际条件只有 `!replicatingLogs_`。代码没有使用 `commitInThisTerm_` 限制为每个 term 一次。因此在 leader 空闲且复制正常时，每轮 polling 都会再追加一条空日志。

`sendHeartbeat()` 在 `RaftPart.cpp:2051-2081` 还会并行发送真正的 Heartbeat RPC。follower 的 `processHeartbeatRequest()` 在 `RaftPart.cpp:1912-1914` 明确说明该 RPC 不追加日志。两种“心跳”不能混为一谈：产生持续写入的是前面的空 `NORMAL` log。

### 2.3 空日志会落到所有副本

空日志走完整的正常复制链路：

- leader 在 `RaftPart.cpp:891-905` 写自己的 `FileBasedWal`；
- `RaftPart.cpp:912-972` 将日志复制给 peers；
- follower 在 `RaftPart.cpp:1757-1777` 写自己的 `FileBasedWal`；
- `FileBasedWal.cpp:457-499` 先写磁盘文件，再调用 `logBuffer_->push()`。

所以只有 leader 发起空日志，但 leader 和每个 follower 都会持有一份。对 3 副本、3 个 storaged 的空间，每台机器最终都拥有每个 partition 的一个本地副本。

## 3. 空日志为何仍会写 RocksDB

关键代码在 `src/kvstore/Part.cpp:215-358`：

```cpp
lastId = iter->logId();
lastTerm = iter->logTerm();
auto log = iter->logMsg();
if (log.empty()) {
  VLOG(4) << "Skip the heartbeat!";
  ++(*iter);
  continue;
}
...
if (lastId >= 0) {
  putCommitMsg(batch.get(), lastId, lastTerm);
}
engine_->commitBatchWrite(...);
```

代码先记录 `lastId/lastTerm`，再跳过空 payload。跳出循环后仍调用 `putCommitMsg()`，因此所谓 `Skip the heartbeat` 只是不应用业务 KV，不是不做持久化写入。

`Part.cpp:403-410` 把 8 字节 LogID 和 8 字节 TermID 写入 `NebulaKeyUtils::systemCommitKey(partId_)`；该 key 在 `src/common/utils/NebulaKeyUtils.cpp:101-108` 中按 partition 构造。最终 `src/kvstore/RocksEngine.cpp:124-134` 调用 `db_->Write()`。

结果是：每个 partition 始终覆盖同一个用户 key，但 RocksDB 的 memtable 使用带 sequence/type 的 internal key 保存每次写入版本。覆盖同一个 key 不会原地覆盖前一个 memtable entry。

这也符合 [RocksDB 官方 MemTable 文档](https://github.com/facebook/rocksdb/wiki/MemTable)：新写入进入 mutable memtable；达到 `write_buffer_size` 后变为 immutable，由后台 flush 成 SST，之后旧 memtable 才可销毁。

## 4. 周期性回落的主导机制：每 space 的 RocksDB memtable flush

### 4.1 为什么 space 越多，内存蓄水池越多

`src/kvstore/NebulaStore.cpp:354-365,395-421` 表明：每个 `space × data_path` 创建一个独立 `RocksEngine`，也就是一个独立 RocksDB 实例。发布配置在 `conf/nebula-storaged.conf.default:102` 设置：

```text
write_buffer_size = 67108864       # 64 MiB
max_write_buffer_number = 4
```

在本题的低吞吐空心跳场景，后台 flush 足够快，通常表现为每个 space 一个约 64 MiB 的 mutable memtable 逐渐增长，满后立即切换和 flush。space 数增加的是独立 memtable 数量；partition 数增加的是每个 memtable 内 commit-key 版本的写入速度。

### 4.2 本机长跑日志给出的直接证据

此前 task-wal 验证集群配置为：

- 3 个 storaged；
- 5 个空 space；
- 每 space 20 partitions、3 replicas；
- 无业务 DML；
- `raft_heartbeat_interval_secs=1`，用于加速观察；
- 每 space 的 RocksDB `write_buffer_size=64 MiB`。

该长跑集群使用兄弟工作树 `/home/sch/nebula/nebula-3.6-wal` 构建。其提交 `a3a8e5f5f49cce1507f91e3653cfcc4dddab643a` 的产品源码与当前 3.6 基线在 `src/`、`conf/` 下无差异；工作树只在 `AtomicLogBuffer` 和 storage HTTP stats 中增加观测计数，没有改变空日志、commit 或 RocksDB 写入逻辑。以下 `Write Buffer Full` 数据直接来自 RocksDB 自己的 DB `LOG`，不依赖新增指标。

对 3 个 storaged × 5 个 space 的 15 个 RocksDB `LOG` 做完整统计：

```text
包含 Write Buffer Full 的 DB 数：15
Write Buffer Full 事件总数：900
每个 DB 的事件数：60

每次 flush 平均 num_entries：1,281,118
范围：1,231,550 ～ 1,316,940

每次 flush 平均 memory_usage：66,262,552 bytes（约 63.19 MiB）
范围：64,935,336 ～ 66,911,368 bytes

相邻 flush 平均周期：10.3765 小时
```

代表样本 `/usr/local/nebula/data/task-wal-storage1/nebula/1/data/LOG`：

```text
2026/07/07-07:31:56
flush_reason = "Write Buffer Full"
num_entries = 1,272,918
memory_usage = 66,647,512

flush 生成的 SST：1,565 bytes，num_entries = 41

2026/07/07-17:53:42
flush_reason = "Write Buffer Full"
num_entries = 1,280,025
memory_usage = 66,452,944

flush 生成的 SST：1,443 bytes，num_entries = 20
raw_key_size = 320，raw_value_size = 320
```

第二次及后续 flush 后只剩 20 个最终 key，恰好对应该 space 的 20 个 partition commit keys。约 128 万个 memtable 版本最终折叠成 20 个 key，这排除了“真实业务数据逐渐变多”这一解释。

### 4.3 理论频率与日志逐条闭环

`H=1` 时平均轮询间隔是 0.5825 秒。一个 space 在一台 storaged 上有 20 个本地 replicas，所以理论写入率为：

```text
20 / 0.5825 = 34.33 entries/s
```

按实测平均 flush 阈值 1,281,118 entries：

```text
1,281,118 / 34.33 = 37,311 秒 = 10.364 小时
```

实测为 10.3765 小时，两者相差约 0.12%。这几乎逐条证明了 memtable 中的 entries 就是每 partition 周期性空日志形成的 commit-key 版本。

同一模型外推：

| `raft_heartbeat_interval_secs` | 单 space、20 partitions 的预计 flush 周期 |
|---:|---:|
| 1 | 0.432 天（10.36 小时） |
| 5 | 1.42 天 |
| 20 | 5.13 天 |
| 30 | 7.60 天 |

所以现网“约 5 天”与该机制在时间量级上高度一致：如果运行时有效值约为 20，理论值几乎就是 5 天；如果确实为发布默认 30，则纯心跳预计约 7.6 天，少量业务写入或其他 RocksDB 写入会将它提前。报告不能在未取得现网运行时 flag 和 RocksDB flush 时间戳前，把默认 30 秒精确等同于 5 天。

memtable flush 后 Arena 内存被释放；storaged 默认每秒执行内存检查，并在启用 jemalloc 时约每 10 秒执行 arena purge（`src/storage/StorageServer.cpp:73-90`、`src/common/memory/MemoryUtils.cpp:149-208`、`conf/nebula-storaged.conf.default:140-143`）。因此 jemalloc purge 只是让已释放页较快反映到 RSS，**不是数天周期的定时器**。

多个 space 的创建时间、写入量和后台调度并不完全相同，flush 会错峰；新 mutable memtable 同时继续增长，jemalloc 也可能保留部分 extent。因此 RSS 回落值不必等于 `64 MiB × flush 的 space 数`，只看到约 100 MiB 级别的净回落并不矛盾。

现有长跑记录没有与每一次 flush 同时间戳采集进程 RSS，这是证据边界。当前归因由四部分构成：源码证明每次空日志都写 commit key；写入频率与 128 万 entries/10.38 小时精确闭合；RocksDB 日志明确记录 `Write Buffer Full`；源码与 RocksDB 官方机制证明 flush 后旧 memtable/Arena 可销毁。因此“memtable flush 主导周期回落”是高置信度推断，但报告不声称已经量出它在现网每次约 100 MiB 回落中的精确百分比。

## 5. 长期上涨的第二条路径：每 partition 的 AtomicLogBuffer

### 5.1 每个本地 partition 都有独立 buffer

`src/kvstore/NebulaStore.cpp:484-515` 为每个本地 partition 构造一个 `Part`；`RaftPart.cpp:357-365` 使用 `wal_buffer_size` 建立 `FileBasedWal`；`FileBasedWal.cpp:59` 再建立一个 `AtomicLogBuffer`。默认逻辑容量为 8 MiB。

`AtomicLogBuffer.h:18,23-39,44-110` 显示：

- 一个 `Node` 固定保存 64 个 `Record`；
- 空日志的 `Record::size()` 只计算 ClusterID 8 字节 + TermID 8 字节，即 16 字节；
- 容量判断不计 `Record` 对象、`std::string` 对象、Node 指针、原子变量和对齐开销。

当前 x86_64 Debug ABI 实测：

```text
sizeof(Record) = 48 bytes
sizeof(Node) = 3,200 bytes
alignof(Node) = 64
jemalloc usable size = 3,584 bytes/Node
```

空字符串使用 SSO，不另分配 payload heap。每 64 条空日志分配一个 Node，因此直接请求约 50 字节/日志，jemalloc size class 约 56 字节/日志；但 `size_` 只按 16 字节/日志增长。

所谓 8 MiB buffer 对空日志实际对应：

```text
8 MiB / 16 = 524,288 条日志
524,288 / 64 = 8,192 个 Node
Node 直接请求 = 8,192 × 3,200 = 25 MiB
jemalloc usable ≈ 28 MiB
```

所以 `wal_buffer_size=8MiB` 是逻辑记录大小上限，不是物理内存硬上限。

### 5.2 GC 不是约 5 天回落的触发器

`src/kvstore/wal/AtomicLogBuffer.cpp:71-119` 在逻辑容量超限后才把最旧 Node 标记为 deleted；真正的 `delete Node` 位于 `releaseRef()` 的 `AtomicLogBuffer.cpp:167-214`，条件为 reader 引用降到零且 dirty Node 超过 5 个，或逻辑 `size_` 超过 `max_log_buffer_size=16MiB`。

正常 Raft commit 路径会频繁创建并析构 iterator：leader 在 `RaftPart.cpp:1068-1078`，follower 在 `RaftPart.cpp:1793-1797`。所以“GC 依赖 iterator 析构”是事实，但没有证据表明本题存在 iterator 不释放；前 8 MiB 内不发生 GC，主要是因为根本还没有 dirty Node。

这里确有一个需要保留的边界条件：历史官方 [nebula-storage issue #390](https://github.com/vesoft-inc/nebula-storage/issues/390)指出，引用计数作用于整个 `AtomicLogBuffer`，当 iterator 长期存在或持续重叠、使 reader 数一直大于 1 时，已标脏 Node 的 GC 可能长期推迟。3.6 的 `AtomicLogBuffer.cpp:174-176` 仍保留该 issue 的 TODO，并在 `readers > 1` 时直接返回。这会让 Atomic 实际占用进一步超过容量估计，但普通空闲心跳的短生命周期 commit iterator 会反复降到零，现有证据不能把该边界条件当成本题约 5 天现象的主触发器。

空日志首次积累到可触发六个 dirty Node 约需 524,610 条：

| `H` | AtomicLogBuffer 首次 GC 的预计时间 |
|---:|---:|
| 1 | 3.54 天 |
| 5 | 11.63 天 |
| 20 | 41.99 天 |
| 30 | 62.23 天 |

因此：

- 本地 `H=1` 时 RocksDB 首次约 10.4 小时就 flush，远早于 Atomic 的约 3.54 天；
- 发布配置 `H=30` 时 RocksDB 约 7.6 天 flush，Atomic 首次 GC 约 62.2 天；
- Atomic 确实造成长期、按 partition 数线性放大的基线增长，但不能解释约 5 天的首次回落。

在 iterator refs 能周期归零的正常路径中，达到容量后 Atomic 会持续标记和回收旧 Node，内存趋于高位稳定，而不是无限泄漏；但每 partition 约 25～28 MiB 的实际持有量乘以大量本地 replicas 后可以非常大。若命中前述长生命周期/持续重叠 reader 边界，这个估计也不是严格上限。

## 6. 60 space、20 partition、3 副本场景的数量关系

设：

```text
S = space 数
P = 每 space partition 数
R = replica factor
N = storaged 节点数
T = statusPolling 实际平均周期
```

则：

```text
Raft group/leader 数                 = S × P
集群空日志发起率                     ≈ S × P / T
集群 WAL/buffer/commit 写入率        ≈ S × P × R / T
单节点本地 replica/buffer 数         ≈ S × P × R / N
单节点写入率                         ≈ S × P × R / (N × T)
```

本题 `S=60, P=20, R=N=3, H=30` 时：

```text
Raft groups                    = 1,200
每节点本地 partition replicas = 1,200
每节点空日志/commit 写入       ≈ 10.12 million/day
```

用本地长跑得到的每个 RocksDB entry 平均内存约 51.72 字节估算，memtable 分配速度约 499 MiB/天/节点；Atomic Node 直接分配约 482 MiB/天/节点（jemalloc size class 约 540 MiB/天），直至各自达到 flush 或容量阈值。

这些是干净实验模型下的分配流量，不等于最终 RSS 日增量：RocksDB flush、allocator 保留/归还、space 错峰、复制延迟、业务写入及实际生效参数都会改变 RSS 曲线。但它解释了为何 partition/space 数量一多，增长会由不明显迅速放大到生产可见。

## 7. 为什么 `wal_ttl=4h` 不是内存回落原因

这里有两种容易混淆的 WAL：Nebula Raft `FileBasedWal` 和 RocksDB 自己的 WAL。本题中的 `wal_ttl` 控制前者的磁盘文件，不控制 `AtomicLogBuffer`，也不是 RocksDB memtable 的生命周期。

`src/kvstore/wal/FileBasedWal.cpp:15-17` 默认：

```text
wal_ttl = 14,400 seconds
wal_file_size = 16 MiB
wal_buffer_size = 8 MiB
```

空日志的磁盘编码大小为：

```text
LogID 8 + TermID 8 + length 4 + ClusterID 8 + trailing length 4 = 32 bytes
```

填满一个 16 MiB 文件需要约 524,288 条。`H=30` 时每 partition 约 62.2 天才首次 rollover。`FileBasedWal.cpp:640-706` 的清理逻辑还至少保留最近两个文件，而且只 `unlink` 磁盘文件、更新 `walFiles_`，没有 reset/trim `AtomicLogBuffer`。

因此“4 小时 TTL 到期导致内存下降”与源码及时间尺度都不符。

## 8. 外部资料交叉验证

外部资料支持该结论，但不替代本地证据：

1. NebulaGraph 官方论坛的[《nebula-storaged内存持续升高》](https://discuss.nebula-graph.com.cn/t/topic/13075)是最接近本题时间尺度的独立案例：v3.1、10 个 space、共 212 个 partitions，在几乎无写入和查询时，RSS 仍在 5 天内从 5.7 GiB 涨到 6.1 GiB。帖子 #8 与 #10 的两次独立 in-use heap 截图显示，总 heap 从 5,062.8 MB 增至 5,464.1 MB（+401.3 MB），`AtomicLogBuffer::push` 从 107.9 MB 增至 565.9 MB（+458.0 MB），而两个主要 RocksDB 读/解压栈基本不变。按本报告公式，212 个本地 partitions、`H=30`、每条空日志的 Node 直接请求/allocator usable 开销约 50/56 字节，5 天预计增加约 0.43～0.48 GiB，与截图的 +458 MB 高度吻合。这是对“空闲五天上涨来自空日志 Atomic Node”的近乎定量复现。帖子 #7 的 [`jeprof --base` 差分附件](https://discuss.nebula-graph.com.cn/uploads/short-url/bsPWKDeIjm7ASd8EvQ6Jgqs4af9.pdf)又同时显示 `FileBasedWal::appendLogs`、`AtomicLogBuffer::push` 和 RocksDB `MemTable/Arena` 的正增长，并显示 `FlushJob` 路径为负值。该附件是两个 in-use heap 快照的差分，证明 WAL buffer 增长和 memtable flush 释放可以同时存在；它并未给出本报告发现的 commit-key 代码链。
2. 官方论坛的[《storage 启动后内存一直提升无法降下来》](https://discuss.nebula-graph.com.cn/t/topic/13677)记录了 v3.3、127 个 space × 100 partitions 的场景：数据导入结束后无操作，RSS 先缓升到 20 GiB 以上、回落到约 13 GiB、随后又升至约 16 GiB。其[单点 jeprof 附件](https://discuss.nebula-graph.com.cn/uploads/short-url/btEMiwlhVWgXVz1eWgw56OUZ2z2.pdf)同时包含 `AtomicLogBuffer::push` 约 160.5 MB 和 RocksDB `MemTable/Arena` 约 295.9 MB；用户将 partition 数降下来后内存趋稳。这独立复现了“Raft buffer 抬升基线 + memtable/flush 形成较大回落”的双路径现象。
3. 官方论坛的[《单节点 storaged 内存持续上涨至系统崩溃》](https://discuss.nebula-graph.com.cn/t/topic/14898)是 Nebula 3.6、完全无业务操作仍上涨的案例；将 partition 降到 10 后，增长速度显著下降到约 10 MB/日，直接支持“增长率与本地 partition 数相关”。
4. 官方论坛的[《storaged 节点内存持续上涨》](https://discuss.nebula-graph.com.cn/t/topic/14718)也是 v3.6 案例。维护者按 `space × partition × replica / storaged` 计算出每节点 24,300 个本地 replicas，并将问题方向指向 Raft log buffer。这是规模关系上的强佐证，但回复中“3.7 可能有 fix”没有公开 PR/commit，不能当作已发布修复。
5. 另一个[v3.6、400 多 space 的案例](https://discuss.nebula-graph.com.cn/t/topic/16594)在清理 OS cache 后仍持续增长，说明这类现象不能全部归因于 page cache；但该帖缺少 heap profile，证据强度低于前两份 jeprof 附件。
6. 官方论坛的[内存问题汇总](https://discuss.nebula-graph.com.cn/t/topic/14316)列出 RocksDB cache/index/filter、Raft buffer、jemalloc 等多种来源。它适合排查一般内存问题，但其中把 WAL 上限简单描述为 `max_log_buffer_size × part` 与 3.6 源码并不完全一致：本版本 `wal_buffer_size=8MiB` 才是 Atomic 的逻辑容量，`max_log_buffer_size=16MiB` 是 `releaseRef()` 的额外 GC 条件。因此准确机制仍以本地代码为准。
7. 上游 GitHub [PR #4386](https://github.com/vesoft-inc/nebula/pull/4386)为 `AtomicLogBuffer` 增加 `max_log_buffer_size` GC 条件，目的是避免单条超大日志令 dirty Node 占用远超预期。该提交已经存在于本仓库 3.6 基线；它并未改变本题的周期性空日志产生、按 16 字节逻辑计数或 RocksDB commit-key 写入。
8. 历史官方 [issue #390](https://github.com/vesoft-inc/nebula-storage/issues/390)是 Atomic GC 被长生命周期 reader 推迟的代码级已知问题，且 3.6 源码仍引用它。它说明 `wal_buffer_size` 并非严格物理上限，但不能直接证明当前普通空闲场景存在长寿命 iterator。

对论坛所称“3.7 raft log buffer fix”做了额外审计：公开仓库没有 v3.7.0 tag，而 v3.6.0 与 v3.8.0 tag 中的 `AtomicLogBuffer.cpp/.h`、`FileBasedWal.cpp` 和 `RaftPart.cpp` 四个关键文件字节相同。公开提交历史中最后一个相关实质修改仍是 2022 年的 PR #4386。该说法可能指企业内部/未公开分支、当时的计划，或版本口误；不能据此宣称开源版本升级到 3.7/3.8 已修复。

其他以大量查询、导入、block cache、index/filter、page cache 或 compaction 为主的帖子，与本题“多数空间为空且无业务访问”的前提不同，只能作为替代原因清单，不能覆盖本地代码与日志形成的闭环。

## 9. 对初步分析的更正

原 `tasks/task-wal-analysis.md` 的短时指标验证正确证明了：

- 每本地 partition 存在一个 Atomic buffer；
- 无业务写入时 Node 仍随心跳增加；
- partition 数与增长率相关。

但它把 Atomic 判为唯一“主要来源”证据不足，原因有三点：

1. 90 秒采样没有跨越 Atomic 的容量/GC 周期，也没有跨越 RocksDB memtable flush 周期；
2. 实验指标 `estimated_held_bytes = size_ + nodes × sizeof(Node)` 对空日志重复计算了已经包含在 Node 内的 ClusterID/TermID，因此会高估 Atomic；
3. 同一空日志同时写入 RocksDB，短时 RSS 相关性不能区分两条共同增长的路径。

长跑 RocksDB `LOG` 补齐了缺失证据，所以最终应将结论修正为“空 Raft 日志是共同上游；RocksDB memtable flush 解释数天锯齿回落；AtomicLogBuffer 解释长期按 partition 放大的持续基线增长”。

## 10. 最终根因判定

按源码层次定位：

1. **触发根因位置**：`src/kvstore/raftex/RaftPart.cpp:2041-2048`。`sendHeartbeat()` 的实现每个空闲 polling 周期都追加空 `NORMAL` log，实际条件与注释所述“本 term 尚未 commit”不一致。
2. **周期回落的内存放大位置**：`src/kvstore/Part.cpp:225-232,349-358,403-410`。空日志跳过业务 payload 后仍覆盖 partition commit key，使 RocksDB memtable 累积百万级 internal versions。
3. **长期基线增长位置**：`src/kvstore/wal/AtomicLogBuffer.cpp:71-119` 和 `AtomicLogBuffer.h:18,23-39,44-110`。每条空日志都进入每 partition buffer，物理 Node 开销没有纳入 8 MiB 逻辑容量计数。
4. **空间维度放大位置**：`src/kvstore/NebulaStore.cpp:354-365,395-421`。每个 `space × data_path` 是独立 RocksDB 实例和独立 memtable。

综合判定：本题不是单一 allocator 泄漏，也不是 `wal_ttl` 未清理；它是 Raft 空日志产生策略驱动的、随本地 partition replica 数线性放大的确定性内存保留行为，并叠加了每 space RocksDB memtable 的周期性 flush。现有本地源码、短时指标和 26 天 RocksDB 长跑日志已经对“空闲写入与内存分配/flush 机制”形成闭环；现网约 5 天的精确周期和每次 RSS 回落占比，仍需以现网实际 flag 与同步时间线为准。
