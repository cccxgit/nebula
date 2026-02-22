# NebulaGraph 任务二：系统学习路径与源码技术进阶（初学者定制）

> 适用对象：已经开始阅读 NebulaGraph 3.6 源码、希望从“会用”进阶到“能改内核/能定位问题”的学习者。
>
> 版本说明：本文路线基于你当前工程 `nebula-release-3.6`，外部资料链接于 2026-02-22 复核可访问（部分 Bilibili 链接在自动抓取环境可能返回 412，但链接本身有效）。

## 1. 学习目标（3 层）
- 第 1 层（使用者）：能独立完成建模、导入、CRUD、查询调优。
- 第 2 层（源码阅读者）：能讲清一条 nGQL 在 `graphd/metad/storaged` 的调用链。
- 第 3 层（开发者）：能修改一个执行器/处理器并完成验证，提交规范 PR。

## 2. 总体路线（12 周）

### 阶段 A（第 1-2 周）：产品与语言基础
目标：先把 nGQL 和部署操作打牢，不急着“深潜”源码。

每周任务：
- Week 1
  - 完成单机部署、连接、基础 CRUD、索引创建与重建。
  - 输出：你的 `basketballplayer` 练习脚本（建模+导入+10 条查询）。
- Week 2
  - 重点学习 `GO`、`LOOKUP`、`MATCH`、`EXPLAIN/PROFILE`。
  - 输出：5 条查询的执行计划解读（为什么这样走）。

建议资料：
- 快速入门与 CRUD（官方）：
  - https://docs.nebula-graph.com.cn/3.5.0/2.quick-start/3.quick-start-on-premise/4.nebula-graph-crud/
- nGQL 命令汇总：
  - https://docs.nebula-graph.com.cn/3.5.0/2.quick-start/6.cheatsheet-for-ngql-command/
- nGQL 简明教程 vol.01：
  - https://www.nebula-graph.com.cn/posts/ngql-tutorial
- EXPLAIN/PROFILE：
  - https://docs.nebula-graph.com.cn/3.5.0/3.ngql-guide/17.query-tuning-statements/1.explain-and-profile/

---

### 阶段 B（第 3-4 周）：架构认知与服务边界
目标：理解为什么要分 `graphd/metad/storaged`，以及请求如何在三者间流转。

每周任务：
- Week 3
  - 学完架构总览 + 三大服务文档。
  - 输出：你自己画一张服务交互图（登录、DDL、DML、读查询四条路径）。
- Week 4
  - 在本地跑 `SHOW HOSTS/SHOW PARTS/SHOW META LEADER` 并对应到架构。
  - 输出：一份“分区与 leader 路由”口头讲解提纲。

建议资料：
- 架构总览：
  - https://docs.nebula-graph.com.cn/3.5.0/1.introduction/3.nebula-graph-architecture/1.architecture-overview/
- Meta 服务：
  - https://docs.nebula-graph.com.cn/3.5.0/1.introduction/3.nebula-graph-architecture/2.meta-service/
- Graph 服务：
  - https://docs.nebula-graph.com.cn/3.5.0/1.introduction/3.nebula-graph-architecture/3.graph-service/
- Storage 服务：
  - https://docs.nebula-graph.com.cn/3.5.0/1.introduction/3.nebula-graph-architecture/4.storage-service/

---

### 阶段 C（第 5-8 周）：源码主干精读（最关键）
目标：掌握从 Query 字符串到 RPC 请求的完整“编译+执行”链。

每周任务：
- Week 5（入口与上下文）
  - 阅读：`GraphService -> QueryEngine -> QueryInstance`。
  - 输出：手写调用链（函数级）。
- Week 6（Parser/Validator/Planner/Optimizer）
  - 阅读：`parser/`、`validator/`、`planner/`、`optimizer/`。
  - 输出：1 条 INSERT 和 1 条 GO 的 AST->PlanNode 过程。
- Week 7（Scheduler/Executor）
  - 阅读：`AsyncMsgNotifyBasedScheduler`、`Executor::makeExecutor`。
  - 输出：解释依赖调度、future 链、错误传播。
- Week 8（客户端与 RPC）
  - 阅读：`StorageClient/MetaClient`、`thrift` 接口定义。
  - 输出：`CREATE TAG` 和 `UPDATE EDGE` 的 RPC 时序图。

你当前仓库的推荐阅读顺序：
1. `src/interface/graph.thrift`
2. `src/graph/service/GraphService.cpp`
3. `src/graph/service/QueryEngine.cpp`
4. `src/graph/service/QueryInstance.cpp`
5. `src/parser/GQLParser.h`
6. `src/graph/validator/Validator.cpp`
7. `src/graph/planner/Planner.cpp` / `src/graph/planner/PlannersRegister.cpp`
8. `src/graph/optimizer/Optimizer.cpp`
9. `src/graph/scheduler/AsyncMsgNotifyBasedScheduler.cpp`
10. `src/graph/executor/Executor.cpp`
11. `src/clients/storage/StorageClient.cpp` / `src/clients/storage/StorageClientBase-inl.h`
12. `src/clients/meta/MetaClient.cpp`
13. `src/meta/MetaServiceHandler.cpp` + `src/meta/processors/*`
14. `src/storage/GraphStorageServiceHandler.cpp` + `src/storage/*Processor.cpp`

配套资料（源码向）：
- 新手阅读 NebulaGraph 源码的姿势：
  - https://www.nebula-graph.com.cn/posts/how-to-read-nebula-graph-source-code
- NebulaGraph Source Code Explained：
  - https://www.nebula-graph.io/posts/nebula-graph-source-code-reading-01
- Query Engine 概览（中/英）：
  - https://www.nebula-graph.com.cn/posts/nebula-graph-query-engine-overview
  - https://www.nebula-graph.io/posts/nebula-graph-query-engine-overview
- Storage Engine 概览：
  - https://www.nebula-graph.io/posts/nebula-graph-storage-engine-overview

---

### 阶段 D（第 9-12 周）：关键技术深挖 + 实战改造
目标：把“看懂代码”提升为“能改代码”。

每周任务：
- Week 9（执行计划与优化）
  - 学会解读 profile 字段、识别瓶颈算子。
  - 输出：3 条慢查询调优前后对比。
- Week 10（存储与一致性）
  - 深入分区、raft、leader 变更处理、部分成功语义。
  - 输出：`E_LEADER_CHANGED` 场景排障手册（你自己的版本）。
- Week 11（内存与并发）
  - 关注 `MemoryCheckGuard`、inflight 限流、executor 生命周期。
  - 输出：一次 OOM 保护实验记录。
- Week 12（小改动闭环）
  - 选 1 个小功能/日志改进/错误提示优化，完成：改动->测试->文档。
  - 输出：可 review 的 patch + 自测报告。

建议资料（执行计划/调优）：
- 执行计划文档：
  - https://docs.nebula-graph.com.cn/3.5.0/8.service-tuning/4.plan/
- nGQL 简明教程 vol.02（执行计划）：
  - https://discuss.nebula-graph.com.cn/t/topic/12010
- 从真实案例出发解读执行计划：
  - https://www.nebula-graph.com.cn/posts/Query-Parser

## 3. 关键技术知识点与对应外部资料（按主题）

### 3.1 分布式一致性 / Raft（对应 metad 与 storaged）
必学知识点：
- leader 选举、日志复制、成员变更、安全性约束。
- 分区（part）与副本（replica）如何映射到服务节点。

建议资料：
- Raft 论文（USENIX ATC 2014）：
  - https://www.usenix.org/conference/atc14/technical-sessions/presentation/ongaro
- NebulaGraph 论文（架构总览）：
  - https://arxiv.org/abs/2206.07278

### 3.2 存储引擎 / LSM（对应 storaged + RocksDB）
必学知识点：
- LSM、compaction、WAL、写放大/读放大/空间放大。
- 为什么图数据库常把点边编码到 KV key/value。

建议资料：
- RocksDB Wiki：
  - https://github.com/facebook/rocksdb/wiki
- RocksDB 仓库：
  - https://github.com/facebook/rocksdb

### 3.3 查询编译链（Parser/Validator/Planner/Optimizer）
必学知识点：
- AST、语义校验、逻辑到物理计划、RBO 优化。
- 为什么 EXPLAIN/PROFILE 结果能映射到算子执行成本。

建议资料：
- Bison C++ 示例（理解 parser 生成器思路）：
  - https://www.gnu.org/s/bison/manual/html_node/A-Complete-C_002b_002b-Example.html
- 执行计划专题文章（NebulaGraph 社区）：
  - https://www.nebula-graph.com.cn/posts/Query-Parser

### 3.4 RPC 与异步并发（Thrift/Folly/Future）
必学知识点：
- thrift 接口 IDL、请求/响应结构、失败重试与超时。
- Future 链式调度与异常传播。

建议资料：
- Apache Thrift Tutorial：
  - https://cwiki.apache.org/confluence/display/thrift/Tutorial
- Apache Thrift C++ 文档：
  - https://thrift.apache.org/lib/cpp.html
- Folly 仓库（含 futures 相关实现）：
  - https://github.com/facebook/folly

### 3.5 C++ 内存模型（源码读懂并发逻辑的基础）
必学知识点：
- `memory_order_acquire/release/seq_cst`。
- 原子计数与结果生命周期管理（如 executor drop 流程）。

建议资料：
- cppreference `std::memory_order`：
  - https://www.cppreference.com/w/cpp/atomic/memory_order.html

## 4. 视频/课程入口（官方优先）
- NebulaGraph Academy（培训总入口）：
  - https://academic.nebula-graph.io/intro/
- 图基础知识课程：
  - https://academic.nebula-graph.io/graph-basics/basic-knowledge/overview/
- 实践课程（应用开发）：
  - https://academic.nebula-graph.io/practice-nebulagraph-app/
- 官方 YouTube 频道：
  - https://www.youtube.com/channel/UC73V8q795eSEMxDX4Pvdwmw
- 官方 Bilibili 空间（文档学习路径页给出）：
  - https://space.bilibili.com/472621355
- 文档学习路径中的示例视频（可直接访问）：
  - 图世界概念术语：https://www.bilibili.com/video/BV17X4y1A7p9
  - 编译源码安装教程：https://www.bilibili.com/video/BV1YJ411i7Jn
  - Studio 图探索功能：https://www.bilibili.com/video/BV1QN411Z7Vh
  - nebula-br 工具介绍：https://www.bilibili.com/video/BV11L4y1g7rD

## 5. 你的“每周复盘模板”（建议固定执行）
每周固定回答 5 个问题：
1. 我本周读完了哪些模块（文件路径）？
2. 我能否把调用链口述到函数级？
3. 我本周做了哪些可复现实验（命令+结果）？
4. 哪些地方仍是黑盒（下周怎么打通）？
5. 我沉淀了哪些文档/脚本/图？

## 6. 达标标准（12 周结束）
- 能独立解释 4 条路径：`CREATE TAG`、`INSERT EDGE`、`UPDATE EDGE`、`GO/MATCH`。
- 能根据 `PROFILE` 找出慢点并提出可执行优化建议。
- 能完成一次小改动并给出测试证据（不是只改注释）。
- 能把你当前仓库中的关键模块关系画成一页图并讲清楚。

---

如果你愿意，我可以在下一步按这个学习路径直接给你生成一份“第 1-2 周的每日任务清单（可打卡版）”，每天 45~90 分钟即可执行。
