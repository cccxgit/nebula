# 生产调参分析的新增证据

本目录属于分析与验证，不修改内核机制、不启动数据库或修改现网。主文见 [生产调参分析](../PRODUCTION-PARAMETER-TUNING.md)。

2026-10-11 新增 `wal_buffer_size` 对应容量的原始组件对照：8、4、2、1 MiB 和 256 KiB。每档独立进程、一个缓存、全部为空日志，每次 push 后读取最新记录并释放读者。采样点覆盖第一条、64/65 条、恰好填满、开始淘汰、待回收节点积累、两倍/四倍填充量和析构。

运行命令在工程根目录执行：

```bash
bash task/wal-investigation/production-tuning-evidence/run-capacity-probe.sh
```

输出已存在时脚本拒绝覆盖；重跑请复制脚本并使用新的输出目录。`capacity-run.log` 是本次完整运行记录，含时间、HEAD、源码与可执行文件散列、实际参数和 CSV 输出。`capacity-*.meta` 记录 Record/Node 大小、分配参数和读者模式；`capacity-*.csv` 是原始样本。

复用此前已构建的 `../atomic-buffer-probe-jemalloc`；构建源码和原始实现未改。其完整编译方式在 `../run-atomic-buffer-probe.sh`，原构建环境在 `../atomic-buffer-probe-environment.txt`，不是本次重新编译。脚本首先逐字节核对 `../baseline/kvstore/wal/AtomicLogBuffer.{cpp,h}` 与基线提交 `de9b3ed800a6627d9845e9289b6bbc5b6faf460a` 一致。重新编译该组件只需单个编译进程；完整数据库编译仍受用户 `make -j5` 限制。

`node_malloc_usable_bytes` 是仍存活 Node 逐个调用 `malloc_usable_size` 后相加。因为全部消息为空，本例无额外 payload 堆分配。它不是 RSS；CSV 同时记录 RSS/HWM，析构后 Node 为零但 RSS 未立即下降，正说明两种口径不能混淆。

实验支持“正常读者释放时，更小容量会形成更低空日志缓存平台”。它没有覆盖并发长期持有读者、大 payload、大批量追加、完整 Raft/RocksDB 及生产等价性能与恢复，不能据此宣称某个参数值零风险。主文按此边界给出灰度条件。

其他证据笔记由并行代码核查生成，记录 RocksDB 配置作用范围、内存保护路径以及 WAL 缩容的边界；最终结论以主文为准。

后续新增的 `space-*-review.txt`、`space-policy-review-start.json` 记录用户提出图空间级机制后的源码评估，设计见 [图空间级参数方案](../SPACE-LEVEL-KERNEL-PARAMETERS.md)。这次只更新设计与原生产方案，未实现该机制、未新增服务测试。`space-policy-prior-manifest-check.json` 确认修订开始后，上一版清单中除预期改写的生产方案外，其余 17 个文件保持原字节。

`verification.json` 保留首轮五档容量实验与当时文档的核验结果；`space-policy-verification.json` 记录空间级方案修订后的链接、证据和代码未变检查。当前 `SHA256SUMS` 从工程根目录核验，覆盖本目录证据文件及两份方案文档；它随文档修订重新生成，不把历史文档散列当作新版本散列：

```bash
sha256sum -c task/wal-investigation/production-tuning-evidence/SHA256SUMS
```
