# Nebula Graph 与 RocksDB 性能指标和调优参数分析

## 1. 结论摘要

本任务面向“网络故障告警接入或拓扑变更时，Nebula Graph 语句执行慢，但 CPU/内存未完全利用”的场景。根据当前仓库源码，建议先用现有 HTTP 接口建立一套分层指标体系，不新增内核接口：

- Nebula 内核指标：通过 graphd/storaged/metad 的 `/stats` 获取，适合定位 graph 层查询、storage RPC/processor、Raft/WAL 延迟。
- RocksDB 累计统计：通过 storaged 的 `/rocksdb_stats` 获取，适合定位读写、cache、bloom、stall、compaction、flush、WAL 等累计量和直方图。
- RocksDB 状态属性：通过 storaged 的 `/rocksdb_property?space=...&property=...` 获取，适合定位 memtable、L0/LSM、compaction backlog、block cache、写停顿等瞬时状态。

调优应按“业务端延迟 -> storage processor 延迟 -> Raft/WAL -> RocksDB write/read/compaction/cache”逐层收敛。不要只看 CPU/内存利用率；RocksDB 写停顿、L0 文件堆积、pending compaction bytes、block cache miss、WAL sync、Raft 复制延迟都可能在 CPU/内存未打满时造成慢请求。

## 2. 源码依据

Nebula 指标与接口：

- `/stats` 路由在 `src/webservice/WebService.cpp` 注册，读取逻辑在 `src/webservice/GetStatsHandler.cpp`。
- graph 指标在 `src/graph/stats/GraphStats.cpp` 注册。
- storage processor 指标在 `src/storage/CommonUtils.h` 的 `ProcessorCounters::init()` 中统一注册，并在 `src/storage/GraphStorageServiceHandler.cpp` 初始化各 processor。
- storage 写入数量指标在 `src/storage/stats/StorageStats.cpp` 注册。
- Raft/KV 指标在 `src/kvstore/stats/KVStats.cpp` 注册。
- RocksDB `/rocksdb_stats` 在 `src/storage/http/StorageHttpStatsHandler.cpp` 暴露，底层读取 `rocksdb::Statistics::getTickerMap()` 和 `rocksdb::HistogramsNameMap`。
- RocksDB `/rocksdb_property` 在 `src/storage/http/StorageHttpPropertyHandler.cpp` 暴露，底层调用 `kv_->getProperty(spaceId, property)`，`NebulaStore::getProperty()` 会返回该 space 下各 engine 的 property。

RocksDB 指标与属性：

- RocksDB ticker/histogram 名称来自 `rocksdb-7.5.3/monitoring/statistics.cc`。
- RocksDB property 名称和语义来自 `rocksdb-7.5.3/include/rocksdb/db.h` 与 `rocksdb-7.5.3/db/internal_stats.cc`。

RocksDB/Nebula 调优参数：

- RocksDB option 初始化在 `src/kvstore/RocksEngineConfig.cpp`。
- RocksDB 读写路径在 `src/kvstore/RocksEngine.cpp`。
- RocksDB 在线 `SetOptions/SetDBOptions` 通道在 `src/kvstore/NebulaStore.cpp`。
- 在线支持的 RocksDB option 白名单在 `src/kvstore/PartManager.cpp`。
- graph/storage 线程参数分别在 `src/graph/service/GraphFlags.cpp`、`src/storage/StorageServer.cpp`、`src/storage/StorageFlags.cpp`。
- Raft/WAL 参数在 `src/kvstore/raftex/RaftPart.cpp`、`src/kvstore/wal/FileBasedWal.cpp`。

## 3. 接口采集规则

### 3.1 `/stats`

格式：

```text
http://<host>:<http_port>/stats?stats=<metric1>,<metric2>&format=json
```

指标名格式：

```text
<counter>.<method>.<window>
```

- `method`：普通 counter 支持 `rate`、`sum`、`avg`、`count`；histogram 支持 `avg`、`p75`、`p95`、`p99`、`p999`，以源码注册为准。
- `window`：`5`、`60`、`600`、`3600` 秒。
- 示例：`query_latency_us.p99.60`、`add_edges_latency_us.p95.60`、`num_queries.rate.60`。
- 开启 `--enable_space_level_metrics=true` 后，graph 层部分指标可按 space 打标签，读取形式如 `query_latency_us{space=my_space}.p99.60`。

采集策略：

- 告警接入执行期间，P0 指标建议每 5 秒采一次，至少覆盖执行前 5 分钟基线、执行全过程、执行后 5 分钟恢复期。
- `/stats` 已经按窗口聚合，定位瞬时抖动优先看 `.5`，判断稳定瓶颈优先看 `.60`，复盘趋势看 `.600`。
- 三实例 graphd、storaged、metad 都要采。分析时对延迟取每个实例的 p95/p99 和最大值，不建议只看平均值。

### 3.2 `/rocksdb_stats`

格式：

```text
http://<storaged_http>:<port>/rocksdb_stats?stats=<stat1>,<stat2>&format=json
```

注意：

- 必须启动 storaged 时设置 `--enable_rocksdb_statistics=true`，否则接口返回为空。
- ticker 是累计值，采集后应计算相邻两次的差值和速率。
- histogram 只有在 `rocksdb_stats_level` 高于 `kExceptHistogramOrTimers` 时才会由 Nebula 接口输出；短期诊断 RocksDB 延迟时建议临时调高到 `kExceptTimeForMutex` 或 `kAll`，注意观测开销。
- histogram 查询参数使用基础名称，例如 `stats=rocksdb.db.write.micros`，返回字段是 `rocksdb.db.write.micros.p50/p95/p99`。不要把 `.p99` 放进 query 参数。

采集策略：

- P0 ticker 每 5 秒采一次并做 delta/s。
- RocksDB histogram 开销更高，建议只在压测窗口或慢请求复现窗口开启，采样周期 5-10 秒。
- `/rocksdb_stats` 的 Statistics 对象是进程级共享统计，不按 space 区分；需要结合 `/rocksdb_property` 按 space/engine 定位。

### 3.3 `/rocksdb_property`

格式：

```text
http://<storaged_http>:<port>/rocksdb_property?space=<space>&property=<prop1>,<prop2>
```

返回值：

- 返回 JSON array。每个 property 对应一个对象。
- 对一个 space，如果 storaged 有多个 RocksEngine，会返回 `Engine 0`、`Engine 1` 等值。

采集策略：

- property 是瞬时状态，P0 property 建议每 5 秒采一次。
- 分析时优先看每个 engine 的最大值和异常值，而不是平均值。单个 hot engine/partition 堆积就足以拖慢业务。
- `rocksdb.total-sst-files-size` 在 RocksDB 源码注释中提示文件过多时可能拖慢在线查询，建议低频采集或复盘时采。

## 4. 指标分级

分级原则：

- P0：直接判断慢请求、写停顿、compaction backlog、cache 不足、Raft/WAL 慢，告警接入执行期必须采。
- P1：用于解释 P0 异常原因和选择调参方向，建议同步采或在 P0 异常后加密采。
- P2：容量、背景、长期趋势或复盘指标，低频采集。

## 5. P0 指标清单

### 5.1 Graph 请求入口

| 指标 | 接口 | 含义 | 采集方式 | 策略 | 调优指向 |
| --- | --- | --- | --- | --- | --- |
| `query_latency_us.p95.60`, `query_latency_us.p99.60`, `query_latency_us.p999.60` | graphd `/stats` | graphd 端请求总延迟 | `stats=query_latency_us.p95.60,query_latency_us.p99.60,query_latency_us.p999.60` | P0，5s 周期；三台 graphd 分别采，取最大值 | graph 线程、storage RPC、查询计划、下游 storage/RocksDB |
| `num_queries.rate.60` | graphd `/stats` | graphd QPS | `stats=num_queries.rate.60` | P0，和延迟一起采，用于判断是否流量突增 | 限流、批量大小、并发模型 |
| `num_active_queries.sum.5` | graphd `/stats` | 正在执行的查询数 | `stats=num_active_queries.sum.5` | P0，活跃查询持续升高说明排队或下游阻塞 | `num_worker_threads`、下游 storage 慢、慢查询治理 |
| `num_slow_queries.rate.60`, `slow_query_latency_us.p99.60` | graphd `/stats` | 慢查询速率和慢查询延迟 | `stats=num_slow_queries.rate.60,slow_query_latency_us.p99.60` | P0，配合 `slow_query_threshold_us` 判断 | 慢查询阈值、查询改写、索引、storage/RocksDB |
| `num_query_errors.rate.60` | graphd `/stats` | 查询错误速率 | `stats=num_query_errors.rate.60` | P0，错误上升时延迟结论需分开分析 | 服务稳定性、超时、下游错误 |
| `num_query_errors_leader_changes.rate.60` | graphd `/stats` | leader change 相关查询错误 | `stats=num_query_errors_leader_changes.rate.60` | P0，非 0 时优先排查 Raft/leader 稳定性 | Raft、分区 leader、网络 |
| `num_queries_hit_memory_watermark.rate.60` | graphd `/stats` | 查询触发内存水位 | `stats=num_queries_hit_memory_watermark.rate.60` | P0，非 0 时延迟可能来自内存保护 | `system_memory_high_watermark_ratio`、查询结果规模、GC |

### 5.2 Storage processor

Storage processor 指标命名规则：

- 调用量：`num_<processor>.rate.60`、`num_<processor>.sum.60`
- 错误量：`num_<processor>_errors.rate.60`
- 延迟：`<processor>_latency_us.avg.60`、`<processor>_latency_us.p95.60`、`<processor>_latency_us.p99.60`

关键 processor：

| processor | 对应业务 | 指标示例 | 策略 | 调优指向 |
| --- | --- | --- | --- | --- |
| `add_vertices` | 增点 | `add_vertices_latency_us.p99.60`, `num_add_vertices.rate.60`, `num_add_vertices_errors.rate.60` | P0，拓扑变更写入必须采 | RocksDB write、Raft/WAL、写 buffer、compaction |
| `add_edges` | 增边 | `add_edges_latency_us.p99.60`, `num_add_edges.rate.60` | P0，拓扑边写入必须采 | 同上 |
| `update_vertex` | 更新点属性 | `update_vertex_latency_us.p99.60` | P0，告警状态写入/更新采 | 读改写、reader pool、RocksDB get/write |
| `update_edge` | 更新边属性 | `update_edge_latency_us.p99.60` | P0 | 同上 |
| `delete_vertices` | 删点 | `delete_vertices_latency_us.p99.60` | P0，拓扑删除采 | delete tombstone、compaction backlog |
| `delete_edges` | 删边 | `delete_edges_latency_us.p99.60` | P0 | delete tombstone、compaction backlog |
| `get_neighbors` | 邻接查询/拓扑遍历 | `get_neighbors_latency_us.p99.60` | P0，告警影响分析常见瓶颈 | prefix scan、block cache、bloom、read amp |
| `get_prop` | 点/边属性读取 | `get_prop_latency_us.p99.60` | P0 | cache、memtable/table reader、MultiGet |
| `lookup` | 索引查找 | `lookup_latency_us.p99.60` | P0，如告警规则依赖索引查询 | 索引选择、index scan、RocksDB read |
| `scan_vertex`, `scan_edge` | 扫描 | `scan_vertex_latency_us.p99.60` | P1/P0，若业务含扫描则升为 P0 | 扫描放大、cache 污染、限流 |

补充写入量指标：

| 指标 | 接口 | 含义 | 策略 | 调优指向 |
| --- | --- | --- | --- | --- |
| `num_vertices_inserted.rate.60` | storaged `/stats` | 点写入速率 | P0，写入任务采 | 写入放大和批量规模 |
| `num_edges_inserted.rate.60` | storaged `/stats` | 边写入速率 | P0 | 同上 |
| `num_vertices_deleted.rate.60`, `num_edges_deleted.rate.60`, `num_tags_deleted.rate.60` | storaged `/stats` | 删除速率 | P0，删除/拓扑变更采 | tombstone、compaction |

### 5.3 Raft 与 WAL

| 指标 | 接口 | 含义 | 采集方式 | 策略 | 调优指向 |
| --- | --- | --- | --- | --- | --- |
| `append_wal_latency_us.p99.60` | storaged `/stats` | Raft 追加本地 WAL 延迟 | `stats=append_wal_latency_us.p99.60` | P0，写慢时必采 | WAL 盘、`wal_sync`、`wal_buffer_size`、`wal_file_size` |
| `replicate_log_latency_us.p99.60` | storaged `/stats` | Raft 日志复制到多数派延迟 | `stats=replicate_log_latency_us.p99.60` | P0 | 网络、follower 慢、leader 分布 |
| `append_log_latency_us.p99.60` | storaged `/stats` | append log 总体延迟 | `stats=append_log_latency_us.p99.60` | P0 | Raft 写路径整体 |
| `commit_log_latency_us.p99.60` | storaged `/stats` | 日志提交延迟 | `stats=commit_log_latency_us.p99.60` | P0 | 下游 apply/RocksDB 写入 |
| `num_start_elect.rate.60` | storaged `/stats` | 选举发生速率 | `stats=num_start_elect.rate.60` | P0，非 0 说明集群不稳定 | 网络、心跳、负载、节点稳定性 |
| `num_send_snapshot.rate.60` | storaged `/stats` | snapshot 发送速率 | `stats=num_send_snapshot.rate.60` | P1/P0，复制落后时升 P0 | follower 落后、磁盘/网络 |

### 5.4 RocksDB 写入和写停顿

| 指标 | 接口 | 含义 | 采集方式 | 策略 | 调优指向 |
| --- | --- | --- | --- | --- | --- |
| `rocksdb.is-write-stopped` | `/rocksdb_property` | 当前写入是否停止，1 表示停写 | `property=rocksdb.is-write-stopped` | P0，5s；任一 engine 为 1 即严重 | `level0_stop_writes_trigger`、compaction、flush |
| `rocksdb.actual-delayed-write-rate` | `/rocksdb_property` | 当前实际写限速，0 表示未限速 | `property=rocksdb.actual-delayed-write-rate` | P0，非 0 说明写入被 RocksDB 限速 | `delayed_write_rate`、L0/compaction |
| `rocksdb.estimate-pending-compaction-bytes` | `/rocksdb_property` | 预计待 compaction 字节数 | `property=rocksdb.estimate-pending-compaction-bytes` | P0，看趋势；持续上升说明 compaction 追不上 | `max_background_jobs`、`max_compaction_bytes`、level size |
| `rocksdb.compaction-pending` | `/rocksdb_property` | 是否有待 compaction | `property=rocksdb.compaction-pending` | P0 | compaction 资源 |
| `rocksdb.mem-table-flush-pending` | `/rocksdb_property` | 是否有 memtable 等待 flush | `property=rocksdb.mem-table-flush-pending` | P0 | flush 线程、写 buffer、磁盘 |
| `rocksdb.num-immutable-mem-table` | `/rocksdb_property` | 未 flush 的 immutable memtable 数 | `property=rocksdb.num-immutable-mem-table` | P0，持续升高易触发 stall | `write_buffer_size`、`max_write_buffer_number`、flush |
| `rocksdb.stall.micros` | `/rocksdb_stats` | writer 等待 compaction/flush 的累计时间 | `stats=rocksdb.stall.micros` | P0，计算 delta；delta>0 表示发生写停顿 | L0/flush/compaction |
| `rocksdb.db.write.stall` | `/rocksdb_stats` | write stall histogram | `stats=rocksdb.db.write.stall` | P0，短期开 histogram | 同上 |
| `rocksdb.number.keys.written`, `rocksdb.bytes.written` | `/rocksdb_stats` | RocksDB 逻辑写入 key/byte 累计量 | `stats=rocksdb.number.keys.written,rocksdb.bytes.written` | P0，计算 delta/s | 写吞吐、批量大小 |
| `rocksdb.db.write.micros` | `/rocksdb_stats` | RocksDB 写延迟直方图 | `stats=rocksdb.db.write.micros` | P0，短期诊断开启 | RocksDB write path |
| `rocksdb.wal.bytes`, `rocksdb.wal.synced`, `rocksdb.wal.file.sync.micros` | `/rocksdb_stats` | RocksDB WAL 写入、sync 次数和 sync 延迟 | `stats=rocksdb.wal.bytes,rocksdb.wal.synced,rocksdb.wal.file.sync.micros` | P0，写慢时采 | `rocksdb_wal_sync`、WAL 盘、`wal_bytes_per_sync` |

### 5.5 RocksDB 读取、cache、bloom

| 指标 | 接口 | 含义 | 采集方式 | 策略 | 调优指向 |
| --- | --- | --- | --- | --- | --- |
| `rocksdb.db.get.micros`, `rocksdb.db.multiget.micros`, `rocksdb.db.seek.micros` | `/rocksdb_stats` | Get/MultiGet/Seek 延迟直方图 | `stats=rocksdb.db.get.micros,rocksdb.db.multiget.micros,rocksdb.db.seek.micros` | P0，读慢时短期开 | read path、cache、LSM 层级 |
| `rocksdb.number.keys.read`, `rocksdb.bytes.read` | `/rocksdb_stats` | 逻辑读 key/byte 累计量 | `stats=rocksdb.number.keys.read,rocksdb.bytes.read` | P0，计算 delta/s | 读吞吐、读放大 |
| `rocksdb.number.db.seek`, `rocksdb.number.db.next`, `rocksdb.number.db.prev` | `/rocksdb_stats` | iterator seek/next/prev 次数 | `stats=rocksdb.number.db.seek,rocksdb.number.db.next,rocksdb.number.db.prev` | P0，遍历/邻接查询采 | prefix scan、遍历放大 |
| `rocksdb.db.iter.bytes.read` | `/rocksdb_stats` | iterator 读取字节 | `stats=rocksdb.db.iter.bytes.read` | P0，结合 `get_neighbors` 延迟 | scan 放大 |
| `rocksdb.block.cache.hit`, `rocksdb.block.cache.miss` | `/rocksdb_stats` | block cache 总命中/未命中 | `stats=rocksdb.block.cache.hit,rocksdb.block.cache.miss` | P0，计算 hit ratio | `rocksdb_block_cache` |
| `rocksdb.block.cache.data.hit`, `rocksdb.block.cache.data.miss` | `/rocksdb_stats` | data block cache 命中/未命中 | `stats=rocksdb.block.cache.data.hit,rocksdb.block.cache.data.miss` | P0，读慢时优先看 | block cache 容量 |
| `rocksdb.block.cache.index.hit/miss`, `rocksdb.block.cache.filter.hit/miss` | `/rocksdb_stats` | index/filter block cache 命中 | 对应 stats 名称 | P1/P0，cache miss 时升 P0 | partitioned index/filter、cache index/filter |
| `rocksdb.block-cache-capacity`, `rocksdb.block-cache-usage`, `rocksdb.block-cache-pinned-usage` | `/rocksdb_property` | block cache 容量、使用量、pin 住的使用量 | `property=rocksdb.block-cache-capacity,rocksdb.block-cache-usage,rocksdb.block-cache-pinned-usage` | P0，结合 hit ratio | `rocksdb_block_cache`、partitioned index/filter |
| `rocksdb.bloom.filter.prefix.checked`, `rocksdb.bloom.filter.prefix.useful` | `/rocksdb_stats` | prefix bloom 检查次数和有效避免读次数 | `stats=rocksdb.bloom.filter.prefix.checked,rocksdb.bloom.filter.prefix.useful` | P0，邻接/prefix 读必须采 | `enable_rocksdb_prefix_filtering`、访问模式 |
| `rocksdb.bloom.filter.useful` | `/rocksdb_stats` | whole-key bloom 有效次数 | `stats=rocksdb.bloom.filter.useful` | P1/P0，点查多时采 | `enable_rocksdb_whole_key_filtering` |
| `rocksdb.memtable.hit`, `rocksdb.memtable.miss` | `/rocksdb_stats` | memtable 命中/未命中 | `stats=rocksdb.memtable.hit,rocksdb.memtable.miss` | P1，读写混合时采 | 写后读、memtable/flush |
| `rocksdb.row.cache.hit`, `rocksdb.row.cache.miss` | `/rocksdb_stats` | row cache 命中/未命中 | `stats=rocksdb.row.cache.hit,rocksdb.row.cache.miss` | P1，启用 row cache 时采 | `rocksdb_row_cache_num` |

### 5.6 RocksDB compaction/flush/LSM

| 指标 | 接口 | 含义 | 采集方式 | 策略 | 调优指向 |
| --- | --- | --- | --- | --- | --- |
| `rocksdb.num-files-at-level0` | `/rocksdb_property` | L0 文件数 | `property=rocksdb.num-files-at-level0` | P0，持续升高会导致 slowdown/stop | L0 trigger、compaction |
| `rocksdb.num-files-at-level<N>` | `/rocksdb_property` | 各层文件数 | `property=rocksdb.num-files-at-level0,...,rocksdb.num-files-at-level6` | P1，复盘 LSM 分布 | level size、target file size |
| `rocksdb.num-running-compactions` | `/rocksdb_property` | 正在执行的 compaction 数 | `property=rocksdb.num-running-compactions` | P0 | `max_background_jobs`、`num_compaction_threads` |
| `rocksdb.num-running-flushes` | `/rocksdb_property` | 正在执行的 flush 数 | `property=rocksdb.num-running-flushes` | P0 | flush 资源、磁盘 |
| `rocksdb.compact.read.bytes`, `rocksdb.compact.write.bytes` | `/rocksdb_stats` | compaction 读写字节累计量 | `stats=rocksdb.compact.read.bytes,rocksdb.compact.write.bytes` | P0，计算 delta/s | compaction IO、写放大 |
| `rocksdb.flush.write.bytes` | `/rocksdb_stats` | flush 写字节累计量 | `stats=rocksdb.flush.write.bytes` | P0 | flush IO |
| `rocksdb.compaction.times.micros`, `rocksdb.db.flush.micros` | `/rocksdb_stats` | compaction/flush 耗时直方图 | `stats=rocksdb.compaction.times.micros,rocksdb.db.flush.micros` | P1/P0，积压时短期开 | compaction/flush 参数 |
| `rocksdb.compaction.cancelled` | `/rocksdb_stats` | compaction 取消次数 | `stats=rocksdb.compaction.cancelled` | P1 | 磁盘空间、后台任务 |
| `rocksdb.total-sst-files-size`, `rocksdb.live-sst-files-size` | `/rocksdb_property` | SST 总大小、当前 live SST 大小 | `property=rocksdb.live-sst-files-size`，`total` 低频采 | P2/P1，容量趋势 | compaction、存储容量 |

## 6. P1/P2 辅助指标

| 指标 | 接口 | 级别 | 含义 | 采集策略 | 调优指向 |
| --- | --- | --- | --- | --- | --- |
| `optimizer_latency_us.p99.60` | graphd `/stats` | P1 | graph optimizer 耗时 | 查询慢但 storage 不慢时采 | 查询计划、优化器 |
| `num_indexscan_executors.rate.60` | graphd `/stats` | P1 | index scan executor 使用量 | lookup 慢时采 | 索引选择、查询改写 |
| `num_sort_executors.rate.60`, `num_aggregate_executors.rate.60` | graphd `/stats` | P1 | sort/aggregate 执行器数量 | graph 层 CPU/内存压力时采 | 查询改写、限制返回规模 |
| `num_rpc_sent_to_storaged.rate.60`, `num_rpc_sent_to_storaged_failed.rate.60` | graphd `/stats` | P1 | graph 到 storage RPC 量和失败 | graph 慢且 storage 异常时采 | 网络、storage 可用性 |
| `num_rpc_sent_to_metad.rate.60`, `num_rpc_sent_to_metad_failed.rate.60` | graphd/storaged `/stats` | P2 | meta RPC 量和失败 | 元数据异常时采 | meta 稳定性 |
| `rocksdb.levelstats`, `rocksdb.cfstats`, `rocksdb.dbstats`, `rocksdb.sstables` | `/rocksdb_property` | P2 | RocksDB 多行状态报告 | 复盘或低频采；不要高频拉大文本 | LSM/文件分布 |
| `rocksdb.estimate-num-keys` | `/rocksdb_property` | P2 | key 数估计 | 容量趋势 | 分区和数据规模 |
| `rocksdb.estimate-table-readers-mem` | `/rocksdb_property` | P1 | table reader 内存估计，不含 block cache | cache/内存定位时采 | block/table reader 内存 |
| `rocksdb.num-live-versions`, `rocksdb.num-snapshots`, `rocksdb.oldest-snapshot-time` | `/rocksdb_property` | P1 | live version 和 snapshot 保留 | SST 文件无法释放时采 | 长事务/iterator/snapshot |
| `rocksdb.base-level` | `/rocksdb_property` | P2 | L0 compact 目标层 | LSM 复盘时采 | level size |

## 7. 派生指标

建议在采集侧用原始指标计算以下派生值：

| 派生指标 | 计算方式 | 用途 |
| --- | --- | --- |
| graph 请求错误率 | `num_query_errors.rate.60 / num_queries.rate.60` | 判断慢请求是否伴随错误 |
| storage processor 错误率 | `num_<processor>_errors.rate.60 / num_<processor>.rate.60` | 区分慢和失败 |
| block cache 命中率 | `hit / (hit + miss)` | 判断是否需要扩大 cache 或优化访问模式 |
| data block cache 命中率 | `data_hit / (data_hit + data_miss)` | 比总 cache 命中率更贴近真实数据读 |
| prefix bloom 有效率 | `prefix_useful / prefix_checked` | 判断 prefix bloom 是否有效避免 SST 读 |
| RocksDB 写吞吐 | `delta(rocksdb.bytes.written) / interval` | 评估写入压力 |
| RocksDB 读吞吐 | `delta(rocksdb.bytes.read) / interval` | 评估读压力 |
| compaction 写放大近似值 | `delta(rocksdb.compact.write.bytes + rocksdb.flush.write.bytes) / delta(rocksdb.bytes.written)` | 判断 compaction 写放大 |
| write stall 占比 | `delta(rocksdb.stall.micros) / (interval * 1e6)` | 判断写停顿严重程度 |
| pending compaction 增长率 | `delta(rocksdb.estimate-pending-compaction-bytes) / interval` | 判断 compaction 是否追不上 |
| L0 压力 | `rocksdb.num-files-at-level0` 对比 `level0_*_trigger` | 判断是否临近 slowdown/stop |

## 8. 调优参数分析

### 8.1 写入慢或写停顿

判断信号：

- `add_vertices/add_edges/update/delete *_latency_us.p99.60` 升高。
- `rocksdb.is-write-stopped=1` 或 `rocksdb.actual-delayed-write-rate>0`。
- `delta(rocksdb.stall.micros)>0` 或 `rocksdb.db.write.stall.p99` 升高。
- `rocksdb.num-immutable-mem-table`、`rocksdb.mem-table-flush-pending`、`rocksdb.estimate-pending-compaction-bytes` 持续升高。
- `rocksdb.num-files-at-level0` 接近或超过 L0 slowdown/stop trigger。

可调参数：

| 参数 | 位置/方式 | 含义 | 指标关联 | 建议 |
| --- | --- | --- | --- | --- |
| `write_buffer_size` | `rocksdb_column_family_options`，白名单在线支持 | 单个 memtable 大小 | active/all memtable size、flush pending、写停顿 | 写入突发大时可增大，减少 flush 频率；会增加内存和 crash recovery 成本 |
| `max_write_buffer_number` | `rocksdb_column_family_options`，白名单在线支持 | memtable 总数上限 | immutable memtable、flush pending | flush 追不上且内存充足时增大 |
| `level0_file_num_compaction_trigger` | `rocksdb_column_family_options`，白名单在线支持 | L0 文件数达到后触发 compaction | L0 文件数 | 可适当下调让 compaction 更早开始，或配合后台线程上调吞吐 |
| `level0_slowdown_writes_trigger` | 同上 | L0 文件数达到后写 slowdown | delayed write rate、stall | 不建议只粗暴上调；应同时提升 compaction 能力 |
| `level0_stop_writes_trigger` | 同上 | L0 文件数达到后停写 | is-write-stopped | 只作为缓解，根因通常是 compaction/flush 慢 |
| `max_background_jobs` | `rocksdb_db_options`，白名单在线支持 | RocksDB flush/compaction 后台任务总数 | running compactions/flushes、pending compaction bytes | CPU/IO 未满且 backlog 上升时优先增大 |
| `max_compaction_bytes` | `rocksdb_column_family_options`，白名单在线支持 | 单次 compaction 最大字节数 | compaction times、pending bytes | 大 compaction 卡顿时调小；吞吐不足时配合后台并发评估 |
| `soft_pending_compaction_bytes_limit`, `hard_pending_compaction_bytes_limit` | `rocksdb_column_family_options`，白名单在线支持 | pending compaction bytes 软/硬限制 | estimate-pending-compaction-bytes、stall | 只能缓解停写阈值，不能代替提升 compaction 能力 |
| `delayed_write_rate` | `rocksdb_db_options`，白名单在线支持 | 写 slowdown 时的限速值 | actual-delayed-write-rate | 调高可减少慢写，但可能加剧 compaction backlog |
| `rocksdb_batch_size` | gflag，重启生效 | Nebula `WriteBatch` 预留大小 | write latency、bytes written | 批量写较大时可调整预留，影响内存分配，不是业务 batch size |
| `rocksdb_disable_wal` | gflag，重启生效 | 是否关闭 RocksDB WAL | WAL stats、写延迟 | Nebula 还有 Raft WAL；关闭 RocksDB WAL 可提升吞吐，但需严格评估恢复语义 |
| `rocksdb_wal_sync` | gflag，重启生效 | RocksDB WAL 是否同步刷盘 | wal.synced、wal.file.sync.micros | 打开更安全但慢；关闭延迟低 |

使用示例：

```ngql
UPDATE CONFIGS storage:rocksdb_column_family_options = {
  "write_buffer_size":"268435456",
  "max_write_buffer_number":"6",
  "level0_file_num_compaction_trigger":"4",
  "level0_slowdown_writes_trigger":"20",
  "level0_stop_writes_trigger":"36"
};

UPDATE CONFIGS storage:rocksdb_db_options = {
  "max_background_jobs":"8",
  "delayed_write_rate":"104857600"
};
```

实际是否在线生效要满足 `PartManager.cpp` 白名单，并通过 `/rocksdb_property?property=rocksdb.options-statistics`、日志或 `SHOW CONFIGS` 验证。非白名单和 table/block cache 结构类参数通常需要重启 storaged 或重建 RocksDB options。

### 8.2 读慢、邻接查询慢、cache miss

判断信号：

- `get_neighbors_latency_us.p99.60`、`get_prop_latency_us.p99.60`、`lookup_latency_us.p99.60` 升高。
- `rocksdb.db.get.micros.p99`、`rocksdb.db.seek.micros.p99`、`rocksdb.read.block.get.micros.p99` 升高。
- `rocksdb.block.cache.data.miss` delta 高，data block cache 命中率低。
- `rocksdb.bloom.filter.prefix.checked` 高但 `prefix.useful` 低。
- iterator 指标 `number.db.seek/next`、`db.iter.bytes.read` 高。

可调参数：

| 参数 | 位置/方式 | 含义 | 指标关联 | 建议 |
| --- | --- | --- | --- | --- |
| `rocksdb_block_cache` | gflag，重启生效 | BlockBasedTable block cache 大小，单位 MB | block-cache-capacity/usage、cache hit/miss | cache miss 高且内存充足时增大 |
| `rocksdb_row_cache_num` | gflag，重启生效 | row cache key 数 | row.cache hit/miss | 点查重复度高时评估开启/增大 |
| `enable_partitioned_index_filter` | gflag，重启生效 | 分区 index/filter，降低大索引/filter 内存压力 | index/filter cache miss、table readers mem | 大数据量读多时建议评估 |
| `enable_rocksdb_prefix_filtering` | gflag，默认 true | 使用 prefix bloom/filter | prefix checked/useful、get_neighbors latency | 邻接/prefix 查询应保持开启 |
| `enable_rocksdb_whole_key_filtering` | gflag | whole-key bloom | bloom.filter.useful | 点查不存在 key 较多时评估 |
| `rocksdb_block_based_table_options` | gflag JSON，重启生效 | BlockBasedTable 选项 | cache、filter、index | 可设置 block size、cache_index_and_filter_blocks 等 |
| `max_sequential_skip_in_iterations` | CF option，白名单在线支持 | iterator 顺序跳过上限 | number.reseeks.iteration、seek/next | 遍历中大量相同 user key 时评估 |

### 8.3 Compaction backlog 或写放大高

判断信号：

- `rocksdb.estimate-pending-compaction-bytes` 持续上升。
- `rocksdb.num-files-at-level0` 长时间高位。
- `rocksdb.compact.read.bytes/write.bytes` delta 高，业务写入不高但磁盘繁忙。
- `rocksdb.compaction.times.micros.p99` 高。
- `rocksdb.actual-delayed-write-rate>0` 或 `rocksdb.stall.micros` 增长。

可调参数：

| 参数 | 位置/方式 | 含义 | 指标关联 | 建议 |
| --- | --- | --- | --- | --- |
| `max_background_jobs` | DB option，白名单在线支持 | 后台 flush/compaction 并发 | running compactions、pending bytes | IO/CPU 未满时优先增大 |
| `num_compaction_threads` | gflag，重启生效 | Nebula 设置 RocksDB compaction task limiter | running compactions、CPU | 默认 0 不限制；如果被设置过低会限速 |
| `target_file_size_base`, `target_file_size_multiplier` | CF option，白名单在线支持 | SST 目标文件大小 | LSM 文件数、compaction times | 小文件过多时可增大 |
| `max_bytes_for_level_base`, `max_bytes_for_level_multiplier` | CF option，白名单在线支持 | 各层目标容量 | pending compaction、base-level | 调整 LSM 层级容量，需谨慎压测 |
| `compaction_readahead_size` | DB option，白名单在线支持 | compaction 预读 | compaction read bytes/time | HDD 或顺序 IO 场景可增大 |
| `rocksdb_rate_limit` | gflag，重启生效 | RocksDB 写/compaction rate limiter，单位 MB/s | rate.limit.delay、compaction backlog | 如果设得过低会限制吞吐；需要核对配置 |
| `rocksdb_compact_change_level`, `rocksdb_compact_target_level` | compact 参数 | 手动 compact 目标层行为 | compaction 后 LSM 分布 | `SUBMIT JOB COMPACT` 低峰执行，避免在线冲击 |

### 8.4 Raft/WAL 慢

判断信号：

- `append_wal_latency_us.p99.60` 高：本地 Nebula Raft WAL 慢。
- `replicate_log_latency_us.p99.60` 高：复制多数派慢，通常是网络或 follower 慢。
- `commit_log_latency_us.p99.60` 高：提交或 apply 链路慢。
- `num_start_elect.rate.60` 非 0：leader 不稳定。
- RocksDB `rocksdb.wal.file.sync.micros.p99` 高：RocksDB WAL sync 慢。

可调参数：

| 参数 | 位置 | 含义 | 指标关联 | 建议 |
| --- | --- | --- | --- | --- |
| `wal_sync` | Raft WAL gflag | Nebula Raft WAL 每次写是否 fsync | append_wal_latency | 更安全但慢；低延迟场景需评估关闭风险 |
| `wal_buffer_size` | Raft WAL gflag | WAL 内存 buffer 大小 | append_wal_latency、WAL IO | 写入突发时可增大 |
| `wal_file_size` | Raft WAL gflag | WAL 文件滚动大小 | append_wal_latency | 文件滚动频繁时可增大 |
| `max_batch_size` | Raft gflag | Raft append log buffer 最大日志数 | E_RAFT_BUFFER_OVERFLOW、append/replicate latency | 写入突发且 buffer overflow 时增大，但会增加单批延迟 |
| `raft_heartbeat_interval_secs` | Raft gflag | heartbeat 间隔 | leader change、replicate | 不建议为性能盲调，优先查网络/follower |
| `rocksdb_wal_dir` | RocksDB gflag | RocksDB WAL 单独目录 | RocksDB WAL sync | 可把 WAL 放到更快磁盘，但需评估部署和恢复 |
| `wal_bytes_per_sync`, `bytes_per_sync` | DB option，白名单在线支持 | RocksDB WAL/文件按字节 sync | wal sync、IO 抖动 | 降低长尾，但可能改变 IO 模式 |

### 8.5 线程和并发未打满

判断信号：

- CPU/内存未打满，但 `num_active_queries` 或 storage processor 延迟高。
- RocksDB 无明显 stall/backlog，Raft/WAL 正常。
- graph 层 `query_latency` 高于 storage processor 延迟总和。
- storage 读 processor 延迟高但 RocksDB read histogram 不高，可能卡在线程池/队列。

可调参数：

| 参数 | 服务 | 含义 | 指标关联 | 建议 |
| --- | --- | --- | --- | --- |
| `num_worker_threads` | graphd | graph 查询执行线程数，0 时按硬件并发 | active queries、query latency | 并发高且排队时增大 |
| `num_netio_threads` | graphd | graph 网络 IO 线程 | RPC 延迟、连接数 | 网络 IO 繁忙时调整 |
| `num_operator_threads` | graphd | 单 operator 并行线程 | path/traverse 算子 | 复杂遍历可评估 |
| `max_job_size`, `min_batch_size` | graphd | 多 job 模式 batch 划分 | traverse/expand 延迟 | 大结果集遍历时评估 |
| `storage_num_worker_threads` / `num_worker_threads` | storaged | storage worker 线程 | processor latency | storage 层排队时增大 |
| `num_io_threads` | storaged | storage IO 线程池 | Raft/storage RPC | 网络/IO 任务并发 |
| `reader_handlers`, `reader_handlers_type` | storaged | storage 读 processor 的 reader pool | get/update/read latency | 读请求或读改写慢时增大；`cpu/io` 类型需压测 |
| `query_concurrently` | storaged | lookup/go 按 part 并发查询 | get_neighbors/lookup latency | 多 part 查询可评估开启 |

## 9. 诊断流程

1. 先看 graphd：`query_latency_us.p99.60` 是否升高，`num_queries.rate.60` 是否流量突增，`num_active_queries.sum.5` 是否堆积。
2. 再看 storaged processor：找出 `add_edges/get_neighbors/update_*` 等哪个 processor 的 p99 最高，确认慢点在读还是写。
3. 写慢时看 Raft/WAL：`append_wal_latency_us`、`replicate_log_latency_us`、`commit_log_latency_us` 谁高。
4. 写慢时看 RocksDB：`is-write-stopped`、`actual-delayed-write-rate`、`stall.micros`、`num-immutable-mem-table`、`pending compaction bytes`、`L0 file count`。
5. 读慢时看 RocksDB：`db.get/seek/multiget` histogram、block cache hit/miss、bloom prefix useful ratio、iterator seek/next/bytes。
6. 如果 RocksDB 和 Raft 都正常，再回到 graph/storage 线程池、查询计划、索引、返回结果规模。
7. 每次只调整一类参数，保留基线、调参窗口和回滚点；用同一批 P0 指标验证收益和副作用。

## 10. 推荐采集组合

### 10.1 graphd P0

```text
query_latency_us.p95.60,query_latency_us.p99.60,query_latency_us.p999.60,
num_queries.rate.60,num_active_queries.sum.5,
num_slow_queries.rate.60,slow_query_latency_us.p99.60,
num_query_errors.rate.60,num_query_errors_leader_changes.rate.60,
num_queries_hit_memory_watermark.rate.60
```

示例：

```bash
curl 'http://<graphd_http>/stats?format=json&stats=query_latency_us.p99.60,num_queries.rate.60,num_active_queries.sum.5'
```

### 10.2 storaged `/stats` P0

```text
add_vertices_latency_us.p99.60,num_add_vertices.rate.60,num_add_vertices_errors.rate.60,
add_edges_latency_us.p99.60,num_add_edges.rate.60,num_add_edges_errors.rate.60,
update_vertex_latency_us.p99.60,update_edge_latency_us.p99.60,
delete_vertices_latency_us.p99.60,delete_edges_latency_us.p99.60,
get_neighbors_latency_us.p99.60,get_prop_latency_us.p99.60,lookup_latency_us.p99.60,
append_wal_latency_us.p99.60,replicate_log_latency_us.p99.60,
append_log_latency_us.p99.60,commit_log_latency_us.p99.60,
num_start_elect.rate.60
```

### 10.3 storaged `/rocksdb_stats` P0

```text
rocksdb.number.keys.written,rocksdb.bytes.written,
rocksdb.number.keys.read,rocksdb.bytes.read,
rocksdb.stall.micros,rocksdb.rate.limit.delay.millis,
rocksdb.wal.bytes,rocksdb.wal.synced,
rocksdb.compact.read.bytes,rocksdb.compact.write.bytes,rocksdb.flush.write.bytes,
rocksdb.block.cache.hit,rocksdb.block.cache.miss,
rocksdb.block.cache.data.hit,rocksdb.block.cache.data.miss,
rocksdb.bloom.filter.prefix.checked,rocksdb.bloom.filter.prefix.useful,
rocksdb.number.db.seek,rocksdb.number.db.next,rocksdb.db.iter.bytes.read
```

短期 RocksDB histogram：

```text
rocksdb.db.write.micros,rocksdb.db.get.micros,rocksdb.db.multiget.micros,
rocksdb.db.seek.micros,rocksdb.db.write.stall,
rocksdb.db.flush.micros,rocksdb.compaction.times.micros,
rocksdb.wal.file.sync.micros,rocksdb.read.block.get.micros,rocksdb.sst.read.micros
```

### 10.4 storaged `/rocksdb_property` P0/P1

```text
rocksdb.is-write-stopped,
rocksdb.actual-delayed-write-rate,
rocksdb.estimate-pending-compaction-bytes,
rocksdb.compaction-pending,
rocksdb.mem-table-flush-pending,
rocksdb.num-immutable-mem-table,
rocksdb.num-running-compactions,
rocksdb.num-running-flushes,
rocksdb.num-files-at-level0,
rocksdb.cur-size-active-mem-table,
rocksdb.cur-size-all-mem-tables,
rocksdb.size-all-mem-tables,
rocksdb.block-cache-capacity,
rocksdb.block-cache-usage,
rocksdb.block-cache-pinned-usage,
rocksdb.background-errors,
rocksdb.estimate-table-readers-mem,
rocksdb.live-sst-files-size
```

示例：

```bash
curl 'http://<storaged_http>/rocksdb_property?space=<space>&property=rocksdb.is-write-stopped,rocksdb.actual-delayed-write-rate,rocksdb.estimate-pending-compaction-bytes,rocksdb.num-files-at-level0'
```

## 11. 调优验证策略

基线：

- 选取一段无告警接入的稳定窗口，采集 P0 指标 5-10 分钟。
- 记录当前配置：graph/storage 线程参数、RocksDB gflags、`rocksdb.options-statistics`、space partition/replica 分布。

调参：

- 一次只调整一类瓶颈参数，例如只调 compaction 并发，或只调 block cache。
- 每次调参后至少观察一个完整告警接入窗口和恢复窗口。
- 对 RocksDB 累计统计必须比较 delta/s，不比较累计绝对值。
- 对多实例、多 engine 指标优先看最大值、长尾、异常 engine，避免平均值掩盖热点。

回滚：

- 在线 option 使用 `UPDATE CONFIGS` 前记录旧值。
- 重启类 gflag 需要变更配置文件并滚动重启，单次只滚动一台 storaged 验证。
- 涉及 WAL sync、disable WAL、rate limit、compaction limit 的参数需要单独评估可靠性和恢复时间风险。

## 12. 推荐优先级

第一批必须落地的采集：

1. graphd P0：query latency、slow query、active query、query error。
2. storaged P0：告警链路涉及的 processor latency/calls/errors。
3. Raft/WAL P0：append WAL、replicate、commit log。
4. RocksDB P0 property：write stopped、delayed rate、pending compaction、flush pending、immutable memtable、L0 文件数。
5. RocksDB P0 stats：write/read bytes、stall micros、cache hit/miss、bloom prefix、compaction/flush bytes。

第一批优先调参方向：

1. 若 `is-write-stopped/delayed-write/stall` 命中，先处理 RocksDB flush/compaction 能力。
2. 若 `append_wal/replicate_log` 高，先处理 Raft WAL 磁盘或多数派复制链路。
3. 若 `get_neighbors/get_prop` 高且 cache miss 高，先处理 block cache、prefix bloom、访问模式。
4. 若下游都正常但 graph `active_queries` 高，处理 graph/storage 线程池和查询计划。

