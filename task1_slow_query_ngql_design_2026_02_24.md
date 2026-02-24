# 任务一设计方案：慢查询 NGQL 记录功能

## 1. 目标与范围
- 目标：当查询总时延 `latency_us > FLAGS_slow_query_threshold_us` 时，把对应 NGQL 语句写入独立的慢查询日志。
- 范围：仅设计，不修改代码实现。
- 兼容性：保留现有慢查询统计指标逻辑（`num_slow_queries`、`slow_query_latency_us`），新增“日志记录”能力。

## 2. 现状分析（基于当前源码）
- 已有阈值配置：`src/graph/stats/GraphStats.cpp` 中 `DEFINE_int32(slow_query_threshold_us, 200000, ...)`。
- 已有统计埋点：`src/graph/service/QueryInstance.cpp` 的 `addSlowQueryStats(...)` 中判定慢查询并累加统计。
- 当前缺口：慢查询仅做 metrics，无专用慢查询日志文件，也未稳定输出完整 NGQL。

## 3. 设计原则
- 低侵入：复用 `QueryInstance` 现有慢查询判定时机，不改执行主流程。
- 可运维：慢查询写独立文件，格式固定、便于 grep/采集。
- 低开销：仅在“已判定为慢查询”后做一次文本写入。
- 可降级：日志写失败不影响查询结果，仅打印告警。

## 4. 总体方案
- 在 Graph 服务增加一个轻量 `SlowQueryLogger`（单例/进程级实例）。
- `QueryInstance` 在 slow 判定成立后，除原有统计外，再调用 `SlowQueryLogger::log(...)` 输出一行结构化日志。
- 日志文件放在 `FLAGS_log_dir` 下，文件名可配置（默认 `nebula-slow-query.log`）。

## 5. 关键设计细节

### 5.1 新增配置项（建议）
- `--enable_slow_query_log=true`
  - 是否启用慢查询日志记录。
- `--slow_query_log_filename=nebula-slow-query.log`
  - 慢查询日志文件名（相对 `--log_dir`）。
- `--slow_query_log_max_query_len=4096`
  - 单条 NGQL 最大落盘长度，超过截断并标记 `truncated=true`。

说明：
- 阈值继续复用现有 `--slow_query_threshold_us`，不新增重复阈值配置。
- 配置声明建议放在 `src/graph/stats/GraphStats.h/.cpp`（与 slow query 语义集中），或放 `GraphFlags` 亦可；优先前者，便于维护。

### 5.2 日志内容与格式
- 单行日志（便于采集），建议 key=value：
  - `ts`：本地时间（ISO8601）
  - `latency_us`
  - `threshold_us`
  - `space`
  - `user`
  - `session_id`
  - `plan_id`（ExecutionPlanID）
  - `status`（SUCCEEDED / E_EXECUTION_ERROR 等）
  - `query`（转义换行后的一行 NGQL）

示例：
```
ts=2026-02-24T21:05:32.128+08:00 latency_us=512340 threshold_us=200000 space=test user=root session_id=17 plan_id=103 status=SUCCEEDED query="MATCH (v) RETURN v LIMIT 1000"
```

### 5.3 触发时机
- 保持当前慢查询判定点不变：`QueryInstance::addSlowQueryStats(...)`。
- 在 `if (latency > FLAGS_slow_query_threshold_us)` 分支内新增日志写入调用。
- `onFinish` 与 `onError` 已都会调用该函数，因此成功/失败慢查询都能覆盖。

### 5.4 代码落点（实现阶段建议）
- `src/graph/service/SlowQueryLogger.h/.cpp`
  - 提供 `initIfNeeded()`、`logSlowQuery(const SlowQueryRecord&)`。
  - 内部持有文件句柄和互斥锁，使用 `O_APPEND` 原子追加。
- `src/graph/service/QueryInstance.cpp`
  - 在现有慢查询判定分支补充 `SlowQueryLogger` 调用。
  - 采集字段来自 `rctx->query()`、`rctx->session()`、`qctx_->plan()->id()`、`rctx->resp().errorCode`。
- `src/graph/service/CMakeLists.txt`
  - 把 `SlowQueryLogger.cpp` 加入 `query_engine_obj`。
- `conf/nebula-graphd.conf.default`、`conf/nebula-graphd.conf.production`
  - 增加上述 3 个配置项及注释。

### 5.5 并发与性能
- 写路径只发生在慢查询分支，低频。
- 使用进程内互斥 + `O_APPEND`，避免多线程写入串行化之外的乱序/覆盖问题。
- 不做 `fsync`，交由 OS 缓冲；慢查询记录对极端断电可接受少量丢失。
- 对超长 query 做长度限制和字符转义，避免日志污染与爆量写入。

### 5.6 异常与降级
- 慢查询日志文件打开/写入失败：
  - 查询流程不失败。
  - `LOG(WARNING)` 打一次限频告警（避免刷屏）。
- 若 `session` 为空（理论上不常见），相关字段写默认值（如 `session_id=0 user=""`）。

## 6. 测试方案（实现后执行）

### 6.1 单元测试
- 新增 `src/graph/service/test/SlowQueryLoggerTest.cpp`：
  - 正常写入、并发写入、超长截断、特殊字符转义、写失败降级。

### 6.2 集成验证
- 配置 `--slow_query_threshold_us` 为很小值（如 `1`）制造慢查询。
- 执行多条 nGQL，确认：
  - 慢查询文件创建于 `log_dir`。
  - 每条日志包含 query 与 latency。
  - 与 `num_slow_queries` 指标数量匹配。

### 6.3 回归关注点
- 高并发下 query 吞吐是否受明显影响（预期无显著影响）。
- 非慢查询不应写入慢日志。
- 失败查询（语义执行失败但时延超阈值）应正确落盘。

## 7. 分阶段实施计划
- 第一阶段：提交最小可用能力（写文件 + 基础字段 + 截断）。
- 第二阶段：补充限频告警、更多字段、完善测试。
- 第三阶段：按需要增加日志轮转策略（或接入运维 logrotate 规范）。

## 8. 验收标准
- 当 `latency_us > slow_query_threshold_us` 时，慢查询日志中必有对应 NGQL 记录。
- 不影响原有响应结果与错误码。
- 慢查询统计指标行为与当前实现一致。
- 配置可开关，关闭后不写慢日志。

