# memor_analysis.md reading log

- Date: 2026-07-04
- Working directory: `/home/sch/nebula/nebula-release-3.6`
- Read target: `task-wal/memor_analysis.md`
- Commands:
  - `pwd && rg --files task-wal | sort | sed -n '1,120p'`
  - `sed -n '1,260p' task-wal/memor_analysis.md`
- Summary:
  - The task asks to analyze and eventually fix long-running storage memory growth in Nebula Graph 3.6.
  - The reported pattern is faster growth when graph space count or partition count is high.
  - The expected work is split into short-term source-location analysis, mid-term monitoring metrics and validation, and long-term fix plus impact assessment.

## Follow-up reading: 2026-07-04 15:52:58 CST

- Commands:
  - `rg --files src/kvstore src/storage | rg '(wal|raftex|Part|Store|Stats|Flags|Memory|Log)'`
  - `rg -n "DEFINE_.*(wal|raft|memory|heartbeat|clean|ttl|interval|part)|wal_ttl|clean_wal|raft_heartbeat|reserved_logs|snapshot|commitLog|LogBuffer|Wal" src/kvstore src/storage src/common`
  - `nl -ba src/kvstore/wal/FileBasedWal.cpp | sed -n '1,180p;260,330p;430,545p;630,725p'`
  - `nl -ba src/kvstore/wal/AtomicLogBuffer.h | sed -n '1,340p'`
  - `nl -ba src/kvstore/wal/AtomicLogBuffer.cpp | sed -n '1,340p'`
  - `nl -ba src/kvstore/NebulaStore.cpp | sed -n '1,95p;470,515p;1280,1325p'`
  - `nl -ba src/kvstore/raftex/RaftPart.cpp | sed -n '1380,1460p;1450,1465p;2088,2138p;2228,2282p'`
  - `nl -ba src/kvstore/Part.cpp | sed -n '1,120p;200,380p;520,610p'`
- Key observations:
  - `NebulaStore::newPart()` creates one `Part` per partition; `RaftPart` creates one `FileBasedWal`; `FileBasedWal` creates one `AtomicLogBuffer`.
  - Raft leader heartbeat path appends an empty NORMAL log through `RaftPart::sendHeartbeat() -> appendLogAsync(...)`, so idle partitions can still append WAL records.
  - `Part::commitLogs()` skips empty heartbeat logs for state-machine writes, but the empty logs have already been written to WAL and `AtomicLogBuffer`.
  - `AtomicLogBuffer` GC is coupled to iterator release and dirty-node/max-size thresholds, so memory can grow in steps and later drop.
  - This path matches the symptom that memory growth rate correlates with graph-space/partition count, even when most spaces have little or no user data.

## 代码修改记录：2026-07-04 16:05:41 CST

- 修改文件：
  - `src/kvstore/raftex/RaftPart.cpp`
  - `src/kvstore/stats/KVStats.h`
  - `src/kvstore/stats/KVStats.cpp`
  - `task-wal/memor_analysis_root_cause.md`
- 关键修改：
  - `RaftPart::sendHeartbeat()` 增加 `commitInThisTerm_` 判断，仅在当前 leader 任期尚未提交日志时追加空 heartbeat log。
  - 新增监控指标 `num_raft_heartbeat`、`num_raft_heartbeat_empty_log`、`num_raft_heartbeat_without_empty_log`。
  - 新增中文根因分析文档 `task-wal/memor_analysis_root_cause.md`，记录源码链路、根因、验证方式和影响评估。

## 验证记录：2026-07-04 16:22 CST

- 编译：
  - 命令：`cd build && make -j10 2>&1 | tee ../task-wal/build_20260704_raft_wal_fix.log`
  - 结果：成功，`nebula-storaged`、`nebula-graphd`、`nebula-metad` 均构建完成。
- 安装：
  - 命令：`cd build && make install 2>&1 | tee ../task-wal/install_20260704_raft_wal_fix.log`
  - 结果：核心二进制安装成功；后续设置 `/usr/local/nebula/etc/nebula-metad.conf.default` 权限时报 `Operation not permitted`，因此 `make install` 最终失败。
- 启动：
  - 命令记录见 `task-wal/runtime_start_20260704_raft_wal_fix.log`
  - 结果：`metad`、`storaged`、`graphd` 均运行中。
- 指标查询：
  - 命令记录见 `task-wal/runtime_stats_20260704_raft_wal_fix.log`
  - 结果：`num_raft_heartbeat_empty_log.sum.60=0`，`num_raft_heartbeat_without_empty_log.sum.60` 与 `num_raft_heartbeat.sum.60` 一致，符合修复预期。

## WAL 分片级指标开发记录：2026-07-04

- 目标：
  - 增加图空间和分片级 WAL 内存占用、待释放 dirty 内存、reader 引用、GC 释放情况监控。
- 设计文档：
  - `task-wal/wal_stats_metrics_design.md`
- 关键代码修改：
  - `AtomicLogBuffer` 增加 `stats()` 快照。
  - `NebulaStore` 增加 `walStats()` 聚合。
  - storage HTTP 增加 `/wal_stats` 路由。
- 设计取舍：
  - 不把所有 per-part 指标注册到 `StatsManager`，避免高基数分片指标带来额外监控内存压力。
  - 使用 HTTP 按需查询返回 JSON，用于根因排查和长周期观测。

## WAL 分片级指标验证记录：2026-07-04 运行后补充

- 编译：
  - 命令：`cd build && set -o pipefail; make -j10 2>&1 | tee ../task-wal/build_20260704_wal_stats_metrics_retry.log`
  - 结果：成功，`nebula-storaged`、`nebula-metad`、tools 等目标均构建完成。
- 安装：
  - 命令：`cd build && set -o pipefail; make install 2>&1 | tee ../task-wal/install_20260704_wal_stats_metrics.log`
  - 结果：核心二进制已安装到 `/usr/local/nebula/bin`；安装后续修改 `/usr/local/nebula/etc/nebula-metad.conf.default` 权限时报 `Operation not permitted`，因此 `make install` 最终返回失败。该失败点与本次代码逻辑无关，需要单独处理安装目录权限。
- 启停和状态：
  - 停止旧进程日志：`task-wal/runtime_stop_before_wal_stats_20260704.log`
  - 启动命令执行前已设置 `ulimit -n 65536`。
  - 启动和状态日志：`task-wal/runtime_start_20260704_wal_stats_metrics.log`
  - 结果：`metad`、`storaged`、`graphd` 均处于运行状态。
- `/wal_stats` 查询：
  - 日志：`task-wal/runtime_wal_stats_20260704.log`
  - 全量查询命令：`curl --noproxy '*' -s 'http://127.0.0.1:19779/wal_stats'`
  - 返回关键汇总：
    - `total.parts=84`
    - `total.active_bytes=1344`
    - `total.active_nodes=84`
    - `total.dirty_bytes=0`
    - `total.dirty_nodes=0`
    - `total.reader_refs=0`
    - `total.gc_count=0`
    - `total.gc_deleted_bytes=0`
    - `total.heartbeat_empty_logs=84`
  - 指定分片查询命令：`curl --noproxy '*' -s 'http://127.0.0.1:19779/wal_stats?space=1&part=1'`
  - 结果：只返回 `space_id=1`、`part_id=1` 的本地分片明细。
  - 异常参数查询命令：`curl --noproxy '*' -s -o - -w '\nHTTP_CODE=%{http_code}\n' 'http://127.0.0.1:19779/wal_stats?space=abc'`
  - 结果：返回 `HTTP_CODE=400`，说明参数校验生效。
- 本次指标能支持的判断：
  - `active_bytes/active_nodes` 用于观察当前仍在 WAL 内存 buffer 中的记录规模。
  - `dirty_bytes/dirty_nodes` 用于观察已经逻辑删除但等待 GC 的 WAL buffer。
  - `reader_refs` 用于判断 GC 是否可能被 WAL iterator 引用阻塞。
  - `gc_count/gc_deleted_bytes/gc_deleted_nodes` 用于判断 GC 是否发生以及释放规模。
  - `heartbeat_empty_logs` 用于确认 heartbeat 空 log 是否仍在某些分片持续产生。
