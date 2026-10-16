# WAL 研究成果索引与开发交接材料

交付日期：2026 年 10 月 16 日。目标仓库：`cccxgit/nebula`，分支：`3.6-memory`。

**接手开发先读 [开发接手指南](DEVELOPER-HANDOFF.md)。** 本索引回答每份材料在哪里、用于什么；完整开发契约在 [0.9 需求设计评审稿](SPACE-LEVEL-WAL-REQUIREMENTS-DESIGN.md)，异机操作在 [迁移说明](DELIVERY-AND-MIGRATION.md)。

## 交付状态和阅读口径

源码和本地实验确认了空闲内部日志、WAL 缓存节点与 RocksDB 提交位点写缓冲的增长路径。当前产品源码只有 124 行观测改动，空间级配置仍待开发，建议容量范围和性能门槛待责任人评审；完整 50 空间真实业务写入和生产收益尚未验收。

历史文章或审计中的“只保留本机”“未提交”描述其生成时刻；本次交付补齐下列清单中的证据与工具。Word 设计稿保留原评审版本，状态更新以本指南和 Git 为准。源码行号链接已尽量固定到分析基线；原机 `/opt/vesoft/...` 头文件位置是依赖记录，不能当成仓库内文件。

本索引逐项覆盖 **376 个交付路径**，与 [publish-files.txt](migration/publish-files.txt) 一一对应。分类是阅读用途，不代表每个文件都是本次新增或都可执行。

## 按目的找入口

| 你要做什么 | 先看哪里 |
| --- | --- |
| 接手需求和拆分开发 | [接手指南](DEVELOPER-HANDOFF.md)、[需求设计](SPACE-LEVEL-WAL-REQUIREMENTS-DESIGN.md) |
| 理解问题和证据强度 | [通俗报告](REPORT.md)、[实验明细](EXPERIMENT-DETAILS.md)、[写入对比](WRITE-VS-IDLE.md) |
| 在另一环境复核 | [迁移说明](DELIVERY-AND-MIGRATION.md)、[脚本审计](migration/SCRIPT-PORTABILITY-AUDIT.md) |
| 核对产品代码改动 | [代码审计](migration/CODE-CHANGE-AUDIT.md)、[完整观测补丁](observation.patch) |
| 确认文件完整性 | [交付 JSON 清单](migration/DELIVERY-MANIFEST.json)、[校验工具](migration/verify_delivery.py) |

## 分类目录

| 材料类别 | 路径数 |
| --- | ---: |
| 研究文章 需求设计与图表 | 19 |
| 产品观测代码与原始基线 | 5 |
| 复现 采样 解析与组件工具源码 | 12 |
| 迁移工具 审计与重新构建记录 | 58 |
| 历史配置与 nGQL 模板 | 10 |
| 原始服务分阶段采样 | 60 |
| 原始服务日志与必要别名 | 36 |
| 原始 WAL 与解析观测 | 21 |
| 容量对照与参数方案证据 | 23 |
| 空闲与 payload 对照证据 | 12 |
| 解析器合成测试固件 | 12 |
| 需求设计独立审查与出版记录 | 19 |
| 实验构建 环境 时间线与汇总 | 81 |
| 工程说明与协作约束 | 6 |
| 发布文件规则 | 2 |

## 研究文章 需求设计与图表

| 仓库相对路径 | 用途和注意点 |
| --- | --- |
| [task/wal-investigation/ARTIFACT-INDEX.md](ARTIFACT-INDEX.md) | 完整交付导航，本文件 |
| [task/wal-investigation/CODE-WALKTHROUGH-RAFT.md](CODE-WALKTHROUGH-RAFT.md) | Raft 对象、心跳、复制、提交和读者 |
| [task/wal-investigation/CODE-WALKTHROUGH-ROCKSDB.md](CODE-WALKTHROUGH-ROCKSDB.md) | 提交位点、写缓冲、共享缓存和内存统计 |
| [task/wal-investigation/CODE-WALKTHROUGH-WAL.md](CODE-WALKTHROUGH-WAL.md) | 磁盘 WAL、缓存节点、标删、GC 和文件回退 |
| [task/wal-investigation/COMPONENT-ANALYSIS.md](COMPONENT-ANALYSIS.md) | 结构开销、容量模型与独立组件机制 |
| [task/wal-investigation/DELIVERY-AND-MIGRATION.md](DELIVERY-AND-MIGRATION.md) | 精确提交范围、目标环境依赖和迁移步骤 |
| [task/wal-investigation/DEVELOPER-HANDOFF.md](DEVELOPER-HANDOFF.md) | 接手第一入口：阅读、离线复查、评审、开发与验收顺序 |
| [task/wal-investigation/EXPERIMENT-DETAILS.md](EXPERIMENT-DETAILS.md) | 原始实验时间窗口、操作、复现和限制 |
| [task/wal-investigation/PRODUCTION-PARAMETER-TUNING.md](PRODUCTION-PARAMETER-TUNING.md) | 生产空间分级、参数取舍和验证策略 |
| [task/wal-investigation/README.md](README.md) | 研究总览和阅读入口 |
| [task/wal-investigation/REPORT.md](REPORT.md) | 通俗因果解释与服务实验关键证据 |
| [task/wal-investigation/SPACE-LEVEL-KERNEL-PARAMETERS.md](SPACE-LEVEL-KERNEL-PARAMETERS.md) | 空间级配置可行性和方案演进 |
| [task/wal-investigation/SPACE-LEVEL-WAL-REQUIREMENTS-DESIGN.docx](SPACE-LEVEL-WAL-REQUIREMENTS-DESIGN.docx) | 需求设计 Word 评审版 |
| [task/wal-investigation/SPACE-LEVEL-WAL-REQUIREMENTS-DESIGN.md](SPACE-LEVEL-WAL-REQUIREMENTS-DESIGN.md) | 0.9 需求设计评审稿，首期开发与验收契约 |
| [task/wal-investigation/WRITE-VS-IDLE.md](WRITE-VS-IDLE.md) | 空闲与持续 payload 的对比及整机待验边界 |
| [task/wal-investigation/component-wal.png](component-wal.png) | 由采样生成的研究图表，核对数字时回到原始数据 |
| [task/wal-investigation/component-wal.svg](component-wal.svg) | 由采样生成的研究图表，核对数字时回到原始数据 |
| [task/wal-investigation/runtime-memory.png](runtime-memory.png) | 由采样生成的研究图表，核对数字时回到原始数据 |
| [task/wal-investigation/runtime-memory.svg](runtime-memory.svg) | 由采样生成的研究图表，核对数字时回到原始数据 |

## 产品观测代码与原始基线

| 仓库相对路径 | 用途和注意点 |
| --- | --- |
| [src/kvstore/wal/AtomicLogBuffer.cpp](../../src/kvstore/wal/AtomicLogBuffer.cpp) | 唯一产品源码改动，124 行观测；缓存策略未改 |
| [task/wal-investigation/baseline/kvstore/wal/AtomicLogBuffer.cpp](baseline/kvstore/wal/AtomicLogBuffer.cpp) | 固定提交 de9b3ed 的未改动源码，供原算法组件对照 |
| [task/wal-investigation/baseline/kvstore/wal/AtomicLogBuffer.h](baseline/kvstore/wal/AtomicLogBuffer.h) | 固定提交 de9b3ed 的未改动源码，供原算法组件对照 |
| [task/wal-investigation/instrumentation-notes.txt](instrumentation-notes.txt) | 观测开关、计数口径与开销边界 |
| [task/wal-investigation/observation.patch](observation.patch) | 124 行产品观测补丁；已含补丁的源码不要重复应用 |

## 复现 采样 解析与组件工具源码

| 仓库相对路径 | 用途和注意点 |
| --- | --- |
| [task/wal-investigation/analyze_results.py](analyze_results.py) | 重算汇总和绘图，会写同目录输出，先复制证据 |
| [task/wal-investigation/analyze_runtime_rocks.py](analyze_runtime_rocks.py) | 复算有效窗口、RSS 与 FLUSH，会写同目录输出 |
| [task/wal-investigation/atomic-buffer-probe.cpp](atomic-buffer-probe.cpp) | 原始空日志缓存组件源码，目标机器重建 |
| [task/wal-investigation/cluster.py](cluster.py) | 历史完整集群编排，会启停服务/建空间/改心跳/FLUSH，先适配 |
| [task/wal-investigation/parse_wal.py](parse_wal.py) | 逐条读取 WAL 并验证结构，输出使用新位置 |
| [task/wal-investigation/production-tuning-evidence/run-capacity-probe.sh](production-tuning-evidence/run-capacity-probe.sh) | 工具源码；执行前按迁移说明核对副作用与路径 |
| [task/wal-investigation/rocks-commit-probe.cpp](rocks-commit-probe.cpp) | RocksDB 系统位点覆盖写组件，会创建新临时 DB |
| [task/wal-investigation/run-atomic-buffer-probe.sh](run-atomic-buffer-probe.sh) | 工具源码；执行前按迁移说明核对副作用与路径 |
| [task/wal-investigation/sample_runtime.py](sample_runtime.py) | 只读采样进程、RocksDB HTTP 与磁盘 WAL，需显式目标 |
| [task/wal-investigation/test_parse_wal.py](test_parse_wal.py) | 10 项合成边界检查，复制到新目录后运行 |
| [task/wal-investigation/write-idle-comparison-20261011/payload-probe.cpp](write-idle-comparison-20261011/payload-probe.cpp) | 原始缓存 payload 和停写转空日志组件源码 |
| [task/wal-investigation/write-idle-comparison-20261011/run.sh](write-idle-comparison-20261011/run.sh) | 工具源码；执行前按迁移说明核对副作用与路径 |

## 迁移工具 审计与重新构建记录

| 仓库相对路径 | 用途和注意点 |
| --- | --- |
| [task/wal-investigation/migration/CODE-CHANGE-AUDIT.md](migration/CODE-CHANGE-AUDIT.md) | 配套说明、环境记录或分析材料 |
| [task/wal-investigation/migration/DELIVERY-MANIFEST.json](migration/DELIVERY-MANIFEST.json) | 本次文件、类型、字节和摘要；自身摘要由 Git 锚定 |
| [task/wal-investigation/migration/DELIVERY-REVIEW.md](migration/DELIVERY-REVIEW.md) | 配套说明、环境记录或分析材料 |
| [task/wal-investigation/migration/INVENTORY-AUDIT.md](migration/INVENTORY-AUDIT.md) | 配套说明、环境记录或分析材料 |
| [task/wal-investigation/migration/INVENTORY-FOLLOWUP.md](migration/INVENTORY-FOLLOWUP.md) | 配套说明、环境记录或分析材料 |
| [task/wal-investigation/migration/PUBLISH-SHA256SUMS](migration/PUBLISH-SHA256SUMS) | 本次普通文件摘要；链接另由 JSON 核验 |
| [task/wal-investigation/migration/SCRIPT-PORTABILITY-AUDIT.md](migration/SCRIPT-PORTABILITY-AUDIT.md) | 配套说明、环境记录或分析材料 |
| [task/wal-investigation/migration/build_artifact_index.py](migration/build_artifact_index.py) | 按发布清单重新生成本索引；随后刷新摘要 |
| [task/wal-investigation/migration/build_delivery.py](migration/build_delivery.py) | 生成明确发布清单与摘要，可选生成覆盖包 |
| [task/wal-investigation/migration/code-change-verification.json](migration/code-change-verification.json) | 结构化证据或阶段性审查结果，按其时间和范围解释 |
| [task/wal-investigation/migration/code-change-verification.log](migration/code-change-verification.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/migration/component-full-20261016/after-stop.csv](migration/component-full-20261016/after-stop.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/migration/component-full-20261016/after-stop.meta](migration/component-full-20261016/after-stop.meta) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/migration/component-full-20261016/atomic-build.log](migration/component-full-20261016/atomic-build.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/migration/component-full-20261016/capacity-1048576.csv](migration/component-full-20261016/capacity-1048576.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/migration/component-full-20261016/capacity-1048576.meta](migration/component-full-20261016/capacity-1048576.meta) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/migration/component-full-20261016/capacity-2097152.csv](migration/component-full-20261016/capacity-2097152.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/migration/component-full-20261016/capacity-2097152.meta](migration/component-full-20261016/capacity-2097152.meta) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/migration/component-full-20261016/capacity-262144.csv](migration/component-full-20261016/capacity-262144.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/migration/component-full-20261016/capacity-262144.meta](migration/component-full-20261016/capacity-262144.meta) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/migration/component-full-20261016/capacity-4194304.csv](migration/component-full-20261016/capacity-4194304.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/migration/component-full-20261016/capacity-4194304.meta](migration/component-full-20261016/capacity-4194304.meta) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/migration/component-full-20261016/capacity-8388608.csv](migration/component-full-20261016/capacity-8388608.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/migration/component-full-20261016/capacity-8388608.meta](migration/component-full-20261016/capacity-8388608.meta) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/migration/component-full-20261016/compiler-version.log](migration/component-full-20261016/compiler-version.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/migration/component-full-20261016/empty.csv](migration/component-full-20261016/empty.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/migration/component-full-20261016/empty.meta](migration/component-full-20261016/empty.meta) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/migration/component-full-20261016/payload-build.log](migration/component-full-20261016/payload-build.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/migration/component-full-20261016/payload1024.csv](migration/component-full-20261016/payload1024.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/migration/component-full-20261016/payload1024.meta](migration/component-full-20261016/payload1024.meta) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/migration/component-full-20261016/payload256.csv](migration/component-full-20261016/payload256.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/migration/component-full-20261016/payload256.meta](migration/component-full-20261016/payload256.meta) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/migration/component-full-20261016/run.json](migration/component-full-20261016/run.json) | 本轮组件重编译和运行命令、源码/二进制摘要及结果 |
| [task/wal-investigation/migration/component-smoke-20261016/atomic-build.log](migration/component-smoke-20261016/atomic-build.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/migration/component-smoke-20261016/capacity-smoke.csv](migration/component-smoke-20261016/capacity-smoke.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/migration/component-smoke-20261016/capacity-smoke.meta](migration/component-smoke-20261016/capacity-smoke.meta) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/migration/component-smoke-20261016/compiler-version.log](migration/component-smoke-20261016/compiler-version.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/migration/component-smoke-20261016/payload-build.log](migration/component-smoke-20261016/payload-build.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/migration/component-smoke-20261016/payload-smoke.csv](migration/component-smoke-20261016/payload-smoke.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/migration/component-smoke-20261016/payload-smoke.meta](migration/component-smoke-20261016/payload-smoke.meta) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/migration/component-smoke-20261016/run.json](migration/component-smoke-20261016/run.json) | 本轮组件重编译和运行命令、源码/二进制摘要及结果 |
| [task/wal-investigation/migration/delivery-review.json](migration/delivery-review.json) | 结构化证据或阶段性审查结果，按其时间和范围解释 |
| [task/wal-investigation/migration/delivery-verification.json](migration/delivery-verification.json) | 发布准备阶段的本地验证结果及未验边界 |
| [task/wal-investigation/migration/inventory-audit-run.json](migration/inventory-audit-run.json) | 结构化证据或阶段性审查结果，按其时间和范围解释 |
| [task/wal-investigation/migration/inventory-audit.json](migration/inventory-audit.json) | 结构化证据或阶段性审查结果，按其时间和范围解释 |
| [task/wal-investigation/migration/inventory-followup.json](migration/inventory-followup.json) | 结构化证据或阶段性审查结果，按其时间和范围解释 |
| [task/wal-investigation/migration/local-only-files.txt](migration/local-only-files.txt) | 初审排除项快照，13 个必要链接后续已补回 |
| [task/wal-investigation/migration/migration-start.json](migration/migration-start.json) | 结构化证据或阶段性审查结果，按其时间和范围解释 |
| [task/wal-investigation/migration/portability-validation.log](migration/portability-validation.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/migration/preflight-complete-20261016/preflight.json](migration/preflight-complete-20261016/preflight.json) | 结构化证据或阶段性审查结果，按其时间和范围解释 |
| [task/wal-investigation/migration/preflight-complete-20261016/preflight.log](migration/preflight-complete-20261016/preflight.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/migration/preflight.py](migration/preflight.py) | 现有材料静态预检，区别于未实现的策略身份预检 |
| [task/wal-investigation/migration/publication-validation.json](migration/publication-validation.json) | 发布前探针规范整理、当前源码重编译与完整运行输出 |
| [task/wal-investigation/migration/publish-files.txt](migration/publish-files.txt) | 逐行列出本次交付的仓库相对路径 |
| [task/wal-investigation/migration/recommended-files.txt](migration/recommended-files.txt) | 初审 299 项候选历史快照，不是最终清单 |
| [task/wal-investigation/migration/run_components.py](migration/run_components.py) | 参数化串行重建 Atomic/payload 探针，支持 smoke/full |
| [task/wal-investigation/migration/source-link-navigation.json](migration/source-link-navigation.json) | 本轮将历史源码引用转换为 GitHub 导航的记录 |
| [task/wal-investigation/migration/verify_delivery.py](migration/verify_delivery.py) | 只读核对文件摘要、类型和安全相对链接 |

## 历史配置与 nGQL 模板

| 仓库相对路径 | 用途和注意点 |
| --- | --- |
| [task/wal-investigation/configs/nebula-graphd.conf](configs/nebula-graphd.conf) | 原实验模板，路径/端口/账号/空间名需按目标环境适配 |
| [task/wal-investigation/configs/nebula-metad.conf](configs/nebula-metad.conf) | 原实验模板，路径/端口/账号/空间名需按目标环境适配 |
| [task/wal-investigation/configs/nebula-storaged-s1.conf](configs/nebula-storaged-s1.conf) | 原实验模板，路径/端口/账号/空间名需按目标环境适配 |
| [task/wal-investigation/configs/nebula-storaged-s2.conf](configs/nebula-storaged-s2.conf) | 原实验模板，路径/端口/账号/空间名需按目标环境适配 |
| [task/wal-investigation/configs/nebula-storaged-s3.conf](configs/nebula-storaged-s3.conf) | 原实验模板，路径/端口/账号/空间名需按目标环境适配 |
| [task/wal-investigation/create-spaces.ngql](create-spaces.ngql) | 原实验模板，路径/端口/账号/空间名需按目标环境适配 |
| [task/wal-investigation/flush.ngql](flush.ngql) | 原实验模板，路径/端口/账号/空间名需按目标环境适配 |
| [task/wal-investigation/inspect.ngql](inspect.ngql) | 原实验模板，路径/端口/账号/空间名需按目标环境适配 |
| [task/wal-investigation/jobs.ngql](jobs.ngql) | 原实验模板，路径/端口/账号/空间名需按目标环境适配 |
| [task/wal-investigation/spaces.txt](spaces.txt) | 配套说明、环境记录或分析材料 |

## 原始服务分阶段采样

| 仓库相对路径 | 用途和注意点 |
| --- | --- |
| [task/wal-investigation/phase-00-no-spaces.log](phase-00-no-spaces.log) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-00-no-spaces/manifest.json](phase-00-no-spaces/manifest.json) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-00-no-spaces/raw-samples.jsonl](phase-00-no-spaces/raw-samples.jsonl) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-00-no-spaces/s1.csv](phase-00-no-spaces/s1.csv) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-00-no-spaces/s2.csv](phase-00-no-spaces/s2.csv) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-00-no-spaces/s3.csv](phase-00-no-spaces/s3.csv) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-01-warmup.log](phase-01-warmup.log) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-01-warmup/manifest.json](phase-01-warmup/manifest.json) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-01-warmup/raw-samples.jsonl](phase-01-warmup/raw-samples.jsonl) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-01-warmup/s1.csv](phase-01-warmup/s1.csv) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-01-warmup/s2.csv](phase-01-warmup/s2.csv) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-01-warmup/s3.csv](phase-01-warmup/s3.csv) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-02-default30.log](phase-02-default30.log) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-02-default30/manifest.json](phase-02-default30/manifest.json) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-02-default30/raw-samples.jsonl](phase-02-default30/raw-samples.jsonl) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-02-default30/s1.csv](phase-02-default30/s1.csv) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-02-default30/s2.csv](phase-02-default30/s2.csv) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-02-default30/s3.csv](phase-02-default30/s3.csv) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-03-default30.log](phase-03-default30.log) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-03-default30/manifest.json](phase-03-default30/manifest.json) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-03-default30/raw-samples.jsonl](phase-03-default30/raw-samples.jsonl) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-03-default30/s1.csv](phase-03-default30/s1.csv) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-03-default30/s2.csv](phase-03-default30/s2.csv) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-03-default30/s3.csv](phase-03-default30/s3.csv) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-04-transition1.log](phase-04-transition1.log) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-04-transition1/manifest.json](phase-04-transition1/manifest.json) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-04-transition1/raw-samples.jsonl](phase-04-transition1/raw-samples.jsonl) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-04-transition1/s1.csv](phase-04-transition1/s1.csv) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-04-transition1/s2.csv](phase-04-transition1/s2.csv) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-04-transition1/s3.csv](phase-04-transition1/s3.csv) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-05-accelerated1.log](phase-05-accelerated1.log) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-05-accelerated1/manifest.json](phase-05-accelerated1/manifest.json) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-05-accelerated1/raw-samples.jsonl](phase-05-accelerated1/raw-samples.jsonl) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-05-accelerated1/s1.csv](phase-05-accelerated1/s1.csv) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-05-accelerated1/s2.csv](phase-05-accelerated1/s2.csv) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-05-accelerated1/s3.csv](phase-05-accelerated1/s3.csv) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-06-transition30.log](phase-06-transition30.log) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-06-transition30/manifest.json](phase-06-transition30/manifest.json) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-06-transition30/raw-samples.jsonl](phase-06-transition30/raw-samples.jsonl) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-06-transition30/s1.csv](phase-06-transition30/s1.csv) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-06-transition30/s2.csv](phase-06-transition30/s2.csv) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-06-transition30/s3.csv](phase-06-transition30/s3.csv) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-07-restored30.log](phase-07-restored30.log) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-07-restored30/manifest.json](phase-07-restored30/manifest.json) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-07-restored30/raw-samples.jsonl](phase-07-restored30/raw-samples.jsonl) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-07-restored30/s1.csv](phase-07-restored30/s1.csv) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-07-restored30/s2.csv](phase-07-restored30/s2.csv) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-07-restored30/s3.csv](phase-07-restored30/s3.csv) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-08-beforeflush.log](phase-08-beforeflush.log) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-08-beforeflush/manifest.json](phase-08-beforeflush/manifest.json) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-08-beforeflush/raw-samples.jsonl](phase-08-beforeflush/raw-samples.jsonl) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-08-beforeflush/s1.csv](phase-08-beforeflush/s1.csv) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-08-beforeflush/s2.csv](phase-08-beforeflush/s2.csv) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-08-beforeflush/s3.csv](phase-08-beforeflush/s3.csv) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-09-afterflush.log](phase-09-afterflush.log) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-09-afterflush/manifest.json](phase-09-afterflush/manifest.json) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-09-afterflush/raw-samples.jsonl](phase-09-afterflush/raw-samples.jsonl) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-09-afterflush/s1.csv](phase-09-afterflush/s1.csv) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-09-afterflush/s2.csv](phase-09-afterflush/s2.csv) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |
| [task/wal-investigation/phase-09-afterflush/s3.csv](phase-09-afterflush/s3.csv) | 历史阶段采样/命令记录；保留过渡、缺口和失败样本 |

## 原始服务日志与必要别名

| 仓库相对路径 | 用途和注意点 |
| --- | --- |
| [task/wal-investigation/service-logs/graphd/graphd-stderr.log](service-logs/graphd/graphd-stderr.log) | 历史服务原始输出，保留真实字节 |
| [task/wal-investigation/service-logs/graphd/graphd-stdout.log](service-logs/graphd/graphd-stdout.log) | 历史服务原始输出，保留真实字节 |
| [task/wal-investigation/service-logs/graphd/nebula-graphd.INFO](service-logs/graphd/nebula-graphd.INFO) | 相对日志别名，目标随包交付，统计程序依赖 |
| [task/wal-investigation/service-logs/graphd/nebula-graphd.sch-PC.sch.log.INFO.20261010-065853.77494](service-logs/graphd/nebula-graphd.sch-PC.sch.log.INFO.20261010-065853.77494) | 历史服务原始输出，保留真实字节 |
| [task/wal-investigation/service-logs/metad/metad-stderr.log](service-logs/metad/metad-stderr.log) | 历史服务原始输出，保留真实字节 |
| [task/wal-investigation/service-logs/metad/metad-stdout.log](service-logs/metad/metad-stdout.log) | 历史服务原始输出，保留真实字节 |
| [task/wal-investigation/service-logs/metad/nebula-metad.ERROR](service-logs/metad/nebula-metad.ERROR) | 相对日志别名，目标随包交付，统计程序依赖 |
| [task/wal-investigation/service-logs/metad/nebula-metad.INFO](service-logs/metad/nebula-metad.INFO) | 相对日志别名，目标随包交付，统计程序依赖 |
| [task/wal-investigation/service-logs/metad/nebula-metad.WARNING](service-logs/metad/nebula-metad.WARNING) | 相对日志别名，目标随包交付，统计程序依赖 |
| [task/wal-investigation/service-logs/metad/nebula-metad.sch-PC.sch.log.ERROR.20261010-065850.77094](service-logs/metad/nebula-metad.sch-PC.sch.log.ERROR.20261010-065850.77094) | 历史服务原始输出，保留真实字节 |
| [task/wal-investigation/service-logs/metad/nebula-metad.sch-PC.sch.log.INFO.20261010-065850.77094](service-logs/metad/nebula-metad.sch-PC.sch.log.INFO.20261010-065850.77094) | 历史服务原始输出，保留真实字节 |
| [task/wal-investigation/service-logs/metad/nebula-metad.sch-PC.sch.log.WARNING.20261010-065850.77094](service-logs/metad/nebula-metad.sch-PC.sch.log.WARNING.20261010-065850.77094) | 历史服务原始输出，保留真实字节 |
| [task/wal-investigation/service-logs/s1/nebula-storaged.ERROR](service-logs/s1/nebula-storaged.ERROR) | 相对日志别名，目标随包交付，统计程序依赖 |
| [task/wal-investigation/service-logs/s1/nebula-storaged.INFO](service-logs/s1/nebula-storaged.INFO) | 相对日志别名，目标随包交付，统计程序依赖 |
| [task/wal-investigation/service-logs/s1/nebula-storaged.WARNING](service-logs/s1/nebula-storaged.WARNING) | 相对日志别名，目标随包交付，统计程序依赖 |
| [task/wal-investigation/service-logs/s1/nebula-storaged.sch-PC.sch.log.ERROR.20261010-065856.77312](service-logs/s1/nebula-storaged.sch-PC.sch.log.ERROR.20261010-065856.77312) | 历史服务原始输出，保留真实字节 |
| [task/wal-investigation/service-logs/s1/nebula-storaged.sch-PC.sch.log.INFO.20261010-065853.77312](service-logs/s1/nebula-storaged.sch-PC.sch.log.INFO.20261010-065853.77312) | 历史服务原始输出，保留真实字节 |
| [task/wal-investigation/service-logs/s1/nebula-storaged.sch-PC.sch.log.WARNING.20261010-065853.77312](service-logs/s1/nebula-storaged.sch-PC.sch.log.WARNING.20261010-065853.77312) | 历史服务原始输出，保留真实字节 |
| [task/wal-investigation/service-logs/s1/storaged-stderr.log](service-logs/s1/storaged-stderr.log) | 历史服务原始输出，保留真实字节 |
| [task/wal-investigation/service-logs/s1/storaged-stdout.log](service-logs/s1/storaged-stdout.log) | 历史服务原始输出，保留真实字节 |
| [task/wal-investigation/service-logs/s2/nebula-storaged.ERROR](service-logs/s2/nebula-storaged.ERROR) | 相对日志别名，目标随包交付，统计程序依赖 |
| [task/wal-investigation/service-logs/s2/nebula-storaged.INFO](service-logs/s2/nebula-storaged.INFO) | 相对日志别名，目标随包交付，统计程序依赖 |
| [task/wal-investigation/service-logs/s2/nebula-storaged.WARNING](service-logs/s2/nebula-storaged.WARNING) | 相对日志别名，目标随包交付，统计程序依赖 |
| [task/wal-investigation/service-logs/s2/nebula-storaged.sch-PC.sch.log.ERROR.20261010-065856.77368](service-logs/s2/nebula-storaged.sch-PC.sch.log.ERROR.20261010-065856.77368) | 历史服务原始输出，保留真实字节 |
| [task/wal-investigation/service-logs/s2/nebula-storaged.sch-PC.sch.log.INFO.20261010-065853.77368](service-logs/s2/nebula-storaged.sch-PC.sch.log.INFO.20261010-065853.77368) | 历史服务原始输出，保留真实字节 |
| [task/wal-investigation/service-logs/s2/nebula-storaged.sch-PC.sch.log.WARNING.20261010-065853.77368](service-logs/s2/nebula-storaged.sch-PC.sch.log.WARNING.20261010-065853.77368) | 历史服务原始输出，保留真实字节 |
| [task/wal-investigation/service-logs/s2/storaged-stderr.log](service-logs/s2/storaged-stderr.log) | 历史服务原始输出，保留真实字节 |
| [task/wal-investigation/service-logs/s2/storaged-stdout.log](service-logs/s2/storaged-stdout.log) | 历史服务原始输出，保留真实字节 |
| [task/wal-investigation/service-logs/s3/nebula-storaged.ERROR](service-logs/s3/nebula-storaged.ERROR) | 相对日志别名，目标随包交付，统计程序依赖 |
| [task/wal-investigation/service-logs/s3/nebula-storaged.INFO](service-logs/s3/nebula-storaged.INFO) | 相对日志别名，目标随包交付，统计程序依赖 |
| [task/wal-investigation/service-logs/s3/nebula-storaged.WARNING](service-logs/s3/nebula-storaged.WARNING) | 相对日志别名，目标随包交付，统计程序依赖 |
| [task/wal-investigation/service-logs/s3/nebula-storaged.sch-PC.sch.log.ERROR.20261010-065856.77431](service-logs/s3/nebula-storaged.sch-PC.sch.log.ERROR.20261010-065856.77431) | 历史服务原始输出，保留真实字节 |
| [task/wal-investigation/service-logs/s3/nebula-storaged.sch-PC.sch.log.INFO.20261010-065853.77431](service-logs/s3/nebula-storaged.sch-PC.sch.log.INFO.20261010-065853.77431) | 历史服务原始输出，保留真实字节 |
| [task/wal-investigation/service-logs/s3/nebula-storaged.sch-PC.sch.log.WARNING.20261010-065853.77431](service-logs/s3/nebula-storaged.sch-PC.sch.log.WARNING.20261010-065853.77431) | 历史服务原始输出，保留真实字节 |
| [task/wal-investigation/service-logs/s3/storaged-stderr.log](service-logs/s3/storaged-stderr.log) | 历史服务原始输出，保留真实字节 |
| [task/wal-investigation/service-logs/s3/storaged-stdout.log](service-logs/s3/storaged-stdout.log) | 历史服务原始输出，保留真实字节 |

## 原始 WAL 与解析观测

| 仓库相对路径 | 用途和注意点 |
| --- | --- |
| [task/wal-investigation/wal-accelerated-end.csv](wal-accelerated-end.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/wal-accelerated-end.json](wal-accelerated-end.json) | 结构化证据或阶段性审查结果，按其时间和范围解释 |
| [task/wal-investigation/wal-accelerated-end.log](wal-accelerated-end.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/wal-accelerated-start.csv](wal-accelerated-start.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/wal-accelerated-start.json](wal-accelerated-start.json) | 结构化证据或阶段性审查结果，按其时间和范围解释 |
| [task/wal-investigation/wal-accelerated-start.log](wal-accelerated-start.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/wal-archive-verification.json](wal-archive-verification.json) | 结构化证据或阶段性审查结果，按其时间和范围解释 |
| [task/wal-investigation/wal-archive.log](wal-archive.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/wal-before-acceleration.csv](wal-before-acceleration.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/wal-before-acceleration.json](wal-before-acceleration.json) | 结构化证据或阶段性审查结果，按其时间和范围解释 |
| [task/wal-investigation/wal-before-acceleration.log](wal-before-acceleration.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/wal-evidence-files.txt](wal-evidence-files.txt) | 配套说明、环境记录或分析材料 |
| [task/wal-investigation/wal-evidence.tar.gz](wal-evidence.tar.gz) | 停机 WAL 原始归档：3000 文件、6730574 条空 payload 记录 |
| [task/wal-investigation/wal-full.csv](wal-full.csv) | 原始 WAL 按文件/分片/实例聚合表 |
| [task/wal-investigation/wal-full.json](wal-full.json) | 逐条解析原始 WAL 的完整结构与计数报告 |
| [task/wal-investigation/wal-full.log](wal-full.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/wal-observe-s1.csv](wal-observe-s1.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/wal-observe-s2.csv](wal-observe-s2.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/wal-observe-s3.csv](wal-observe-s3.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/wal-parser-notes.txt](wal-parser-notes.txt) | 配套说明、环境记录或分析材料 |
| [task/wal-investigation/wal-parser-selftest.log](wal-parser-selftest.log) | 对应步骤的实际运行/编译记录，非可执行入口 |

## 容量对照与参数方案证据

| 仓库相对路径 | 用途和注意点 |
| --- | --- |
| [task/wal-investigation/production-tuning-evidence/README.md](production-tuning-evidence/README.md) | 研究总览和阅读入口 |
| [task/wal-investigation/production-tuning-evidence/SHA256SUMS](production-tuning-evidence/SHA256SUMS) | 所在实验批次的历史摘要，不能替代本次发布清单 |
| [task/wal-investigation/production-tuning-evidence/capacity-1048576.csv](production-tuning-evidence/capacity-1048576.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/production-tuning-evidence/capacity-1048576.meta](production-tuning-evidence/capacity-1048576.meta) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/production-tuning-evidence/capacity-2097152.csv](production-tuning-evidence/capacity-2097152.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/production-tuning-evidence/capacity-2097152.meta](production-tuning-evidence/capacity-2097152.meta) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/production-tuning-evidence/capacity-262144.csv](production-tuning-evidence/capacity-262144.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/production-tuning-evidence/capacity-262144.meta](production-tuning-evidence/capacity-262144.meta) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/production-tuning-evidence/capacity-4194304.csv](production-tuning-evidence/capacity-4194304.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/production-tuning-evidence/capacity-4194304.meta](production-tuning-evidence/capacity-4194304.meta) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/production-tuning-evidence/capacity-8388608.csv](production-tuning-evidence/capacity-8388608.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/production-tuning-evidence/capacity-8388608.meta](production-tuning-evidence/capacity-8388608.meta) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/production-tuning-evidence/capacity-run.log](production-tuning-evidence/capacity-run.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/production-tuning-evidence/memory-limits.txt](production-tuning-evidence/memory-limits.txt) | 配套说明、环境记录或分析材料 |
| [task/wal-investigation/production-tuning-evidence/rocks-scope.txt](production-tuning-evidence/rocks-scope.txt) | 配套说明、环境记录或分析材料 |
| [task/wal-investigation/production-tuning-evidence/space-parameter-scope-review.txt](production-tuning-evidence/space-parameter-scope-review.txt) | 配套说明、环境记录或分析材料 |
| [task/wal-investigation/production-tuning-evidence/space-policy-design-review.txt](production-tuning-evidence/space-policy-design-review.txt) | 配套说明、环境记录或分析材料 |
| [task/wal-investigation/production-tuning-evidence/space-policy-prior-manifest-check.json](production-tuning-evidence/space-policy-prior-manifest-check.json) | 结构化证据或阶段性审查结果，按其时间和范围解释 |
| [task/wal-investigation/production-tuning-evidence/space-policy-review-start.json](production-tuning-evidence/space-policy-review-start.json) | 结构化证据或阶段性审查结果，按其时间和范围解释 |
| [task/wal-investigation/production-tuning-evidence/space-policy-verification.json](production-tuning-evidence/space-policy-verification.json) | 结构化证据或阶段性审查结果，按其时间和范围解释 |
| [task/wal-investigation/production-tuning-evidence/space-wal-design-review.txt](production-tuning-evidence/space-wal-design-review.txt) | 配套说明、环境记录或分析材料 |
| [task/wal-investigation/production-tuning-evidence/verification.json](production-tuning-evidence/verification.json) | 结构化证据或阶段性审查结果，按其时间和范围解释 |
| [task/wal-investigation/production-tuning-evidence/wal-review.txt](production-tuning-evidence/wal-review.txt) | 配套说明、环境记录或分析材料 |

## 空闲与 payload 对照证据

| 仓库相对路径 | 用途和注意点 |
| --- | --- |
| [task/wal-investigation/write-idle-comparison-20261011/README.md](write-idle-comparison-20261011/README.md) | 研究总览和阅读入口 |
| [task/wal-investigation/write-idle-comparison-20261011/SHA256SUMS](write-idle-comparison-20261011/SHA256SUMS) | 所在实验批次的历史摘要，不能替代本次发布清单 |
| [task/wal-investigation/write-idle-comparison-20261011/after-stop.csv](write-idle-comparison-20261011/after-stop.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/write-idle-comparison-20261011/after-stop.meta](write-idle-comparison-20261011/after-stop.meta) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/write-idle-comparison-20261011/build.log](write-idle-comparison-20261011/build.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/write-idle-comparison-20261011/empty.csv](write-idle-comparison-20261011/empty.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/write-idle-comparison-20261011/empty.meta](write-idle-comparison-20261011/empty.meta) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/write-idle-comparison-20261011/environment.txt](write-idle-comparison-20261011/environment.txt) | 配套说明、环境记录或分析材料 |
| [task/wal-investigation/write-idle-comparison-20261011/payload1024.csv](write-idle-comparison-20261011/payload1024.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/write-idle-comparison-20261011/payload1024.meta](write-idle-comparison-20261011/payload1024.meta) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/write-idle-comparison-20261011/payload256.csv](write-idle-comparison-20261011/payload256.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/write-idle-comparison-20261011/payload256.meta](write-idle-comparison-20261011/payload256.meta) | 组件或服务观测原始样本/结构与分配器元数据 |

## 解析器合成测试固件

| 仓库相对路径 | 用途和注意点 |
| --- | --- |
| [task/wal-investigation/wal-parser-selftest/append-during-read/nebula/4/wal/2/1.wal](wal-parser-selftest/append-during-read/nebula/4/wal/2/1.wal) | 配套说明、环境记录或分析材料 |
| [task/wal-investigation/wal-parser-selftest/gaps/nebula/4/wal/2/1.wal](wal-parser-selftest/gaps/nebula/4/wal/2/1.wal) | 配套说明、环境记录或分析材料 |
| [task/wal-investigation/wal-parser-selftest/gaps/nebula/4/wal/2/8.wal](wal-parser-selftest/gaps/nebula/4/wal/2/8.wal) | 配套说明、环境记录或分析材料 |
| [task/wal-investigation/wal-parser-selftest/good/nebula/4/wal/2/1.wal](wal-parser-selftest/good/nebula/4/wal/2/1.wal) | 配套说明、环境记录或分析材料 |
| [task/wal-investigation/wal-parser-selftest/good/nebula/4/wal/2/3.wal](wal-parser-selftest/good/nebula/4/wal/2/3.wal) | 配套说明、环境记录或分析材料 |
| [task/wal-investigation/wal-parser-selftest/negative/nebula/4/wal/2/1.wal](wal-parser-selftest/negative/nebula/4/wal/2/1.wal) | 配套说明、环境记录或分析材料 |
| [task/wal-investigation/wal-parser-selftest/report.csv](wal-parser-selftest/report.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/wal-parser-selftest/report.json](wal-parser-selftest/report.json) | 结构化证据或阶段性审查结果，按其时间和范围解释 |
| [task/wal-investigation/wal-parser-selftest/short-header/nebula/4/wal/2/1.wal](wal-parser-selftest/short-header/nebula/4/wal/2/1.wal) | 配套说明、环境记录或分析材料 |
| [task/wal-investigation/wal-parser-selftest/short-record/nebula/4/wal/2/1.wal](wal-parser-selftest/short-record/nebula/4/wal/2/1.wal) | 配套说明、环境记录或分析材料 |
| [task/wal-investigation/wal-parser-selftest/shrink-during-read/nebula/4/wal/2/1.wal](wal-parser-selftest/shrink-during-read/nebula/4/wal/2/1.wal) | 配套说明、环境记录或分析材料 |
| [task/wal-investigation/wal-parser-selftest/wrong-footer/nebula/4/wal/2/1.wal](wal-parser-selftest/wrong-footer/nebula/4/wal/2/1.wal) | 配套说明、环境记录或分析材料 |

## 需求设计独立审查与出版记录

| 仓库相对路径 | 用途和注意点 |
| --- | --- |
| [task/wal-investigation/design-evidence/README.md](design-evidence/README.md) | 研究总览和阅读入口 |
| [task/wal-investigation/design-evidence/SHA256SUMS](design-evidence/SHA256SUMS) | 所在实验批次的历史摘要，不能替代本次发布清单 |
| [task/wal-investigation/design-evidence/build_design_docx.py](design-evidence/build_design_docx.py) | 工具源码；执行前按迁移说明核对副作用与路径 |
| [task/wal-investigation/design-evidence/contract-audit.md](design-evidence/contract-audit.md) | 配套说明、环境记录或分析材料 |
| [task/wal-investigation/design-evidence/docx-build.json](design-evidence/docx-build.json) | 结构化证据或阶段性审查结果，按其时间和范围解释 |
| [task/wal-investigation/design-evidence/docx-build.log](design-evidence/docx-build.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/design-evidence/link-verification.json](design-evidence/link-verification.json) | 结构化证据或阶段性审查结果，按其时间和范围解释 |
| [task/wal-investigation/design-evidence/prior-evidence-check.log](design-evidence/prior-evidence-check.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/design-evidence/render_with_host_libreoffice.py](design-evidence/render_with_host_libreoffice.py) | 工具源码；执行前按迁移说明核对副作用与路径 |
| [task/wal-investigation/design-evidence/reproduction-audit-run.log](design-evidence/reproduction-audit-run.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/design-evidence/reproduction-audit-verification.json](design-evidence/reproduction-audit-verification.json) | 结构化证据或阶段性审查结果，按其时间和范围解释 |
| [task/wal-investigation/design-evidence/reproduction-audit.md](design-evidence/reproduction-audit.md) | 配套说明、环境记录或分析材料 |
| [task/wal-investigation/design-evidence/verification.json](design-evidence/verification.json) | 结构化证据或阶段性审查结果，按其时间和范围解释 |
| [task/wal-investigation/design-evidence/word-render-delivery.log](design-evidence/word-render-delivery.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/design-evidence/word-render-host-final.log](design-evidence/word-render-host-final.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/design-evidence/word-render-host-v3.log](design-evidence/word-render-host-v3.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/design-evidence/word-render-host-v4.log](design-evidence/word-render-host-v4.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/design-evidence/word-render-host.log](design-evidence/word-render-host.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/design-evidence/word-render.log](design-evidence/word-render.log) | 对应步骤的实际运行/编译记录，非可执行入口 |

## 实验构建 环境 时间线与汇总

| 仓库相对路径 | 用途和注意点 |
| --- | --- |
| [task/wal-investigation/SHA256SUMS](SHA256SUMS) | 所在实验批次的历史摘要，不能替代本次发布清单 |
| [task/wal-investigation/atomic-buffer-probe-build.log](atomic-buffer-probe-build.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/atomic-buffer-probe-environment.txt](atomic-buffer-probe-environment.txt) | 配套说明、环境记录或分析材料 |
| [task/wal-investigation/atomic-buffer-probe-validation.txt](atomic-buffer-probe-validation.txt) | 配套说明、环境记录或分析材料 |
| [task/wal-investigation/atomic-default-1.csv](atomic-default-1.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/atomic-default-1.meta](atomic-default-1.meta) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/atomic-default-100.csv](atomic-default-100.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/atomic-default-100.meta](atomic-default-100.meta) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/atomic-default-1000.csv](atomic-default-1000.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/atomic-default-1000.meta](atomic-default-1000.meta) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/atomic-default-threshold.csv](atomic-default-threshold.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/atomic-default-threshold.meta](atomic-default-threshold.meta) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/atomic-jemalloc-default-1.csv](atomic-jemalloc-default-1.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/atomic-jemalloc-default-1.meta](atomic-jemalloc-default-1.meta) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/atomic-jemalloc-default-100.csv](atomic-jemalloc-default-100.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/atomic-jemalloc-default-100.meta](atomic-jemalloc-default-100.meta) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/atomic-jemalloc-default-1000.csv](atomic-jemalloc-default-1000.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/atomic-jemalloc-default-1000.meta](atomic-jemalloc-default-1000.meta) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/atomic-jemalloc-default-threshold.csv](atomic-jemalloc-default-threshold.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/atomic-jemalloc-default-threshold.meta](atomic-jemalloc-default-threshold.meta) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/atomic-jemalloc-small-each.csv](atomic-jemalloc-small-each.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/atomic-jemalloc-small-each.meta](atomic-jemalloc-small-each.meta) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/atomic-jemalloc-small-held.csv](atomic-jemalloc-small-held.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/atomic-jemalloc-small-held.meta](atomic-jemalloc-small-held.meta) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/atomic-jemalloc-small-none.csv](atomic-jemalloc-small-none.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/atomic-jemalloc-small-none.meta](atomic-jemalloc-small-none.meta) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/atomic-jemalloc-threshold.csv](atomic-jemalloc-threshold.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/atomic-jemalloc-threshold.meta](atomic-jemalloc-threshold.meta) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/atomic-small-each.csv](atomic-small-each.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/atomic-small-each.meta](atomic-small-each.meta) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/atomic-small-held.csv](atomic-small-held.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/atomic-small-held.meta](atomic-small-held.meta) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/atomic-small-none.csv](atomic-small-none.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/atomic-small-none.meta](atomic-small-none.meta) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/build-j5.log](build-j5.log) | 遵守最大 make -j5 约束后的工程编译记录 |
| [task/wal-investigation/build.log](build.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/cli-resume-update-20261011.json](cli-resume-update-20261011.json) | 结构化证据或阶段性审查结果，按其时间和范围解释 |
| [task/wal-investigation/cluster-inspect.log](cluster-inspect.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/cluster-review.txt](cluster-review.txt) | 配套说明、环境记录或分析材料 |
| [task/wal-investigation/code-analysis-independent.txt](code-analysis-independent.txt) | 配套说明、环境记录或分析材料 |
| [task/wal-investigation/code-analysis-wal.txt](code-analysis-wal.txt) | 配套说明、环境记录或分析材料 |
| [task/wal-investigation/configure.log](configure.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/create-spaces.log](create-spaces.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/document-verification.json](document-verification.json) | 结构化证据或阶段性审查结果，按其时间和范围解释 |
| [task/wal-investigation/environment.txt](environment.txt) | 配套说明、环境记录或分析材料 |
| [task/wal-investigation/events.jsonl](events.jsonl) | 原完整服务实验的命令和阶段时间线 |
| [task/wal-investigation/flags-initial.json](flags-initial.json) | 结构化证据或阶段性审查结果，按其时间和范围解释 |
| [task/wal-investigation/flags-raft-1-1791587228.json](flags-raft-1-1791587228.json) | 结构化证据或阶段性审查结果，按其时间和范围解释 |
| [task/wal-investigation/flags-raft-30-1791588481.json](flags-raft-30-1791588481.json) | 结构化证据或阶段性审查结果，按其时间和范围解释 |
| [task/wal-investigation/flush-job-verification.json](flush-job-verification.json) | 结构化证据或阶段性审查结果，按其时间和范围解释 |
| [task/wal-investigation/flush.log](flush.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/hosts-before-spaces.log](hosts-before-spaces.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/install.log](install.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/job-status.log](job-status.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/plot-dependencies-pypi.log](plot-dependencies-pypi.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/plot-dependencies.log](plot-dependencies.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/plot-requirements.txt](plot-requirements.txt) | 配套说明、环境记录或分析材料 |
| [task/wal-investigation/process-manifest.json](process-manifest.json) | 结构化证据或阶段性审查结果，按其时间和范围解释 |
| [task/wal-investigation/raft-walkthrough-validation.log](raft-walkthrough-validation.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/readable-analysis-verification.json](readable-analysis-verification.json) | 结构化证据或阶段性审查结果，按其时间和范围解释 |
| [task/wal-investigation/register-hosts.log](register-hosts.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/rocks-commit-20-writes.csv](rocks-commit-20-writes.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/rocks-commit-20-writes.meta](rocks-commit-20-writes.meta) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/rocks-commit-60-writes.csv](rocks-commit-60-writes.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/rocks-commit-60-writes.meta](rocks-commit-60-writes.meta) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/rocks-commit-first-write.csv](rocks-commit-first-write.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/rocks-commit-first-write.meta](rocks-commit-first-write.meta) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/rocks-commit-probe-build.log](rocks-commit-probe-build.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/rocks-commit-probe-reproduction.txt](rocks-commit-probe-reproduction.txt) | 配套说明、环境记录或分析材料 |
| [task/wal-investigation/rocks-commit-probe.csv](rocks-commit-probe.csv) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/rocks-commit-probe.meta](rocks-commit-probe.meta) | 组件或服务观测原始样本/结构与分配器元数据 |
| [task/wal-investigation/runtime-environment.txt](runtime-environment.txt) | 配套说明、环境记录或分析材料 |
| [task/wal-investigation/runtime-metrics.json](runtime-metrics.json) | 结构化证据或阶段性审查结果，按其时间和范围解释 |
| [task/wal-investigation/runtime-metrics.log](runtime-metrics.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/runtime-rocks-analysis.txt](runtime-rocks-analysis.txt) | 配套说明、环境记录或分析材料 |
| [task/wal-investigation/runtime-rocks-options-comparison.json](runtime-rocks-options-comparison.json) | 结构化证据或阶段性审查结果，按其时间和范围解释 |
| [task/wal-investigation/sample-verification.json](sample-verification.json) | 结构化证据或阶段性审查结果，按其时间和范围解释 |
| [task/wal-investigation/service-control.log](service-control.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [task/wal-investigation/shutdown-verification.json](shutdown-verification.json) | 结构化证据或阶段性审查结果，按其时间和范围解释 |
| [task/wal-investigation/summary.json](summary.json) | 结构化证据或阶段性审查结果，按其时间和范围解释 |
| [task/wal-investigation/term-stability.json](term-stability.json) | 结构化证据或阶段性审查结果，按其时间和范围解释 |

## 工程说明与协作约束

| 仓库相对路径 | 用途和注意点 |
| --- | --- |
| [AGENTS.md](../../AGENTS.md) | 配套说明、环境记录或分析材料 |
| [CODEX-CLI-RESUME.md](../../CODEX-CLI-RESUME.md) | 配套说明、环境记录或分析材料 |
| [README.md](../../README.md) | 研究总览和阅读入口 |
| [chatgpt_shutdown_2026-10-10.log](../../chatgpt_shutdown_2026-10-10.log) | 对应步骤的实际运行/编译记录，非可执行入口 |
| [nebula_graph_readable_analysis_prompts.md](../../nebula_graph_readable_analysis_prompts.md) | 配套说明、环境记录或分析材料 |
| [task/task-wal.md](../task-wal.md) | 配套说明、环境记录或分析材料 |

## 发布文件规则

| 仓库相对路径 | 用途和注意点 |
| --- | --- |
| [task/wal-investigation/.gitattributes](.gitattributes) | 配套说明、环境记录或分析材料 |
| [task/wal-investigation/.gitignore](.gitignore) | 配套说明、环境记录或分析材料 |

## 后续维护这份索引

新增/删除交付材料后，按顺序更新发布清单、生成索引、最后冻结摘要。日志写到不纳入交付的 runs 目录；不要修改旧实验原件或旧校验快照。

```bash
mkdir -p task/wal-investigation/migration/runs
python3 task/wal-investigation/migration/build_delivery.py
python3 task/wal-investigation/migration/build_artifact_index.py
python3 task/wal-investigation/migration/build_delivery.py
python3 task/wal-investigation/migration/verify_delivery.py
```

ELF、临时数据库和 Word QA 缓存按忽略规则留在本机；目标机器从源码重建。Git 不迁移 Codex 本机会话、安装目录或运行服务。
