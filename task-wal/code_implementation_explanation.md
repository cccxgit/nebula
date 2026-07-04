# WAL 内存上涨修复代码实现解读文档

## 1. 改动概览

本次代码实现分为三部分：

1. 修复 Raft heartbeat 稳定任期内持续追加空 WAL log 的问题。
2. 增加全局 heartbeat 计数指标。
3. 增加图空间和 partition 级别 WAL buffer 观测接口 `/wal_stats`。

核心修改文件：

- `src/kvstore/raftex/RaftPart.cpp`
- `src/kvstore/raftex/RaftPart.h`
- `src/kvstore/stats/KVStats.cpp`
- `src/kvstore/stats/KVStats.h`
- `src/kvstore/wal/AtomicLogBuffer.cpp`
- `src/kvstore/wal/AtomicLogBuffer.h`
- `src/kvstore/NebulaStore.cpp`
- `src/kvstore/NebulaStore.h`
- `src/storage/http/StorageHttpWalStatsHandler.cpp`
- `src/storage/http/StorageHttpWalStatsHandler.h`
- `src/storage/StorageServer.cpp`
- `src/storage/CMakeLists.txt`

## 2. RaftPart heartbeat 修复

### 2.1 修改位置

文件：

- `src/kvstore/raftex/RaftPart.cpp`
- `src/kvstore/raftex/RaftPart.h`

### 2.2 核心逻辑

`RaftPart::sendHeartbeat()` 入口增加全局计数：

```cpp
stats::StatsManager::addValue(kNumRaftHeartbeat);
```

随后在 `raftLock_` 保护下读取当前 Raft 状态：

```cpp
needAppendEmptyLog = status_ == Status::RUNNING && role_ == Role::LEADER && !commitInThisTerm_;
```

只有在 `needAppendEmptyLog == true` 且当前没有正在复制日志时，才追加空 log：

```cpp
if (needAppendEmptyLog && !replicatingLogs_.load(std::memory_order_acquire)) {
  stats::StatsManager::addValue(kNumRaftHeartbeatEmptyLog);
  numHeartbeatEmptyLogs_.fetch_add(1, std::memory_order_relaxed);
  folly::via(executor_.get(), [this] {
    std::string log = "";
    appendLogAsync(clusterId_, LogType::NORMAL, std::move(log));
  });
} else {
  stats::StatsManager::addValue(kNumRaftHeartbeatWithoutEmptyLog);
}
```

### 2.3 行为变化

修复前：

- 稳定 leader 任期内，只要没有正在复制业务日志，每轮 heartbeat 都可能追加空 WAL log。

修复后：

- leader 当前任期尚未提交日志时，仍允许追加空 log。
- 当前任期已经提交日志后，heartbeat 不再追加空 WAL log。
- heartbeat RPC 仍然正常发送，不影响 follower 保活和 lease response 更新。

### 2.4 新增分片级计数

`RaftPart.h` 新增：

```cpp
std::atomic<uint64_t> numHeartbeatEmptyLogs_{0};
```

并提供 accessor：

```cpp
uint64_t numHeartbeatEmptyLogs() const;
```

该计数用于 `/wal_stats` 返回 partition 级别 heartbeat 空 log 次数。

同时新增：

```cpp
const char* roleStr() const;
```

用于 `/wal_stats` 输出 Raft 角色。

## 3. 全局 StatsManager 指标

### 3.1 修改位置

文件：

- `src/kvstore/stats/KVStats.h`
- `src/kvstore/stats/KVStats.cpp`

### 3.2 新增指标

```cpp
kNumRaftHeartbeat
kNumRaftHeartbeatEmptyLog
kNumRaftHeartbeatWithoutEmptyLog
```

注册名称：

```cpp
num_raft_heartbeat
num_raft_heartbeat_empty_log
num_raft_heartbeat_without_empty_log
```

注册方式：

```cpp
stats::StatsManager::registerStats(..., "rate, sum");
```

### 3.3 使用方式

这些指标用于判断修复是否生效：

- `num_raft_heartbeat`：heartbeat 总量。
- `num_raft_heartbeat_empty_log`：heartbeat 触发追加空 WAL log 的次数。
- `num_raft_heartbeat_without_empty_log`：heartbeat 未追加空 WAL log 的次数。

修复后，稳定 leader 任期内应主要增长 `num_raft_heartbeat_without_empty_log`。

## 4. AtomicLogBuffer 统计能力

### 4.1 修改位置

文件：

- `src/kvstore/wal/AtomicLogBuffer.h`
- `src/kvstore/wal/AtomicLogBuffer.cpp`

### 4.2 新增 Stats 结构

`AtomicLogBuffer.h` 新增：

```cpp
struct Stats {
  int64_t activeBytes;
  int64_t dirtyBytes;
  int64_t activeNodes;
  int64_t dirtyNodes;
  int64_t readerRefs;
  int64_t gcCount;
  int64_t gcDeletedNodes;
  int64_t gcDeletedBytes;
  LogID firstLogId;
  LogID lastLogId;
};
```

并新增：

```cpp
Stats stats() const;
```

### 4.3 统计字段来源

- `activeBytes`：来自原有 `size_`。
- `dirtyNodes`：来自原有 `dirtyNodes_`。
- `readerRefs`：来自原有 `refs_`。
- `firstLogId`：来自 `firstLogId()`。
- `lastLogId`：来自 `lastLogId()`。

新增原子字段：

```cpp
dirtyBytes_
activeNodes_
gcCount_
gcDeletedNodes_
gcDeletedBytes_
```

### 4.4 push 路径维护 active 和 dirty

新增 node 时：

```cpp
activeNodes_.fetch_add(1, std::memory_order_relaxed);
```

当 tail node 被标记删除时：

```cpp
size_.fetch_sub(tail->size_, std::memory_order_relaxed);
dirtyBytes_.fetch_add(tail->size_, std::memory_order_relaxed);
activeNodes_.fetch_sub(1, std::memory_order_relaxed);
dirtyNodes_.fetch_add(1, std::memory_order_release);
```

含义：

- 从 active 中扣除旧 node。
- 把旧 node 计入 dirty。
- 等待后续 GC 真正释放。

### 4.5 reset 路径维护 dirty

`reset()` 会将当前链表中未删除的 node 标记为 deleted。本次增加字节和 active node 计数维护：

```cpp
dirtyBytes_.fetch_add(bytes, std::memory_order_relaxed);
activeNodes_.fetch_sub(count, std::memory_order_relaxed);
dirtyNodes_.fetch_add(count, std::memory_order_release);
```

### 4.6 releaseRef 路径维护 GC 指标

GC 真正删除 dirty node 时，累计释放数量和字节：

```cpp
gcCount_.fetch_add(1, std::memory_order_relaxed);
gcDeletedNodes_.fetch_add(deletedNodes, std::memory_order_relaxed);
gcDeletedBytes_.fetch_add(deletedBytes, std::memory_order_relaxed);
dirtyBytes_.fetch_sub(deletedBytes, std::memory_order_relaxed);
```

含义：

- `dirtyBytes` 表示当前待释放规模。
- `gcDeletedBytes` 表示历史累计释放规模。
- `gcCount` 表示 GC 执行次数。

## 5. NebulaStore 聚合 WAL 统计

### 5.1 修改位置

文件：

- `src/kvstore/NebulaStore.h`
- `src/kvstore/NebulaStore.cpp`

### 5.2 新增接口

```cpp
folly::dynamic walStats(std::optional<GraphSpaceID> spaceId = std::nullopt,
                        std::optional<PartitionID> partId = std::nullopt);
```

### 5.3 实现逻辑

`NebulaStore::walStats()` 使用 `lock_` 的读锁遍历本地 `spaces_` 和 `parts_`：

1. 如果传入 `spaceIdFilter`，只统计指定图空间。
2. 如果传入 `partIdFilter`，只统计指定 partition。
3. 对每个 part 调用：
   - `part->wal()->buffer()->stats()`
   - `part->numHeartbeatEmptyLogs()`
   - `part->roleStr()`
   - `part->isLeader()`
4. 同时构造 `total` 汇总和 `parts` 明细数组。

返回类型使用 `folly::dynamic`，方便 HTTP handler 直接转 JSON。

## 6. HTTP 接口实现

### 6.1 修改位置

文件：

- `src/storage/http/StorageHttpWalStatsHandler.h`
- `src/storage/http/StorageHttpWalStatsHandler.cpp`
- `src/storage/StorageServer.cpp`
- `src/storage/CMakeLists.txt`

### 6.2 handler 行为

`StorageHttpWalStatsHandler::onRequest()`：

- 只支持 GET。
- 解析 query 参数 `space` 和 `part`。
- 参数非法时设置 `E_ILLEGAL_ARGUMENT`。

`StorageHttpWalStatsHandler::onEOM()`：

- 非 GET 返回 405。
- 参数非法返回 400。
- 如果 `kv_` 不是 `NebulaStore`，返回 500。
- 正常情况下调用 `nebulaStore->walStats(spaceId_, partId_)`，并通过 `folly::toPrettyJson(stats)` 返回 JSON。

### 6.3 路由注册

`StorageServer::initWebService()` 新增：

```cpp
router.get("/wal_stats").handler([this](web::PathParams&&) {
  return new storage::StorageHttpWalStatsHandler(kvstore_.get());
});
```

`src/storage/CMakeLists.txt` 增加：

```cmake
http/StorageHttpWalStatsHandler.cpp
```

## 7. 接口返回示例

全量查询：

```bash
curl --noproxy '*' -s 'http://127.0.0.1:19779/wal_stats'
```

返回结构示例：

```json
{
  "parts": [
    {
      "active_bytes": 16,
      "active_nodes": 1,
      "dirty_bytes": 0,
      "dirty_nodes": 0,
      "first_log_id": 4,
      "gc_count": 0,
      "gc_deleted_bytes": 0,
      "gc_deleted_nodes": 0,
      "heartbeat_empty_logs": 1,
      "is_leader": true,
      "last_log_id": 4,
      "part_id": 1,
      "reader_refs": 0,
      "role": "Leader",
      "space_id": 1
    }
  ],
  "total": {
    "active_bytes": 1344,
    "active_nodes": 84,
    "dirty_bytes": 0,
    "dirty_nodes": 0,
    "gc_count": 0,
    "gc_deleted_bytes": 0,
    "gc_deleted_nodes": 0,
    "heartbeat_empty_logs": 84,
    "parts": 84,
    "reader_refs": 0
  }
}
```

## 8. 验证结果

编译验证：

- 日志：`task-wal/build_20260704_wal_stats_metrics_retry.log`
- 结果：成功。

安装验证：

- 日志：`task-wal/install_20260704_wal_stats_metrics.log`
- 结果：核心二进制已安装；后续配置文件权限修改失败，属于安装目录权限问题。

运行验证：

- 日志：`task-wal/runtime_start_20260704_wal_stats_metrics.log`
- 结果：`metad`、`storaged`、`graphd` 均运行中。

接口验证：

- 日志：`task-wal/runtime_wal_stats_20260704.log`
- 全量查询返回 `total.parts=84`。
- 返回 `total.dirty_bytes=0`、`total.reader_refs=0`。
- 指定 `space=1&part=1` 能正确过滤到单个分片。
- 非法参数 `space=abc` 返回 HTTP 400。

## 9. 实现注意事项

1. `/wal_stats` 是本地 storaged 视角，不是集群聚合视角。
2. `active_bytes` 和 `dirty_bytes` 是 WAL record 逻辑字节数，不等同于进程 RSS。
3. `heartbeat_empty_logs` 是进程内计数，进程重启后会重新开始。
4. `AtomicLogBuffer::stats()` 使用 relaxed 原子读取，适合观测和排查，不用于强一致业务判断。
5. 高基数 per-part 指标没有注册到 `StatsManager`，避免监控体系自身放大内存和指标压力。
