# WAL 内存持续上涨：不改源码机制的生产缓解方案

> 适用范围：NebulaGraph 3.6、3 个 storaged、RF=3；目标是不修改 Raft/WAL 源码。
>
> 结论边界：配置方案只能让 AtomicLogBuffer 更早进入淘汰并形成较低平台，不能停止周期空
> NORMAL 日志，也不能消除 RocksDB commit-key 次路径。不存在可以承诺“零副作用”的配置。

## 1. 结论

当前集群最优的低风险方案是：

1. 只在 storaged 配置中降低 `wal_buffer_size`，第一轮不同时修改 Raft 心跳、WAL TTL 或 RocksDB
   参数；
2. 推荐先用 `2MiB` 做单节点 canary；如果故障追赶或磁盘回退压力较高，改用更保守的 `4MiB`；
   如果 Atomic Node 的单机预算不足约 7GiB/1000 local parts，再经压测评估 `1MiB`；
3. RF=3 下每次只滚动重启一个 storaged，待其 ONLINE、分片和 Leader 恢复后再处理下一台；
4. 长期对新 Space 按磁盘数和真实并行度控制 `partition_num`；现有 Space 不能原地缩分片，只能新建
   Space 后迁移。

不建议首先增大 `raft_heartbeat_interval_secs`。它虽然能降低空日志速率，但同时改变选举超时、Peer
存活判断和 Leader lease，属于可用性语义变化，不符合本方案的低风险约束。

## 2. 为什么首选 wal_buffer_size

默认值是 8MiB，定义于
[`FileBasedWal.cpp:15-18`](../src/kvstore/wal/FileBasedWal.cpp#L15-L18)。每个 Part 构造时把该值
复制进自己的 WAL policy 和 AtomicLogBuffer：

- [`RaftPart.cpp:357-360`](../src/kvstore/raftex/RaftPart.cpp#L357-L360)；
- [`FileBasedWal.cpp:40-60`](../src/kvstore/wal/FileBasedWal.cpp#L40-L60)；
- [`AtomicLogBuffer.cpp:74-77`](../src/kvstore/wal/AtomicLogBuffer.cpp#L74-L77)。

容量超过 `wal_buffer_size` 后，`push()` 才开始把旧 tail Node 标脏：
[`AtomicLogBuffer.cpp:144-169`](../src/kvstore/wal/AtomicLogBuffer.cpp#L144-L169)。旧 Node 最终在 iterator
释放引用时按 GC 条件删除：
[`AtomicLogBuffer.cpp:220-268`](../src/kvstore/wal/AtomicLogBuffer.cpp#L220-L268)。

因此降低该值：

- 不改变 Raft 选举、Leader lease、RF 和数据一致性语义；
- 不改变空日志生成率和触顶前斜率；
- 只缩短“只分配、不淘汰”的时间，并降低长期有效 Node 窗口；
- 若日志已离开内存窗口，iterator 会回退磁盘 WAL，而不是直接丢日志：
  [`FileBasedWal.cpp:530-536`](../src/kvstore/wal/FileBasedWal.cpp#L530-L536)。

回退磁盘会增加 WAL 读取、系统调用和落后 Follower 追赶延迟，所以不能宣称完全无性能代价。

## 3. 容量选择

下表只用于当前实验 ABI/allocator 的条件估算：空 Record 逻辑计费 16B、每 Node 64 条、
`sizeof(Node)=3200B`、jemalloc usable=3584B；假设发布配置 H=30、1000 个 local replica-parts。
不含 partial/dirty Node、allocator 元数据和 RocksDB。

| wal_buffer_size | 首次接近容量的名义时间 | 1000 parts 有效 Node usable 平台 | 相对 8MiB |
|---:|---:|---:|---:|
| 8MiB | 约 62.2 天 | 约 27.34GiB | 基线 |
| 4MiB | 约 31.1 天 | 约 13.67GiB | 降约 50% |
| 2MiB | 约 15.5 天 | 约 6.84GiB | 降约 75% |
| 1MiB | 约 7.8 天 | 约 3.42GiB | 降约 87.5% |

在达到各自容量前，1000 parts 的 Node usable 初期斜率仍约 450MiB/day。调小 buffer 不是把斜率降为
零，而是让它更早进入“分配新 Node、淘汰旧 Node”的稳态。

本机构建的近似选值公式为：

```text
Atomic Node usable 平台
  ≈ local replica-parts × wal_buffer_size × 3584 / (64 × 16)
  ≈ local replica-parts × wal_buffer_size × 3.5
```

所以不应机械地固定为 2MiB，而应先确定每台机器允许给 Atomic Node 的内存预算。商业生产二进制若
编译器 ABI 或 allocator 不同，应重新测量 `sizeof(Node)` 和 allocator usable size。

## 4. 配置内容与生效方式

在三台 storaged 各自实际使用的配置中只保留一条：

```text
--wal_buffer_size=2097152
```

第一轮保持以下参数不变：

```text
--raft_heartbeat_interval_secs=30
--wal_ttl=14400
```

`wal_buffer_size` 不在 Nebula 3.6 的动态配置白名单中，且已有 Part 的 capacity 在构造时已经固定；
不能依赖在线改 flag，必须滚动重启 storaged。动态配置白名单见
[`GflagsManager.cpp:49-66`](../src/common/meta/GflagsManager.cpp#L49-L66)。

重启后会新建空 AtomicLogBuffer，同时扫描保留的磁盘 WAL；因此当前进程已经积累的 Node 会随旧进程
退出而释放。graphd 和 metad 不需要因该参数重启。

## 5. RF=3 滚动实施闸门

变更前：

- 3/3 storaged 均 ONLINE；所有 Space 的 Part 均有 RF3、Leader 和完整 Peers；
- 无 balance、rebuild、snapshot 等运行中管理作业；
- 无持续 `E_RAFT_*`、`LOG_GAP`、`WAITING_SNAPSHOT`；
- 备份已验证可恢复；剩余两台足以承接一台下线后的负载。

执行：

1. 选 Leader 最少、负载最低的 storaged 做 canary；备份其配置并记录校验值；
2. 只修改 `wal_buffer_size`，确认没有重复 flag；
3. 优雅停止 canary；任何时刻绝不同时停止第二个 storaged；
4. 在同一启动 shell 中先执行 `ulimit -n 65536`，再使用该实例实际配置启动；
5. 等待其 ONLINE、Part/Leader 数恢复、无持续追日志/快照和 term 抖动；至少观察 15 分钟，并以现网
   SLO 为准；
6. canary 的错误率、P99、CPU、磁盘延迟和 WAL 追赶均正常后，才逐台处理第二、第三个 storaged；
7. 全量后至少观察 24 小时。重启后的 RSS 下降只证明旧进程释放，不证明较小容量已经进入 GC 平台。

若任一节点不能在正常时长内恢复、Part/Leader 不完整、snapshot backlog 增长或业务 SLO 恶化，冻结
后续滚动，恢复原配置并在集群重新 3/3 健康后单节点回滚。不要删除 data/WAL 作为回滚手段。

## 6. 长期措施

`partition_num` 同时决定 Raft Group、FileBasedWal 和 AtomicLogBuffer 数量，因此减少不必要的分片能
同时降低 Atomic 主路径与 RocksDB commit-key 次路径。3.6 只支持创建 Space 时指定分片：

- 创建时读取并分配 `partition_num`：
  [`CreateSpaceProcessor.cpp:44-64`](../src/meta/processors/parts/CreateSpaceProcessor.cpp#L44-L64)、
  [`236-250`](../src/meta/processors/parts/CreateSpaceProcessor.cpp#L236-L250)；
- `ALTER SPACE` 只有 `ADD ZONE`：
  [`parser.yy:3461-3471`](../src/parser/parser.yy#L3461-L3471)、
  [`AlterSpaceProcessor.cpp:10-30`](../src/meta/processors/parts/AlterSpaceProcessor.cpp#L10-L30)。

因此：

- 无用且确认可删除的空 Space，可经完整审计后删除；
- 新 Space 按磁盘数、数据量和并行度设置较少但足够的分片；
- 现有非空 Space 只能新建目标 Space、逻辑迁移、校验、切流后再删除旧 Space，不能手改 Meta KV。

## 7. 不应作为首选的操作

| 操作 | 判断 |
|---|---|
| 增大 Raft H | 能降斜率，但改变选举、存活判断和 lease；另开变更评估，不作为首波 |
| 降低 wal_ttl | 只删除磁盘 WAL，不回收 Atomic Node；还会缩短慢副本追赶窗口 |
| 调 max_log_buffer_size | 不能把尚未标脏的有效 Node 变成可删除对象 |
| 加快 memory purge/drop caches | live Node 尚未 delete，purge 无法归还 |
| 降 RocksDB block cache | 只影响有界 cache；可能增加读 IO，不改变 Atomic 斜率 |
| 降 RocksDB write buffer | 只压缩次路径并增加 flush/compaction，不解决 Atomic 主路径 |
| 定期重启但不改参数 | 只能临时清空，增长会重新开始；作为应急，不是长期方案 |
| RF3 改 RF1 | 直接牺牲生产容错，不接受 |

## 8. 最终边界

在“不修改源码机制”这一约束下，最合理的目标是把主增长项从长达数十天的高平台压缩为业务可接受
的有界窗口。若要求空载周期日志、Atomic 分配和 RocksDB commit-key 更新全部停止，则只能消除
`sendHeartbeat()` 的重复空 NORMAL 日志上游；纯配置无法做到。
