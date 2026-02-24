# 任务三执行结果：metad leader 周期扫描慢查询方案评估与实现设计

## 1. 结论（先给答案）
- 可以做：在 **metad leader** 增加周期任务，扫描 session/query 元数据，识别“正在执行且超过阈值”的 query，并写入 metad 慢查询日志。
- 但不等价于“全量慢查询系统”：该方案天然是**近实时补充视图**，不能替代 graphd 侧的慢查询落盘。
- 关键原因：graphd 上报 query 到 meta 是周期同步（默认 60s），短慢查询可能完全错过；且 meta 中 `QueryStatus` 只表达 `RUNNING/KILLING`，不是历史慢查询仓库。

## 2. 源码证据链（当前实现）
- `SHOW QUERIES` 全局路径来自 meta `listSessions()`：`src/graph/executor/admin/ShowQueriesExecutor.cpp:45`
- 代码明确说明 query 同步到 meta 可能不完整：`src/graph/executor/admin/ShowQueriesExecutor.cpp:44`
- query 在 graphd 本地进入/退出会话：
  - 进入：`src/graph/service/QueryInstance.cpp:37` -> `src/graph/session/ClientSession.cpp:36`
  - 退出：`src/graph/service/QueryInstance.cpp:140` -> `src/graph/session/ClientSession.cpp:50`
- graphd 周期同步 session/query 到 meta：`src/graph/session/GraphSessionManager.cpp:232`
- 默认同步周期 60s：`src/graph/service/GraphFlags.cpp:15`
- meta 的 query 元数据结构：`src/interface/meta.thrift:1075`
- meta 会把更新后的 session 覆盖写回 KV：`src/meta/processors/session/SessionManagerProcessor.cpp:100`

## 3. 任务三需求拆解
需求原文核心：
1. 在 metad leader 做定时任务；
2. 周期查询底层存储；
3. 判断 query 时延是否超过 `FLAGS_slow_query_threshold_us`；
4. 超过后写独立慢日志；
5. 长查询不能重复记录。

## 4. 可实施方案（如果坚持在 metad 侧做）

### 4.1 设计定位
- 定位为：**Running Query Slow Detector（运行中慢查询探测器）**
- 作用：补充“当前正在执行的超时长查询”观察能力。
- 不承诺：覆盖所有已完成慢查询历史。

### 4.2 组件设计
- 新增 `meta/service/SlowQueryScanner`（建议）
  - 内含 `thread::GenericWorker` 定时扫描。
  - 每轮仅在本机是 leader 时执行（非 leader 直接返回）。
  - 扫描来源：`MetaKeyUtils::sessionPrefix()` 对应的 session KV 前缀。
- 新增 `meta/service/SlowQueryLogWriter`
  - 负责写独立日志文件（`log_dir/metad-slow-query.log`）。
  - 使用 `O_APPEND`，写失败降级告警，不影响主流程。

### 4.3 阈值与配置
- 不能直接复用 graphd 进程内 flag `FLAGS_slow_query_threshold_us`（定义在 graph 模块）。
- 需要在 meta 增加自己的配置（建议同名语义）：
  - `--meta_slow_query_threshold_us`
  - `--meta_slow_query_scan_interval_secs`
  - `--enable_meta_slow_query_log`
  - `--meta_slow_query_log_filename`

### 4.4 慢查询判定
- 对每条 query：
  - `elapsed_us = now_us - query.start_time`
  - 若 `query.status in {RUNNING, KILLING}` 且 `elapsed_us > meta_slow_query_threshold_us`，判定为慢。
- 注：优先使用 `now - start_time`，不依赖 `duration` 字段的新鲜度。

### 4.5 去重（满足“长查询只记一次”）
- 去重主键建议：
  - `(session_id, plan_id, start_time, graph_addr)`
- 去重状态存储分层：
  - L1：leader 内存 `unordered_set`（快速判重）
  - L2：meta KV 持久标记（防 leader 切换后重复）
- 标记清理策略：
  - 当 query 不再出现在 session.queries 中，累计 N 个扫描周期后清理其去重标记。
  - 或按时间窗口清理（例如 24h），防止无限增长。

### 4.6 日志内容
- 建议字段：
  - `ts`、`elapsed_us`、`threshold_us`
  - `session_id`、`plan_id`、`graph_addr`
  - `status`、`query`
- 示例：
```
ts=2026-02-24T22:00:01.123456+08:00 elapsed_us=8123456 threshold_us=200000 session_id=101 plan_id=9901 graph_addr=10.0.0.1:9669 status=RUNNING query="MATCH (v) RETURN v"
```

## 5. 风险与边界
- 漏报：短慢查询可能在两次 graph->meta 同步间开始并结束，meta 看不到。
- 重复风险：leader 切换时若仅内存去重会重复记录，必须加持久去重标记。
- 性能风险：频繁全量扫描 session 前缀会给 meta 带来额外压力，需要控制扫描周期与日志频率。
- 语义差异：该方案观察的是“运行中慢查询”，不是“执行完成后的慢查询统计全景”。

## 6. 推荐落地策略
- 主路径：继续以 graphd 侧慢查询落盘为准（已实现）。
- 增强路径：metad 侧只做“运行中慢查询探测补充”，并在文档中明确其非全量语义。

## 7. 验收标准（任务三）
- metad leader 周期任务可运行并扫描 session KV。
- 对同一长查询（同去重主键）最多记录 1 次。
- 非 leader 不执行扫描，不写日志。
- 写日志失败不影响 meta 主流程。
- 文档明确“非全量慢查询”边界。

---------------------
按照你的结论，在我执行show queries可能查出的语句在console中显示未执行完，实际在nebula内核已经执行完了。这因为存在延时session_reclaim_interval_secs?

是的，你这个现象和 session_reclaim_interval_secs 相关，但更准确说是“会话信息上报到 metad 的周期延迟”。

关键点：

SHOW QUERIES（不带 LOCAL）读的是 metad 的 listSessions 快照，不是每次实时拉各 graphd 内存状态。
查询在 graphd 内核执行完后，会立刻从本地 session.queries 删除。
但 metad 里的 session/query 要等后台线程下一次 updateSessionsToMeta() 才会同步掉。
这个后台线程周期由 session_reclaim_interval_secs 控制，默认是 60 秒。
所以会出现：

console 里 SHOW QUERIES 还看到“在跑”
实际该 query 在 graphd 已经执行完
补充：

SHOW LOCAL QUERIES 更接近当前 graphd 实时状态（但只看本地实例）。
session_idle_timeout_secs 不是这个“显示延迟”的主因。
把 session_reclaim_interval_secs 调小可以减轻延迟，但会增加 metad 压力。