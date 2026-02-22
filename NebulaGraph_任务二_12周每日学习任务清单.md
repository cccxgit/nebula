# NebulaGraph 任务二：12 周每日学习任务清单（可打卡）

> 使用方式：每天 45-90 分钟，完成当天任务并打勾。
> 建议目录：在工程根目录新建 `study-notes/weekXX/dayXX.md` 记录当日结果。

## 0. 每日固定动作（每天都做）
- [ ] 阅读 20-40 分钟（文档/源码）。
- [ ] 实操 20-40 分钟（console 命令/断点/日志）。
- [ ] 记录 10-20 分钟（1 页以内学习笔记）。
- [ ] 回答 3 个问题：今天新学到了什么？还有什么不懂？明天要验证什么？

---

## 第 1 周：部署与 CRUD 基础

### Day 1
- [ ] 安装/启动 NebulaGraph（或确认本地环境可用）。
- [ ] 登录 `nebula-console`，执行 `SHOW HOSTS;`。
- [ ] 记录服务角色：graphd/metad/storaged。
- [ ] 产出：`study-notes/week01/day01.md`（含启动命令与截图/输出）。

### Day 2
- [ ] 完成空间与 Schema 建模：`CREATE SPACE/TAG/EDGE`。
- [ ] 学习 `USE <space>` 和 `DESCRIBE TAG/EDGE`。
- [ ] 记录 Tag/Edge 与关系建模原则。
- [ ] 产出：`study-notes/week01/day02.md`（你的最小图模型）。

### Day 3
- [ ] 练习 `INSERT VERTEX` / `INSERT EDGE`。
- [ ] 插入不少于 20 个点、30 条边。
- [ ] 验证插入结果：`FETCH PROP ON`。
- [ ] 产出：`study-notes/week01/day03.md`（插入脚本）。

### Day 4
- [ ] 学习 `GO` 基本语法和方向控制。
- [ ] 写 5 条不同 hops 的 `GO` 查询。
- [ ] 对比结果列含义（`_src/_dst/_rank/_type`）。
- [ ] 产出：`study-notes/week01/day04.md`。

### Day 5
- [ ] 练习 `LOOKUP` 和基础 `MATCH`。
- [ ] 创建 1 个 tag 索引并执行 `REBUILD INDEX`。
- [ ] 比较有索引/无索引查询差异。
- [ ] 产出：`study-notes/week01/day05.md`。

### Day 6
- [ ] 练习 `UPDATE` / `DELETE`（点、边、Tag）。
- [ ] 验证删除后查询行为。
- [ ] 总结 CRUD 对应 nGQL 模板。
- [ ] 产出：`study-notes/week01/day06.md`（CRUD 速查表）。

### Day 7（复盘）
- [ ] 回顾本周命令，整理成一个可重复执行脚本。
- [ ] 自测：从空库到可查询，30 分钟内重建。
- [ ] 列出下周问题清单（至少 5 条）。
- [ ] 产出：`study-notes/week01/day07.md`。

---

## 第 2 周：nGQL 进阶与执行计划基础

### Day 8
- [ ] 学习管道 `|`、变量 `\$var`、`YIELD`。
- [ ] 写 3 条多段流水线查询。
- [ ] 记录中间结果如何传递。
- [ ] 产出：`study-notes/week02/day01.md`。

### Day 9
- [ ] 学习 `GROUP BY`、`ORDER BY`、`LIMIT`。
- [ ] 做 2 组聚合统计练习。
- [ ] 记录排序和分页的常见坑。
- [ ] 产出：`study-notes/week02/day02.md`。

### Day 10
- [ ] 学习 `FIND PATH`/`GET SUBGRAPH`（选至少 1 个）。
- [ ] 分析结果结构。
- [ ] 对比 `GO` 与 `MATCH` 的适用场景。
- [ ] 产出：`study-notes/week02/day03.md`。

### Day 11
- [ ] 学习 `EXPLAIN` 与 `PROFILE`。
- [ ] 对 3 条查询执行 `PROFILE`。
- [ ] 记录 latency、rows、关键算子。
- [ ] 产出：`study-notes/week02/day04.md`。

### Day 12
- [ ] 学习权限与会话相关命令（用户、角色、session）。
- [ ] 执行 `SHOW SESSIONS` / `SHOW QUERIES`。
- [ ] 记录连接/会话生命周期。
- [ ] 产出：`study-notes/week02/day05.md`。

### Day 13
- [ ] 做一个小综合练习（建模->导入->查询->调优）。
- [ ] 至少完成 10 条业务查询。
- [ ] 输出你自己的查询规范。
- [ ] 产出：`study-notes/week02/day06.md`。

### Day 14（复盘）
- [ ] 汇总“常用 30 条 nGQL”。
- [ ] 整理“执行计划怎么看”的 1 页笔记。
- [ ] 准备进入架构/源码阶段。
- [ ] 产出：`study-notes/week02/day07.md`。

---

## 第 3 周：架构全景与三服务协作

### Day 15
- [ ] 阅读官方架构总览。
- [ ] 画出三服务 + 客户端交互图。
- [ ] 记录每个服务“只做什么/不做什么”。
- [ ] 产出：`study-notes/week03/day01.md`。

### Day 16
- [ ] 学习 graphd 角色：会话、解析、计划、执行。
- [ ] 将你熟悉的 1 条查询映射到 graphd 职责。
- [ ] 产出：`study-notes/week03/day02.md`。

### Day 17
- [ ] 学习 metad 角色：Schema、分区、leader 元数据。
- [ ] 执行 `SHOW PARTS` 观察分区信息。
- [ ] 产出：`study-notes/week03/day03.md`。

### Day 18
- [ ] 学习 storaged 角色：读写处理器、索引查询。
- [ ] 记录写路径和读路径的差别。
- [ ] 产出：`study-notes/week03/day04.md`。

### Day 19
- [ ] 对比 DDL 与 DML 经过的服务链路。
- [ ] 各举 1 个例子：`CREATE TAG` 和 `INSERT EDGE`。
- [ ] 产出：`study-notes/week03/day05.md`。

### Day 20
- [ ] 做一次“口述演练”：讲清 `GO` 的三服务交互。
- [ ] 将讲解写成 10-15 行提纲。
- [ ] 产出：`study-notes/week03/day06.md`。

### Day 21（复盘）
- [ ] 完成一页“架构速记卡”（术语+链路）。
- [ ] 自检：不看资料能否画出主流程。
- [ ] 产出：`study-notes/week03/day07.md`。

---

## 第 4 周：集群与运维基础认知

### Day 22
- [ ] 阅读集群部署与配置文档。
- [ ] 理解 `heartbeat`、leader、replica 参数含义。
- [ ] 产出：`study-notes/week04/day01.md`。

### Day 23
- [ ] 学习分区数和副本数设计原则。
- [ ] 给你的练习数据估算合理参数。
- [ ] 产出：`study-notes/week04/day02.md`。

### Day 24
- [ ] 学习 balance/rebuild/index rebuild 场景。
- [ ] 记录“何时触发、代价是什么”。
- [ ] 产出：`study-notes/week04/day03.md`。

### Day 25
- [ ] 学习备份恢复工具（nebula-br）基础能力。
- [ ] 记录“最小恢复流程”。
- [ ] 产出：`study-notes/week04/day04.md`。

### Day 26
- [ ] 学习日志位置与常见错误码。
- [ ] 归类 5 种典型故障（连接、权限、leader、超时、内存）。
- [ ] 产出：`study-notes/week04/day05.md`。

### Day 27
- [ ] 编写你的“运维检查清单 v1”。
- [ ] 包括启动前、运行中、变更后三部分。
- [ ] 产出：`study-notes/week04/day06.md`。

### Day 28（复盘）
- [ ] 做一次“故障演练脚本”设计（不一定实操）。
- [ ] 总结当前最大知识短板。
- [ ] 产出：`study-notes/week04/day07.md`。

---

## 第 5 周：源码入口层（GraphService / QueryEngine / QueryInstance）

### Day 29
- [ ] 阅读 `src/interface/graph.thrift`。
- [ ] 明确 `execute/executeWithParameter` 请求结构。
- [ ] 产出：`study-notes/week05/day01.md`。

### Day 30
- [ ] 阅读 `src/graph/service/GraphService.cpp`。
- [ ] 重点：`init`、`future_executeWithParameter`。
- [ ] 产出：`study-notes/week05/day02.md`。

### Day 31
- [ ] 阅读 `src/graph/service/QueryEngine.cpp`。
- [ ] 重点：schema/index manager、optimizer 初始化。
- [ ] 产出：`study-notes/week05/day03.md`。

### Day 32
- [ ] 阅读 `src/graph/service/QueryInstance.cpp`（上半）。
- [ ] 重点：`execute`、`validateAndOptimize`。
- [ ] 产出：`study-notes/week05/day04.md`。

### Day 33
- [ ] 阅读 `src/graph/service/QueryInstance.cpp`（下半）。
- [ ] 重点：`onFinish`、`onError`、错误码映射。
- [ ] 产出：`study-notes/week05/day05.md`。

### Day 34
- [ ] 阅读 `src/parser/GQLParser.h`。
- [ ] 记录 parse 入口和失败处理。
- [ ] 产出：`study-notes/week05/day06.md`。

### Day 35（复盘）
- [ ] 手写“请求进入 graphd 后的 20 步流程”。
- [ ] 对照源码修正遗漏。
- [ ] 产出：`study-notes/week05/day07.md`。

---

## 第 6 周：Validator / Planner / Optimizer 深入

### Day 36
- [ ] 阅读 `src/graph/validator/Validator.cpp`。
- [ ] 重点：`makeValidator` 分派表。
- [ ] 产出：`study-notes/week06/day01.md`。

### Day 37
- [ ] 阅读 `src/graph/validator/MaintainValidator.cpp`。
- [ ] 重点：`CreateTag/CreateEdge` 语义检查。
- [ ] 产出：`study-notes/week06/day02.md`。

### Day 38
- [ ] 阅读 `src/graph/validator/MutateValidator.cpp`（Insert）。
- [ ] 重点：`InsertVertices/InsertEdges::toPlan`。
- [ ] 产出：`study-notes/week06/day03.md`。

### Day 39
- [ ] 阅读 `src/graph/validator/MutateValidator.cpp`（Delete/Update）。
- [ ] 重点：删除点先查边、更新边双向更新。
- [ ] 产出：`study-notes/week06/day04.md`。

### Day 40
- [ ] 阅读 `src/graph/planner/Planner.cpp` 与 `PlannersRegister.cpp`。
- [ ] 重点：kind -> planner 的匹配机制。
- [ ] 产出：`study-notes/week06/day05.md`。

### Day 41
- [ ] 阅读 `src/graph/optimizer/Optimizer.cpp`。
- [ ] 重点：`findBestPlan`、`doExploration`、`postprocess`。
- [ ] 产出：`study-notes/week06/day06.md`。

### Day 42（复盘）
- [ ] 选 2 条语句（DDL + DML），写 AST->PlanNode 过程图。
- [ ] 产出：`study-notes/week06/day07.md`。

---

## 第 7 周：Scheduler / Executor 执行框架

### Day 43
- [ ] 阅读 `src/graph/scheduler/AsyncMsgNotifyBasedScheduler.cpp`（入口）。
- [ ] 重点：`schedule`、`doSchedule`。
- [ ] 产出：`study-notes/week07/day01.md`。

### Day 44
- [ ] 阅读同文件的 `runSelect/runLoop`。
- [ ] 重点：依赖 future 收敛、分支/循环执行。
- [ ] 产出：`study-notes/week07/day02.md`。

### Day 45
- [ ] 阅读 `src/graph/executor/Executor.cpp`（工厂映射）。
- [ ] 重点：PlanNode -> Executor。
- [ ] 产出：`study-notes/week07/day03.md`。

### Day 46
- [ ] 阅读 `Executor::open/close/checkMemoryWatermark`。
- [ ] 理解执行前后生命周期。
- [ ] 产出：`study-notes/week07/day04.md`。

### Day 47
- [ ] 阅读 `Executor::finish/drop`。
- [ ] 理解变量引用计数与生命周期优化。
- [ ] 产出：`study-notes/week07/day05.md`。

### Day 48
- [ ] 阅读 mutate executors：Insert/Delete/Update。
- [ ] 对齐你前一周的 validator toPlan。
- [ ] 产出：`study-notes/week07/day06.md`。

### Day 49（复盘）
- [ ] 完成一张“Scheduler + Executor 数据流图”。
- [ ] 自检：能否解释错误如何向上抛。
- [ ] 产出：`study-notes/week07/day07.md`。

---

## 第 8 周：MetaClient / StorageClient / Handler / Processor 全链路

### Day 50
- [ ] 阅读 `src/clients/storage/StorageClient.cpp`（请求构造）。
- [ ] 重点：`CommonRequestParam::toReqCommon`。
- [ ] 产出：`study-notes/week08/day01.md`。

### Day 51
- [ ] 阅读 `src/clients/storage/StorageClientBase-inl.h`。
- [ ] 重点：`collectResponse` 并发聚合与失败收敛。
- [ ] 产出：`study-notes/week08/day02.md`。

### Day 52
- [ ] 阅读同文件 `getResponse`、`clusterIdsToHosts`。
- [ ] 重点：leader changed 更新与路由。
- [ ] 产出：`study-notes/week08/day03.md`。

### Day 53
- [ ] 阅读 `src/clients/meta/MetaClient.cpp`。
- [ ] 重点：`getResponse` 重试、`createTagSchema/createEdgeSchema`。
- [ ] 产出：`study-notes/week08/day04.md`。

### Day 54
- [ ] 阅读 `src/meta/MetaServiceHandler.cpp` + `CreateTag/EdgeProcessor.cpp`。
- [ ] 重点：RPC 分发与 KV 持久化。
- [ ] 产出：`study-notes/week08/day05.md`。

### Day 55
- [ ] 阅读 `src/storage/GraphStorageServiceHandler.cpp` + 1 个 mutate + 1 个 query processor。
- [ ] 产出：`study-notes/week08/day06.md`。

### Day 56（复盘）
- [ ] 画两条全链路时序图：`CREATE TAG`、`UPDATE EDGE`。
- [ ] 产出：`study-notes/week08/day07.md`。

---

## 第 9 周：执行计划与查询调优实战

### Day 57
- [ ] 挑选 3 条慢查询作为基线。
- [ ] 记录 profile 结果。
- [ ] 产出：`study-notes/week09/day01.md`。

### Day 58
- [ ] 调整查询写法（过滤前置、减少扩展范围）。
- [ ] 比较前后 latency。
- [ ] 产出：`study-notes/week09/day02.md`。

### Day 59
- [ ] 调整索引（新增/重建/替换）。
- [ ] 对比 `LOOKUP/MATCH` 执行变化。
- [ ] 产出：`study-notes/week09/day03.md`。

### Day 60
- [ ] 分析大结果集对性能影响。
- [ ] 通过 `LIMIT/ORDER BY` 控制代价。
- [ ] 产出：`study-notes/week09/day04.md`。

### Day 61
- [ ] 针对 1 条复杂查询，逐算子解释瓶颈。
- [ ] 形成“调优建议清单”。
- [ ] 产出：`study-notes/week09/day05.md`。

### Day 62
- [ ] 整理“查询调优 SOP v1”。
- [ ] 包含：定位->诊断->改写->验证。
- [ ] 产出：`study-notes/week09/day06.md`。

### Day 63（复盘）
- [ ] 形成 1 份完整调优报告（前后对照）。
- [ ] 产出：`study-notes/week09/day07.md`。

---

## 第 10 周：一致性、容错与异常路径

### Day 64
- [ ] 学习 Raft 核心概念（选举/复制/提交）。
- [ ] 将概念映射到 Nebula 分区副本。
- [ ] 产出：`study-notes/week10/day01.md`。

### Day 65
- [ ] 阅读 leader 相关缓存与更新逻辑（meta/storage client）。
- [ ] 记录缓存失效策略。
- [ ] 产出：`study-notes/week10/day02.md`。

### Day 66
- [ ] 专题：`E_LEADER_CHANGED` 错误从哪里来，怎么恢复。
- [ ] 画错误传播路径。
- [ ] 产出：`study-notes/week10/day03.md`。

### Day 67
- [ ] 专题：`partial success` 的语义和边界。
- [ ] 阅读 `failedParts/completeness` 处理流程。
- [ ] 产出：`study-notes/week10/day04.md`。

### Day 68
- [ ] 阅读超时与重试配置项（meta/storage client timeout/retry）。
- [ ] 给出你的推荐默认值（学习环境版）。
- [ ] 产出：`study-notes/week10/day05.md`。

### Day 69
- [ ] 编写“异常排障手册 v1”（连接失败、leader 变更、执行错误）。
- [ ] 产出：`study-notes/week10/day06.md`。

### Day 70（复盘）
- [ ] 做 20 分钟口述：一条查询失败后系统如何恢复。
- [ ] 产出：`study-notes/week10/day07.md`。

---

## 第 11 周：内存保护与并发控制

### Day 71
- [ ] 阅读内存检查相关工具类与 guard 使用点。
- [ ] 产出：`study-notes/week11/day01.md`。

### Day 72
- [ ] 阅读 graph 执行器中的内存水位检查。
- [ ] 重点：`checkMemoryWatermark`。
- [ ] 产出：`study-notes/week11/day02.md`。

### Day 73
- [ ] 阅读 storage client 聚合过程中的内存中断逻辑。
- [ ] 重点：`collectResponse` 中断未发请求。
- [ ] 产出：`study-notes/week11/day03.md`。

### Day 74
- [ ] 阅读 storaged query processor 的 memory exceeded 返回路径。
- [ ] 产出：`study-notes/week11/day04.md`。

### Day 75
- [ ] 学习 inflight 并发参数（`max_storage_inflight_per_query`）。
- [ ] 设计 2 组参数对比实验。
- [ ] 产出：`study-notes/week11/day05.md`。

### Day 76
- [ ] 阅读 executor 生命周期优化（drop/userCount）。
- [ ] 解释 acquire/release 在这里的作用。
- [ ] 产出：`study-notes/week11/day06.md`。

### Day 77（复盘）
- [ ] 形成“内存与并发调优清单 v1”。
- [ ] 产出：`study-notes/week11/day07.md`。

---

## 第 12 周：小改动闭环（从学习到贡献）

### Day 78
- [ ] 选定一个小改动题目（日志、错误提示、小优化、注释重构）。
- [ ] 写清“目标/影响范围/风险”。
- [ ] 产出：`study-notes/week12/day01.md`。

### Day 79
- [ ] 阅读相关模块上下文并确定改动点。
- [ ] 补齐最小设计说明。
- [ ] 产出：`study-notes/week12/day02.md`。

### Day 80
- [ ] 实施代码改动（小步提交本地 patch）。
- [ ] 编译并修复基础问题。
- [ ] 产出：`study-notes/week12/day03.md`。

### Day 81
- [ ] 增加或运行对应测试（单测/集成/手测脚本）。
- [ ] 记录测试证据。
- [ ] 产出：`study-notes/week12/day04.md`。

### Day 82
- [ ] 做回归检查与性能对比（如适用）。
- [ ] 更新文档与注释。
- [ ] 产出：`study-notes/week12/day05.md`。

### Day 83
- [ ] 整理 PR 材料：背景、改动、风险、测试、截图/日志。
- [ ] 做一次自审（是否影响兼容性）。
- [ ] 产出：`study-notes/week12/day06.md`。

### Day 84（最终复盘）
- [ ] 完成“12 周学习总报告”。
- [ ] 给出下一阶段计划（性能专项/存储专项/查询引擎专项）。
- [ ] 产出：`study-notes/week12/day07.md`。

---

## 附录 A：每周达标检查
- [ ] 本周是否完成 6 天学习 + 1 天复盘？
- [ ] 是否至少输出 6 份日记 + 1 份周总结？
- [ ] 是否能口述本周核心调用链？
- [ ] 是否有可复现命令/脚本？

## 附录 B：日记模板（复制即用）
```md
# WeekXX DayXX

## 今日目标
- 

## 今日完成
- 

## 关键命令/函数/文件
- 

## 结果与证据
- 

## 遇到的问题
- 

## 明日计划
- 
```
