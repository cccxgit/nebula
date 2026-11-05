# NebulaGraph 内置 Tantivy 竞赛交付材料

本原型将 Tantivy 嵌入普通 `storaged`，支持普通属性 `CONTAINS`、`STARTS WITH`、`ENDS WITH` 查询下推，也提供 `TEXT_SEARCH` 词法全文检索。业务使用普通谓词时无需改成 `ES_QUERY()`。

已有 **93/93 项不重复的真实集群功能检查**通过记录，部署为单机三个 storage 进程、20 分区、3 副本；底层另有 **9 项真实磁盘 Rust 测试**及 C++ 静态库集成测试。百万真实记录已完成同集群下推开关对照，并发 1 的本轮 QPS 比值为 **112.883 倍**，这不是 ES 加速比。该 MATCH 对照先于最新 TopK/清空/重建修正；新版生命周期、TopK 与立即重建已有复验，新版百万原生检索和 ES 绝对指标也已实测。两引擎完整结果集合不同，不能计算跨引擎加速或降配比例；五百万真实记录已完成导入、完整结果集合与查询实测，不宣称已经完成亿级验证。

## 材料导航

| 材料 | 内容 |
| --- | --- |
| [company-migration.md](company-migration.md) | 公司电脑克隆、无 Rust 构建、三 storage 验证与参赛材料迁移 |
| [build-and-run.md](build-and-run.md) | 实测工具链、最多 5 路构建、依赖锁、隔离部署与正确性复现 |
| [architecture.md](architecture.md) | 数据流、索引结构、写入屏障、生命周期、评分与恢复边界 |
| [demo-guide.md](demo-guide.md) | 现场三分钟演示顺序、只读查询、完整功能复现及失败时的处理 |
| [speaker-notes-and-qa.md](speaker-notes-and-qa.md) | 八页讲稿、评委追问及当前可支持的答复 |
| [fulltext-compatibility.md](fulltext-compatibility.md) | 原生全文语法逐项状态、analyzer 差异及候选集规模限制 |
| [benchmark-data.md](benchmark-data.md) | 公开语料来源、许可、128 字符投影与数据准备证据 |
| [benchmark-runbook.md](benchmark-runbook.md) | 分阶段实测、完整结果集合校验、同机串行对照方法 |
| [benchmark-report.md](benchmark-report.md) | 百万与五百万导入、查询及资源实测，ES 绝对观测与容量门槛 |
| [benchmark-report-template.md](benchmark-report-template.md) | 最终性能报告字段和统计口径；未填项不代表已测 |

正式答辩使用 [八页竞赛演示文稿](nebula-tantivy-competition-final-20261105-v2.pptx)，对应最终 93 项集群检查、百万同谓词开关对照、百万原生检索及五百万规模实测。详细口径与证据在下列文档和报告中保留。

最终已恢复 `tantivy_demo_v3` 并复跑 39/39 项，`ft_regression_final` 的 18/18 项语法回归也通过。当前保留这两个正确性空间及 `ft_bench_5m`；启动与索引就绪说明见演示指南。

## 竞赛评判点与证据

| 评判点 | 已有证据 | 仍需补齐 |
| --- | --- | --- |
| 技术创新 | 普通 storage 内置倒排、复用图分区路由、原文三元组召回与精确复验、普通谓词下推 | 完善兼容与长期演进；不将 Tantivy 本身称为原创算法 |
| 性能提升 | 百万同 MATCH 开关对照：并发 1 为 11.5048 / 0.101917 QPS，完整结果集合一致 | 两组采样与预热不同；重复轮次与等价语义的 ES 对照待补 |
| 资源降配 | 新路径为 5 个服务，基线含 Listener/ES 共 7 个；已采集全服务 CPU、PSS 与磁盘 | 全文索引结构、复制策略和查询集合不同，不能换算等条件降配比例；均为一台机器 |
| 可行性 | 93 项不重复的集群检查及 9 项 Rust 测试通过 | 更大数据量、并发、故障与业务 workload 验收 |
| 可落地 | 实际 DDL、自动回填、显式重建、更新删除、同名重建、清空后重插 | 增量恢复、故障注入、分区迁移、升级与运维能力 |

## 功能证据

| 测试组 | 结果 | 原始报告 |
| --- | ---: | --- |
| 普通谓词、词法查询及数据变更 | 39/39 | `task-logs/tantivy-demo/verified-final/report.json` |
| INT64 VID 与索引生命周期（新版复验） | 23/23 | `task-logs/tantivy-lifecycle/post-topk/report.json` |
| 原生 query-string 主要模式 | 18/18 | `task-logs/tantivy-benchmark/regression-tantivy-final/regression.json` |
| 有限候选分页与完整排序对照 | 10/10 | `task-logs/tantivy-topk/verified-v2/report.json` |
| 立即重建与首次 storage 可见性 | 3/3 | `task-logs/tantivy-rebuild-immediate/verified-v2/report.json` |
| Rust 磁盘索引、语法与评分 | 9/9 | `task-logs/tantivy-bridge/query-compat-test.log` |
| C++ / Rust 调用边界 | 通过 | `task-logs/tantivy-bridge/cpp-test.log` |

五组集群检查共 39 + 23 + 18 + 10 + 3 = 93 项，属于小规模正确性检查，不合并成性能样本。生命周期 23 项为新版复验，未把旧版相同案例再次累加；主演示 39 项与语法 18 项也已最终复验，当前表格均指向实际通过报告。原生语法组包含 17 个正确结果集合和 1 个预期语法错误；错误被正确拒绝才算通过。全部检查通过不等于兼容全部 ES/Lucene 语义或完成商业可靠性验收。

百万输入使用真实唯一原始 PID 与 128 字符投影，99.308% 的文本被裁剪。开关对照为 Debug 构建、单轮测量，开启 800 次、关闭 80 次，预热次数不同且未清缓存；详细条件和绝对指标见性能报告。公开源有 138,364,198 条，不等于本机导入量；一亿阶段未通过本机容量门槛，五百万阶段已完成实测，SHOW STATS 确认恰好 5,000,000 个顶点、0 条边。

## 已完成实测的不同口径

- 普通 MATCH：百万数据、同谓词开关单并发 QPS 比值 112.883；开启下推动态四并发为 36.6311 QPS。
- 五百万普通 MATCH：四个完整结果集合与 oracle 一致，单并发 3.9051 QPS、动态四并发 11.6882 QPS，各 400 次且零失败。五百万只报告绝对值，没有五百万关闭下推或 ES 对照，也不计算百万到五百万的扩展比。
- 新版原生 TEXT_SEARCH：百万数据，每组 12,000 请求、0 失败；单并发 36.2224 QPS，P95/P99 55.8222/63.3650 ms，动态四并发 52.7110 QPS。五服务同轮 PSS 采样峰值分别为 0.6487/0.6642 GiB。
- ES 原生查询已有同请求规模的绝对实测：单并发 129.1826 QPS，动态四并发 662.1524 QPS。但 `water`、短语 `New York`、`water AND river` 的完整差集分别为 54/77/2 条（均仅 Tantivy 命中），不能与上述数值计算速度比。ES 词法索引为 1 primary/0 replicas，新方案在三 storage 保存词法及 trigram/原文结构，资源也不构成等条件降配对照。

完整采样条件、失败阶段排除和原始证据见 [benchmark-report.md](benchmark-report.md)。PSS 为服务采样观测值，不是全机内存峰值或可配置内存上限。五百万输入裁剪率 99.41032%；全机快照观测到 swapout 增量约 143.965 MiB、OOM kill 计数增量为 0，无法把 swapout 归因到某一进程。

## 已知原型边界

- RocksDB 与 Tantivy 是两个提交点；当前恢复方式为从图主数据全量重建，未实现精确 WAL 增量恢复。
- `standard`、`keyword`、`jieba` 有明确实现，但不能视为 ES standard 或 IK 的等价替代；正则仅支持 Tantivy 子集。
- 部分无额外过滤的 FIXED_STRING 顶点 `TEXT_SEARCH` 已接入每分区 `offset + count` 有界候选；不适用路径及安全回退仍收集全部命中。`LIMIT 10` 不代表整个查询只保留十条候选，详见兼容矩阵。
- Graph 合并各分区返回的本地分数排序；分区过滤贡献零分，但未做全局 IDF 校准。即使副本逻辑数据相同，分段和删除统计也可能造成不同 BM25 分值。
- 单机三进程满足本轮部署约束，不能证明三个物理故障域的高可用。
