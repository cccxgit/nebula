# 数据集与可复现基准

本文件区分**公开语料规模、已准备样本、数据库实际导入量、实际测试结果与容量估算**。语料超过一亿条，不代表已经完成亿级数据库测试。数值结果应以 `task-logs/tantivy-benchmark/` 中带时间的报告为准。

## 数据源选择

选择 **MS MARCO passage v2** 作为英文检索研究语料：官方记录数 **138,364,198**，原始记录具有稳定 `pid` 和 `passage` 文本，归档包含 70 个 gzip JSONL 文件。[官方语料说明](https://microsoft.github.io/msmarco/TREC-Deep-Learning#passage-ranking-dataset)

下载地址：<https://msmarco.z22.web.core.windows.net/msmarcoranking/msmarco_v2_passage.tar>。2026-11-04 本机 HEAD 核查大小 **21,768,192,000 bytes / 20.27 GiB**，支持字节范围请求。只保留原始 tar，通过流式解码读取，不保留整份解压结果。

数据使用条款限于**非商业研究**；不能把 MS MARCO 仓库文档的 CC BY 或示例代码的 MIT 许可当作语料的商用许可。基准语料与商业产品数据分开。[官方数据条款](https://microsoft.github.io/msmarco/TREC-Deep-Learning#terms-and-conditions)

备选来源：

| 来源 | 官方规模 | 本机适用性 |
| --- | ---: | --- |
| [Amazon Reviews 2023](https://amazon-reviews-2023.github.io/) | 约 5.7154 亿条评论 | Clothing + Home 两类别约 1.334 亿条；原始文本授权范围未确认，不能把其处理代码 MIT 许可等同于评论内容许可 |
| [AllenAI C4 英文](https://huggingface.co/datasets/allenai/c4) | 训练集 364,868,892 条 | 数据卡标注 ODC-BY，并受 Common Crawl 内容条款约束；约 326.8 GB 压缩下载大于当前可用磁盘，不适合次日演示 |

机器可读来源清单：`scripts/tantivy-benchmark/dataset-manifest.json`。中文、英文混合的正确性演示需另列人工测试数据，不宣称 MS MARCO 是中文业务基准。

## 已完成的数据准备与容量估算

2026-11-04 下载官方归档前 4 MiB，流式读取前 **10,000 条真实记录**；这是**前缀样本，不是随机抽样**。准备结果如下，所有记录保留原始 `pid`，不重复扩充：

| 文本投影 | 实际行数 | 平均 UTF-8 bytes / 条 | 裁剪行数 | gzip 文件 bytes |
| --- | ---: | ---: | ---: | ---: |
| 完整原文 | 10,000 | 290.2426 | 0 | 1,074,268 |
| 前 128 个 Unicode 字符 | 10,000 | 128.5657 | 9,944 | 555,049 |
| 前 256 个 Unicode 字符 | 10,000 | 249.4825 | 7,615 | 954,696 |

样本在 `/home/sch/nebula-data/tantivy/sample-10000-{full,128,256}.jsonl.gz`，对应准备清单在 `task-logs/tantivy-benchmark/sample-10000-*.manifest.json`。Unicode 字符数与 UTF-8 字节数不同。裁剪数据应称为“真实记录的短文本属性投影”，不能称为完整原文测试。

随后已准备 **1,000,000 条真实记录的 128 字符投影**：`/home/sch/nebula-data/tantivy/msmarco-1m-128.jsonl.gz`，gzip 54,984,169 bytes，平均文本 128.396899 UTF-8 bytes，993,080 条被裁剪。独立检查确认 **1,000,000 个唯一原始 PID**；证据见 `msmarco-1m-128.manifest.json` 与 `msmarco-1m-unique-validation.json`。百万数据库导入及查询随后亦已完成，实测结果单独见 [benchmark-report.md](benchmark-report.md)，不以准备行数替代导入证据。

2026-11-04 16:28 UTC 已完成 **5,000,000 条真实记录的相同投影准备**。文件 `/home/sch/nebula-data/tantivy/msmarco-5m-128.jsonl.gz` 为 273,338,589 bytes；4,970,516 条被裁剪，平均文本 128.383227 UTF-8 bytes，0 空文本。离线完整扫描确认 5,000,000 个唯一原始 PID；JSONL SHA256 为 `95864ebf78c175cf6090518d9e2222ad318a7b547b63d6d0681299cf2807775a`，与准备清单一致。四条既定透明谓词完整命中数为 **82,845 / 16,252 / 1,062 / 138**，均小于 100,000，未截断或放大候选阈值。证据：[清单](../../task-logs/tantivy-benchmark/msmarco-5m-128.manifest.json)、[完整 oracle](../../task-logs/tantivy-benchmark/oracle-5m-128.json)、[准备日志](../../task-logs/tantivy-benchmark/prepare-5m)。

随后本机已完成这 **500 万条的实际导入、三个 storage 的全文索引构建、四项完整结果集合校验及并发 1/4 各 400 次查询**。独立的 [SHOW STATS 记录](../../task-logs/tantivy-benchmark/final-5m-stats.jsonl) 于 17:25 UTC 确认图中恰有 5,000,000 个顶点；[storage 完成日志](../../task-logs/tantivy-benchmark/storage-ready-5m.json) 确认每个本地全文副本各有 5,000,000 条。查询集合校验与资源、吞吐的详细口径见 [性能报告](benchmark-report.md)。来源规模仍为 138,364,198 条，实际数据库测试上限为 5,000,000 条；一亿阶段未通过本机容量门槛，未执行。

以该非随机样本外推，一亿条完整原文 × 3 副本，仅文本约 **81.1 GiB**；128 字符投影约 **35.9 GiB**；256 字符约 **69.7 GiB**。这不是落盘大小预测，未包含 VID、RocksDB、倒排表、n-gram、WAL、段合并与压缩临时空间。实际字节数必须实测，且索引大小可能非线性增长。

建议预算：原始压缩语料约 20.3 GiB；保留至少 40 GiB 构建和临时空间；先用 1 万 → 10 万 → 100 万条逐级验证，测量三 storage 总目录增长，再决定是否能推进一亿条。禁止以缩小副本数、重复记录或省略真实索引来伪造用户要求的规模。

## 工具与运行方式

脚本位于 `scripts/tantivy-benchmark/`。标准化、下载、资源监测只用 Python 标准库；连接数据库需要 `nebula3-python==3.8.3`。本机已安装此依赖。准备了使用现有包的用户态环境 `.venv`；新机器可自行创建常规虚拟环境并安装 `requirements.txt`。

所有命令在仓库根目录执行。原始数据在专属 `/home/sch/nebula-data/tantivy/`，运行日志在 `task-logs/tantivy-benchmark/`。

### 可恢复下载

```bash
python3 scripts/tantivy-benchmark/benchmark.py fetch \
  --url https://msmarco.z22.web.core.windows.net/msmarcoranking/msmarco_v2_passage.tar \
  --output /home/sch/nebula-data/tantivy/msmarco_v2_passage.tar \
  --connections 8 --chunk-mib 8 \
  > task-logs/tantivy-benchmark/msmarco-download.stdout.log \
  2> task-logs/tantivy-benchmark/msmarco-download.log
```

重复执行相同命令即可续传。程序验证 Content-Range、ETag 与总长度，全部完成后计算 SHA256。并行下载在单个稀疏 `.partial` 文件中按范围写入，**文件显示 21G 不等于已下载 21G**；看 `.download.json` 的 `downloaded_bytes`、`completed_chunks` 和 `state`。只有 `state=complete` 且正式 `.tar` 文件生成才是完成。完成前不要把整个稀疏文件当作有效归档读取。

本次用已核验的连续前缀完成了百万和五百万样本。性能测量期间下载进程暂停，全部测量结束后已停止该进程；部分归档与续传元数据保留，没有长期挂起的下载任务。[最终停止记录](../../task-logs/tantivy-benchmark/download-final-stop.json)

需要提前准备小样本时，可给 `normalize` 传入 `.tar.partial`，同时指定 `--allow-partial-download --limit 1000000`。程序读取下载元数据，仅暴露**已完整下载且从第 0 块连续的前缀**，绝不读取尚未完成的空洞。前缀不足会失败且不生成正式输出；数据足够后重跑即可。百万投影已通过这一路径完成，清单保留当时的核验前缀字节数。

### 流式标准化

```bash
python3 scripts/tantivy-benchmark/benchmark.py normalize \
  --input /home/sch/nebula-data/tantivy/msmarco_v2_passage.tar \
  --format msmarco --limit 1000000 --max-chars 128 \
  --output /home/sch/nebula-data/tantivy/msmarco-1m-128.jsonl.gz \
  --report task-logs/tantivy-benchmark/msmarco-1m-128.manifest.json \
  2> task-logs/tantivy-benchmark/normalize-1m.log
```

输出格式为每行 `{"id":"原始pid","text":"原始或明确裁剪文本"}`。省略 `--max-chars` 保留完整文本；`--max-text-bytes` 可另外限制 UTF-8 字节数。省略 `--limit` 读取全部记录。`--skip` 从固定源顺序继续取数；清单记录跳过数量、裁剪行数、文本字节数、规范 JSONL SHA256 和耗时。ID 不得超过 64 UTF-8 bytes。

### 图写入

前提是 graphd 9669 / metad 9559 / 三个 storaged 9779、9789、9799 正常运行；启动前执行 `ulimit -n 65536`。目标图空间 `ft_bench` 必须为 **20 分区、3 副本、FIXED_STRING(64)**，Tag 为 `passage(text string)`。全文索引创建和可见性以原型实际接口与演示脚本为准。

原型约定的索引语句为 `CREATE FULLTEXT TAG INDEX ft_passage_text ON passage(text) ANALYZER="standard";`，允许导入后建索引并自动回填。`REBUILD FULLTEXT INDEX` 可用于等待重建完成；最终计时前仍需验证查询成功与结果一致。构建遵循本次用户最新负载要求，使用 **`make -j5`**；构建阶段不同时运行压测，不启动额外基线服务。

```bash
python3 scripts/tantivy-benchmark/benchmark.py import \
  --input /home/sch/nebula-data/tantivy/msmarco-1m-128.jsonl.gz \
  --space ft_bench --tag passage --field text \
  --batch-size 200 --concurrency 4 \
  --output task-logs/tantivy-benchmark/import-1m-128 \
  2> task-logs/tantivy-benchmark/import-1m-128.log
```

密码从 `NEBULA_PASSWORD` 读取，未设置则使用本地约定 `nebula`，不把密码写进命令行。固定 VID 的 INSERT 可重复执行；失败时会记录已观察到的确认数，但并发中的其他批次可能已经落库，不能简单把该数当成精确断点。安全做法是同一源文件按原 VID 重放，单独记录重放行为。

`import-summary.json` 的吞吐是**客户端获得图写入成功确认的速率**，包含输入解压、语句生成和客户端开销；不是独立 storage 极限吞吐，也不是“已可检索”速率。建索引完成时间必须额外等待全文可见性并记录，不能拿图写入确认时间代替。

### 查询测量

```bash
python3 scripts/tantivy-benchmark/benchmark.py query \
  --space ft_bench --queries scripts/tantivy-benchmark/queries-example.jsonl \
  --warmup 3 --repeats 1000 --concurrency 4 --label tantivy \
  --output task-logs/tantivy-benchmark/query-1m-c4 \
  2> task-logs/tantivy-benchmark/query-1m-c4.log
```

查询文件每行包含 `name` 和完整 `query`。对于非常常见的短词和前后缀，应同时记录候选数和返回限制，避免仅用罕见词报喜。查询集在对比前固定；每个后端执行相同文本语义、相同结果限制、相同客户端并发。

实验分两套，避免混淆原文子串和分词语义：

1. **透明下推**：同一个新集群分别关闭、开启 `enable_tantivy_pushdown`，使用完全相同的 `queries-transparent-match.jsonl` 中 `MATCH ... CONTAINS` 等语句，比较扫描与内置索引；关闭时需确保普通 Tag 扫描所需索引存在。先验证两个路径的完整命中集合，再测 LIMIT 和并发。
2. **原生全文检索**：新组用 `queries-text-search.jsonl` 的 `TEXT_SEARCH(ft_passage_text, ...)`，旧组用 `queries-es-example.jsonl` 的 `ES_QUERY(nebula_index_ft_passage, ...)`。两端都使用 `standard` analyzer，输入同一投影，覆盖词项、短语与 AND；返回同样的 VID、score、text 字段和 LIMIT。评分值和排序可不同，完整候选集合与命中数单独核验，不能要求不同分片统计下分数数值相等。

**计时前必须停止编译**，两组测试顺序运行；记录并尽量暂停下载等额外活动。现有 `queries-example.jsonl` 保留为 LOOKUP 属性运算语法示例，正式透明下推对照使用上述 MATCH 文件。

输出逐请求 `query-latencies.csv`、`query-latencies.jsonl` 及 `query-summary.json`。计时范围是客户端请求往返；连接与预热不计入。P50/P95/P99 使用成功请求的 nearest-rank 分位数，失败单独计数；QPS 同时报告尝试与成功值。先验证结果一致性，再做性能对比；零结果、报错或截断导致的“快”不能当作优化收益。

### 资源采样

```bash
python3 scripts/tantivy-benchmark/monitor.py \
  --pid GRAPH_PID --pid META_PID --pid STORAGE1_PID --pid STORAGE2_PID --pid STORAGE3_PID \
  --interval 1 --duration 600 \
  --output task-logs/tantivy-benchmark/resources.csv
```

把大写占位符替换为实际 PID。每个采样记录 CPU、RSS、可读取的 PSS、线程数及累计块 I/O；CPU 100% 表示一个逻辑核，24 核机器可超过 100%。RSS 相加会重复计共享页，报告时优先使用 PSS，辅以整机 MemAvailable。数据目录占用另外用相同口径的 `du` 在导入前后测量；块 I/O 计数不是数据占用。

## 对照实验与报告口径

对照组完整包含 Nebula graph/meta/三 storage + Listener + ES；实验组包含 graph/meta/三 storage 和其内置索引。主机数量均为一台，比较的是必需服务数量和总资源，不声称机器数量实测降低。同机必须串行进行两个后端测试，避免相互争用；构建模式、数据、分区、副本、客户端并发保持一致，记录下载或编译是否仍占用 CPU/磁盘/网络。

建议记录表：

| 项目 | 必填证据 |
| --- | --- |
| 数据规模 | 来源、原始/投影、实际独立 VID 数、导入报告、裁剪比例 |
| 运行环境 | commit、构建类型、CPU、内存、磁盘、graph/meta/storage/ES/Listener 进程参数 |
| 图写入 | batch 大小、并发、成功行数、失败行数、总耗时、rows/s |
| 建索引 | 开始时间、图写入结束、索引可见完成、索引目录增量 |
| 查询 | 固定查询集、返回结果一致性、QPS、P50/P95/P99、失败率、冷热缓存状态 |
| 资源 | 全服务 CPU/PSS/RSS、三副本图存储与全文索引占用、峰值临时空间 |
| 基线状态 | ES 实际运行则填实测；未运行填“未测”，不填估计百分比 |

当前本文件包含数据来源核查、10,000 条原文/投影及 1,000,000 / 5,000,000 条投影准备实测。**没有在本文件中声称完成亿级导入、亿级查询、ES 对照或性能提升百分比。** 后续报告引用实际输出并注明瓶颈与未覆盖项。

## ES 7 对照环境准备

已下载 [Elastic 官方 7.17.29 Linux 归档](https://www.elastic.co/guide/en/elasticsearch/reference/7.17/targz.html)，归档 325,443,139 bytes，经官方 SHA512 验证；归档与解压总约 **830 MiB**。运行时自带 JDK，不改系统 Java。初始堆设置为 512 MiB，实际 RSS、PSS、索引磁盘须运行后采样，不能把堆大小当作整体内存占用。以下表格记录准备时的配置，后续启动和测量状态以运行日志、实测报告为准。

对照 Nebula 使用 `/home/sch/nebula/nebula-3.6-io/build/bin` 下现有 Debug 二进制。三个可执行文件均自报 Git `de9b3ed80`、构建时间 `Jul 10 2026 13:39:33`；其源码目录当前 HEAD 为 `b3457870fb7c11b29a35f364f2c23544ec41be01`，两者分开记录在 `settings.json`。比较前还需核对编译器、构建参数和运行配置，不能只依据源码 HEAD 认定二进制完全一致。

隔离目录 `/home/sch/nebula-run/3.6-tantivy-es-baseline`，端口如下，避免占用原型 RPC 与 HTTP 端口：

| 服务 | RPC / HTTP | 配置用途 |
| --- | --- | --- |
| graphd | 29669 / 39669 | 隔离对照环境 |
| metad | 29559 / 39559 | 隔离对照环境 |
| storaged × 3 | 29779、29789、29799 / 39779、39789、39799 | 隔离对照环境 |
| Listener | 29879 / 39879 | 隔离对照环境 |
| Elasticsearch | HTTP 19200，transport 19300 | 隔离对照环境 |

`es_baseline.py prepare` 生成配置，`start` 检查专属端口并设置文件描述符限制后启动，`initialize` 建立 20 分区、3 副本 `ft_bench`、`passage`、Listener 和 ES 全文索引，`status` 查看 PID，`stop` 只终止命令行属于专属目录的进程。构建完成并安排单独实验窗口后再启动，避免与原型压测同时运行。

配置与原型对齐：graph 2 个网络线程 / 4 个工作线程；每个 storage 和 Listener 2 个 I/O 线程 / 4 个工作线程；心跳 1s、Raft 心跳 5s，storage 的 block cache 64 MiB、LZ4、WAL TTL 3600s。Listener 保持默认分区管理方式。端口检查覆盖 RPC、RPC−1 管理端口、RPC−2 内部端口与 RPC+1 Raft 端口。

```bash
python3 scripts/tantivy-benchmark/es_baseline.py start
python3 scripts/tantivy-benchmark/es_baseline.py initialize --analyzer standard
python3 scripts/tantivy-benchmark/benchmark.py import \
  --port 29669 --input /home/sch/nebula-data/tantivy/sample-10000-128.jsonl.gz \
  --output task-logs/tantivy-benchmark/es-import-10k
```

基线通过 [Nebula 原生 Listener](https://docs.nebula-graph.com.cn/3.8.0/4.deployment-and-installation/6.deploy-text-based-index/3.deploy-listener/) 写入 ES，查询从 graphd 的 `ES_QUERY()` 经过原生路径执行；不能拿绕过 Nebula 的 ES HTTP 请求冒充完整链路延迟。

ES 对照使用 `standard` analyzer 和词项/短语/布尔查询，候选语句见 `queries-es-example.jsonl`；原始子串的扫描/下推性能使用同一个新集群单独比较。ES 默认配置保持一个 primary shard，显式零 ES 副本以避免单节点无法分配副本；图数据仍是 20 分区、3 副本。内置索引可能随三个 storage 各保留一份，**两侧全文索引副本策略必须在资源报告中明确说明**。

本机 `vm.max_map_count` 为 65530；准备使用仅 loopback 的 ES 单节点开发模式，不修改系统内核设置。如果运行时拒绝启动，记录实际错误并处理为基线未测，不能跳过失败编造结果。

## 可执行首轮流程与离线校验（2026-11-04 更新）

完整命令见 [benchmark-runbook.md](benchmark-runbook.md)。新增 `readiness.py` 用于创建新空间、等待拓扑与 schema 传播、等待重建 job/task 完成、比较完整候选 VID 并保存 PROFILE。`oracle` 子命令仅扫描本地规范化数据，不连接数据库。

已完成离线校验：10k / 1m 均为唯一原始 pid；四个低负载透明查询（water / New York / STARTS WITH The United / ENDS WITH water.）的命中数分别为 264/36/15/0 与 18,267/3,166/147/28。证据为 `task-logs/tantivy-benchmark/oracle-10000-128-safe.json` 和 `oracle-1m-128-safe.json`。这些是文本 oracle 的预期命中数，**不是数据库实测返回行数**。

原始 `STARTS WITH "The"` 在百万前缀语料中超过 100k 完整命中，被离线安全门槛阻止；当前正式样例已收窄。当前 `water` 的一亿外推仍超过百万候选，不能直接用于一亿压力测试，须选择更窄的查询并重新验证。
