# Nebula 指标深度分析（master，运维监控视角）

> 分析范围：以当前仓库 master 代码中 Nebula 自带 `StatsManager` 指标为主，重点覆盖 `/stats` 可读出的业务指标。  
> 特别说明：你要求“暂不关注 rocksdb 和 raft 指标”，因此本文**不展开** `KVStats`（如 `append_wal_latency_us`、`num_start_elect` 等）与 RocksDB 内部指标。

---

## 1. 指标体系与采集方式

## 1.1 指标暴露入口

Nebula 的内建统计由 WebService 路由 `/stats` 暴露，可按 query 参数读取：

- 全量读取：`GET /stats`
- 指定项读取：`GET /stats?stats=<metric.method.window>`（多个逗号分隔）
- JSON 输出：`GET /stats?stats=...&format=json`

示例：

- `num_queries.rate.60`：过去 60 秒平均 QPS
- `query_latency_us.p99.600`：过去 10 分钟的 99 分位延迟

## 1.2 命名结构

每个指标都可按如下结构读取：

```text
<counter_name>.<statistic_type>.<time_range>
```

- `statistic_type` 常见：`sum / count / avg / rate / p75 / p95 / p99 / p999`
- `time_range` 支持：`5 / 60 / 600 / 3600` 秒

## 1.3 有标签（space 维度）指标

当 `--enable_space_level_metrics=true` 时，Graph 侧会为部分指标动态创建 `space=<spaceName>` 标签版本（逻辑上是同名指标的按图库分维）。

---

## 2. 指标总览与分组

> 以下为“需重点监控”的 Nebula 内建业务指标（排除 raft/rocksdb 细项），并补充每项用途。

## 2.1 Graphd：查询与会话核心指标

| 指标名 | 类型 | 主要用途 |
|---|---|---|
| `num_queries` | counter | 总查询量/QPS 基线，容量与趋势判断 |
| `num_active_queries` | gauge-like counter | 当前并发执行查询数，拥塞与排队风险 |
| `num_slow_queries` | counter | 慢查询数量，性能退化主哨兵 |
| `num_query_errors` | counter | 查询错误总量，稳定性主指标 |
| `num_query_errors_leader_changes` | counter | 因 leader 变化导致的失败，拓扑/一致性扰动定位 |
| `num_sentences` | counter | nGQL 语句执行总量（一个 query 可含多句） |
| `query_latency_us` | histogram | 全量查询延迟分位（p95/p99） |
| `slow_query_latency_us` | histogram | 慢查询延迟分布，定位长尾 |
| `num_killed_queries` | counter | 被 kill 查询数，排障与治理行为观测 |
| `num_queries_hit_memory_watermark` | counter | 命中内存水位保护次数，容量预警 |
| `optimizer_latency_us` | histogram | 优化器耗时，SQL 复杂度/规划压力 |
| `num_aggregate_executors` | counter | 聚合执行器触发量，评估聚合负载 |
| `num_sort_executors` | counter | 排序执行器触发量，评估排序热点 |
| `num_indexscan_executors` | counter | 索引扫描触发量，评估索引路径使用 |
| `num_Limit_executors` | counter | Limit 执行器触发量 |
| `limit_executors_latency_us` | histogram | Limit 阶段耗时分布 |
| `num_opened_sessions` | counter | 新建 session 总量，连接活跃度 |
| `num_auth_failed_sessions` | counter | 认证失败总量，安全/配置异常 |
| `num_auth_failed_sessions_bad_username_password` | counter | 用户名密码错误导致失败 |
| `num_auth_failed_sessions_out_of_max_allowed` | counter | 超过每用户每 IP 会话上限导致失败 |
| `num_active_sessions` | gauge-like counter | 当前活跃 session 数，连接池与资源压测核心 |
| `num_reclaimed_expired_sessions` | counter | 被回收过期会话数量，空闲治理有效性 |

### 运维解读建议（Graphd）

- **负载面**：`num_queries.rate.60` + `num_active_queries.sum.60`
- **性能面**：`query_latency_us.p95.60` + `query_latency_us.p99.60` + `num_slow_queries.rate.60`
- **稳定性面**：`num_query_errors.rate.60`、`num_query_errors_leader_changes.rate.60`
- **容量面**：`num_queries_hit_memory_watermark.rate.60`、`num_active_sessions.sum.60`

---

## 2.2 Graphd：访问下游 RPC 指标（客户端侧）

| 指标名 | 类型 | 主要用途 |
|---|---|---|
| `num_rpc_sent_to_metad` | counter | graphd -> metad RPC 发送总量 |
| `num_rpc_sent_to_metad_failed` | counter | graphd -> metad RPC 失败量 |
| `num_rpc_sent_to_storaged` | counter | graphd -> storaged RPC 发送总量 |
| `num_rpc_sent_to_storaged_failed` | counter | graphd -> storaged RPC 失败量 |

### 运维解读建议

- 失败率建议计算：
  - `num_rpc_sent_to_metad_failed.rate.60 / num_rpc_sent_to_metad.rate.60`
  - `num_rpc_sent_to_storaged_failed.rate.60 / num_rpc_sent_to_storaged.rate.60`
- 当失败率与 `num_query_errors` 同时抬升时，优先检查网络抖动、下游节点健康、连接池耗尽。

---

## 2.3 Storaged：图数据变更吞吐指标

| 指标名 | 类型 | 主要用途 |
|---|---|---|
| `num_edges_inserted` | counter | 边写入吞吐 |
| `num_vertices_inserted` | counter | 点写入吞吐 |
| `num_edges_deleted` | counter | 边删除吞吐 |
| `num_tags_deleted` | counter | Tag 删除吞吐 |
| `num_vertices_deleted` | counter | 点删除吞吐 |

### 运维解读建议

- 这些指标适合用于构建“写放大观察面板”：
  - 写入峰值变化是否对应查询延迟抬升；
  - 删除任务是否引起短时延迟尖峰。

---

## 2.4 Storaged：RPC 处理器通用指标（动态生成）

Storaged 的大多数处理器会自动生成 3 类指标：

- `num_<op>`：调用次数
- `num_<op>_errors`：错误次数
- `<op>_latency_us`：处理延迟分布

当前代码中初始化的 `<op>` 包括：

`add_vertices`、`add_edges`、`delete_vertices`、`delete_tags`、`delete_edges`、`update_vertex`、`update_edge`、`get_neighbors`、`get_dst_by_src`、`get_prop`、`lookup`、`scan_vertex`、`scan_edge`、`kv_put`、`kv_get`、`kv_remove`。

因此可推导出完整指标族（示例）：

- `num_add_vertices` / `num_add_vertices_errors` / `add_vertices_latency_us`
- `num_get_neighbors` / `num_get_neighbors_errors` / `get_neighbors_latency_us`
- `num_scan_edge` / `num_scan_edge_errors` / `scan_edge_latency_us`
- ...（共 16 * 3 = 48 项）

### 运维解读建议

- 这些是**定位 storaged 读写瓶颈的第一现场指标**。
- 典型定位方法：
  1) 先看 `num_<op>_errors.rate.60` 是否异常；
  2) 再看 `<op>_latency_us.p95.60 / p99.60`；
  3) 最后对照上游 `num_rpc_sent_to_storaged_failed` 和 graph 查询错误。

---

## 2.5 Metad：心跳链路指标

| 指标名 | 类型 | 主要用途 |
|---|---|---|
| `num_heartbeats` | counter | graphd/storaged 等向 metad 心跳次数 |
| `heartbeat_latency_us` | histogram | 心跳处理延迟 |
| `num_agent_heartbeats` | counter | 运维 agent 心跳次数 |
| `agent_heartbeat_latency_us` | histogram | agent 心跳处理延迟 |

### 运维解读建议

- 心跳延迟上升 + leader change 相关错误上升，常提示元数据面存在抖动或负载挤压。

---

## 3. 指标优先级清单（P0/P1/P2）

## P0（必须告警，强相关稳定性）

1. `num_query_errors.rate.60`
2. `query_latency_us.p99.60`
3. `num_queries_hit_memory_watermark.rate.60`
4. `num_rpc_sent_to_storaged_failed.rate.60`
5. `num_rpc_sent_to_metad_failed.rate.60`
6. `num_<核心读写op>_errors.rate.60`（如 `get_neighbors`、`get_prop`、`add_edges`）
7. `<核心读写op>_latency_us.p99.60`
8. `heartbeat_latency_us.p99.60`

## P1（建议告警，性能与容量趋势）

1. `num_slow_queries.rate.60`
2. `slow_query_latency_us.p99.60`
3. `num_active_queries.sum.60`
4. `num_active_sessions.sum.60`
5. `optimizer_latency_us.p95.60`
6. `num_query_errors_leader_changes.rate.60`

## P2（排障/优化辅助）

1. `num_sentences.rate.60`
2. `num_aggregate_executors.rate.60`
3. `num_sort_executors.rate.60`
4. `num_indexscan_executors.rate.60`
5. `num_Limit_executors.rate.60`
6. `limit_executors_latency_us.p95.60`
7. `num_reclaimed_expired_sessions.rate.60`
8. 图数据变更吞吐类（`num_edges_inserted` 等）

---

## 4. Grafana 面板组织建议

建议按“用户体验 -> 下游链路 -> 存储执行 -> 元数据健康”四层组织：

## Dashboard A：Query SLO 总览（Graphd）

- QPS：`num_queries.rate.60`
- 并发：`num_active_queries.sum.60`
- 延迟：`query_latency_us.p95.60 / p99.60`
- 慢查询：`num_slow_queries.rate.60`、`slow_query_latency_us.p99.60`
- 错误：`num_query_errors.rate.60`
- 内存保护：`num_queries_hit_memory_watermark.rate.60`

## Dashboard B：Session & Auth

- 新建会话速率：`num_opened_sessions.rate.60`
- 活跃会话：`num_active_sessions.sum.60`
- 认证失败：
  - `num_auth_failed_sessions.rate.60`
  - `num_auth_failed_sessions_bad_username_password.rate.60`
  - `num_auth_failed_sessions_out_of_max_allowed.rate.60`

## Dashboard C：Graph -> Meta/Storage RPC

- `num_rpc_sent_to_metad.rate.60` & failed rate
- `num_rpc_sent_to_storaged.rate.60` & failed rate
- 与 `num_query_errors.rate.60` 联动展示

## Dashboard D：Storaged 处理器

- 每个核心 op 三联图：
  1) `num_<op>.rate.60`
  2) `num_<op>_errors.rate.60`
  3) `<op>_latency_us.p95.60 / p99.60`
- 推荐重点 op：`get_neighbors`、`get_prop`、`lookup`、`add_edges`、`update_edge`

## Dashboard E：Meta 心跳

- `num_heartbeats.rate.60`
- `heartbeat_latency_us.p95.60/p99.60`
- 若使用运维 agent：`num_agent_heartbeats.rate.60`、`agent_heartbeat_latency_us.p99.60`

---

## 5. 告警建议（可直接落地）

> 阈值需按业务基线微调。以下为推荐起点（连续 3~5 分钟触发）。

1. **查询错误率告警（P0）**  
   条件：`num_query_errors.rate.60 > 0` 且持续升高；或错误率 > 1%

2. **查询长尾延迟告警（P0）**  
   条件：`query_latency_us.p99.60` 超过 SLO（例如 > 200ms 或你的业务阈值）

3. **内存水位保护告警（P0）**  
   条件：`num_queries_hit_memory_watermark.rate.60 > 0`

4. **storage RPC 失败率告警（P0）**  
   条件：`num_rpc_sent_to_storaged_failed.rate.60 / num_rpc_sent_to_storaged.rate.60 > 1%`

5. **meta RPC 失败率告警（P0）**  
   条件：`num_rpc_sent_to_metad_failed.rate.60 / num_rpc_sent_to_metad.rate.60 > 1%`

6. **核心 storaged 操作错误告警（P0）**  
   条件：`num_get_neighbors_errors.rate.60 > 0` 或 `num_get_prop_errors.rate.60 > 0`

7. **核心 storaged 操作延迟告警（P0）**  
   条件：`get_neighbors_latency_us.p99.60` / `get_prop_latency_us.p99.60` 超阈值

8. **心跳处理延迟告警（P1）**  
   条件：`heartbeat_latency_us.p99.60` 连续高于基线（例如 > 100ms）

9. **会话异常告警（P1）**  
   条件：`num_active_sessions.sum.60` 接近系统经验上限；或
   `num_auth_failed_sessions_out_of_max_allowed.rate.60` 持续 > 0

10. **慢查询暴涨告警（P1）**  
    条件：`num_slow_queries.rate.60` 较 7 日同时间窗口基线上涨 N 倍

---

## 6. 实施建议（你要做监控开发时）

1. 先上线 P0 告警与 A/C/D 三个核心面板；
2. 将所有指标统一增加实例维度（host / service）；
3. 逐步打开 `--enable_space_level_metrics`，按空间做 TopN 分析（先在中小规模环境验证基数）；
4. 每次版本升级后自动对比 `/stats` 全量清单，识别新增/变更指标；
5. 将告警分级：故障告警（立即） vs 趋势告警（工单/通知）。

---

## 7. 本次范围外（按你的要求暂不分析）

- `kvstore/stats/KVStats.cpp` 中 raft 相关指标：
  - `append_wal_latency_us`
  - `replicate_log_latency_us`
  - `num_start_elect`
  - `num_grant_votes`
  - `num_send_snapshot`
  - 等
- RocksDB 原生统计项

如果你下一步需要，我可以在此文档基础上再补一版《raft + rocksdb 专项指标与故障地图》。
