# 比赛现场演示说明

目标是在约三分钟内展示：已有字符串谓词实际选择内置索引，查询结果符合原文语义，数据变更与索引生命周期有真实验证。完整测试使用独立人工数据空间，性能数据使用另外的公开语料与基准流程。

## 已核实的演示基础

> 最终运行状态：`tantivy_demo_v3` 已恢复并通过 39/39 项最终复验，报告为 `task-logs/tantivy-demo/verified-final/report.json`，完成时间 2026-11-05 01:25:21（北京时间）。`ft_regression_final` 的 18/18 项语法复验也通过，`ft_bench_5m` 保留恰好 5,000,000 个顶点。下列查询使用当前保留的主演示空间。

已完成主演示使用空间 `tantivy_demo_v3`，20 分区、3 副本、初始 240 条中英文混合记录，每分区 12 条；另有边数据。脚本最后执行过一次顶点更新、顶点删除和边删除，因此现场查询不要机械套用变更前的行数。

集群使用 graph `9669`、meta `9559`、storage `9779/9789/9799`，运行目录为 `/home/sch/nebula-run/3.6-tantivy/tantivy-demo/`。演示时先确认该隔离集群正在运行。若需要启动，按仓库 `AGENTS.md` 先设置 `ulimit -n 65536`，使用已有隔离集群管理流程；不同时启动另一套同端口服务。

```bash
cd /home/sch/nebula/nebula-3.6-tantivy
python3 scripts/tantivy-demo/cluster.py status
/home/sch/nebula-run/nebula-console -addr 127.0.0.1 -port 9669 -u root -p nebula
```

上述状态检查只读取进程信息，进程存活不等于全文索引 ready。现场建议保留当前已就绪进程。若重启，storage 会从图主数据全量回填保留的五百万全文索引，应预留数分钟，并以各 storage 的 `Tantivy index ready` 日志及完整查询校验为准。后续只读展示不改动现有演示数据。

## 三分钟现场顺序

### 1. 拓扑与索引（约 30 秒）

```ngql
USE tantivy_demo_v3;
SHOW HOSTS;
SHOW PARTS;
SHOW FULLTEXT INDEXES;
```

指明三 storage、20 分区与每分区三个 peer。展示索引依赖的 Tag/Edge 和字段，说明 Tantivy 运行在普通 storage 进程内。服务拓扑证据与进程资源测量是两回事。

### 2. 普通业务谓词下推（约 60 秒）

```ngql
PROFILE MATCH (v:article)
WHERE v.article.body CONTAINS "数据库"
RETURN id(v) AS id LIMIT 5;

PROFILE LOOKUP ON article
WHERE article.body CONTAINS "Graph"
YIELD id(vertex) AS id;

LOOKUP ON article
WHERE article.body CONTAINS "graph"
YIELD id(vertex) AS id;
```

说明业务仍在写普通字符串谓词；PROFILE 中 `engine=tantivy` 或 `indexCtx` 的引擎信息证明使用内置路径。`Graph` 与 `graph` 是大小写敏感的原文匹配，不能用分词后的结果替代。这里展示的是查询结果与计划，不把屏幕单次耗时解释为性能收益。

### 3. 词法全文检索及评分（约 35 秒）

```ngql
LOOKUP ON article
WHERE TEXT_SEARCH(tantivy_demo_v3_article, "body:graph")
YIELD id(vertex) AS id, score() AS relevance
| ORDER BY $-.relevance DESC, $-.id ASC
| LIMIT 0, 5;
```

`TEXT_SEARCH` 用于词项、短语及布尔等词法检索；普通字符串查询无需调用它。该例展示全文分数与全局分页，不能仅凭 LIMIT 或上述排序语句宣称触发有限候选优化，须另查执行计划。Graph 合并的是各分区实际返回的本地分数，没有全局 IDF 校准；即使副本逻辑数据相同，分段和删除统计也可能导致分值不同，分数也不承诺与 ES 相同。前缀、通配、正则、模糊、邻近短语、加权和范围的完整结果集合见独立 18 例语法报告。

### 4. 变更与生命周期证据（约 55 秒）

打开主演示报告中 `vertex_update_removes_old_terms`、`vertex_update_adds_new_terms`、`vertex_delete`、`edge_delete` 四项；再展示新版生命周期报告的 `rebuild_job_finished` 和 `after_clear_insert_*`。说明这不是静态预置索引：图更新会去掉旧词并增加新词，删除后不再命中，重建与清空重插也已验证。

若现场要重新执行写入、更新、删除，使用下面的自动演示创建新的专属空间。不要直接修改已留存的报告对应空间后继续沿用旧结果数量。

## 完整自动演示

这些命令会创建自己的新空间，并执行其中的数据写入、更新或清空；不会启动、停止或编译服务。示例名字应改为本次未使用的名称，日志目录也使用新名称。密码可由 `NEBULA_PASSWORD` 提供，未设置时沿用本机测试约定。

```bash
python3 scripts/tantivy-demo/demo.py \
  --space tantivy_stage_1105a \
  --output task-logs/tantivy-demo/stage-1105a

python3 scripts/tantivy-demo/lifecycle.py \
  --space tantivy_lifecycle_stage_1105a \
  --output task-logs/tantivy-lifecycle/stage-1105a
```

第一组复验 39 项，第二组复验 23 项。脚本会等待元数据传播和可见性，保留 JSON 报告与查询计划；退出成功且所有检查 `passed=true` 才可报告通过。不要在正式性能计时窗口同时运行这些写入演示。

最新生命周期已在 `post-topk/report.json` 复验 23/23，TopK 10/10 与立即重建 3/3 另有新报告；合计不重复检查为 93 项，不再累加旧生命周期同一组案例。主演示 39 项与语法 18 项现已最终复验，证据入口均更新为最终路径。最新 `CLEAR SPACE` 成功返回需等待全文重建 ready；若重建失败，已经完成的主数据清空不会回滚。

原生语法 18 例使用 `scripts/tantivy-benchmark/regression.py`，分阶段创建 8 条人工数据、等待索引可见后运行。具体命令与边界见 [fulltext-compatibility.md](fulltext-compatibility.md)，它的单次查询耗时不计入正式性能报告。

## 演示中断时

先保存失败查询、错误、时间与日志。若只剩很短时间，展示注明时间和数据范围的既有通过报告及查询计划，明确称为已完成运行的记录。不要把旧输出伪装成当前查询结果，也不要用返回空集合掩盖索引未就绪。

不在台上临时执行全量数据导入、大范围正则、无选择性空串查询、重新编译或切换 ES 基线。必要的现场查询限制在小型人工演示空间；最终性能结论引用独立基准报告。百万 MATCH 下推开关的 112.883 倍属于同谓词对照；新版 TEXT_SEARCH 与 ES 已有各自绝对实测，但三项完整差集为 54/77/2，不能在现场说成相对 ES 的速度提升。不要把普通 MATCH 动态四并发 36.6311 QPS 与新版 TEXT_SEARCH 动态四并发 52.7110 QPS 混写为同一试验。

## 证据入口

| 证据 | 路径 |
| --- | --- |
| 主演示 39 项及拓扑 | `task-logs/tantivy-demo/verified-final/report.json` |
| 主演示实际计划 | `task-logs/tantivy-demo/verified-final/*.plan.txt` |
| 现场固定查询只读复验 | `task-logs/tantivy-demo/final-guide-smoke.jsonl` |
| 新版生命周期 23 项 | `task-logs/tantivy-lifecycle/post-topk/report.json` |
| TopK 10 项 | `task-logs/tantivy-topk/verified-v2/report.json` |
| 立即重建 3 项 | `task-logs/tantivy-rebuild-immediate/verified-v2/report.json` |
| 原生语法 18 项 | `task-logs/tantivy-benchmark/regression-tantivy-final/regression.json` |
| Rust 9 项 | `task-logs/tantivy-bridge/query-compat-test.log` |

功能覆盖与兼容差异见 [fulltext-compatibility.md](fulltext-compatibility.md)；答辩叙述见 [speaker-notes-and-qa.md](speaker-notes-and-qa.md)。
