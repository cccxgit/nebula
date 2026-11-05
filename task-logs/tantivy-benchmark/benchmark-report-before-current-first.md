# Tantivy 内置全文索引：1 万与百万条阶段实测报告

**本轮在同一 Nebula 集群、同一 MATCH 查询集合上比较 Tantivy 下推开关。** 10,000 条真实记录、并发 1、两组各 160 个请求且均无失败：开启下推为 **116.20 QPS**，关闭下推为 **9.24 QPS**，本轮达到 **12.58 倍**；混合查询 P95 从 **123.67 ms** 降到 **15.71 ms**，P99 从 **130.86 ms** 降到 **21.96 ms**。这是一轮短时、小样本的功能与性能验证，**不是对 Elasticsearch 的性能或成本结论**。

截至 2026-11-04 17:08 UTC（北京时间 11 月 5 日 01:08），百万阶段已完成同 MATCH 的开关对照：并发 1 开启 11.5048 QPS、关闭 0.101917 QPS，本轮比值 112.88 倍；开启采样 800 次、关闭 80 次，预热次数亦不同，具体条件见追加章节。新版 Tantivy 的原生 TopK 查询已完成单并发 12,000 请求（36.2224 QPS），四并发仍在测量；这些是重编译、重启后的独立结果。ES 百万基线也已完成有效导入、建索引、每组 12,000 请求的延长查询及 18 项人工语料语法回归；但百万真实语料三项原生查询完整 VID 集合与 Tantivy 不同，**不能计算跨引擎性能提升倍数**。500 万数据已准备、数据库实测待完成；亿级未实测。原始查询、导入、资源 CSV 均已独立重算。

## 实验条件

| 项目 | 本轮设置与证据 |
| --- | --- |
| 机器 | 本机 Intel Core i7-13700K，24 逻辑 CPU，31.08 GiB 物理内存；Linux 6.8.0-138 |
| 部署 | 1 graphd + 1 metad + 3 storaged，20 分片、3 副本；完整查询校验时 20 分片均有 leader、3 个 peer、无 lost replica |
| 构建 | Debug；构建并行度 -j5；测量前编译已停止 |
| 二进制 | 两组使用同一批运行进程；运行中 `/proc/PID/exe` SHA256 已在测量后归档，见下方证据 |
| 空间 / Tag / 字段 | `ft_bench_10k` / `passage` / `text` |
| 普通 Tag 索引 | `passage_tag_lookup`，用于关闭下推后的 MATCH 索引扫描 |
| 全文索引 | `ft_passage_text_10k`，DDL analyzer 为 standard；同时保留词法字段与 trigram / 原文验证结构，三个 storage 各有本地索引副本；本轮测原始文本子串/前后缀谓词 |
| graph 配置 | netio 2 / worker 4，`accept_partial_success=false` |
| storage 配置 | io 2 / worker 4，RocksDB block cache 64 MiB/进程，LZ4，WAL TTL 3600 s，heartbeat 1 s、raft heartbeat 5 s |
| 输入 | MS MARCO passage v2 官方归档的连续前 10,000 条，保留原始 pid；前 128 个 Unicode 字符，无重复扩充 |
| 裁剪 | 9,944 条被裁剪（99.44%）；文本共 1,285,657 UTF-8 bytes，均值 128.5657 bytes；gzip 文件 555,049 bytes |
| 查询 | 四条 MATCH 各 40 次，按固定顺序轮转；每组先预热三轮（12 次）并排除于计时 |
| 并发与返回 | 并发 1；性能查询 LIMIT 10，包含三个有命中查询和一个零命中查询 |
| 对照顺序 | 开启下推→关闭下推→恢复下推；切换 flag，不重启集群，不清理 OS/RocksDB 缓存 |
| 同机负载 | 两次查询前置检查均无编译进程，语料下载进程处于暂停状态；预检是瞬时记录，不等同全机持续负载审计 |

输入规范化 JSONL SHA256 为 `3af6f2ea60a26d832f9759236a9d8a0da044dff790640cf60fe32e912f31b6e4`，与三次完整可见性校验一致。样本为连续前缀而非随机抽样；前 128 字符投影不代表全文原文的存储或查询成本。MS MARCO 语料条款为非商业研究使用，不能用代码仓库许可推导语料商业使用权。来源与许可详见 [benchmark-data.md](benchmark-data.md)。

环境配置为测量后的只读核查，未声称有不可变的测前配置快照：[10k-environment-audit.json](../../task-logs/tantivy-benchmark/10k-environment-audit.json)。同一批进程的实际二进制摘要与命令行见 [10k-running-binary-sha256.json](../../task-logs/tantivy-benchmark/10k-running-binary-sha256.json)。

## 查询正确性与性能

开启、关闭和恢复下推三次均执行不带 LIMIT 的完整 VID 集合比对；没有缺失、额外或重复 VID。三次 PROFILE 分别确认 Tantivy 被选择、未被选择、再次被选择。性能循环返回行数亦一致：10 / 10 / 10 / 0。未指定排序的 LIMIT 查询不要求两条执行路径返回同一个任意前十集合，语义一致性由完整 VID 集合校验提供。

| 文本谓词 | 完整命中数 | 开启 P50 / P95 / P99（ms） | 关闭 P50 / P95 / P99（ms） |
| --- | ---: | ---: | ---: |
| `CONTAINS "water"` | 264 | 11.76 / 21.88 / 22.49 | 106.90 / 123.27 / 130.86 |
| `CONTAINS "New York"` | 36 | 7.73 / 15.31 / 16.87 | 108.01 / 123.67 / 128.88 |
| `STARTS WITH "The United"` | 15 | 5.18 / 13.43 / 13.77 | 109.20 / 119.06 / 132.88 |
| `ENDS WITH "water."` | 0 | 4.72 / 10.92 / 14.37 | 106.09 / 124.80 / 126.09 |

| 混合查询指标 | 下推开启 | 下推关闭 |
| --- | ---: | ---: |
| 成功请求 / 总请求 | 160 / 160 | 160 / 160 |
| 失败数 | 0 | 0 |
| 有效计时长度 | 1.3770 s | 17.3237 s |
| 成功 QPS | 116.1955 | 9.2359 |
| P50 | 7.7954 ms | 107.0027 ms |
| P95 | 15.7129 ms | 123.6712 ms |
| P99 | 21.9650 ms | 130.8628 ms |
| 最大单次延迟 | 22.4906 ms | 132.8783 ms |

本轮 QPS 比值为 12.5808；P95、P99 分别降低 87.29%、83.22%。这些比例仅适用于上述相同 MATCH 的单轮对照。吞吐按成功请求数 / 计时墙钟长度计算；延迟是 Python 客户端至 graph 的往返时间，百分位采用成功请求的 nearest-rank。独立 CSV 重算与原 summary 完全一致。并发 1 下单次延迟之和分别为 1.37595 s 与 17.32258 s，与计时墙钟长度仅差约 1 ms。

样本量限制：每条查询只有 40 次，其 P99 正好等于该条查询最大值；混合 160 次的 P99 是第 159 个有序样本。没有多轮重复、置信区间、随机对照次序或冷缓存实验，不能把本轮结果用作稳定生产 SLA 或系统最大吞吐。两组仍保留全文索引及其内存，关闭 flag 的结果也不等同“完全不含全文索引的 Nebula 部署”。

## 写入与索引进度

| 项目 | 本轮观测 | 解释 |
| --- | ---: | --- |
| 图写入确认 | 10,000 条，50 批，各 200 条 | 并发 1；所有批次成功 |
| 图写入确认时间 | 0.40917 s | 含客户端构造、解压、会话准备和写入确认，不含随后创建全文索引 |
| 图写入确认吞吐 | 24,439.83 rows/s | 约 0.41 秒的小样本突发结果，不是持续带索引写入吞吐 |
| 普通 Tag 索引 REBUILD job 8 | 4.0197 s 观察到完成 | 含 2 秒轮询粒度 |
| 全文 CREATE 返回 | 2026-11-04 15:45:42 UTC | CREATE 会触发异步自动回填 |
| 全文 REBUILD job 9 | 2.0070 s 观察到 job/task FINISHED | 不能作为全文建索引耗时，见下文 |
| 四个谓词首次完整校验 | 15:47:22 UTC，全部通过 | 校验阶段自身耗时 0.0598 s；不是写入至可见的耗时 |
| 完整索引构建速度、精确可见延迟 | **未有效测定** | 待修复后的连续观测与重测 |

本轮实现存在 CREATE 元数据尚未传播到 storage 时，REBUILD 可能因本地 entries 为空而提前成功的问题，主任务已定位并在修复。因而 `job_finished` 和 `index_build_jobs_finished` 不能单独证明此时全部全文索引已经构建完成；本报告不以 10,000 / 2.007 计算建索引速度。约 100 秒后四个谓词完整结果一致，是这些谓词最终可见的独立证据；期间没有持续轮询，无法确定首次可见的精确时刻，也不能宣称“建索引用时 100 秒”。CREATE 自动回填与显式 REBUILD 的重叠工作量需要在后续重测中单独说明。

## 全服务资源采样核查

监控覆盖同一批五个服务 PID：metad 92995、graphd 93161、storage 93220 / 93418 / 93655；与集群启动日志一致。所有已采样行的进程均存活，PSS 字段均可读。下表是**采样轮次中的观测最大值**，不代表操作系统记录的真实峰值。

| 查询阶段观测 | 下推开启 | 下推关闭 |
| --- | ---: | ---: |
| 采样轮数 | 2 | 19 |
| 有效 CPU 差分区间 | 1 | 18 |
| 实际轮间隔 | 1.010 s | 1.008–1.015 s |
| 同轮五服务 RSS 合计最大值 | 430.81 MiB | 511.10 MiB |
| 同轮五服务 PSS 合计最大值 | 336.15 MiB | 416.43 MiB |
| 五服务 CPU 区间平均之和的最大值 | 1.357 核 | 2.727 核 |

字段定义：`max_sum_rss_bytes` 是先把一轮内五个 PID 的 RSS 相加，再在轮次中取最大值；不是把各进程不同时刻的峰值相加。RSS 会重复计算共享页。`max_sum_available_pss_bytes` 使用相同方式计算可读 PSS 之和；实现会忽略不可读项，但本轮没有缺项。每轮按 PID 依次读取，并非严格同时快照。

`cpu_percent_one_core` 来自相邻采样的进程 user+system CPU tick 差 / 实际间隔；100% 表示一个逻辑核，合计 135.698% 即 1.357 核，不是整机利用率 135.698%。第一轮没有差分基线，因此 CPU 留空。导入阶段只采到一轮，原 summary 的 CPU 最大值 0 是聚合初始值，**应解释为未测，不能解释为 CPU 为零**；独立审计文件已将此值正确记为 null。

资源 wrapper 的窗口包括客户端初始化和预热，未与有效查询计时区间精确对齐。开启下推只采到一个 CPU 区间，窗口明显短于关闭组，还可能漏掉结束前的峰值。两组顺序运行且未清理缓存，驻留内存保留状态也不同。因此本轮**不计算 CPU/PSS 节省百分比，不据此宣称机器数量或内存降配**。

这些进程指标未包含 Python 基准客户端、系统内核与所有文件缓存，也不是全机成本。`read_bytes/write_bytes` 为进程累计块 I/O 计数，不是图数据或索引磁盘占用。图数据增量、三副本全文索引磁盘增量、过程磁盘峰值和降配后的稳定吞吐尚未测定。

## 尚待验证

| 项目 | 状态 |
| --- | --- |
| 1,000,000 条真实记录的导入、查询、资源 | 已完成导入、建索引、同 MATCH 开关对照、动态四并发及原生查询，见追加章节 |
| 100,000,000 条实际导入与查询 | 未测；来源规模不能替代运行规模 |
| 5,000,000 条真实记录 | 数据与完整 oracle 已准备；数据库导入、查询及资源实测待完成 |
| Nebula + Listener + ES 整体对照 | 百万导入、建索引、12,000 请求查询及幂等写入已实测；原生候选不一致，不能计算对 ES 的提升或成本节省 |
| TEXT_SEARCH 与 ES_QUERY 原生查询语法/候选一致性 | ES 人工 8 行/18 项回归全通过；百万真实语料三项完整候选比对未通过，不能由人工用例推广为真实语料等价 |
| 持续带索引写入、更新、删除和并发稳定性 | 百万空间完成十万次同 VID 同文本写入确认；仍未证明持续新增/更新吞吐或索引可见延迟 |
| 精确全文构建速度和写入至可见延迟 | 百万阶段修复后重测了 job 完成与最终可见性；CREATE 自动回填与显式 REBUILD 的重叠成本仍需说明 |
| 资源降配与机器数减少的实测 | 未测；本机单节点多进程只能验证进程部署成本，不能模拟多机故障隔离 |

后续遵循 [benchmark-runbook.md](benchmark-runbook.md)：先完成修复后的百万验证，再用实测磁盘、内存、时间和完整候选集数量决定下一有界规模。不得用 LIMIT 10 隐藏大规模候选物化成本，不把样本外推写成亿级实测。

## 原始证据

| 证据 | 路径 |
| --- | --- |
| 独立汇总重算 | [10k-independent-audit.json](../../task-logs/tantivy-benchmark/10k-independent-audit.json) |
| 数据 manifest | [sample-10000-128.manifest.json](../../task-logs/tantivy-benchmark/sample-10000-128.manifest.json) |
| 导入 | [20261104T154532Z-import-10k](../../task-logs/tantivy-benchmark/20261104T154532Z-import-10k) |
| 索引 job 事件 | [20261104T154538Z-index-10k](../../task-logs/tantivy-benchmark/20261104T154538Z-index-10k) |
| 开启下推完整 VID / PROFILE | [20261104T154722Z-visible-10k](../../task-logs/tantivy-benchmark/20261104T154722Z-visible-10k) |
| 开启下推性能 / 资源 | [20261104T154743Z-query-10k-on](../../task-logs/tantivy-benchmark/20261104T154743Z-query-10k-on) |
| 关闭下推完整 VID / PROFILE | [20261104T154840Z-visible-10k-off](../../task-logs/tantivy-benchmark/20261104T154840Z-visible-10k-off) |
| 关闭下推性能 / 资源 | [20261104T154847Z-query-10k-off](../../task-logs/tantivy-benchmark/20261104T154847Z-query-10k-off) |
| 恢复下推完整 VID / PROFILE | [20261104T154940Z-visible-10k-restored](../../task-logs/tantivy-benchmark/20261104T154940Z-visible-10k-restored) |
| flag 关闭 / 恢复读回 | [toggle-10k-off.json](../../task-logs/tantivy-benchmark/toggle-10k-off.json)、[toggle-10k-restore-on.json](../../task-logs/tantivy-benchmark/toggle-10k-restore-on.json) |

## 百万条追加记录

本节证据时间为 2026-11-04 15:56–16:22 UTC（北京时间 11 月 4 日 23:56 至 11 月 5 日 00:22）。新的 `ft_bench_1m` 空间仍为 20 分片、3 副本；使用修复 REBUILD 元数据传播问题后的新一批五个服务进程，因此不能把上文 10k 的二进制 SHA256 当作百万构建的摘要。百万各组使用同一批五个服务 PID：100191、100357、100390、100512、100631；本节均为后续 TopK 改动重新编译、重启之前的结果。

输入为 1,000,000 个真实、唯一的原始 pid，前 128 个 Unicode 字符，993,080 条（99.308%）被裁剪；JSONL SHA256 为 `de46025a28771d1b88e20ab78dd2aeda05ef407bf7aab002a12e7afe876a2acd`。本节已有结果的逐条 CSV、批次确认与资源最大值均通过 [1m-first-stage-independent-audit.json](../../task-logs/tantivy-benchmark/1m-first-stage-independent-audit.json) 的独立重算。

| 指标 | 百万阶段已观测值 | 口径 |
| --- | ---: | --- |
| 无全文索引时图写入确认 | 1,000,000 条，0 失败 | 5,000 批×200 条，并发 1 |
| 写入确认耗时 / 吞吐 | 39.7642 s / 25,148.22 rows/s | 仍不是带全文索引写入或可见吞吐 |
| 全文 REBUILD job 12 观察完成 | 66.1502 s | 包括后台工作及 2 秒轮询；CREATE 自动回填与重建不能假定只做了一次构建 |
| 完整透明谓词 VID | 18,267 / 3,166 / 147 / 28 | 与本地原始投影文本 oracle 完全一致；开启、关闭、恢复下推及幂等写入后均通过，PROFILE 符合相应开关 |
| 三项原生 TEXT_SEARCH 完整 VID | 16,505 / 3,184 / 159 | 对应 water、短语 New York、water AND river；没有重复，分数有限，Tantivy 计划已确认；后续 ES 集合比对不一致，见后文 |

从导入阶段开始的 15:56:08 到完整透明谓词校验完成的 15:58:36 相差 148 秒，包含操作者间隔；它只是首次观测到完全可见的总流程上界，不是精确索引延迟。百万原生词项 `water` 的 16,505 个候选不同于大小写敏感子串 `water` 的 18,267 个候选，进一步说明两类查询必须分别报告。

| 查询试验 | 请求数 / 失败 | QPS | P50 / P95 / P99（ms） |
| --- | ---: | ---: | ---: |
| 透明 MATCH，下推开启，并发 1 | 800 / 0 | 11.5048 | 44.99 / 251.60 / 263.72 |
| 透明 MATCH，下推关闭，并发 1 | 80 / 0 | 0.101917 | 9,808.00 / 9,951.78 / 10,059.81 |
| 透明 MATCH，下推开启，动态并发 4 | 800 / 0 | 36.6311 | 75.43 / 276.63 / 299.96 |
| 原生 TEXT_SEARCH，三项查询，并发 1 | 600 / 0 | 18.9293 | 35.05 / 120.96 / 131.09 |

同 MATCH 并发 1 开启/关闭的有效计时分别为 69.5364 / 784.9499 秒，QPS 比值 **112.8833**，P95/P99 分别降低 **97.47% / 97.38%**。该比值只比较同一集群的开关，不是 ES 对照。开启组每项重复 200 次、预热三轮，关闭组每项重复 20 次、预热一轮；开关间不重启服务、不清缓存，两组采样长度与缓存状态不同。关闭组 80 个混合样本的 nearest-rank P99 就是最大值，每条查询仅 20 个样本，其 P95 为第 19 个样本、P99 为最大值；开启组 800 个样本的混合 P99 为第 792 个样本。没有重复轮次或置信区间，不能据此推定尾延迟 SLA。开关间脚本由固定分工改为共享队列，但并发 1 的查询顺序、计时边界不变。

动态四并发组共享 FIFO 队列，四个 worker 分别完成 193 / 198 / 203 / 206 个请求，每项仍为 200 次、预热三轮，0 漏失、0 重复；有效计时 21.8393 秒，所有请求时长之和 87.0012 秒，平均在途请求约 3.984。它是此负载下四并发的观测值，仍未通过并发扫描证明系统饱和点。以上性能查询均返回 LIMIT 10；原生 TEXT_SEARCH 使用不同语义及查询集合，不能与 MATCH 的 QPS 直接计算彼此提升，也不能代表对 ES 的收益。

另保留一轮旧调度器的“四个固定 worker”试验：800 请求零失败，16.2684 QPS，P95 257.00 ms、P99 284.40 ms。此轮每个 worker 固定处理一种查询，四种工作量累计耗时约 49.17 / 14.52 / 7.04 / 6.80 秒，较快 worker 提前退出；按所有请求时长之和 / 总计时长度估算平均在途请求仅约 1.577。**此结果属于固定分工试验，不能标为持续四并发混合负载或饱和吞吐。** 原证据保留于 [20261104T160144Z-query-1m-c4-on](../../task-logs/tantivy-benchmark/20261104T160144Z-query-1m-c4-on)，诊断见 [1m-original-c4-scheduling-audit.json](../../task-logs/tantivy-benchmark/1m-original-c4-scheduling-audit.json)。后续脚本已改为共享 FIFO 队列动态补位，summary 显式记录调度策略与每 worker 请求数，离线慢/快混合模拟证明 0 漏失、0 重复、并发不超过配置值，且并发 1 顺序不变。

百万已完成阶段的五服务采样最大值：导入 PSS 1.032 GiB、CPU 4.839 核；建索引 PSS 2.059 GiB、CPU 5.909 核；开启下推并发 1 查询 PSS 1.570 GiB、CPU 2.539 核；关闭下推并发 1 查询 PSS 4.048 GiB、CPU 6.756 核；动态四并发 PSS 1.938 GiB、CPU 7.824 核；原生查询 PSS 1.489 GiB、CPU 2.381 核。开启/关闭并发 1 分别采到 69 / 788 轮、68 / 787 个 CPU 差分区间；动态四并发为 23 轮、22 个有效区间，全部 PID 存活且 PSS 可读。这些仍为轮询观测值，包含 wrapper 的初始化和预热窗口，非全机成本或精确峰值，不能直接计算可降配内存比例。关闭下推完整 VID 校验的 PSS 3.848 GiB 属于另一校验阶段，不能与只取 LIMIT 10 的查询延迟混算。

百万已建全文索引空间再次写入前 100,000 个原始 VID 及**完全相同文本**，500 批×200 条、并发 1，全部成功，4.72054 秒，**21,184.01 rows/s 写入确认吞吐**。其后四个完整 VID 集合仍与原 oracle 相同。本轮属于短时幂等重写，不是新增记录吞吐，不是改变文本后的索引可见吞吐：即使索引尚未处理重写事件，查询结果也可以保持相同，故不能用相同结果反推索引已应用全部更新。资源仅采到 5 轮、4 个有效 CPU 区间，观测 PSS 2.198 GiB、CPU 10.059 核，不作为持续负载结论。

关闭/恢复阶段独立重算见 [1m-off-and-restored-independent-audit.json](../../task-logs/tantivy-benchmark/1m-off-and-restored-independent-audit.json)，动态四并发及幂等写入见 [1m-dynamic-c4-and-upsert-independent-audit.json](../../task-logs/tantivy-benchmark/1m-dynamic-c4-and-upsert-independent-audit.json)。历史固定分工四并发只用于问题排查，主结果采用动态调度这一轮。

## 百万容量核算与一亿门槛

[snapshot-after-1m-index.json](../../task-logs/tantivy-benchmark/snapshot-after-1m-index.json) 相对导入前快照，三个 storaged 合计图数据目录实占增量 **1,617,575,936 bytes（1.506 GiB）**，全文索引目录增量 **941,432,832 bytes（0.877 GiB）**。三份副本已经包含在合计中，不能再乘 3；图目录含 RocksDB、WAL、普通 Tag 索引与保留分配。Tantivy 索引同时包含词法字段及用于保持原文子串语义的 trigram / 原文验证结构，不能把这一合计当作单份、仅词法倒排索引的大小。此数值是在 128 字符投影上测得，不代表完整原文。

截至该快照的全机计数器新增 swap-in 83,578,880 bytes、swap-out 0、oom_kill 0；swap-in 可能读入此前已经换出的页，不能解释为本阶段新增同等内存缺口，也不能只看当前 SwapUsed 判断是否发生本阶段换出。

容量门槛使用百万实测作**估算**：[一亿门槛](../../task-logs/tantivy-benchmark/capacity-gate-100m.json) 需要可用空间约 460.69 GiB（含增长不确定性、索引合并余量、剩余下载与 40 GiB 保留），而当时实测余量约 246.45 GiB；`water` 完整候选数线性估计约 182.67 万，也超过本地安全门槛。两项检查均未通过，**不能在本机直接推进一亿导入和宽查询，更不能声称亿级已测**。[500 万门槛](../../task-logs/tantivy-benchmark/capacity-gate-5m.json) 当前通过，仅允许下一有界阶段；通过不等于已经执行或保证线性扩展。

百万新增证据：[导入](../../task-logs/tantivy-benchmark/20261104T155608Z-import-1m)、[索引](../../task-logs/tantivy-benchmark/20261104T155709Z-index-1m)、[开启下推完整校验](../../task-logs/tantivy-benchmark/20261104T155835Z-visible-1m)、[开启下推查询](../../task-logs/tantivy-benchmark/20261104T155848Z-query-1m-c1-on)、[原生完整候选集](../../task-logs/tantivy-benchmark/native-1m-tantivy/collection.json)、[原生查询](../../task-logs/tantivy-benchmark/20261104T160033Z-native-1m-c1)、[关闭下推完整校验](../../task-logs/tantivy-benchmark/20261104T160318Z-visible-1m-off)、[关闭下推查询](../../task-logs/tantivy-benchmark/20261104T160715Z-query-1m-c1-off)、[恢复下推完整校验](../../task-logs/tantivy-benchmark/20261104T162110Z-visible-1m-restored)、[动态四并发](../../task-logs/tantivy-benchmark/20261104T162117Z-query-1m-c4-dynamic)、[十万次幂等写入](../../task-logs/tantivy-benchmark/20261104T162222Z-upsert-100k-indexed)、[幂等写入后完整校验](../../task-logs/tantivy-benchmark/20261104T162247Z-visible-after-upsert-100k)。

## ES 基线批次配置校正：旧索引阶段排除计时

2026-11-04 16:42 UTC 核查旧版 Listener：`listener_commit_batch_size` 默认 1000 的实际单位是 **key/value 字符串字节**，并非 1000 条记录。旧 `ESListener::processLogs()` 在处理完整条 WAL 后检查 `BatchHolder::size() > listener_commit_batch_size`；`BatchHolder::put()` 累加 key 和 value 的长度；每个分片的 `Listener::doApply()` 每轮处理后再等待 `listener_commit_interval_secs`（默认 1 秒）。因此默认配置可以让有积压的分片每秒只提交一个很小的批次，不能把此时较低的端到端速率归因于 ES 引擎算力不足。

对照配置调整为 **`listener_commit_batch_size=1048576`（1 MiB），`listener_commit_interval_secs=1`**，继续保留现有线程、分片与图副本设置。1 MiB 是批次停止阈值，整条 WAL 可以让实际字节数超过阈值；它也不包括容器、字符串副本、ES bulk JSON 和并发分片的额外内存，不能称作严格 HTTP 批次大小或进程内存上限。本轮只选择有界的合理配置，尚未声称该值是最优调参结果。较大的批次可能改变吞吐、CPU、内存和尾部可见延迟，均应在新实验中实测。

运行中 Listener PID 141494 的动态读回证据显示值由 1000 改为 1048576，等待间隔仍为 1 秒：[es-listener-batch-tune.json](../../task-logs/tantivy-benchmark/es-listener-batch-tune.json)。后续启动配置也已同步，文件前后内容见 [es-listener-batch-config.json](../../task-logs/tantivy-benchmark/es-listener-batch-config.json)。配置文件编辑本身没有重启或修改运行中的进程；动态修改另有上述独立证据。

由于 [20261104T163904Z-es-index-1m](../../task-logs/tantivy-benchmark/20261104T163904Z-es-index-1m) 混合了默认与调整后的配置，其**整个索引阶段耗时、建索引吞吐及资源对照无效**，仅保留为问题排查证据，不能选取调参后的局部窗口宣称建索引速度，也不能与 Tantivy 结果计算提升倍数。[排除记录](../../task-logs/tantivy-benchmark/20261104T163904Z-es-index-1m/invalid-for-build-performance.json) 明确要求等待此次任务完成后，删除该专属全文索引并在固定配置下重新创建和重建，新的计时目录与结果必须单独归档。该排除不自动否定先前独立完成的图写入阶段。

旧 `RebuildFTIndexTask` 会 reset Listener 后等待其追上 leader，因而完整重建耗时包含 Listener 追赶；`commitSnapshot()` 还有直接提交快照的路径，不受上述 WAL 字节阈值的同一循环约束。新一轮仍须同时检查重建 job/task、正常 refresh 后的精确百万文档计数以及完整查询 VID 集合，不能只以 job 完成当作全部数据可见。此处源码依据是旧 `3.6-io` 工作目录，现有二进制版本与源码 HEAD 的差异仍需在最终对照中披露。

## ES 元数据传播检查：计数不完整的阶段同样排除

固定 1 MiB 批次后，[20261104T164301Z-es-index-1m](../../task-logs/tantivy-benchmark/20261104T164301Z-es-index-1m) 的 job 3 虽报告 FINISHED，ES 可见计数却稳定在 **996,388**，较输入少 **3,612**；未出现完整可见的成功事件。[日志独立核查](../../task-logs/tantivy-benchmark/es-incomplete-stage-count-audit.json) 保留全部计数观测。此轮不满足完整性要求，**不能填入有效索引时间、吞吐或资源收益**。

旧 MetaClient 的心跳线程在 heartbeat 后执行 `loadData()`，按元数据最后修改时间决定是否刷新，其中包含全文索引元数据。Listener 的 schema 查询从该本地缓存读取索引；旧 `pickTagAndEdgeData()` 在索引集合为空时直接跳过该条记录，外层仍可能推进已应用位置。这与 CREATE 后立即重建造成首批索引遗漏的现象相符，但没有逐条缓存状态追踪，不能把推断写成已完全证实的因果链。[只读 flag 核查](../../task-logs/tantivy-benchmark/es-listener-meta-refresh-readback.json) 显示此二进制无 `load_meta_interval_secs`（返回 null），实际 `heartbeat_interval_secs=1`。

后续流程在 CREATE 和 ES 字段/配置检查后、REBUILD 前读取 Listener 的实际刷新相关参数，等待 `max(5, 2 × 周期 + 2)` 秒，所需等待超过 30 秒时拒绝继续；当前实际等待为 5 秒。等待开始、结束和耗时均独立记录。**这是保守时间保护，不是缓存已就绪的证明**；最终成功证据仍是 job/task 完成、精确百万文档计数和完整查询 VID 一致。总流程耗时包含这段等待，另列的 REBUILD 至可见耗时不含等待，但仍包含任务轮询和正常 refresh，不能称作纯 ES 构建时间。

缓存已稳定后，恢复性 job 4 再次重建并得到完整百万计数，见 [恢复观测](../../task-logs/tantivy-benchmark/es-recovery-rebuild-observation.json) 与 [后续清理记录](../../task-logs/tantivy-benchmark/es-clean-index-after-recovery.json)；这一恢复操作也不计入性能对照。最终新建索引的独立阶段为 `20261104T164706Z-es-index-1m`，已完成核验，见下一节。此前代理失败的 `163611Z`、中途调参的 `163904Z`、计数缺失的 `164301Z` 三个索引阶段全部保留并排除；不能从失败阶段挑选较快的局部数字。单独的图写入阶段 `20261104T163531Z-es-import-1m` 已另行核查，它与后续重启后索引阶段的服务 PID 分别记录。

## ES 百万基线：有效构建与延长查询测量

本节采用同一份百万原始 PID、128 字符投影、20 图分片及 3 图副本，部署为 graph、meta、3 storage、Listener、ES 共 7 个服务。旧 Nebula Debug 二进制自报 Git `de9b3ed80`，与源码目录当前 HEAD 不同；ES 为 7.17.29，JVM heap 512 MiB。Listener 固定 1 MiB 字节阈值、1 秒间隔。原型与 ES 组顺序运行，未清理文件缓存；多次前置排查已经预热了旧组数据与 WAL，不能称作冷缓存构建对照。

**索引结构与可用性条件不同，下面的构建、磁盘及 CPU 绝对值必须连同这些条件阅读：** Tantivy 同时建立词法字段及保持原文语义的 trigram / 原文验证结构，三个 storage 各有一份本地索引；ES 基线只建立 standard 词法索引，配置为 1 primary / 0 replicas，另有 Listener WAL。图数据同为 3 副本，不代表全文索引具有相同复制策略与可用性。两者承担的索引工作、查询执行结构和故障隔离条件不同；本报告不把这些绝对差值换算成通用的构建加速、CPU 节省、磁盘压缩或机器降配百分比。相同 MATCH 下推开关的独立对照仍只解释其明确限定的查询工作负载。

| 有效阶段 | 实测结果 | 口径 |
| --- | ---: | --- |
| 无全文索引图写入确认 | 1,000,000 条，0 失败；36.4521 秒，27,433.28 rows/s | 5,000 批×200，单并发；仅图写入确认 |
| 最终新建索引流程 | 41.9425 秒 | 只有 `164706Z` 阶段有效；job 5 完成且 ES 正常 refresh 后精确 1,000,000 文档 |
| 其中显式元数据时间保护 | 5.0042 秒 | 按实际 1 秒心跳选择 5 秒等待，已单独记录 |
| REBUILD 至完整可见 | 36.7696 秒 | 包含 Listener 重建、任务轮询和正常 refresh，非纯 ES 引擎构建时间 |
| 有索引时同 VID 同文本重写 | 100,000 次，0 失败；4.56878 秒，21,887.67 rows/s | 500 批×200，单并发；幂等确认吞吐，不是新增或索引可见吞吐 |

图写入阶段 PID 为 131155 / 131356 / 131408 / 131532 / 131648 / 131769 / 136093。重启后的索引阶段 PID 为 140735（ES）、140860（meta）、141084（graph）、141117 / 141250 / 141368（storage）、141494（Listener）。为确保查询优化器在启动时启用，查询前 graph 再次重启为 **151177**；查询和幂等写入使用它替代 141084，其余六个 PID 保持不变。各段资源单独归档，不能把旧 PID 的采样套到新阶段。

完整集合校验使用相同输入哈希，分别收集全部 VID，无重复、有限 score，ES `_count` 与返回数量一致，但两引擎的集合确实不同：

| 原生查询 | Tantivy 完整 VID 数 | ES 完整 VID 数 | 仅 Tantivy / 仅 ES |
| --- | ---: | ---: | ---: |
| `water` | 16,505 | 16,451 | 54 / 0 |
| `"New York"` | 3,184 | 3,107 | 77 / 0 |
| `water AND river` | 159 | 157 | 2 / 0 |

差集文件和完整比较见 [native-1m-comparison](../../task-logs/tantivy-benchmark/native-1m-comparison)。**这些结果尚未达到两引擎相同查询语义的对照条件，不能计算 ES 与 Tantivy 的查询加速倍数。** 相同 analyzer 名称不自动保证分词与语法完全一致；差异原因应另行定位，不能凭两组都有非零返回就判为正确。ES 幂等重写后的三个完整集合及 SHA256 与该引擎重写前完全相同，但同文本重写仍不能证明异步索引应用时间。

主要 ES 查询数字采用以下**每组 12,000 请求**的延长轮次。三项查询各重复 4,000 次，预热 20 轮（60 请求，不计时），LIMIT 10，0 失败；两组按单并发后四并发顺序执行，未清缓存，均为一个测量轮次。上文旧 Tantivy 600 请求、3 轮预热数据属于历史阶段，新版 Tantivy 的同请求规模结果在后文单列；真实语料候选仍不一致，不能据此计算跨引擎加速比。

| ES 主要查询指标 | 单并发 | 动态四并发 |
| --- | ---: | ---: |
| 成功 / 总请求 | 12,000 / 12,000 | 12,000 / 12,000 |
| 有效计时 | 92.89179 s | 18.12272 s |
| 成功 QPS | **129.1826** | **662.1524** |
| P50 | 6.3663 ms | 4.9037 ms |
| P95 | **14.2067 ms** | **10.9491 ms** |
| P99 | **17.4246 ms** | **12.9534 ms** |
| 最大单次延迟 | 26.6966 ms | 21.7661 ms |
| 资源采样轮数 / CPU 差分区间 | 91 / 90 | 19 / 18 |
| 同轮七服务 PSS 合计最大值 | 2.3076 GiB | 2.3117 GiB |
| 同轮七服务 RSS 合计最大值 | 2.4238 GiB | 2.4279 GiB |
| 七服务 CPU 区间平均之和最大值 | 1.332 核 | 3.506 核 |

动态四并发通过共享队列补位，四个 worker 分别完成 2,993 / 3,008 / 2,998 / 3,001 请求，平均在途约 3.991，无漏失或重复。所有采样 PID 存活，PSS 无缺项。资源窗口包含初始化及预热，仍为采样观测峰值，并非全机峰值或可降配证明。18 秒与 93 秒的单轮观测尚不足以证明长期稳定吞吐、冷启动性能或生产 SLA，也未扫描并发度确定饱和点。独立重算见 [es-1m-long-query-independent-audit.json](../../task-logs/tantivy-benchmark/es-1m-long-query-independent-audit.json)；主证据为 [单并发长轮次](../../task-logs/tantivy-benchmark/20261104T165603Z-es-native-1m-c1-long)、[动态四并发长轮次](../../task-logs/tantivy-benchmark/20261104T165815Z-es-native-1m-c4-long)。

下列先前短轮次只保留为**预热和探索记录**，不作为主要吞吐或资源结论：

| ES 短轮次 | 请求 / 失败 | 有效时间 | QPS | P50 / P95 / P99（ms） | 资源轮数 / CPU 区间 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 单并发 | 600 / 0 | 5.17649 s | 115.9086 | 7.37 / 15.30 / 18.70 | 6 / 5 |
| 动态四并发 | 600 / 0 | **0.99712 s** | 601.7309 | 5.54 / 11.12 / 14.11 | **2 / 1** |

两组均每项 200 次、预热三轮、LIMIT 10。约 1 秒的四并发样本不能代表持续吞吐、饱和能力或资源节省；顺序执行、缓存与短窗口可能影响两轮相对结果。不得用此表计算降配比例。完整 CSV、批次确认、各阶段七服务资源汇总均通过 [es-1m-valid-stages-independent-audit.json](../../task-logs/tantivy-benchmark/es-1m-valid-stages-independent-audit.json) 重算。

[ES 索引后目录快照](../../task-logs/tantivy-benchmark/snapshot-after-es-1m-index.json) 相对导入前，三个 graph 数据目录合计增加 1,382,998,016 bytes；ES 数据目录增加 611,684,352 bytes，Listener WAL 增加 313,499,648 bytes，后两项合计 925,184,000 bytes。快照工具的 `index_allocated_bytes` 在此实验显式包含 **ES + Listener WAL**，不是仅倒排索引文件大小；原型的同名字段包括三个 Tantivy 目录，且每份同时包含词法与 trigram / 原文验证结构，比较时必须说明组成。实际索引结构、副本策略、图二进制版本、已保留 WAL 和后台合并状态不同，不能仅凭这些目录差值宣称等可靠性资源降配。

## ES 人工语料语法回归：18 / 18 通过

所有 ES 性能计时结束后，在专属 `ft_regression_es_v1` 空间创建 20 分片、3 副本及 `ftcase(title,body)`，全文索引为 `nebula_index_ft_regression_es_v1`，standard analyzer。只写入仓库中的 **8 条人工正确性数据**，等待 5 秒元数据时间保护并完成 job 7 重建后，先核对 ES 中 8 条完整字段、无重复的全部 VID，以及经过 graph 的全部 VID，再调用现有 `regression.py`。

2026-11-04 16:59:48 UTC，**18 项全部通过**：17 项成功查询的完整 VID 符合预期，1 项未闭合引号明确返回 ES `parse_exception`，没有把连接失败当作语法拒绝。覆盖词项、大小写归一、短语、AND/OR/NOT、字段组合、跨字段、前缀、星号/问号通配、两种 regex、模糊单编辑、短语距离、boost 和范围查询。boost 用例这里只验证命中集合，不能据此证明分数数值或跨引擎排名一致。

原始结果与事件完整保留在 [es-grammar-regression-v1](../../task-logs/tantivy-benchmark/es-grammar-regression-v1)，其中 [regression.json](../../task-logs/tantivy-benchmark/es-grammar-regression-v1/regression.json) 包含每项查询、预期/实际 VID、错误和耗时；[setup-summary.json](../../task-logs/tantivy-benchmark/es-grammar-regression-v1/setup-summary.json) 包含服务 PID、字段与索引配置、语料和用例哈希、重建与等待信息。fixture 与性能空间隔离，运行时间位于主要性能采样之后。

**人工 18 项通过只证明这些具体用例。即使两端都通过同一套 18 项，也不等于百万真实语料三项原生查询集合一致**；真实语料已保存的 54 / 77 / 2 个差异仍然存在，必须在最终功能兼容性结论中保留。人工 fixture 不是公开百万语料，也不是规模、性能或中文检索能力的证据。

## 新版 Tantivy 百万原生 TopK：延长查询测量

此节为重新编译、重启后启用 storage TopK 的原生 `TEXT_SEARCH` 测量，与前文旧构建的 MATCH 开关及 600 请求原生查询分开归档。使用同一百万输入和三个原生 query-string，每项重复 4,000 次，预热 20 轮（60 请求，不计时），返回 VID、score、text，LIMIT 10。[PROFILE 摘要](../../task-logs/tantivy-benchmark/topk-1m-profile-summary.json) 记录 `engine=tantivy`、全文扫描 limit 10 以及每分片 TopK 10 的检查结果。

| 新版原生 TopK 指标 | 单并发 | 动态四并发 |
| --- | ---: | ---: |
| 成功 / 总请求 | 12,000 / 12,000，0 失败 | 12,000 / 12,000，0 失败 |
| 有效计时 | 331.28644 s | 227.65636 s |
| 成功 QPS | **36.2224** | **52.7110** |
| P50 | 22.4880 ms | 72.0617 ms |
| P95 | **55.8222 ms** | **119.7265 ms** |
| P99 | **63.3650 ms** | **139.9441 ms** |
| 最大单次延迟 | 73.4304 ms | 184.9238 ms |
| 资源采样轮数 / CPU 差分区间 | 328 / 327 | 226 / 225 |
| 同轮五服务 PSS 合计最大值 | **0.6487 GiB** | **0.6642 GiB** |
| 同轮五服务 RSS 合计最大值 | 0.7403 GiB | 0.7558 GiB |
| 五服务 CPU 区间平均之和最大值 | 1.492 核 | 2.077 核 |

五个实际服务 PID 为 155648（meta）、155814（graph）、155847 / 155974 / 156183（storage）；所有采样均存活，PSS 无缺项。三项查询每次均返回 10 条，逐请求计数、nearest-rank 百分位、QPS 和同轮资源合计全部独立重算通过，见 [单并发长轮次](../../task-logs/tantivy-benchmark/20261104T170124Z-tantivy-topk-1m-c1-long) / [单并发审计](../../task-logs/tantivy-benchmark/tantivy-topk-1m-c1-long-independent-audit.json)、[动态四并发长轮次](../../task-logs/tantivy-benchmark/20261104T170721Z-tantivy-topk-1m-c4-long) / [四并发审计](../../task-logs/tantivy-benchmark/tantivy-topk-1m-c4-long-independent-audit.json)。四并发 worker 分别完成 3,009 / 2,981 / 3,028 / 2,982 个请求，平均在途约 3.999，无遗漏或重复。吞吐和单次延迟随并发的变化均直接列出，未将其中较有利的一项代替完整结果。

上述数值只描述这轮绝对观测。ES 和此原型虽然请求数、预热轮数及返回字段相同，但真实语料完整候选集合不同，词法/原文 trigram 索引结构与全文副本策略也不同；因此本节不计算跨引擎速度、CPU 或内存节省比例。重启与此前试验留下的缓存状态亦需保留在解释中。资源数值是五服务采样峰值，不含全部文件缓存或客户端，不能直接当作可配置内存上限；一次约五分半的单并发测量也不能证明生产稳定性。
