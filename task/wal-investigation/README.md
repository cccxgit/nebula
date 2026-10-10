# WAL 空闲分片内存调查

本目录执行工程的 `task/task-wal.md`，范围为**解释、观测和复现，不实施修复**。基线提交：`de9b3ed800a6627d9845e9289b6bbc5b6faf460a`。

GitHub 提交收录本目录的六篇结论/代码解读文章和两组关键图表。完整原始采样、服务日志、WAL 归档、复现程序与校验产物保留在实验机器的同目录中，未随文章提交；文中的证据文件名和校验值用于定位本地原始材料。

## 建议阅读顺序

1. [完整服务实验与结论](REPORT.md)：实测环境、阶段结果、复现步骤和证据边界。
2. [源码机制、定量模型与原始组件实验](COMPONENT-ANALYSIS.md)：为什么空闲也写 WAL，为什么 8MiB 的计账可以对应约 28MiB 节点分配，以及默认心跳下的每日增长量。
3. [Raft 逐函数解读](CODE-WALKTHROUGH-RAFT.md)：空间/分片对象生命周期、选举、两条心跳路径、Leader/Follower 复制与提交、迭代器生命周期。
4. [WAL 逐函数解读](CODE-WALKTHROUGH-WAL.md)：文件写入/恢复、64 槽节点、链表方向、逻辑容量、标删、读者引用、GC、reset、析构、磁盘 TTL 与回退读取。
5. [RocksDB 与内存统计逐函数解读](CODE-WALKTHROUGH-ROCKSDB.md)：提交位点的覆盖写、MemTable、刷盘、共享缓存、内存监控、jemalloc purge、RSS。

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

代码链接使用当前工程的绝对路径，原始 `AtomicLogBuffer.cpp` 的引用明确指向 `baseline/`，避免观测补丁增加行数后混淆。图表中的 MiB 均为 `1024²` 字节。

本机服务安装目录为 `/home/sch/nebula-run/nebula-memory`。实验使用独立端口：graph `29669`、meta `29559`、storage `29779/29789/29799`。服务启动前由脚本设置 `ulimit -n 65536` 的等价进程限制。重新执行前先阅读 `REPORT.md`，采样输出目录不允许覆盖已有证据。

本次实验已经结束，5 个服务进程全部退出，原始数据保留。无需启动数据库即可按报告第 6.5 节重新解析归档 WAL，第 6.6 节重新计算指标和绘图；报告区分了真实测量、原始组件验证及长期条件推算。
