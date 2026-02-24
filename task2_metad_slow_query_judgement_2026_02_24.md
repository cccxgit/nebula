# 任务二结论：`SHOW QUERIES` / metad 与“全量慢查询”关系判断

## 一句话结论
你的判断**部分成立**，但“通过 metad leader 就能获取所有慢查询任务”这个结论**不成立**（不完整且不可靠）。

## 1. 你的猜测逐条核对

### 猜测 A：`SHOW QUERIES` 可以拿到所有 graphd 实例查询信息
- 基本成立，但要区分语义：
  - `SHOW LOCAL QUERIES`：只看本地会话（当前 graphd）  
    证据：`src/parser/parser.yy:3288`
  - `SHOW QUERIES`：走 meta 的 `listSessions()` 汇总  
    证据：`src/graph/executor/admin/ShowQueriesExecutor.cpp:45`
- 代码中已有明确注释：查询信息可能没有完全同步到 meta。  
  证据：`src/graph/executor/admin/ShowQueriesExecutor.cpp:44`

### 猜测 B：执行 NGQL 前后都会访问 metad
- 不成立（至少“查询登记/结束”这条链路不是每次都访问 metad）：
  - 查询开始时：`QueryInstance` 调 `session->addQuery()`，仅更新本地 `session_.queries`。  
    证据：`src/graph/service/QueryInstance.cpp:37`、`src/graph/session/ClientSession.cpp:36`
  - 查询结束时：`session->deleteQuery()`，也是本地删除。  
    证据：`src/graph/service/QueryInstance.cpp:140`、`src/graph/session/ClientSession.cpp:50`
  - 同步到 metad 是由后台线程周期上报，不是每条查询前后一次 RPC。  
    证据：`src/graph/session/GraphSessionManager.cpp:178`、`src/graph/session/GraphSessionManager.cpp:232`
  - 默认同步周期 60s。  
    证据：`src/graph/service/GraphFlags.cpp:15`

### 猜测 C：metad 持久化了所有 graphd 执行查询任务信息
- 仅“当前会话快照”层面成立，不是“全历史任务仓库”：
  - meta 的 `Session` 里确实有 `queries` 字段（KV 持久化）。  
    证据：`src/interface/meta.thrift:1085`
  - 但 `QueryStatus` 只有 `RUNNING/KILLING`，没有“已完成慢查询历史”语义。  
    证据：`src/interface/meta.thrift:1070`
  - 查询结束后 graphd 本地会删掉 query，下次周期同步会把 meta 里的对应 query 覆盖掉。  
    证据：`src/graph/session/ClientSession.cpp:50`、`src/meta/processors/session/SessionManagerProcessor.cpp:100`

## 2. 为什么“metad leader 获取所有慢查询”不可靠
- 同步是周期性的（默认 60s），短慢查询可能开始/结束都发生在两次同步之间，meta 根本看不到。
- `SHOW QUERIES` 基于 session 快照，天然是“近实时视图”，不是审计日志。
- meta 中 query 条目随查询结束被移除，不保留完整历史慢查询。
- 代码已明确承认不同步完全：`ShowQueriesExecutor` 注释直接说明。

## 3. 正确使用姿势（建议）
- 如果目标是“全量慢查询追溯/统计”：
  1. 以 graphd 慢查询日志为主（你已实现的 task1 方案）。
  2. 结合指标 `num_slow_queries`、`slow_query_latency_us` 做监控告警。
  3. 通过日志采集系统做跨实例汇总（而不是依赖 metad session 快照）。
- 如果目标是“当前正在跑的长查询”：
  1. 用 `SHOW QUERIES`（全局）做在线排障。
  2. 接受它是近实时、可能漏瞬时查询的事实。

## 4. 最终判断
- “metad leader 可以看 cluster 当前一部分运行查询快照”是合理的。
- “metad leader 可以拿到所有慢查询任务（尤其历史全量）”不合理。

