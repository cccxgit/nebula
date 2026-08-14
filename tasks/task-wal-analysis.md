# task-wal 分析记录

> **更正（2026-08-14）**：本文是短时指标验证记录，只证明了 AtomicLogBuffer 与空 Raft 日志同步增长，不能据此把它判为约 5 天 RSS 回落的唯一主因。长跑 RocksDB 日志显示，周期回落由 commit-key 版本填满 memtable 后的 `Write Buffer Full` flush 触发；AtomicLogBuffer 是长期叠加项。最终报告见 `tasks/task-wal-root-cause-analysis.md`。

## 短时验证结论

本次只增加观测指标，不修改 WAL 内存持续上涨行为本身。

新增指标确认了来源之一：无业务写入时，Raft 心跳空日志写入 WAL 后，`AtomicLogBuffer` 持有的内存 Node 会持续增加。该短测没有区分同一空日志造成的 RocksDB memtable 增长，不能单独判定主要来源。

关键源码路径：

1. `src/kvstore/raftex/RaftPart.cpp`
   - `RaftPart::sendHeartbeat()` 在没有 replication 任务时会通过 `appendLogAsync()` 追加空 normal log。
2. `src/kvstore/wal/FileBasedWal.cpp`
   - `FileBasedWal::appendLogInternal()` 写 WAL 文件后调用 `logBuffer_->push()`。
3. `src/kvstore/wal/AtomicLogBuffer.cpp`
   - `AtomicLogBuffer::push()` 在当前节点为空、满或 deleted 时分配新的 `Node`。
   - `Node` 中保存固定数量 log record，GC 依赖 iterator/ref release 路径触发。

因此在 5 个 graph space、每个 20 partition、replica factor 3 的场景下，每个 storaged 拥有 100 个 partition/WAL buffer。即使没有业务写入，Raft 心跳空日志也会持续进入 WAL buffer，令 buffer Node 增长；完整归因还需同时计入 RocksDB commit-key/memtable 路径。

## 新增指标

新增位置：

- `src/kvstore/wal/AtomicLogBuffer.h`
- `src/kvstore/wal/AtomicLogBuffer.cpp`
- `src/storage/http/StorageHttpStatsHandler.cpp`

新增 `/rocksdb_stats` 指标：

- `wal.log_buffer.instances`
- `wal.log_buffer.capacity_bytes`
- `wal.log_buffer.valid_payload_bytes`
- `wal.log_buffer.estimated_held_bytes`
- `wal.log_buffer.nodes`
- `wal.log_buffer.dirty_nodes`
- `wal.log_buffer.refs`
- `wal.log_buffer.max_valid_payload_bytes`
- `wal.log_buffer.max_estimated_held_bytes`
- `wal.log_buffer.max_nodes`

示例：

```bash
curl --noproxy '*' \
  'http://127.0.0.1:19779/rocksdb_stats?stats=wal.log_buffer.instances,wal.log_buffer.estimated_held_bytes,wal.log_buffer.nodes&format=json'
```

## 验证环境

按仓库任务要求，启动前执行了：

```bash
ulimit -n 65536
```

编译安装：

```bash
cmake -DCMAKE_INSTALL_PREFIX=/usr/local/nebula -DENABLE_TESTING=OFF -DCMAKE_BUILD_TYPE=Debug ..
make -j10
make install
```

本次验证使用临时 task-wal 配置：

- `/usr/local/nebula/etc/nebula-metad-task-wal.conf`
- `/usr/local/nebula/etc/nebula-graphd-task-wal.conf`
- `/usr/local/nebula/etc/nebula-storaged-task-wal-1.conf`
- `/usr/local/nebula/etc/nebula-storaged-task-wal-2.conf`
- `/usr/local/nebula/etc/nebula-storaged-task-wal-3.conf`

storaged HTTP 端口：

- storage1: `19779`
- storage2: `19879`
- storage3: `19979`

创建验证空间：

- `wal_space_1` 到 `wal_space_5`
- 每个 `partition_num=20, replica_factor=3, vid_type=FIXED_STRING(32)`

最终 `SHOW HOSTS` 显示 3 个 storaged 均 ONLINE，每个 storaged 分布 5 个 space、共 100 个 partition。

## 关键采样

创建 space 前，3 个 storaged 均无 WAL buffer：

```text
wal.log_buffer.instances = 0
wal.log_buffer.valid_payload_bytes = 0
wal.log_buffer.estimated_held_bytes = 0
wal.log_buffer.nodes = 0
```

创建 5 个 space 后，首次采样：

```text
wal.log_buffer.instances = 100
wal.log_buffer.nodes = 100
wal.log_buffer.estimated_held_bytes ~= 399 KB
wal.log_buffer.valid_payload_bytes ~= 79 KB
```

随后无业务写入，仅等待 raft 心跳，连续采样显示指标单调增长：

```text
21:14:40 nodes = 201, estimated_held_bytes ~= 837 KB
21:15:10 nodes = 300, estimated_held_bytes ~= 1237 KB
21:15:40 nodes = 400, estimated_held_bytes ~= 1639 KB
```

同一时间段 storaged RSS 也从约 69-70 MB 增长到约 72 MB。

## 运行日志

关键命令、启动状态、错误修正和采样输出记录在：

- `tasks/task-wal-run.log`

验证过程中遇到并处理的问题：

1. 初始 storaged 读取了默认 `/usr/local/nebula/cluster.id`，导致向 task-wal metad 心跳时报 `Wrong cluster`。
   - 处理：为三个 task-wal storaged 配置追加独立 `--cluster_id_path=data/task-wal-cluster.id`。
2. 最初规划的多 storaged 端口与 admin/raft 端口有冲突。
   - 处理：改为 storage1 `9779/9778/9780`，storage2 `9879/9878/9880`，storage3 `9979/9978/9980`。

## 当前状态

> 状态更新（2026-08-14）：以下是初次短测结束时的历史状态；当前已没有 task-wal 或默认 Nebula 进程运行。

初次短测结束时，task-wal 临时集群仍在运行：

- metad: task-wal config, port `9559`
- graphd: task-wal config, port `9669`
- storaged:
  - service `9779`, http `19779`
  - service `9879`, http `19879`
  - service `9979`, http `19979`

默认配置服务在安装前已停止，未自动恢复。
