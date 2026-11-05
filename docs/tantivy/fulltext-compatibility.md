# 原生全文能力与原型验收矩阵

本表依据 [NebulaGraph 3.8 全文搜索文档](https://docs.nebula-graph.com.cn/3.8.0/3.ngql-guide/15.full-text-index-statements/1.search-with-text-based-index/) 和 [Elasticsearch 7.17 query-string 语法](https://www.elastic.co/guide/en/elasticsearch/reference/7.17/query-dsl-query-string-query.html)列出验收范围。原型基于仓库 3.6 代码；旧版本 `PREFIX/WILDCARD/REGEXP/FUZZY` 函数文档不能替代当前 `ES_QUERY` 语义。

**代码存在不等于端到端通过。** 当前实现使用 Tantivy 0.25 查询语法树，并通过 `query_compat.rs` 将前缀、通配、正则和模糊词项替换为对应查询节点，保留布尔组、字段、短语、加权和范围。桥接的 9 项真实磁盘测试已通过；其中语法矩阵测试验证 20 个正确结果集合及 8 个拒绝案例。真实集群已通过主演示 39 项、生命周期 23 项、原生查询语法 18 项，此前共 80/80 项检查；最新生命周期 23 项及原生语法 18 项已复验，另新增 TopK 10 项与立即重建 3 项通过，合计 93 项不重复检查。主演示使用单机三 storage、20 分区、3 副本、240 条人工混合文本记录；独立语法回归使用 8 条人工数据，包含 17 个正常查询和 1 个预期语法错误。

| 能力 | 原生语法形式 | 当前验证状态 | 需要披露的边界 |
| --- | --- | --- | --- |
| Tag / Edge、多字段全文索引 | CREATE FULLTEXT TAG/EDGE INDEX ... ON schema(a,b) | 主演示通过点、边、多字段索引与查询 | 仍需覆盖 schema 演进和复杂类型边界 |
| 词项 / 默认 OR | `title:quick`，`quick fox` | 词项桥接及主演示通过；默认 OR 由解析器保留 | 默认 OR 的所有组合未逐项实测；分词不能假设与 ES 相同 |
| 短语 | `body:"new york"` | 桥接及主演示通过，含中文短语 | 存储 positions；中文 analyzer 差异仍存在 |
| AND / OR / NOT、括号、字段 | `title:(quick OR quack) AND body:new` | 桥接及独立端到端分组案例通过；主演示通过显式 AND/OR/NOT 与多字段 | 混合表达式应显式括号，不能假设与 Lucene 的优先级完全相同 |
| 前缀 | `title:bro*` | 已实现词项正则节点；桥接及独立端到端回归通过 | standard/jieba 规范化字母大小写；keyword 保持大小写 |
| 通配符 | `title:*row*`、`title:qu?ck` | 已实现 `*`/`?` 词项匹配；桥接及独立端到端回归通过 | 匹配完整索引词项，区别于原文 `CONTAINS`；支持转义字面通配字符 |
| 正则 | `title:/qu(i\|a)ck/` | 已实现 RegexQuery；桥接及独立端到端回归通过 | 匹配完整词项并保留正则大小写；不支持 Lucene 交集、补集、数字区间等扩展 |
| 拼写模糊 | `title:quik~1` | 已实现 FuzzyTermQuery；桥接及独立端到端回归通过 | 支持 `~0`、`~1`、`~2`，裸 `~` 为 2；输入必须分成一个词项，不支持与通配符混用 |
| 邻近短语 | `body:"new york"~1` | 保留短语 slop；桥接及独立端到端结果集合通过 | 与单词编辑距离不同；跨引擎距离细节仍需对照 |
| 加权 | `title:fox^4 OR title:hare` | 桥接及独立端到端命中集合通过 | 评分方向需单独验收；不要求跨引擎分数数值相等 |
| 范围 | `title:[blue TO brown]` | 闭区间词项范围桥接及独立端到端回归通过 | 字符串按词典项范围匹配；未承诺全部开闭混合边界及 ES 类型转换 |
| score / OFFSET / LIMIT | score()，LIMIT offset,count | 主演示通过分数输出和全局分页；桥接通过分区过滤不贡献评分；最新有限候选接入通过独立 10 项集群检查 | 精确词项/短语为本地 BM25，特殊节点为自动机评分；无全局 IDF 校准，逻辑数据相同的副本也可能因分段和删除统计产生不同分值 |
| analyzer | standard、其他插件名 | standard/keyword/jieba 桥接通过；主演示验证 standard 中英文 | 并非任意 ES analyzer 或 IK 插件兼容；keyword/jieba 的数据库端到端尚待验收 |
| 创建、删除、重建 | SHOW/DROP/REBUILD FULLTEXT INDEX | 生命周期新版 23 项复验通过：先有数据后建索引、显式重建三 storage 任务、删除后同名重建、CLEAR SPACE 后重插 | 已覆盖 INT64 VID（负数及超过 2^53 的值）；快照、崩溃、磁盘故障恢复仍未验收 |
| 原文属性透明下推 | MATCH/LOOKUP ... CONTAINS/STARTS WITH/ENDS WITH | 主演示通过，相关查询计划确认 Tantivy 路径 | 大小写、中文、短串、空串、特殊字符及残余 AND 已覆盖；混合 OR/NOT 等不能据此宣称全覆盖 |

ES 的同一套人工语法回归也已 18/18 通过（`task-logs/tantivy-benchmark/es-grammar-regression-v1/regression.json`）；人工用例通过不能推导真实语料语义等价，也不将 ES 的检查加入原型 93 项计数。下文保留百万真实语料的完整差集及分词边界证据。

`standard` 名称相同不足以证明结果等价。当前 MixedTokenizer 将汉字逐字切分、字母数字连续组合并小写，ES standard 使用自己的 Unicode 分词规则。英文 apostrophe、缩写、连字符、组合字符、长 token、中文短语与混合文本都应加入质量回归。自定义 `jieba` 也不应表述为等价 IK。

### 百万真实语料中已确认的标准分析器差异

对同一份 MS MARCO 100 万条、每条前 128 个 Unicode 字符的输入，已分别收集三个原生查询的**完整 VID 集合**。ES 查询计划确认 `FulltextIndexScan.limit=100001`，每次查询前后的 ES `_count` 与返回唯一 VID 数一致；Tantivy 也未触及上限。此次差异不是把 ES 默认返回的 10 条结果误当全集。旧基线的 `enable_optimizer` 必须在 graphd 启动时启用；仅运行时修改开关不会注册优化规则。

| 查询 | Tantivy 完整命中 | ES 完整命中 | 仅 Tantivy 命中 | 仅 ES 命中 |
| --- | ---: | ---: | ---: | ---: |
| `water` | 16,505 | 16,451 | 54 | 0 |
| `"New York"` | 3,184 | 3,107 | 77 | 0 |
| `water AND river` | 159 | 157 | 2 | 0 |

离线按差集 VID 回查输入文件，133 条差异记录全部找到。其原文边界特征如下：`water` 的 54 条中，42 条包含 `water's` 或 `water’s`，7 条包含字母间的点号，2 条为冒号，1 条为下划线，2 条为词内软连字符 U+00AD；短语差异的 77 条中，76 条包含 `New York's` 或弯引号形式，另 1 条的 `York` 后为窄不换行空格 U+202F；布尔差异的 2 条分别包含 `River's water` 和 `river’s water`。

可复核的短例包括 `msmarco_passage_00_31196090` 中的 `water's edge`、`msmarco_passage_00_13324933` 中的 `New York's governor`、`msmarco_passage_00_71533253` 中的 `Niagara River's water`。原型把撇号作为分隔符，产生独立的 `water`、`york` 或 `river`；ES 的官方 standard 示例保留 `dog's` 为完整词项，因此这类所有格差异与两边分词规则一致。[ES 7.17 standard analyzer](https://www.elastic.co/guide/en/elasticsearch/reference/7.17/analysis-standard-analyzer.html)

上述数量是实际结果差集和原文特征统计。点号、冒号、下划线及 Unicode 格式字符的具体 token 序列尚未逐条调用 ES `_analyze` 复验，应作为后续定向兼容性回归，不能把特征统计表述为 133 条逐项完成分析器等价证明。短语例外 `msmarco_passage_00_408701006` 的 U+202F 亦保留为待验证边界。

证据：`task-logs/tantivy-benchmark/native-1m-{tantivy,es-v2}/collection.json`、`task-logs/tantivy-benchmark/native-1m-comparison/comparison.json` 和分类说明 `task-logs/tantivy-query/native-analyzer-diff-audit.json`。含有语料原文的本地逐条摘录不提交到 GitHub；公司电脑如需复核须自行取得语料使用权后重建。**三个原生查询当前不具备跨引擎结果等价性，不据此计算 ES 相对性能提升；也不能宣称 standard 分词语义完全兼容。** 普通字符串谓词的扫描开关对照是另一组实验，其原文匹配语义和完整 oracle 独立验证。

证据分别位于 `task-logs/tantivy-bridge/query-compat-test.log`、`task-logs/tantivy-demo/verified-v3/report.json`、`task-logs/tantivy-lifecycle/post-topk/report.json` 和 `task-logs/tantivy-benchmark/regression-tantivy-final/regression.json`。主演示 240 条记录为人工正确性数据，每个分区 12 条，不计入公开语料导入或性能结果。独立语法回归的单次耗时也不作为吞吐或 P95/P99 基准。分数输出通过不等于与 ES 排名一致，9 项桥接测试也不等于只验证了 9 个查询输入。

历史报告保留；新版生命周期 23 项和原生语法 18 项复验未重复累加。原生语法最终复验于 2026-11-04 17:25:10 UTC 完成，使用新空间 `ft_regression_final`、索引 `ftcase_native_final`，检查 20 分区及 3 副本，并在完整 8 个 VID 可见且 PROFILE 确认 Tantivy 后运行全部 18 例；同目录 `report.json`、`provenance.json` 和 `run.jsonl` 保存拓扑、fixture/cases SHA256、可见性及执行日志。新增 `task-logs/tantivy-topk/verified-v2/report.json` 10 项通过 650 条记录的分页与完整排序对照；`task-logs/tantivy-rebuild-immediate/verified-v2/report.json` 3 项通过 200 条记录的立即重建，三个 storage 任务 FINISHED 后首次候选读取完整且 0 重试。Graph 的“全局排序”指合并各分区返回的实际分数后排序，并不表示已计算全局统一的 BM25 统计。

## 可执行回归

`scripts/tantivy-benchmark/fulltext-regression-fixture.jsonl` 是 **8 条人工正确性数据**，不计入公开百万/亿级数据；`fulltext-regression-cases.jsonl` 有 17 个明确命中集合的案例和 1 个必须拒绝的语法错误案例。

原型可用 `tantivy_regression_setup.py` 自动完成新空间、schema、8 条数据及可见性等待，再调用相同用例。复跑须使用新空间、索引名和日志目录：

```bash
python3 scripts/tantivy-benchmark/tantivy_regression_setup.py \
  --space ft_regression_repeat --index ftcase_native_repeat \
  --output task-logs/tantivy-benchmark/regression-tantivy-repeat
```

若需手动分阶段控制，仍可使用以下入口：

```bash
# 只打印建图、建 Tag、建全文索引与 INSERT，便于分阶段执行并等待元数据传播。
python3 scripts/tantivy-benchmark/regression.py emit \
  --space ft_regression --index ftcase_text

# 完成建图/数据导入与可见性校验后执行；当前命令不会自动启动服务。
python3 scripts/tantivy-benchmark/regression.py run \
  --port 9669 --function TEXT_SEARCH --index ftcase_text \
  --output task-logs/tantivy-benchmark/regression-tantivy.json

# 旧组使用独立 ft_regression 空间，先添加该空间的 Listener。
python3 scripts/tantivy-benchmark/regression.py run \
  --port 29669 --function ES_QUERY --index nebula_index_ftcase \
  --output task-logs/tantivy-benchmark/regression-es.json
```

不支持的功能应明确报错，并在交付报告中标为未完成；不能将成功返回的空集或错误集合解释成支持。回归程序把这些情况判失败。语法错误案例必须失败；正常案例必须与预期 VID 集合完全一致。排序和 score 数值另行核验。

## 大规模查询的当前限制

Graph 统一合并、排序、去重和分页。最新代码对一部分 `TEXT_SEARCH` 接入了每分区 `K = offset + count` 候选上限，要求 FIXED_STRING 顶点、单一索引上下文、有限正数 LIMIT，且无额外过滤、统计列或自定义排序。INT64 VID、边、普通 `CONTAINS`/`STARTS WITH`/`ENDS WITH`、无界查询及其他不适用情形仍收集完整命中。

有限候选路径回查主数据；发现已删除 key、非规范填充的 VID、重复解码 VID 或零分边界时，回退为该分区完整检索，再按分数降序、解码 VID 升序选择有效唯一记录。非法 key、非有限分数与主数据读错误会失败。新版 10 项集群检查已把实际分页结果与同一数据状态下完整分数及 VID 排序的 oracle 比较，不能假设所有相同文本在不同副本上具有相同 BM25 分数。

**LIMIT 10 仍不能保证整个查询只在内存中保留 10 条候选**：各 segment 有独立缓冲、各分区返回候选，安全回退和不适用路径仍可能读取全集；有界收集不意味着只访问 K 个文档。原文模式的 1～2 字符条件没有 trigram 过滤能力，会回到较宽候选扫描。通配 `*`、空串、极短子串等应先在小型 fixture 验证，不应直接在一亿条数据上执行。

本轮基准仍把完整候选超过 100,000 的大规模查询排除出正式并发负载，并如实报告该限制。有限候选的正确性复验不等于完成高负载资源验收；普通谓词路径也不能因 `TEXT_SEARCH` 的优化而取消门槛。候选上限是本机保护性实验门槛，不是引擎宣称支持规模。
