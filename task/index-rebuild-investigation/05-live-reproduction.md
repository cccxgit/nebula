# 真实 Nebula 服务复现：构建、输入、对照、日志与边界

代码基线：`de9b3ed800a6627d9845e9289b6bbc5b6faf460a`。仅对 `src/storage/admin/RebuildIndexTask.cpp` 添加观测日志，补丁为 [observation-only.patch](observation-only.patch)，没有清空 batch、调整原算法或实现任何修复。

## 1. 环境与隔离

- 仓库：`/home/sch/nebula/nebula-index`。
- 安装及运行目录：`/home/sch/nebula-run/nebula-index`，遵循本工程 AGENTS.md 指定目录。
- CMake Debug，ENABLE_TESTING=OFF。依赖为本机 `/opt/vesoft/third-party/3.3`。
- Python 客户端 `nebula3-python==3.8.3`。工具链与系统版本保存在 [logs/toolchain.json](logs/toolchain.json)。
- 单 metad、单 graphd、单 storaged；每个实验空间 **1 partition、1 replica、INT64 VID**。单副本同样走实际 storage/Part/Raft/WAL/RocksDB 路径，但没有跨主机 follower 复制成本。
- 为避开本机其他工程，使用独立端口：meta 21431、graph 21531、storage 21631；storage admin 21630、Raft 21632；HTTP 分别 22431、22531、22631。客户端只连接本机实验 graphd。
- 采用模板配置，元数据心跳改为 1 秒、Raft 心跳改为 3 秒以缩短空实例初始化等待；这些不修改回放批次算法。完整配置在 [config](config)。
- 启动前执行 `ulimit -n 65536`。用户补充最大编译并发为 5 后，立即中断本任务原先按 AGENTS.md 启动的 `-j8`，之后改用 `make -j5`；中断和续编保留于同一 build.log。未修改其他任务的编译进程。

这不是生产吞吐量基准。Debug、机器上其他活动、日志观测和单副本都会影响绝对耗时；因果证据是原始 batch 保留行为、连续增长的 entries/bytes，以及超 burst 分支内的明确一秒等待。

## 2. 实际构建与启动方式

本次发布没有直接修改 `src/storage/admin/RebuildIndexTask.cpp`；诊断日志以补丁附录保存。首次从 GitHub 取到本报告后，如果工作区尚未应用观测补丁，先在工程根目录执行：

```bash
git apply --check task/index-rebuild-investigation/observation-only.patch
git apply task/index-rebuild-investigation/observation-only.patch
```

已包含 `INDEX_DIAG` 观测代码的工作区跳过这一步，避免重复应用。补丁只增加日志、计时和计数，不修复回放逻辑。原始源码分析和方法级 harness 的基线仍为上述 `de9b3ed80` 提交。

在工程根目录执行，日志写在本任务目录：

```bash
mkdir -p build task/index-rebuild-investigation/logs
cmake -S . -B build \
  -DCMAKE_INSTALL_PREFIX=/home/sch/nebula-run/nebula-index \
  -DENABLE_TESTING=OFF -DCMAKE_BUILD_TYPE=Debug \
  > task/index-rebuild-investigation/logs/configure.log 2>&1
cd build
make -j5 > ../task/index-rebuild-investigation/logs/build.log 2>&1
make install > ../task/index-rebuild-investigation/logs/install.log 2>&1
cd ..
```

将本目录 `config/nebula-*.conf` 放入该专用实例的 `etc/`。这些是实验配置，不应覆盖其他实例配置。然后：

```bash
ulimit -n 65536
/home/sch/nebula-run/nebula-index/scripts/nebula.service start metad
/home/sch/nebula-run/nebula-index/scripts/nebula.service start storaged
/home/sch/nebula-run/nebula-index/scripts/nebula.service start graphd
/home/sch/nebula-run/nebula-console \
  -addr 127.0.0.1 -port 21531 -u root -p nebula \
  -e 'ADD HOSTS 127.0.0.1:21631; SHOW HOSTS;'
```

首次空 meta 必须 ADD HOSTS，等待 storage 为 ONLINE 再创建空间。完整构建/启动/注册结果见 `logs/`。再次复现实验须换新 space 名和输出目录；脚本不删除已有空间，输出目录已存在会拒绝覆盖。

## 3. 工作负载与实验屏障

驱动脚本：[scripts/live_repro.py](scripts/live_repro.py)。所有真实执行的 nGQL 原文写入各 case 的 `queries.ngql`，每条结果、耗时和时间戳写入 `events.jsonl`。

每组过程：

1. 从 storage HTTP GET 读取当前参数，再 PUT 设置该组 B/R，检查 `errCode=0`，再次 GET 并断言实际生效值。
2. 创建新 space，创建 tag 或 edge `sample`，属性为 `c0..c15`，每列 `string NOT NULL`。
3. 先写入 2,048 条数据，再创建 `sample_idx(c0(256),...,c15(256))`。因此首次 baseline 确实补建历史索引。
4. **对照组**：不并发写，REBUILD 并轮询到 FINISHED。
5. **问题组**：同空间、同索引再次 REBUILD；从实际 storaged 日志确认 `Start building index`，且尚未看到回放开始，才插入新数据。
6. 写入完成后停止产生新业务写，等待任务自然结束，保留整个慢回放阶段。不通过修复、STOP 或重启让问题组“通过”。
7. LOOKUP 校验新增索引记录数；额外断言回放确实包含目标数量的 operation、批次持续增长、超过 R 后至少三次约一秒等待。只看 LOOKUP 正确不算成功复现。

普通点索引完整 key K=4+4+16×256+8=4,112 B，每条 modify 使回放 holder 增加 8,240 B；边索引还含额外 rank/dst，K=4,128 B，增加 8,272 B。字段值实际只用 `seed` / `concurrent` 等短串，索引按固定长度补零。采用宽索引是为了用少量真实记录跨过原始字节阈值；不代表生产必须有 16 列索引才会触发。

单分区消除了跨分区归属的不确定性；本实例逐组串行运行，避免把另一作业的全局 seq 当成本次提交。

## 4. 三组可重复执行命令

```bash
python3 task/index-rebuild-investigation/scripts/live_repro.py \
  --kind TAG --name ir_tag_flags_01 --seed 2048 --backlog 96 \
  --batch 131072 --rate 524288 \
  --output task/index-rebuild-investigation/live/tag_flags \
  > task/index-rebuild-investigation/logs/live-tag-flags.log 2>&1

python3 task/index-rebuild-investigation/scripts/live_repro.py \
  --kind EDGE --name ir_edge_flags_01 --seed 2048 --backlog 96 \
  --batch 131072 --rate 524288 \
  --output task/index-rebuild-investigation/live/edge_flags \
  > task/index-rebuild-investigation/logs/live-edge-flags.log 2>&1

python3 task/index-rebuild-investigation/scripts/live_repro.py \
  --kind TAG --name ir_tag_template_01 --seed 2048 --backlog 576 \
  --batch 1048576 --rate 4194304 \
  --output task/index-rebuild-investigation/live/tag_template \
  > task/index-rebuild-investigation/logs/live-tag-template.log 2>&1
```

前两组显式使用源码编译缺省参数，第三组使用 `.default/.production` 模板参数。每组都包含无并发对照和有并发问题场景；实际生效参数以 case 中 HTTP GET 的原始结果为准。

## 5. 实际结果

三组均已自然执行到 FINISHED，脚本退出码均为 0；最终汇总校验 `all_cases_valid=true`。完整结构化结果见 [summary.json](live/summary.json)、[summary.csv](live/summary.csv) 和 [最终校验日志](logs/live-summary.log)。

| 实验 / B与R | 对照job → 问题job | 新增operation | 无并发耗时 | 有并发耗时 | 实测超burst提交 | 这些提交的limit_ms | 最终新增LOOKUP数 |
|---|---|---:|---:|---:|---:|---:|---:|
| [点，128 KiB / 512 KiB](live/tag_flags/events.jsonl) | 2 → 3 | 96 | 18.047 s | 82.289 s | 34 | 1000–1000 ms | 96 |
| [边，128 KiB / 512 KiB](live/edge_flags/events.jsonl) | 5 → 6 | 96 | 18.059 s | 82.280 s | 34 | 1000–1003 ms | 96 |
| [点，1 MiB / 4 MiB](live/tag_template/events.jsonl) | 8 → 9 | 576 | 4.017 s | 315.063 s | 68 | 1000–1000 ms | 576 |

耗时从提交前计到 SHOW JOB 观察到 FINISHED，包含状态上报及最多约 2 秒轮询延迟。LOOKUP 校验的是此次新增记录数量，不等同于全库历史索引一致性审计。

### 5.1 点索引组的阶段分解

点组 baseline 全量扫描约 **17.266678 s**，问题组全量约 **16.477277 s**；问题组第一轮 operation 回放约 **64.003872 s**。因此两次 job 的主要新增耗时落在回放，不是全量数据更多或状态上报慢。

34 次 `batch_bytes > 524288` 的连续提交全部 `limit_ms=1000`，这些提交的 `append_ms` 合计仅 **180 ms**、最大 **22 ms**。随后 LOCKED 计数等待约 **0.163 ms**，且完成报告立即得到 `report_rc=SUCCEEDED`。这些证据排除了本次实验主要卡在全局计数、长时间底层回调或完成上报的解释。

这 34 秒只是确定的超 burst 固定等待部分；此前超过批次阈值后的重复提交仍会触发 token bucket 等待，加上编码及提交成本，组成约 64 秒的实际回放。不能把 34 秒写成全部回放耗时。

### 5.2 直接展示“累计与重复”的原始日志

以下三行来自 [点组 experiment-storage.log 第59行起](live/tag_flags/experiment-storage.log#L59)，删去日志头，仅保留原始消息：

```text
INDEX_DIAG append space=1 part=1 seq=83 batch_bytes=791040 entries=192 encoded_bytes=792781 limit_ms=1000 append_ms=2 rc=0
INDEX_DIAG append space=1 part=1 seq=84 batch_bytes=791040 entries=192 encoded_bytes=792781 limit_ms=1000 append_ms=4 rc=0
INDEX_DIAG replay_end space=1 part=1 round=1 operations=96 retained_bytes=791040 retained_entries=192
```

前两行 `seq` 不同，batch/entries/encoded_bytes 却完全相同：内层最后一次提交后，尾部又把完整累计 batch 再提交一次。第三行确认本轮仅有 96 条 operation，但 holder 仍保留 192 entries / 791040 B。更早的连续 seq 则表现为每增加一条 operation，entries 只增 2，整个历史前缀继续一起提交。

### 5.3 模板配置也触发相同机制

模板组 baseline 仅 **4.017 s**；576 条新写入约 **0.097 s** 内完成并停止继续写入，问题组仍耗时 **315.063 s** 才自然完成。第一轮记录为 576，后两轮均为 0；最终 holder 为 **4,746,240 B / 1,152 entries**，超出 4 MiB burst。共观察到 68 次超 burst 提交，`limit_ms` 全为 1000，其 `append_ms` 范围为 12–144 ms。

较大模板参数只是把跨界所需的积压量推高，并没有消除累计批次路径。点、边实验均通过，无需持续写入到结束，也无需大规模原始图才能制造这个回放性能问题。

### 5.4 实测记录与公式推导分开列出

| 场景 | 实测首轮operation | 据源码推导的有数据append次数 | 据源码推导的累计编码字节 | 实测最终保留字节 |
|---|---:|---:|---:|---:|
| tag_flags | 96 | 82 | 38,252,122 B | 791,040 B |
| edge_flags | 96 | 82 | 38,400,346 B | 794,112 B |
| tag_template | 576 | 450 | 1,309,922,842 B | 4,746,240 B |

上表累计量由真实 round 的 N/bytes/entries 与已验证原代码公式算出；由于日志采样，它们不是从日志逐行累加得到的实测总量。原样执行真实编码器并逐次计数的实验见 03 报告。相同 key 覆盖及重复删除使最终索引数量仍可正确，累计写入工作量却远大于有效新增数据。

## 6. 怎样阅读证据

每个 case 目录含：

- `queries.ngql`：全部执行语句，包括建空间、schema、装载数据、两次 REBUILD、SHOW 和 LOOKUP。
- `events.jsonl`：UTC 时间戳、单调时钟相对耗时、命令结果、jobId、状态、实际 flags、最后验证结果。
- `storage-observation.log`：从该 case 开始捕获的存储日志，包括 baseline。
- `experiment-storage.log`：只从 baseline 已完成后开始捕获的问题组日志，避免混淆两次重建。

storaged 原始日志时间为本机 Asia/Shanghai（UTC+8），events 的 `utc` 字段为 UTC。相对耗时来自单调时钟，不依赖日志时区换算。

关键字段：

| 日志 | 含义与限制 |
|---|---|
| `replay_begin ... round=` | 开始一个新的 operation iterator |
| `append ... seq= ... entries= ... batch_bytes=` | 本次提交仍持有的累计条目/字节；seq 是进程级全局计数 |
| `encoded_bytes=` | 本次序列化后的完整 batch 字符串长度 |
| `limit_ms= ... append_ms=` | 限速调用时间、提交到异步回调返回时间；编码发生在这两个计时之前 |
| `replay_end ... operations= ... retained_entries= ... retained_bytes=` | 本轮真实遍历的 operation 数及轮末仍保留的 batch 内容 |
| `locked_wait...on_flying=` | 进入全局计数排空等待，超过一秒才周期性输出等待状态 |

append 日志只打印最初几次、每 100 次以及耗时较大的调用，**不能直接把日志行数当作全部 append 次数**。连续 seq 且 entries 每次只增加 2、累计 bytes 继续增加，是本次观测的核心指纹。根据实际 round 记录数和原代码公式计算出的累计量会明确标注为“推导值”；已实际逐次计数的累计编码量在源码级 harness 报告中单列。

## 7. 实验覆盖与未覆盖

本实验覆盖真实 nGQL→meta job→storage task→RocksDB 数据→operation 回放→最终上报链路，并覆盖点、边共同回放逻辑及两套参数。它不模拟生产副本数、网络、磁盘负载和生产 schema；没有连续运行 48 小时，也没有真实负载下制造全局计数永不归零、meta 重启、host 永久离线等故障。

“两天”的结论来自已验证分支的一秒下界和确定输入规模，详见 [01-root-cause.md](01-root-cause.md) 与 [03-rebuild-code-audit.md](03-rebuild-code-audit.md)。生产归因仍须匹配真实故障所在阶段及参数，不以实验代替缺失的生产日志。

测试结束后只停止本次专用实例的三个进程，保留实验数据、配置及日志供复核。停止和最终状态写入 `logs/stop-and-status.log`；没有删除数据目录，也没有修改其他实例。
