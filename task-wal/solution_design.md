# storage WAL 内存上涨问题解决方案设计文档

## 1. 设计目标

本次方案需要同时解决两个问题：

1. 降低或消除空闲 partition 因 Raft heartbeat 持续追加空 WAL log 带来的内存增长。
2. 增加可观测性，能够按图空间和 partition 定位 WAL buffer 的内存占用、待释放内存和 GC 状态。

设计原则：

- 保留 Raft 正确性语义。
- 最小化行为变更。
- 不引入高基数常驻指标导致新的监控内存压力。
- 指标能直接回答“哪个 space/part 在涨、涨的是 active 还是 dirty、GC 是否被阻塞”。

## 2. 修复方案设计

### 2.1 heartbeat 空 log 追加条件收敛

修复前，leader heartbeat 在没有正在复制日志时就追加空 log。修复后增加当前任期提交判断：

```text
仅当满足以下条件时，heartbeat 才追加空 log：

1. partition 状态为 RUNNING
2. 当前角色为 LEADER
3. 当前 leader 任期尚未提交过日志，即 commitInThisTerm_ == false
4. 当前没有正在复制日志，即 replicatingLogs_ == false
```

这样设计的原因：

- leader 当前任期尚未提交日志时，仍然需要追加一条空 log 来完成 Raft 当前任期提交语义。
- 当前任期已经提交过日志后，heartbeat 只需要发送 heartbeat RPC，不需要继续写空 WAL。
- 该改动将空 log 从“每轮 heartbeat 一次”降低为“每个 leader 任期必要时一次”。

### 2.2 全局 heartbeat 指标

新增低基数全局指标：

- `num_raft_heartbeat`
- `num_raft_heartbeat_empty_log`
- `num_raft_heartbeat_without_empty_log`

用途：

- 判断 heartbeat 是否仍在持续追加空 log。
- 对比修复前后空 log 写入频率。
- 验证修复是否生效。

预期：

- 修复前：`num_raft_heartbeat_empty_log` 接近 `num_raft_heartbeat`。
- 修复后：稳定 leader 任期内，`num_raft_heartbeat_empty_log` 接近 0，`num_raft_heartbeat_without_empty_log` 接近 `num_raft_heartbeat`。

## 3. WAL 分片级监控设计

### 3.1 为什么不把所有分片指标注册到 StatsManager

图空间和 partition 数量可能很大。如果每个 space/part 都注册多组 `StatsManager` counter，会带来：

- 指标对象数量膨胀。
- stats 输出内容膨胀。
- 监控系统高基数标签压力。
- 额外内存占用，反而干扰当前内存问题分析。

因此，本次采用分层设计：

- `StatsManager` 只放低基数全局 heartbeat 指标。
- 高基数 space/part WAL 状态通过 storage HTTP 接口按需查询。

### 3.2 新增 HTTP 接口

新增 storage HTTP 接口：

```bash
GET /wal_stats
GET /wal_stats?space=<space_id>
GET /wal_stats?space=<space_id>&part=<part_id>
```

查询示例：

```bash
curl --noproxy '*' -s 'http://127.0.0.1:19779/wal_stats'
curl --noproxy '*' -s 'http://127.0.0.1:19779/wal_stats?space=1'
curl --noproxy '*' -s 'http://127.0.0.1:19779/wal_stats?space=1&part=1'
```

接口返回当前 storaged 本地 partition 的 WAL 状态。它不是集群级聚合接口，如果要看全局，需要分别查询每个 storaged。

### 3.3 返回结构

接口返回 JSON：

```json
{
  "total": {
    "parts": 0,
    "active_bytes": 0,
    "dirty_bytes": 0,
    "active_nodes": 0,
    "dirty_nodes": 0,
    "reader_refs": 0,
    "gc_count": 0,
    "gc_deleted_nodes": 0,
    "gc_deleted_bytes": 0,
    "heartbeat_empty_logs": 0
  },
  "parts": []
}
```

`parts` 数组中每个元素代表一个本地 partition，包含：

- `space_id`
- `part_id`
- `role`
- `is_leader`
- `active_bytes`
- `dirty_bytes`
- `active_nodes`
- `dirty_nodes`
- `reader_refs`
- `first_log_id`
- `last_log_id`
- `gc_count`
- `gc_deleted_nodes`
- `gc_deleted_bytes`
- `heartbeat_empty_logs`

## 4. 指标含义和排查方法

### 4.1 active_bytes / active_nodes

表示当前仍在 WAL buffer 中有效的记录规模。

如果 `active_bytes` 随时间持续增长，说明当前 partition 仍在不断向 WAL buffer 写入记录。结合 `heartbeat_empty_logs` 可以判断增长是否仍由空 heartbeat log 贡献。

### 4.2 dirty_bytes / dirty_nodes

表示已经逻辑删除、但尚未真正释放的 WAL buffer。

这是判断“待释放内存”的核心指标。如果 RSS 高，同时 `dirty_bytes` 高，说明内存已经具备释放条件的一部分，但还未完成 GC。

### 4.3 reader_refs

表示当前 WAL iterator 引用数。

如果 `dirty_bytes` 高且 `reader_refs` 长期不为 0，说明 WAL buffer GC 可能被 iterator 生命周期阻塞，应继续排查 snapshot、replication、log iterator 使用链路。

### 4.4 gc_count / gc_deleted_bytes / gc_deleted_nodes

表示 GC 是否执行以及释放规模。

如果 `dirty_bytes` 高但 `gc_count` 长期不变，说明 GC 触发不足或被引用阻塞。如果 `gc_deleted_bytes` 增长但 RSS 不下降，则需要进一步结合 allocator 行为分析。

### 4.5 heartbeat_empty_logs

表示分片级 heartbeat 空 log 累计次数。

修复后预期每个 leader 任期只在必要时增长，稳定任期内不应持续增长。如果某些 partition 的该值仍快速增长，需要重点检查 `commitInThisTerm_` 是否反复变 false、leader 是否频繁切换、或者 append/commit 流程是否异常。

## 5. 验证方案

### 5.1 编译验证

```bash
cd build
set -o pipefail
make -j10 2>&1 | tee ../task-wal/build_20260704_wal_stats_metrics_retry.log
```

本机验证结果：编译成功。

### 5.2 安装验证

```bash
cd build
set -o pipefail
make install 2>&1 | tee ../task-wal/install_20260704_wal_stats_metrics.log
```

本机验证结果：核心二进制已安装；后续修改 `/usr/local/nebula/etc/nebula-metad.conf.default` 权限时失败，报 `Operation not permitted`。该问题属于安装目录权限问题，不影响本次代码逻辑判断。

### 5.3 启动验证

启动前执行：

```bash
ulimit -n 65536
```

服务启动日志：

- `task-wal/runtime_start_20260704_wal_stats_metrics.log`

本机验证结果：

- `metad` 运行中。
- `storaged` 运行中。
- `graphd` 运行中。

### 5.4 接口验证

接口验证日志：

- `task-wal/runtime_wal_stats_20260704.log`

当前验证环境全量查询关键结果：

- `total.parts=84`
- `total.active_bytes=1344`
- `total.active_nodes=84`
- `total.dirty_bytes=0`
- `total.dirty_nodes=0`
- `total.reader_refs=0`
- `total.gc_count=0`
- `total.gc_deleted_bytes=0`
- `total.heartbeat_empty_logs=84`

解释：

- 当前没有 WAL dirty buffer 堆积。
- 当前没有 reader 引用阻塞 GC。
- `heartbeat_empty_logs=84` 对应服务启动后每个本地 leader partition 在当前任期尚未提交日志时产生的一次空 log，符合预期。

## 6. 方案边界

本次方案解决的是 heartbeat 空 WAL log 导致的持续增长问题，并提供 WAL buffer active/dirty/GC 可观测性。

它不直接解决以下问题：

- allocator 持有内存导致 RSS 不立刻下降。
- RocksDB block cache、memtable、compaction 等非 WAL 方向内存增长。
- 集群级汇总指标，需要外部脚本或监控系统分别采集各 storaged 后聚合。

如果修复后仍观察到 RSS 增长，应先通过 `/wal_stats` 判断 WAL 是否仍是主要贡献者，再决定是否转向 RocksDB 或 allocator 分析。
