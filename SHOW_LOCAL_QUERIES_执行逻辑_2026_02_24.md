# SHOW LOCAL QUERIES 执行逻辑（NebulaGraph 源码）

## 1. 结论先看
- `SHOW LOCAL QUERIES;` 走的是 `ShowQueriesExecutor::showCurrentSessionQueries()` 分支。
- 它读取的是“**当前会话对象里的 queries 快照**”，不是去 metad 拉全局会话。
- 因此它展示的是“当前 session 下、当前时刻仍在 session.queries 中的 query”。

## 2. 语法解析阶段
- 语法定义：
  - `SHOW LOCAL QUERIES` -> `new ShowQueriesSentence()`（`isAll=false`）
  - `SHOW QUERIES` -> `new ShowQueriesSentence(true)`（`isAll=true`）
  - 位置：`src/parser/parser.yy:3288`
- AST 节点：
  - `ShowQueriesSentence(bool isAll=false)`
  - 位置：`src/parser/AdminSentences.h:800`
- 字符串化：
  - `!isAll` 时打印 `SHOW LOCAL QUERIES`
  - 位置：`src/parser/AdminSentences.cpp:424`

## 3. 校验与计划生成
- Validator 分派到 `ShowQueriesValidator`：
  - 位置：`src/graph/validator/Validator.cpp:250`
- 权限检查：
  - `kShowQueries` 直接 `Status::OK()`，默认允许执行
  - 位置：`src/graph/service/PermissionCheck.cpp:241`
- 输出列定义（8列）：
  - `SessionID, ExecutionPlanID, User, Host, StartTime, DurationInUSec, Status, Query`
  - 位置：`src/graph/validator/AdminValidator.cpp:657`
- 生成计划节点：
  - `ShowQueries::make(qctx, nullptr, sentence->isAll())`
  - 位置：`src/graph/validator/AdminValidator.cpp:672`
- 计划节点结构：
  - `ShowQueries` 节点持有 `isAll_` 标志位
  - 位置：`src/graph/planner/plan/Admin.h:1340`

## 4. 执行器绑定与执行分支
- 执行器工厂映射：
  - `PlanNode::kShowQueries -> ShowQueriesExecutor`
  - 位置：`src/graph/executor/Executor.cpp:536`
- 执行入口：
  - `ShowQueriesExecutor::execute()` 根据 `isAll` 选择分支
  - `!isAll` -> `showCurrentSessionQueries()`
  - 位置：`src/graph/executor/admin/ShowQueriesExecutor.cpp:14`

## 5. SHOW LOCAL QUERIES 的取数路径
- 代码路径：
  - `auto* session = qctx()->rctx()->session();`
  - `auto sessionInMeta = session->getSession();`
  - `addQueries(sessionInMeta, dataSet);`
  - 位置：`src/graph/executor/admin/ShowQueriesExecutor.cpp:27`
- `session->getSession()` 返回 `meta::cpp2::Session` 的**拷贝**（读锁保护）：
  - 位置：`src/graph/session/ClientSession.h:134`
- `addQueries()` 遍历 `session.get_queries()` 填充结果集：
  - `SessionID` 来自 session
  - `ExecutionPlanID` 来自 queries map key
  - `User/Host/StartTime/Duration/Status/Query` 来自 `QueryDesc`
  - 位置：`src/graph/executor/admin/ShowQueriesExecutor.cpp:71`

## 6. queries 是何时写入与删除
- Query 创建时写入 session.queries：
  - `QueryInstance` 构造函数调用 `session->addQuery()`
  - 位置：`src/graph/service/QueryInstance.cpp:33`
  - 真正写入 map：`session_.queries_ref()->emplace(...)`
  - 位置：`src/graph/session/ClientSession.cpp:36`
- Query 结束时删除 session.queries：
  - 成功路径 `onFinish()` 删除
  - 位置：`src/graph/service/QueryInstance.cpp:127`
  - 失败路径 `onError()` 也删除
  - 位置：`src/graph/service/QueryInstance.cpp:150`
  - 真正删除 map：`session_.queries_ref()->erase(epId)`
  - 位置：`src/graph/session/ClientSession.cpp:50`

## 7. 与 SHOW QUERIES 的差异
- `SHOW LOCAL QUERIES`：
  - 不访问 metad；
  - 读当前请求上下文 session 的本地快照；
  - 代码：`src/graph/executor/admin/ShowQueriesExecutor.cpp:27`
- `SHOW QUERIES`：
  - 调 `metaClient->listSessions()`；
  - 汇总所有 session；
  - 代码：`src/graph/executor/admin/ShowQueriesExecutor.cpp:45`
  - 注释明确：query 同步到 meta 可能不完整。

## 8. 时序图（简化）
```text
Client
  -> GraphService::future_executeWithParameter()
  -> QueryEngine::execute()
  -> QueryInstance::QueryInstance() -> session.addQuery(epId)
  -> Parser/Validator/Planner
  -> Executor::create(kShowQueries) -> ShowQueriesExecutor
  -> ShowQueriesExecutor::execute()
      -> showCurrentSessionQueries()
      -> session.getSession()  (copy)
      -> addQueries(session.queries)
  -> 返回 DataSet
  -> QueryInstance::onFinish()/onError() -> session.deleteQuery(epId)
```

## 9. 实际排障建议
- 看当前实例当前 session 的实时运行查询：用 `SHOW LOCAL QUERIES;`
- 看跨实例汇总查询快照：用 `SHOW QUERIES;`（但它受 session 上报周期影响）

