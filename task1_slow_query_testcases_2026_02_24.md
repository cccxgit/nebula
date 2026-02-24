# 任务一测试用例与步骤（慢查询 NGQL 记录）

本文档仅提供测试用例内容与步骤，不执行测试。

## 1. 测试目标
- 当查询时延大于 `--slow_query_threshold_us` 时，慢查询日志文件应写入对应 NGQL。
- 兼容现有慢查询统计逻辑，不影响查询执行结果与错误码。

## 2. 相关配置项
- `--slow_query_threshold_us`
- `--enable_slow_query_log`
- `--slow_query_log_filename`
- `--slow_query_log_max_query_len`
- `--log_dir`

## 3. 单元测试用例（已实现）
对应源码：`src/graph/service/test/SlowQueryLoggerTest.cpp`

### 用例 1：基础写入
- 名称：`SlowQueryLoggerTest.WriteOneLine`
- 目的：验证单条慢查询日志写入成功且字段完整。
- 关键断言：
  - 文件存在并有 1 行。
  - 包含 `latency_us`、`threshold_us`、`space`、`user`、`session_id`、`plan_id`、`status_code`。
  - query 中换行被转义为 `\n`。

### 用例 2：开关关闭不写日志
- 名称：`SlowQueryLoggerTest.DisableLogByFlag`
- 目的：验证 `--enable_slow_query_log=false` 时不写文件。
- 关键断言：
  - 目标日志文件不存在。

### 用例 3：长 query 截断
- 名称：`SlowQueryLoggerTest.TruncateLongQuery`
- 目的：验证 `--slow_query_log_max_query_len` 生效。
- 关键断言：
  - 日志行中 `truncated=true`。
  - `query` 仅保留指定最大长度前缀。

### 用例 4：并发写入
- 名称：`SlowQueryLoggerTest.ConcurrentWrite`
- 目的：验证多线程并发写日志无丢行。
- 关键断言：
  - 行数等于 `线程数 * 每线程写入次数`。

## 4. 单元测试执行步骤（手工）
1. 开启测试构建：
   - `cmake -S . -B build -DENABLE_TESTING=ON`
2. 仅编译目标测试：
   - `cmake --build build --target slow_query_logger_test -j4`
3. 执行测试：
   - `./build/bin/test/slow_query_logger_test`
4. 预期结果：
   - 输出 `4 tests ... PASSED`。

## 5. 集成测试用例（建议）

### 用例 5：慢查询触发写日志（成功查询）
- 目的：验证 `QueryInstance` 慢查询路径会落盘 query。
- 前置：
  - 将 `--slow_query_threshold_us` 设为极小值（如 `1`）。
  - `--enable_slow_query_log=true`。
  - `--slow_query_log_filename=nebula-slow-query.log`。
- 步骤：
  1. 启动 graphd。
  2. 执行一条普通 nGQL（如 `MATCH ... RETURN ...`）。
  3. 查看 `${log_dir}/nebula-slow-query.log`。
- 预期：
  - 新增一行慢查询日志，包含该 nGQL 与 `latency_us`。

### 用例 6：慢查询触发写日志（失败查询）
- 目的：验证失败但超阈值的查询也会记录。
- 步骤：
  1. 保持阈值极小。
  2. 执行一条会执行失败的语句（非语法空语句，建议执行期失败）。
  3. 检查慢日志。
- 预期：
  - 有对应日志，`status_code` 为非成功码。

### 用例 7：非慢查询不写日志
- 目的：验证阈值过滤生效。
- 步骤：
  1. 将 `--slow_query_threshold_us` 设为较大值（如默认 `200000`）。
  2. 执行一条快速查询。
  3. 检查日志文件行数。
- 预期：
  - 行数不增加。

### 用例 8：关闭开关不写日志
- 目的：验证全局开关生效。
- 步骤：
  1. `--enable_slow_query_log=false` 并重启服务。
  2. 执行慢查询（阈值可设为 1us）。
  3. 检查日志文件。
- 预期：
  - 不新增日志行。

### 用例 9：日志文件名配置生效
- 目的：验证 `--slow_query_log_filename` 生效。
- 步骤：
  1. 设置 `--slow_query_log_filename=my-slow.log`。
  2. 执行慢查询。
  3. 检查 `${log_dir}/my-slow.log`。
- 预期：
  - 日志写入新文件名，默认文件不增长。

### 用例 10：长 query 截断（集成）
- 目的：验证线上配置下 query 截断行为。
- 步骤：
  1. `--slow_query_log_max_query_len=16`。
  2. 执行长 nGQL 慢查询。
  3. 检查日志中的 `truncated` 与 `query` 长度。
- 预期：
  - `truncated=true`，query 被截断至 16 字符（转义后内容可略长）。

## 6. 验收口径
- 仅慢查询写入慢日志。
- 日志行包含核心字段与 query。
- 并发写入无明显丢行。
- 关闭开关后不写日志。
- 不影响原有查询返回和慢查询统计指标。

