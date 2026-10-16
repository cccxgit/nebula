# WAL 空闲分片内存调查

第一次接手开发，请先阅读 [开发接手指南](DEVELOPER-HANDOFF.md)，再通过 [完整成果索引](ARTIFACT-INDEX.md) 查找文章、代码、工具和证据。

本目录的原调查执行工程 `task/task-wal.md`，范围为解释、观测和复现；后续已形成空间级配置需求设计，**新机制尚未实现**。基线提交：`de9b3ed800a6627d9845e9289b6bbc5b6faf460a`。

历史发布：2026-10-10 的提交仅收录当时六篇文章和两张图表；2026-10-11 又完成通俗改写、场景对比及需求设计。2026-10-16 的开发交接交付将完整选定原始采样、服务日志、WAL 归档、工具源码和观测补丁一起纳入，实际范围以 [发布清单](migration/publish-files.txt) 与当前 Git 提交为准。

后续分析遵循工程根目录的 [可读性与证据规范](../../nebula_graph_readable_analysis_prompts.md)：先理解因果，再核对源码和实验；明确区分已确认机制、实际测量和待验证推断。

2026-10-16 新增 [会话成果清单与开发环境迁移说明](DELIVERY-AND-MIGRATION.md)，包含精确清单、目标环境预检和组件重建步骤。可重建 ELF、临时数据库及 Word QA 缓存只保留本机；各审计报告中的“未提交”描述对应其生成时刻，发布状态以 Git 记录为准。

## 建议阅读顺序

开发启动请先读 [图空间级 WAL 缓存配置需求设计](SPACE-LEVEL-WAL-REQUIREMENTS-DESIGN.md)，另有便于团队批注的 [Word 版](SPACE-LEVEL-WAL-REQUIREMENTS-DESIGN.docx)。该评审稿面向首次接触问题的开发者，覆盖背景、源码因果、原始复现、配置契约、构造链、更新入口、验收和发布回退；首期仅设计空间级 WAL 容量，尚未实现内核改造。设计复核和交付记录见 [design-evidence](design-evidence/README.md)。

1. [通俗调查报告](REPORT.md)：用最小例子解释空闲为什么仍涨内存，再用关键代码与实验逐项验证。
2. [持续写入与空闲的对比](WRITE-VS-IDLE.md)：分别解释 WAL、RocksDB 和整个进程，区分组件实测与完整服务待验证项。
3. [原始实验明细与复现步骤](EXPERIMENT-DETAILS.md)：保存改写前的完整数据、时间窗口、配置、失败阶段和复查命令。
4. [源码机制、定量模型与原始组件实验](COMPONENT-ANALYSIS.md)：为什么 8MiB 的计账可以对应约 28MiB 节点分配，以及每日增长量的条件推算。
5. [Raft 逐函数解读](CODE-WALKTHROUGH-RAFT.md)：空间/分片对象生命周期、选举、两条心跳路径、Leader/Follower 复制与提交、迭代器生命周期。
6. [WAL 逐函数解读](CODE-WALKTHROUGH-WAL.md)：文件写入/恢复、64 槽节点、链表方向、逻辑容量、标删、读者引用、GC、reset、析构、磁盘 TTL 与回退读取。
7. [RocksDB 与内存统计逐函数解读](CODE-WALKTHROUGH-ROCKSDB.md)：提交位点的覆盖写、MemTable、刷盘、共享缓存、内存监控、jemalloc purge、RSS。
8. [生产环境参数调优分析](PRODUCTION-PARAMETER-TUNING.md)：三类冷热空间的适用参数、作用范围与生效方式、WAL 容量实测、风险、生产验证和回退条件；没有操作现网。
9. [图空间级内核参数设计评估](SPACE-LEVEL-KERNEL-PARAMETERS.md)：在用户允许开发新机制后，将方案修订为全局默认加空间覆盖；首期仅 WAL 容量、重启生效，明确未来 RocksDB/Meta 扩展与兼容边界。设计尚未实现。

## 复现与证据

| 文件/目录 | 用途 |
|---|---|
| `cluster.py`、`configs/`、`*.ngql` | 三个本机 storage 实例的配置及真实执行的 nGQL；仅建空空间与观测/刷盘 |
| `events.jsonl` | 编译并行度变更、服务控制、采样阶段、心跳配置变更及命令时间 |
| `configure.log`、`build.log`、`build-j5.log`、`install.log` | 完整工程构建记录；用户新增约束后使用 `make -j5` |
| `sample_runtime.py`、`phase-*/` | 每个进程的 RSS/匿名内存、RocksDB 属性、WAL 文件计数和 HTTP 原始响应 |
| `service-logs/`、`wal-observe-s*.csv` | 原始服务日志与从中抽取的 WAL 节点/空消息/GC 观测 |
| `parse_wal.py`、`wal-full.json`、`wal-full.csv` | 逐条验证磁盘 WAL 的记录长度、ID、任期、空/非空 payload |
| `wal-evidence.tar.gz`、`wal-archive-verification.json` | 停机后的 3000 个原始 WAL 文件；逐文件校验与归档 SHA-256 |
| `run-atomic-buffer-probe.sh`、`atomic-buffer-probe.cpp`、`atomic-*.csv` | 直接调用固定提交原始 WAL 实现的组件实验，含 GLIBC/jemalloc 对照 |
| `baseline/kvstore/wal/` | 从固定提交提取的原始 `.cpp/.h`，没有观测补丁 |
| `rocks-commit-probe*` | 仅反复覆盖提交位点键的 RocksDB 独立实验 |
| `observation.patch`、`instrumentation-notes.txt` | 唯一产品源码改动的完整补丁及统计口径；只增加可关闭的计数/日志 |
| `analyze_results.py`、`summary.json`、`*.png`、`*.svg` | 从原始采样生成汇总与静态曲线 |
| `analyze_runtime_rocks.py`、`runtime-metrics.json`、`runtime-rocks-analysis.txt` | 独立复算实际采样窗口、增长斜率、FLUSH 对照和任期稳定性 |
| `sample-verification.json`、`flush-job-verification.json`、`shutdown-verification.json` | 采样有效性、50 个作业完成及 5 个实验进程退出核验 |
| `SHA256SUMS`、`document-verification.json` | 交付时文件校验清单与文档链接检查结果 |
| [write-idle-comparison-20261011](write-idle-comparison-20261011/README.md) | 新增的空日志、256B、1KiB 日志及停写转空日志对照；原始实现单组件实测，包含独立校验清单 |
| [production-tuning-evidence](production-tuning-evidence/README.md) | 新增 8/4/2/1 MiB、256 KiB 容量对照及生产参数源码核查；包含完整运行日志 |

代码基线为上述固定提交；原始 `AtomicLogBuffer.cpp` 的引用明确指向 `baseline/` 或固定提交的源码，避免观测补丁增加行数后混淆。逐函数文章的仓库源码导航已转换为固定版本 GitHub 链接；原实验机第三方头文件及构建文件以文字路径保留。图表中的 MiB 均为 `1024²` 字节。

本目录的旧 `SHA256SUMS`、`document-verification.json` 记录的是 2026-10-10 交付状态。文档发布和本轮改写会改变文章字节，不应再将旧清单当作新文档的校验结果；原始采样与运行日志没有因此改写。新组件实验使用其子目录自己的 `SHA256SUMS`。

本机服务安装目录为 `/home/sch/nebula-run/nebula-memory`。实验使用独立端口：graph `29669`、meta `29559`、storage `29779/29789/29799`。服务启动前由脚本设置 `ulimit -n 65536` 的等价进程限制。重新执行前先阅读 `REPORT.md`，采样输出目录不允许覆盖已有证据。

原空闲服务实验已经结束，5 个服务进程全部退出，原始数据保留。无需启动数据库即可按 [实验明细](EXPERIMENT-DETAILS.md) 第 6.5 节重新解析归档 WAL、第 6.6 节重新计算指标和绘图。新增组件对照同样没有启动数据库服务。
