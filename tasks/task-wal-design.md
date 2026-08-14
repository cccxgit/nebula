# task-wal 方案设计

## 1. 背景与问题

生产环境长期运行时，storaged 进程内存会持续上涨。已有观察现象：

- 图空间数量越多，内存上涨越快。
- 分片数越多，内存上涨越快。
- 即使多数图空间没有业务写入，内存也会持续上涨。
- 典型测试场景：3 个 storaged，5 个图空间，每个图空间 20 个 partition，3 副本。

本任务的目标不是直接修复内存上涨，而是先找到可疑源码位置，并开发监控指标验证判断是否正确。确认后再决定是否进入修复阶段。

## 2. 术语说明

### 2.1 Nebula Raft WAL

Nebula storage 每个 partition replica 都由 raft 管理。raft 写入日志时会进入 Nebula 自己的 WAL 实现，核心类是：

- `RaftPart`
- `FileBasedWal`
- `AtomicLogBuffer`

本文讨论的内存上涨主要指 `AtomicLogBuffer` 在进程堆内持有的 WAL log buffer 内存。

### 2.2 RocksDB WAL

RocksDB 自己也有 WAL，这是 RocksDB 写入引擎使用的日志，和 Nebula Raft WAL 是两套不同机制。

因此排查时要区分：

- Nebula Raft WAL：`src/kvstore/wal/FileBasedWal.*`、`AtomicLogBuffer.*`
- RocksDB WAL：由 RocksDB options 控制，例如 `rocksdb_wal_dir`

### 2.3 图空间、分片、副本和 WAL buffer 的关系

对于一个 storaged：

- 每个本地 partition replica 对应一个 raft part。
- 每个 raft part 对应一个 `FileBasedWal`。
- 每个 `FileBasedWal` 内部有一个 `AtomicLogBuffer`。

所以在 5 个图空间、每个图空间 20 分片、3 副本、3 个 storaged 的均匀场景下，每个 storaged 本地大约持有：

```text
5 spaces * 20 partitions = 100 partition replicas
```

因此单个 storaged 上大约有 100 个 `AtomicLogBuffer` 实例。

## 3. 已确认的源码路径

本次已确认的关键链路如下：

```text
RaftPart::sendHeartbeat()
  -> appendLogAsync()
  -> FileBasedWal::appendLogInternal()
  -> AtomicLogBuffer::push()
  -> 必要时分配新的 Node
```

关键源码位置：

- `src/kvstore/raftex/RaftPart.cpp`
  - `RaftPart::sendHeartbeat()` 在当前没有 replication 任务时，会追加一条空 normal log。
- `src/kvstore/wal/FileBasedWal.cpp`
  - `FileBasedWal::appendLogInternal()` 写 WAL 文件后，会调用 `logBuffer_->push()`。
- `src/kvstore/wal/AtomicLogBuffer.cpp`
  - `AtomicLogBuffer::push()` 在 head 为空、当前 node 写满或已标记 deleted 时，会分配新的 `Node`。

这个链路解释了为什么“没有业务写入”时仍然可能持续增长：raft 心跳也会周期性追加空日志，这些日志会进入 WAL buffer。

## 4. 已完成方案：storaged 全量聚合指标

### 4.1 设计目标

第一阶段指标用于确认 storaged 内部 WAL buffer 是否随着时间持续增长。

设计要求：

- 不修复源码行为。
- 不改变 WAL 写入、读取、GC 语义。
- 指标尽量贴近 `AtomicLogBuffer` 当前真实持有状态。
- 可通过现有 HTTP stats 接口获取。

### 4.2 改动位置

已完成改动涉及：

- `src/kvstore/wal/AtomicLogBuffer.h`
- `src/kvstore/wal/AtomicLogBuffer.cpp`
- `src/storage/http/StorageHttpStatsHandler.cpp`

### 4.3 采集方式

`AtomicLogBuffer` 增加进程内静态 registry：

- 构造 `AtomicLogBuffer` 时注册当前实例。
- 析构 `AtomicLogBuffer` 时注销当前实例。
- registry 使用 mutex 保护。
- 每个 buffer 增加 `nodes_` 原子计数。
- 分配新 `Node` 时 `nodes_++`。
- 删除 dirty node 时 `nodes_--`。

HTTP stats 请求到来时调用：

```cpp
AtomicLogBuffer::collectMetrics()
```

该方法遍历当前 storaged 进程内所有 `AtomicLogBuffer`，聚合出 WAL buffer 指标。

### 4.4 已完成指标列表

当前已输出 10 个指标：

| 指标名 | 含义 | 聚合方式 |
| --- | --- | --- |
| `wal.log_buffer.instances` | 当前 storaged 进程内 `AtomicLogBuffer` 实例数 | 对所有 buffer 计数 |
| `wal.log_buffer.capacity_bytes` | 所有 buffer 的逻辑容量上限汇总 | 求和 |
| `wal.log_buffer.valid_payload_bytes` | 所有 buffer 当前有效 log payload 字节数 | 求和 |
| `wal.log_buffer.estimated_held_bytes` | 估算 WAL buffer 当前持有内存 | 求和 |
| `wal.log_buffer.nodes` | 所有 buffer 当前持有的 `Node` 数量 | 求和 |
| `wal.log_buffer.dirty_nodes` | 已标记删除但尚未释放的 `Node` 数量 | 求和 |
| `wal.log_buffer.refs` | 当前 iterator/reference 引用数 | 求和 |
| `wal.log_buffer.max_valid_payload_bytes` | 单个 buffer 中最大的有效 payload 字节数 | 取最大 |
| `wal.log_buffer.max_estimated_held_bytes` | 单个 buffer 中最大的估算持有内存 | 取最大 |
| `wal.log_buffer.max_nodes` | 单个 buffer 中最大的 `Node` 数量 | 取最大 |

### 4.5 estimated_held_bytes 计算方式

当前估算公式：

```text
estimated_held_bytes = valid_payload_bytes + nodes * sizeof(AtomicLogBuffer::Node)
```

说明：

- `valid_payload_bytes` 来自 `AtomicLogBuffer::size_`。
- `nodes` 来自新增的 `nodes_`。
- `sizeof(Node)` 反映每个 node 自身结构占用。
- 该值不包含 allocator 元数据、内存碎片、对象对齐之外的额外开销。

因此该指标不是精确 RSS，但能反映 WAL buffer 自身增长趋势。

### 4.6 当前指标粒度

当前指标是“单个 storaged 进程内所有图空间汇总”。

注意：

- 它不是单个图空间粒度。
- 它不是自动跨 storaged 聚合后的集群总量。
- 若要得到真正集群总量，需要分别请求每个 storaged，再由外部系统聚合。

聚合规则：

- `instances`、`capacity_bytes`、`valid_payload_bytes`、`estimated_held_bytes`、`nodes`、`dirty_nodes`、`refs`：跨 storaged 求和。
- `max_valid_payload_bytes`、`max_estimated_held_bytes`、`max_nodes`：跨 storaged 取最大值。

### 4.7 指标获取方式

通过 storaged HTTP `/rocksdb_stats` 获取。

当前 task-wal 验证集群端口：

```text
storage1 http: 19779
storage2 http: 19879
storage3 http: 19979
```

获取关键指标：

```bash
curl --noproxy '*' \
  'http://127.0.0.1:19779/rocksdb_stats?stats=wal.log_buffer.instances,wal.log_buffer.estimated_held_bytes,wal.log_buffer.nodes&format=json'
```

分别获取三个 storaged：

```bash
curl --noproxy '*' \
  'http://127.0.0.1:19779/rocksdb_stats?stats=wal.log_buffer.instances,wal.log_buffer.estimated_held_bytes,wal.log_buffer.nodes&format=json'

curl --noproxy '*' \
  'http://127.0.0.1:19879/rocksdb_stats?stats=wal.log_buffer.instances,wal.log_buffer.estimated_held_bytes,wal.log_buffer.nodes&format=json'

curl --noproxy '*' \
  'http://127.0.0.1:19979/rocksdb_stats?stats=wal.log_buffer.instances,wal.log_buffer.estimated_held_bytes,wal.log_buffer.nodes&format=json'
```

如果环境没有 HTTP 代理，可以去掉 `--noproxy '*'`。

### 4.8 已完成验证

验证环境：

- 3 个 storaged。
- 5 个图空间。
- 每个图空间 20 partition。
- replica factor = 3。
- 无业务写入，仅等待 raft 心跳。

创建图空间前：

```text
wal.log_buffer.instances = 0
wal.log_buffer.valid_payload_bytes = 0
wal.log_buffer.estimated_held_bytes = 0
wal.log_buffer.nodes = 0
```

创建图空间后：

```text
wal.log_buffer.instances = 100
wal.log_buffer.nodes = 100
wal.log_buffer.estimated_held_bytes ~= 399 KB
wal.log_buffer.valid_payload_bytes ~= 79 KB
```

随后无业务写入，仅等待心跳：

```text
21:14:40 nodes = 201, estimated_held_bytes ~= 837 KB
21:15:10 nodes = 300, estimated_held_bytes ~= 1237 KB
21:15:40 nodes = 400, estimated_held_bytes ~= 1639 KB
```

结论：

- `instances = 100` 符合每个 storaged 持有 5 * 20 个 partition replica 的预期。
- `nodes` 和 `estimated_held_bytes` 在无业务写入时持续增长。
- storaged RSS 同期也增长。
- 说明 WAL in-memory buffer 是当前现象的有效观测点。

详细验证记录见：

- `tasks/task-wal-analysis.md`
- `tasks/task-wal-run.log`

## 5. 待开发方案：图空间粒度指标

### 5.1 目标

保留当前全量聚合指标，同时新增图空间粒度指标，用于精准判断哪些图空间的 WAL buffer 内存持续上涨。

明确不做：

- 不开发 partition 粒度指标。
- 不修复内存上涨问题本身。
- 不在 storage HTTP handler 中强依赖 meta 查询图空间名称。

### 5.2 为什么可行

当前构造链路中已经天然具备 `spaceId`：

```text
RaftPart(spaceId, partId)
  -> FileBasedWalInfo.spaceId_
  -> FileBasedWal.spaceId_
  -> AtomicLogBuffer
```

已有源码基础：

- `RaftPart` 构造函数已经接收 `GraphSpaceID spaceId`。
- `FileBasedWalInfo` 已经包含 `spaceId_` 和 `partId_`。
- `FileBasedWal` 构造函数已经保存 `spaceId_`。

当前只缺少最后一步：创建 `AtomicLogBuffer` 时没有把 `spaceId_` 传进去。

### 5.3 推荐实现方案

#### 5.3.1 AtomicLogBuffer 保存 spaceId

新增成员：

```cpp
GraphSpaceID spaceId_{0};
```

调整工厂方法：

```cpp
static std::shared_ptr<AtomicLogBuffer> instance(
    int32_t capacity = 8 * 1024 * 1024,
    GraphSpaceID spaceId = 0);
```

调整构造函数：

```cpp
AtomicLogBuffer(int32_t capacity, GraphSpaceID spaceId);
```

保留默认 `spaceId = 0`，用于兼容测试或非标准调用路径。

#### 5.3.2 FileBasedWal 传递 spaceId

当前代码类似：

```cpp
logBuffer_ = AtomicLogBuffer::instance(policy_.bufferSize);
```

建议改为：

```cpp
logBuffer_ = AtomicLogBuffer::instance(policy_.bufferSize, spaceId_);
```

这样每个 WAL buffer 都知道自己属于哪个图空间。

#### 5.3.3 新增按 space 聚合方法

保留已有：

```cpp
static AtomicLogBufferMetrics collectMetrics();
```

新增：

```cpp
using AtomicLogBufferMetricsBySpace =
    std::map<GraphSpaceID, AtomicLogBufferMetrics>;

static AtomicLogBufferMetricsBySpace collectMetricsBySpace();
```

按 space 聚合时，遍历 registry：

```text
for each buffer in registry:
  metricsBySpace[buffer->spaceId_] += buffer metrics
```

单个 space 内聚合规则和当前全量指标一致：

- 求和：`instances`、`capacityBytes`、`validPayloadBytes`、`estimatedHeldBytes`、`nodes`、`dirtyNodes`、`refs`
- 取最大：`maxValidPayloadBytes`、`maxEstimatedHeldBytes`、`maxNodes`

#### 5.3.4 HTTP 输出命名

推荐使用扁平指标名，保持和现有 `/rocksdb_stats` 风格一致：

```text
wal.log_buffer.space.<space_id>.instances
wal.log_buffer.space.<space_id>.capacity_bytes
wal.log_buffer.space.<space_id>.valid_payload_bytes
wal.log_buffer.space.<space_id>.estimated_held_bytes
wal.log_buffer.space.<space_id>.nodes
wal.log_buffer.space.<space_id>.dirty_nodes
wal.log_buffer.space.<space_id>.refs
wal.log_buffer.space.<space_id>.max_valid_payload_bytes
wal.log_buffer.space.<space_id>.max_estimated_held_bytes
wal.log_buffer.space.<space_id>.max_nodes
```

示例：

```text
wal.log_buffer.space.12.instances
wal.log_buffer.space.12.estimated_held_bytes
wal.log_buffer.space.12.nodes
wal.log_buffer.space.27.estimated_held_bytes
```

选择 `space_id` 而不是 space name 的原因：

- storage 本地 raft/WAL 路径天然持有的是 `spaceId`。
- 在 HTTP stats handler 内查询 meta 获取 space name 会引入额外依赖和失败场景。
- `spaceId` 稳定、唯一，排查时可通过 console 或 meta 信息映射到名称。

### 5.4 推荐同步增强 stats 过滤

当前 `/rocksdb_stats?stats=...` 是精确匹配。例如：

```text
stats=wal.log_buffer.nodes
```

只能匹配完整指标名。

如果新增图空间指标后，指标名会包含动态 `space_id`，建议增强过滤逻辑，兼容简单通配：

```text
stats=wal.log_buffer.space.*
stats=wal.log_buffer.space.12.*
```

推荐规则：

- 不带 `*`：保持原有精确匹配。
- 以 `*` 结尾：按前缀匹配。
- 不支持复杂 glob 或正则，避免语义过重。

示例：

```bash
curl --noproxy '*' \
  'http://127.0.0.1:19779/rocksdb_stats?stats=wal.log_buffer.space.*&format=json'
```

查询某个 space：

```bash
curl --noproxy '*' \
  'http://127.0.0.1:19779/rocksdb_stats?stats=wal.log_buffer.space.12.*&format=json'
```

如果不增强过滤，也可以请求全量 `/rocksdb_stats?format=json` 后使用 `jq` 外部过滤。但对生产排查不够方便。

### 5.5 图空间粒度指标解释方式

假设某个 storaged 返回：

```text
wal.log_buffer.space.12.instances = 20
wal.log_buffer.space.12.nodes = 80
wal.log_buffer.space.12.estimated_held_bytes = 327680
wal.log_buffer.space.27.instances = 20
wal.log_buffer.space.27.nodes = 300
wal.log_buffer.space.27.estimated_held_bytes = 1228800
```

可以判断：

- 当前 storaged 上 space 12 和 space 27 都有 20 个本地 partition replica。
- space 27 的 WAL buffer node 明显更多。
- space 27 的 WAL buffer 估算持有内存更高。
- 如果多轮采样中 space 27 持续增长，而其他 space 稳定，则 space 27 是重点分析对象。

### 5.6 图空间粒度的集群聚合

单个 storaged 返回的是本进程视角。要得到集群内每个图空间的总量，需要对所有 storaged 按 `space_id` 聚合。

聚合规则：

```text
同一 space_id:
  instances: 求和
  capacity_bytes: 求和
  valid_payload_bytes: 求和
  estimated_held_bytes: 求和
  nodes: 求和
  dirty_nodes: 求和
  refs: 求和
  max_valid_payload_bytes: 取最大
  max_estimated_held_bytes: 取最大
  max_nodes: 取最大
```

### 5.7 待开发验证计划

图空间粒度指标开发后，建议按以下步骤验证：

1. 编译安装。

```bash
cmake -DCMAKE_INSTALL_PREFIX=/usr/local/nebula -DENABLE_TESTING=OFF -DCMAKE_BUILD_TYPE=Debug ..
make -j10
make install
```

2. 启动前设置文件句柄上限。

```bash
ulimit -n 65536
```

3. 启动 metad、graphd、3 个 storaged。

4. 创建 5 个图空间，每个图空间 20 partition、3 副本。

5. 获取每个 storaged 的全量聚合指标，确认旧指标仍然可用。

6. 获取每个 storaged 的图空间粒度指标。

```bash
curl --noproxy '*' \
  'http://127.0.0.1:19779/rocksdb_stats?stats=wal.log_buffer.space.*&format=json'
```

7. 期望现象：

```text
每个 storaged:
  每个 space 的 wal.log_buffer.space.<space_id>.instances ~= 20
  所有 space 的 instances 汇总 ~= wal.log_buffer.instances
```

8. 连续采样，确认：

```text
sum(space.*.nodes) == wal.log_buffer.nodes
sum(space.*.estimated_held_bytes) == wal.log_buffer.estimated_held_bytes
max(space.*.max_nodes) == wal.log_buffer.max_nodes
```

9. 若某个图空间增长更快，可通过该 space 的 `nodes` 和 `estimated_held_bytes` 观察趋势。

### 5.8 风险和注意事项

1. 指标名数量会随图空间数增加。
   - 每个 space 输出 10 个指标。
   - 如果 storaged 上有 100 个 space，则新增约 1000 个指标项。

2. registry 遍历有 mutex。
   - 当前只在 HTTP stats 请求时遍历。
   - 正常采样频率下影响较小。

3. `estimated_held_bytes` 是估算值。
   - 不等于 RSS。
   - 不包含 allocator 额外开销和 page cache。

4. 默认 `spaceId = 0` 的 buffer 需要关注。
   - 正常 storage graph space 应为非 0。
   - 若出现 `space.0`，需要确认是否来自测试、默认 space 或未传递 spaceId 的路径。

5. 不输出 partition 粒度。
   - 这是刻意选择，避免指标爆炸。
   - 本阶段目标是定位“哪个图空间”导致上涨。

## 6. WAL 内存占用和 data_path 的关系

### 6.1 当前指标统计的是进程内 WAL buffer

当前 `wal.log_buffer.*` 指标统计的是 `AtomicLogBuffer` 在 storaged 进程内持有的内存估算。

这部分内存主要受以下因素影响：

- 本地 partition replica 数量。
- raft 心跳或业务写入追加的 WAL log 数量。
- `wal_buffer_size`。
- WAL buffer GC 触发和回收情况。
- iterator/reference 是否阻止 dirty node 回收。

这部分不直接由 RocksDB 的 `data_path` 决定。

### 6.2 Nebula Raft WAL 文件路径

Nebula Raft WAL 文件路径和 `data_path`、`wal_path` 有关。

storaged 参数：

- `--data_path`
- `--wal_path`

如果 `--wal_path` 为空，Nebula Raft WAL 默认使用和 RocksDB data 相邻的路径。

如果 `--wal_path` 非空，Nebula Raft WAL 会写到独立 wal path 下。

这影响的是 WAL 文件落盘位置，不直接决定 `AtomicLogBuffer` 的进程堆内存大小。

### 6.3 RocksDB WAL 路径

RocksDB 自己的 WAL 路径由 `rocksdb_wal_dir` 控制：

- 如果 `--rocksdb_wal_dir` 非空，RocksDB WAL 放到该目录下的 `rocksdb_wal/<space_id>`。
- 如果为空，RocksDB WAL 跟 RocksDB data 放在一起。

RocksDB WAL 和当前 `AtomicLogBuffer` 指标不是同一类 WAL。

### 6.4 data_path 放在 tmpfs 时的特殊情况

如果 `data_path` 放在 tmpfs 上：

- Nebula Raft WAL 文件可能消耗系统内存。
- RocksDB data/WAL 文件也可能消耗系统内存。
- 操作系统 page cache 和 tmpfs 占用可能导致机器内存上涨。

但这类上涨不等同于 `AtomicLogBuffer` 堆内存上涨。

排查时建议同时区分：

```text
AtomicLogBuffer 指标上涨:
  说明 storaged 进程内 WAL buffer 持有增长。

RSS 上涨但 AtomicLogBuffer 指标不涨:
  可能是 RocksDB block cache、memtable、allocator、page cache、tmpfs 文件等其他来源。

机器内存上涨但进程 RSS 不明显上涨:
  可能是 page cache、tmpfs 或其他进程占用。
```

## 7. 后续开发顺序建议

如果确认进入图空间粒度指标开发，建议按以下顺序：

1. 修改 `AtomicLogBuffer`，保存 `spaceId_`。
2. 修改 `FileBasedWal`，创建 buffer 时传入 `spaceId_`。
3. 新增 `collectMetricsBySpace()`。
4. 修改 `StorageHttpStatsHandler`，输出 `wal.log_buffer.space.<space_id>.*`。
5. 增强 `statFiltered()`，支持末尾 `*` 前缀匹配。
6. 编译验证。
7. 复用 task-wal 三 storaged 环境做连续采样。
8. 更新 `tasks/task-wal-run.log` 和 `tasks/task-wal-analysis.md`。

## 8. 当前交付物

已完成：

- 全量 WAL buffer 指标开发。
- 编译安装。
- 3 storaged 验证。
- 运行日志记录。
- 初步源码位置确认。

文件：

- `tasks/task-wal.md`
- `tasks/task-wal-analysis.md`
- `tasks/task-wal-run.log`
- `tasks/task-wal-design.md`

待确认后开发：

- 图空间粒度 WAL buffer 指标。
- HTTP stats 简单通配过滤。
