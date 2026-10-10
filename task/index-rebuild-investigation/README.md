# Nebula Graph 索引重建长期不结束调查

基线：`cccxgit/nebula` → `nebula-index`，提交 `de9b3ed800a6627d9845e9289b6bbc5b6faf460a`。本目录是本次分析、代码解读和复现证据的入口。

**已定位并复现的核心缺陷：增量回放提交后没有清空 BatchHolder，超过批次阈值后每处理一条 operation 都重复提交累积内容；累计批次再超过 burst 后，每次提交固定等待一秒。点、边索引共用此路径。** 指定参数和积压规模下，仅这些等待就可以超过两天。

这证明本分支存在能够解释该症状的代码缺陷；尚无生产日志与实际参数，不能将本次本机复现直接写成生产事故的唯一归因。材料中始终区分真实服务测量、原源码方法实验、数学外推和待生产匹配的条件。

## 阅读顺序

| 文档 | 内容 |
|---|---|
| [01 根因分析](01-root-cause.md) | 因果链、触发条件、两天耗时下界、生产归因需要的证据 |
| [05 真实服务复现](05-live-reproduction.md) | 完整构建运行方式、数据与语句、对照结果、关键原始日志及实验边界 |
| [02 点边索引机制](02-index-mechanism.md) | 数据与索引键、排序编码、NULL、复合/字符串/地理索引、TTL、增删改、正反边、查询回表 |
| [03 原源码审查和实验](03-rebuild-code-audit.md) | 原样抽取回放代码运行、累计提交量、公式、限速实测、全局计数等待及取消延迟 |
| [04 作业生命周期](04-job-lifecycle.md) | graph→meta→storage、RUNNING/STOP、结果上报、重启恢复、生产只读采集与判别矩阵 |

## 证据与可执行材料

- [live/summary.json](live/summary.json)、[live/summary.csv](live/summary.csv)：真实服务实验结果，区分观测值与源码推导值。
- `live/<case>/events.jsonl`：实际参数、每条命令结果、时间戳、job 状态、验证断言。
- `live/<case>/queries.ngql`：全部实际 nGQL，包括装载的每条记录。
- `live/<case>/experiment-storage.log`：仅问题场景的原始 storage 日志；同目录 `storage-observation.log` 还包含无并发对照。
- [audit/source_harness.log](audit/source_harness.log)：原源码方法与限速类的实际执行结果；替身范围在 03 报告明确列出。
- [audit/amplification.csv](audit/amplification.csv)：两套参数、不同键宽和积压量的解析外推；不是 48 小时实跑记录。
- [scripts/live_repro.py](scripts/live_repro.py)、[scripts/summarize_live.py](scripts/summarize_live.py)：复现与证据校验脚本。
- [config](config)、[logs](logs)：完整实例配置、构建安装日志、启动/停止记录、资源计数、二进制 SHA-256。

## 改动范围与运行约束

没有实现或试用修复。受 Git 跟踪的业务源码只增加 `RebuildIndexTask.cpp` 中的 43 行日志、计数及计时；完整差异为 [observation-only.patch](observation-only.patch)。批次清理、状态转移、限速和取消检查仍保持原样。

按用户最新要求，源码编译并发上限为 `make -j5`；启动前执行 `ulimit -n 65536`。实例目录为 `/home/sch/nebula-run/nebula-index`，实验使用独立端口；没有连接生产或修改其他实例。当前 AGENTS.md 同样规定 `make -j5`，本次分析没有修改该文件；早期构建中断及改用 `-j5` 续编的记录见 05 报告。

源码行号均以分析基线为准，因为本次观测日志会使工作区回放文件行号偏移。可对照 [基线文件](audit/baseline/RebuildIndexTask.cpp) 或 `git show de9b3ed80:<path>` 查看。本次发布包含分析文章、复现脚本、配置、文本证据及观测补丁；业务源码工作区的 43 行观测改动没有纳入发布提交。重新复现时，先按 05 报告应用观测补丁。
