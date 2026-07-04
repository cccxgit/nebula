# WAL 内存与 GC 监控指标设计及开发记录

## 设计目标

根据前期根因分析，storage 内存缓慢上涨与 WAL 内存缓存 `AtomicLogBuffer` 直接相关。仅监控 heartbeat 空 log 次数不能回答以下问题：

- 哪个图空间或分片的 WAL buffer 正在占用内存？
- 当前占用是有效缓存，还是已经标记删除但尚未 GC 的待释放内存？
- GC 是否发生过，释放了多少 node 和字节？
- 是否存在 iterator 长时间持有引用，导致 dirty node 无法释放？

因此新增按需查询接口 `/wal_stats`，用于监控图空间和分片级别 WAL 内存占用和待释放状态。

## 指标分层

### 全局聚合指标

接口返回 `total` 字段，对当前 storage 实例的所有本地 partition 聚合：

- `parts`
  - 返回的 partition 数。

- `active_bytes`
  - 当前有效 WAL buffer 字节数。
  - 对应 `AtomicLogBuffer::size_`。

- `dirty_bytes`
  - 已标记删除、但尚未真正释放的 WAL buffer 字节数。
  - 这是判断“内存待 GC”的核心指标。

- `active_nodes`
  - 当前有效 node 数。

- `dirty_nodes`
  - 已标记删除、等待 GC 的 node 数。

- `reader_refs`
  - 当前 WAL iterator 引用数。
  - 如果 `dirty_bytes`、`dirty_nodes` 长期较高，并且 `reader_refs` 长期不为 0，说明 GC 可能被 reader 持有阻塞。

- `gc_count`
  - `AtomicLogBuffer` GC 执行次数。

- `gc_deleted_nodes`
  - GC 累计释放 node 数。

- `gc_deleted_bytes`
  - GC 累计释放字节数。

- `heartbeat_empty_logs`
  - 当前 partition 累计 heartbeat 空 log 写入次数聚合。

### 分片级指标

接口返回 `parts` 数组，每个元素代表一个本地 partition：

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

## 查询方式

全部本地分片：

```bash
curl --noproxy '*' -s 'http://127.0.0.1:19779/wal_stats'
```

指定图空间：

```bash
curl --noproxy '*' -s 'http://127.0.0.1:19779/wal_stats?space=1'
```

指定图空间和分片：

```bash
curl --noproxy '*' -s 'http://127.0.0.1:19779/wal_stats?space=1&part=20'
```

## 为什么使用 HTTP 按需查询

没有把所有 space/part 维度都注册成常驻 `StatsManager` counter，原因是分片数可能很大。如果每个分片注册多组常驻指标，监控指标本身会带来额外内存占用和管理成本。

本次采用：

- `StatsManager`：保留低基数的全局 heartbeat 计数指标。
- `/wal_stats`：按需查询高基数的 space/part 级 WAL 内存状态。

这样可以在排查问题时获取分片级明细，同时避免长期引入高基数指标压力。

## 代码修改点

- `src/kvstore/wal/AtomicLogBuffer.h`
  - 新增 `AtomicLogBuffer::Stats`。
  - 新增 `AtomicLogBuffer::stats()`。

- `src/kvstore/wal/AtomicLogBuffer.cpp`
  - 维护 `dirtyBytes_`、`activeNodes_`、`gcCount_`、`gcDeletedNodes_`、`gcDeletedBytes_`。

- `src/kvstore/raftex/RaftPart.h`
  - 新增 `roleStr()`，用于返回分片 Raft 角色。
  - 新增 `numHeartbeatEmptyLogs()`，用于返回分片级 heartbeat 空 log 累计次数。

- `src/kvstore/raftex/RaftPart.cpp`
  - heartbeat 追加空 log 时递增分片级 `numHeartbeatEmptyLogs_`。

- `src/kvstore/NebulaStore.h`
- `src/kvstore/NebulaStore.cpp`
  - 新增 `NebulaStore::walStats()`，聚合并返回全局和分片级 WAL stats。

- `src/storage/http/StorageHttpWalStatsHandler.h`
- `src/storage/http/StorageHttpWalStatsHandler.cpp`
  - 新增 storage HTTP handler。

- `src/storage/StorageServer.cpp`
  - 注册 `/wal_stats` 路由。

- `src/storage/CMakeLists.txt`
  - 加入 `StorageHttpWalStatsHandler.cpp`。

## 排查判断方式

如果后续仍观察到 storage RSS 缓慢上涨，优先看：

1. `total.dirty_bytes`
   - 如果持续上涨，说明待释放 WAL buffer 增多。

2. `parts[].dirty_bytes`
   - 找到具体哪个 space/part 贡献最大。

3. `parts[].reader_refs`
   - 如果 dirty 高且 reader_refs 长期不为 0，重点排查 WAL iterator 生命周期。

4. `parts[].gc_deleted_bytes` 和 `parts[].gc_count`
   - 如果 dirty 高但 GC 不增长，说明 GC 触发不足或被引用阻塞。

5. `parts[].heartbeat_empty_logs`
   - 验证空 heartbeat log 是否仍在某些分片持续产生。

## 本机验证结果

验证时间：2026-07-04。

- 编译日志：
  - `task-wal/build_20260704_wal_stats_metrics_retry.log`
  - 结果：成功。
- 安装日志：
  - `task-wal/install_20260704_wal_stats_metrics.log`
  - 结果：核心二进制已安装；后续安装默认配置文件权限修改失败，报 `Operation not permitted`，该问题属于 `/usr/local/nebula/etc` 权限问题。
- 启动日志：
  - `task-wal/runtime_start_20260704_wal_stats_metrics.log`
  - 启动前已执行 `ulimit -n 65536`。
  - `metad`、`storaged`、`graphd` 均运行中。
- 接口验证日志：
  - `task-wal/runtime_wal_stats_20260704.log`

全量查询：

```bash
curl --noproxy '*' -s 'http://127.0.0.1:19779/wal_stats'
```

返回汇总中的关键值：

- `total.parts=84`
- `total.active_bytes=1344`
- `total.active_nodes=84`
- `total.dirty_bytes=0`
- `total.dirty_nodes=0`
- `total.reader_refs=0`
- `total.gc_count=0`
- `total.gc_deleted_bytes=0`
- `total.heartbeat_empty_logs=84`

指定分片查询：

```bash
curl --noproxy '*' -s 'http://127.0.0.1:19779/wal_stats?space=1&part=1'
```

结果：只返回 `space_id=1`、`part_id=1` 的本地分片明细。

异常参数查询：

```bash
curl --noproxy '*' -s -o - -w '\nHTTP_CODE=%{http_code}\n' 'http://127.0.0.1:19779/wal_stats?space=abc'
```

结果：返回 `HTTP_CODE=400`。

当前验证环境下 `dirty_bytes=0`、`reader_refs=0`，说明没有观察到等待 GC 的 WAL buffer 堆积；`heartbeat_empty_logs=84` 是服务启动后每个本地 leader partition 在当前任期尚未提交日志时产生的一次空 log，符合本次修复后“仅任期初补一次空 log，不再每次 heartbeat 追加空 log”的预期。
