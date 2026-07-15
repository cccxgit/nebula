# NebulaGraph 3.6 磁盘读写性能测试工具

本工具使用 Flexible I/O Tester（fio）对 NebulaGraph storaged/metad 所在文件系统进行可重复的磁盘基线测试。它同时保留原始 fio JSON、主机和挂载信息，按多轮中位数生成摘要，并可直接比较两套环境。

工具只在新建的测试文件上运行，不读取、修改或删除 NebulaGraph 数据。它不会停止服务、修改配置、执行 compaction、清理系统页缓存或访问裸块设备。

> **重要：** 默认 `standard` 是饱和压力测试。生产节点应先摘流或进入维护状态，再停止该节点的 storaged/metad/standalone。不要在承载业务的副本上直接执行。`--allow-active-nebula` 只是显式风险确认，不会限流，也不会让在线测试结果自动变得可比。

## 1. 为什么这些测试适用于 NebulaGraph 3.6

NebulaGraph graphd 通过 RPC 访问 storaged，不直接访问 RocksDB。一个普通写请求在 storaged 上可能依次产生 Raft WAL、RocksDB WAL、memtable flush 和后台 compaction；读请求则包含点查、MultiGet、前缀迭代和范围扫描。因此只测试单一顺序带宽无法反映数据库对磁盘的主要要求。

NebulaGraph 3.6 production 配置的关键默认值包括：

- BlockBasedTable 的 SST block 为 8 KiB。
- RocksDB write buffer 为 64 MiB，最多 4 个 write buffer。
- 最多 4 个 RocksDB background job 和 4 个 subcompaction。
- Raft WAL 使用 buffered append，默认每个文件 16 MiB；`wal_sync=false` 时在滚动/关闭文件时执行 `fsync`，开启后每次 append 都执行 `fsync`。
- `data_path` 支持多个路径，每个路径对应一个 RocksDB 实例；Raft `wal_path` 和 RocksDB `rocksdb_wal_dir` 还可能位于独立磁盘。

工具据此提供以下 profile：

| Profile | fio 模式 | NebulaGraph/RocksDB 对应场景 | 主要指标 |
| --- | --- | --- | --- |
| `seq_write` | 1 MiB、direct、顺序写 | SST flush、snapshot/rebuild 大块写、磁盘写带宽上限 | write MiB/s、write p99 |
| `seq_read` | 1 MiB、direct、顺序读 | 全图扫描、snapshot、compaction 输入和读带宽上限 | read MiB/s、read p99 |
| `randread_8k_qd1` | 8 KiB、direct、单 job/QD1 | 冷数据 SST point lookup 的介质延迟基线 | read p50/p99/p99.9 |
| `randread_8k` | 8 KiB、direct、多 job/异步队列 | 并发 Get/MultiGet、前缀查找 | read IOPS、read p99 |
| `randrw_8k` | 8 KiB、direct、默认 70/30 混合 | 前台读与写/后台 I/O 争用代理；70/30 不是 Nebula 固定比例 | read/write IOPS、双向 p99 |
| `compaction` | 并发 1 MiB direct 顺序读和写 | RocksDB compaction 的同盘双向流量 | read/write MiB/s、双向 p99 |
| `raft_wal` | 4 KiB buffered 顺序写，每 4096 次写 `fsync` | 16 MiB Raft WAL 滚动同步的磁盘代理 | write IOPS/MiB/s、sync p99 |
| `wal_sync` | 4 KiB buffered 顺序写，每次写 `fsync` | `wal_sync=true` 的持久化原语和同步写延迟上限 | sync p50/p99/p99.9、write IOPS |

direct profile 用于隔离页缓存并比较介质/文件系统能力。WAL profile 故意使用 buffered 顺序 I/O，并按 Nebula 的 16 MiB 滚动或逐写同步策略调用 `fsync`。fio 的 psync engine 使用定位写，不复刻 Nebula 的 `O_APPEND + write` syscall，只测试等价的顺序落盘与同步原语；同步测试不能与 direct profile 混为一个总分。

fio 不能模拟 RocksDB 的压缩、Bloom filter、block cache、LSM 写放大、自定义 compaction filter、Raft 网络复制或业务数据分布。本工具是磁盘层基线，不替代 `storage_perf`、真实查询压测或 RocksDB 运行态指标。

## 2. 使用前环境检查

### 2.1 软件要求

- Linux，Bash 4 或更新版本。
- Python 3.8 或更新版本，只使用标准库。
- Flexible I/O Tester fio，且需要 `libaio` 和 `psync` engine。
- 建议安装 `sysstat`，存在 `iostat` 时工具会自动采集扩展设备指标；它不是必需依赖。
- 建议存在 `findmnt`、`lsblk`、`df` 和 `stat`，常见 Linux 发行版默认提供。

安装示例：

```bash
# RHEL / Rocky Linux / AlmaLinux
sudo dnf install -y fio sysstat

# CentOS 7
sudo yum install -y fio sysstat

# Ubuntu / Debian
sudo apt-get update
sudo apt-get install -y fio sysstat
```

确认调用的是 Flexible I/O Tester，而不是同名 Python 命令：

```bash
fio --version
# 正常输出以 fio- 开头，例如 fio-3.33
```

源码构建执行 CMake install/package 时，工具会安装到 `scripts/disk-benchmark/`，不安装测试文件。独立分发时需保持入口、`lib/` 和 `profiles/` 的相对目录不变。

在开发机验证脚本和报告逻辑：

```bash
python3 -m unittest discover \
  -s scripts/disk-benchmark/tests -p 'test_*.py' -v
scripts/disk-benchmark/tests/test-runner.sh
```

`test-runner.sh` 使用 fake fio 验证控制流和安全清理，不产生可用于性能结论的数据；正式使用前仍应在测试机用真实 fio 运行 `quick`。

### 2.2 选择节点和时间窗口

1. 只在 storaged 或 metad 的数据/WAL 挂载点上测试；graphd 的日志盘不能代表图库数据盘。
2. 两套环境使用相同角色的节点。多副本集群应逐台执行，先迁移 leader、摘流或进入维护窗口，不要同时压测同一 partition 的多个副本。
3. 推荐停止被测节点上的 storaged/metad/standalone。脚本检测到进程仍在运行会默认拒绝 `run`。
4. 测试期间不要同时执行 full compaction、balance、snapshot、rebuild index、导入、备份或其他磁盘密集任务。
5. 云盘需要确认规格、基线/突发 IOPS、吞吐额度和 burst credit；两边额度状态不同会直接影响结论。

### 2.3 选择测试目录

为每个被测挂载点创建独立的空闲目录。目录必须与生产数据位于同一个 mount，但不能位于实际 `data_path`、`wal_path`、`rocksdb_wal_dir` 或其子目录内。

示例：生产 `data_path=/data1/nebula-storage` 时，可使用同盘兄弟目录：

```bash
sudo mkdir -p /data1/nebula-fio-scratch
sudo chown "$(id -u):$(id -g)" /data1/nebula-fio-scratch
```

以下目标会被拒绝或不应使用：

- `/dev/nvme0n1` 等裸块设备。
- `/`、tmpfs、ramfs、overlay/容器联合文件系统。
- 已存在 `nebula/` 数据树的目录。
- 配置文件中 `data_path`、`wal_path` 或 `rocksdb_wal_dir` 的内部目录。

脚本会在目标下创建 `.nebula-disk-benchmark-<run-id>`，并且只在 sentinel 文件存在、路径仍位于指定父目录内时清理它。异常退出也使用同一规则。结果报告默认写在执行命令时的 `./nebula-disk-results`，不会随测试数据清理。

### 2.4 容量要求

峰值空间按选定 profile 计算：基础数据文件为 `size`，compaction 需要另一个最大为 `size` 的输出文件，WAL 文件最大为 1 GiB；此外保留 Nebula `minimum_reserved_bytes` 和至少 1 GiB 安全余量。

默认 preset：

| Preset | 数据大小 | 单 profile 计时 | Ramp | 重复 | 随机并发 | 用途 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| `quick` | 1 GiB | 15 s | 3 s | 1 | 2 jobs x QD4 | 安装和流程验证，不能形成生产结论 |
| `standard` | 8 GiB | 60 s | 10 s | 3 | 4 jobs x QD8 | 两套生产硬件的常规对比 |
| `extended` | 32 GiB | 180 s | 20 s | 3 | 4 jobs x QD16 | 稳态和高性能盘深入分析 |

对于很快的 NVMe/云盘，8 GiB 会在单个 profile 内循环覆盖。direct I/O 仍能测介质性能，但若要评估大容量盘的长时间稳态、SLC cache 或云盘额度，应使用更大的 `--size` 和 `extended`。

### 2.5 执行一键检查

先对每个环境执行 `check`。检查会验证命令、fio engine、目录、文件系统、空间、生产配置路径和活动 Nebula 进程，并创建后立即删除一个 4 KiB direct-I/O 探针文件；它不会执行完整压力测试。

```bash
TOOL=./scripts/disk-benchmark/nebula-disk-benchmark.sh

"$TOOL" check \
  --target-dir /data1/nebula-fio-scratch \
  --wal-target-dir /wal1/nebula-fio-scratch \
  --nebula-config /usr/local/nebula/etc/nebula-storaged.conf \
  --preset standard
```

没有独立 WAL 盘时省略 `--wal-target-dir`。建议始终传入实际 `nebula-storaged.conf`，工具只采集 I/O 相关白名单字段，不复制完整配置。

若配置中的 `data_path`/`wal_path`/`rocksdb_wal_dir` 是相对路径，工具需要知道 Nebula 服务工作目录。配置位于标准 `<nebula-home>/etc/` 或 `<nebula-home>/conf/` 时会自动推断；非标准布局必须显式指定，否则检查失败：

```bash
"$TOOL" check \
  --target-dir /data1/nebula-fio-scratch \
  --nebula-config /opt/custom/storage.flags \
  --nebula-home /usr/local/nebula \
  --preset standard
```

## 3. 执行测试

### 3.1 两套环境使用相同参数

环境 A：

```bash
"$TOOL" run \
  --target-dir /data1/nebula-fio-scratch \
  --wal-target-dir /wal1/nebula-fio-scratch \
  --nebula-config /usr/local/nebula/etc/nebula-storaged.conf \
  --label prod-a-data1 \
  --preset standard \
  --yes
```

环境 B 使用相同命令，只修改机器上的路径和 label：

```bash
"$TOOL" run \
  --target-dir /data1/nebula-fio-scratch \
  --wal-target-dir /wal1/nebula-fio-scratch \
  --nebula-config /usr/local/nebula/etc/nebula-storaged.conf \
  --label prod-b-data1 \
  --preset standard \
  --yes
```

不传 `--yes` 时，交互终端必须输入 `RUN`。完整 standard 运行时间约为各 profile 的 `ramp + runtime + cooldown` 乘以 8 个 profile 和 3 轮，再加数据预写时间。

### 3.2 多 data_path

NebulaGraph 对每个 `data_path` 创建一个 RocksDB 实例。若一台 storaged 配置了多个路径，应在每个物理路径的同 mount 兄弟目录分别执行一次，label 中写明路径或设备，例如 `prod-a-data1`、`prod-a-data2`。对比时将 A/B 对应设备逐一比较，不要把不同数量或不同型号的磁盘结果混在一起。

本工具的一次 `run` 只测试一个 data target 和一个可选 WAL target，避免并发压多个生产磁盘或误把多盘聚合性能当成单盘性能。

### 3.3 自定义运行

```bash
"$TOOL" run \
  --target-dir /data1/nebula-fio-scratch \
  --label prod-a-read \
  --size 16G \
  --runtime 120 \
  --ramp-time 15 \
  --repeat 5 \
  --jobs 4 \
  --iodepth 8 \
  --profiles randread_8k_qd1,randread_8k,seq_read \
  --randseed 20260714 \
  --yes
```

两套环境必须使用完全相同的 size、runtime、ramp、cooldown、repeat、jobs、iodepth、profile 列表、read mix、随机种子、fio 版本和工具版本。工具会对本次实际使用的 profile 内容计算 SHA-256 workload fingerprint，`compare` 会连同上述字段一起检查；不一致时仍生成报告，但标记为不可直接比较。

### 3.4 活动服务覆盖选项

```bash
"$TOOL" run ... --allow-active-nebula --yes
```

仅在已评估影响且确实要观察业务争用时使用。该模式仍会尝试压满磁盘，结果同时包含当前业务负载、compaction 和 fio 的影响，不能作为纯磁盘能力结论。两边若业务 offered load 不一致，也不能横向比较。

## 4. 输出文件

每次运行生成独立目录：

```text
nebula-disk-results/prod-a-data1-20260714T080000Z-12345/
├── manifest.json                 # 工具版本、参数、profile SHA-256、状态
├── environment.json              # CPU、内存、内核、挂载、设备和配置白名单
├── summary.json                  # 机器可读的多轮统计
├── summary.md                    # 中文摘要
├── profiles/                     # 本次实际使用的 fio job 文件
├── raw/
│   ├── precondition.json
│   └── round-01/<profile>.json   # 原始 fio JSON
└── telemetry/
    └── round-01/*.iostat.log     # 安装 sysstat 时生成
```

`environment.json` 可能包含 hostname、设备型号、挂载路径以及选定的 Nebula 配置项。将结果发送到外部系统前应按组织要求检查这些信息。

## 5. 解读单套环境结果

### 5.1 指标含义

- **IOPS**：每秒 I/O 次数。主要用于 8 KiB 随机读写和 4 KiB WAL；越高越好。
- **MiB/s**：每秒传输量。主要用于顺序读写和 compaction；越高越好。
- **p50**：中位请求延迟，反映典型 I/O。
- **p95/p99/p99.9**：尾延迟。Nebula 的 query、Raft apply、WAL 和 compaction 争用通常更受尾延迟影响；越低越好。
- **sync latency**：`fsync` 调用延迟。`wal_sync` 优先看此值，而不是只看 write completion latency。
- **CV**：多轮结果的变异系数（标准差/均值）。越小表示环境越稳定。

摘要使用重复轮次的中位数，避免单次异常值决定结论，同时保留最小值、最大值和 CV。

### 5.2 结果有效性检查

形成 A/B 结论前至少满足：

1. `manifest.json` 状态为 `complete`，所有原始 JSON 可解析。
2. 生产比较使用 `repeat >= 3`；`quick` 只验证流程。
3. 关键指标 CV 小于 5% 最理想；5%-10% 需要结合 iostat 和系统负载分析；大于 10% 应排除后台任务、云盘额度和热迁移后重跑。
4. 两边 fio/tool 版本、profile、参数、文件系统、mount option、I/O scheduler、RAID/LVM/云盘规格和剩余容量一致或已明确记录差异。
5. 测试期间两边都处于静默维护状态，或者都具有相同且可测量的业务 offered load。静默结果不能与在线结果比较。
6. 读取原始 fio JSON 的 `iodepth_level`，确认异步 profile 实际达到目标队列深度；设备过慢、engine 不匹配或 buffered libaio 可能导致实际深度偏低。

### 5.3 按业务场景判断

- **点查/邻接查询为主**：先看 `randread_8k_qd1` p99/p99.9，再看 `randread_8k` IOPS 和 p99。高并发 IOPS 更高但 QD1 尾延迟更差时，不能简单判定更优。
- **批量导入/写入为主**：看 `raft_wal`、`wal_sync` 尾延迟，`seq_write` 吞吐，以及 `randrw_8k` 写 IOPS/尾延迟。
- **扫描/snapshot/rebuild 为主**：看 `seq_read` 和 `seq_write` 持续带宽。
- **compaction 压力大**：同时看 `compaction` read/write MiB/s。只有一侧更快可能意味着读写争用不均衡。
- **开启 `wal_sync=true` 或 metad**：优先使用 `wal_sync` 的 sync p99/p99.9；平均吞吐不能掩盖长尾同步延迟。

不要将所有 profile 相加为一个“磁盘总分”。读密集、写密集和强持久化业务的权重不同；工具按场景分别报告，避免产生误导性总排名。

## 6. 对比两套环境

将两套完整结果目录放在同一台分析机，执行：

```bash
"$TOOL" compare \
  --baseline /results/prod-a-data1-20260714T080000Z-12345 \
  --candidate /results/prod-b-data1-20260714T090000Z-23456 \
  --output-dir /results/compare-data1 \
  --threshold-percent 10
```

输出：

```text
/results/compare-data1/
├── comparison.json
└── comparison.md
```

默认阈值为 10%：

- IOPS/带宽提高超过阈值，标记 candidate 更好；降低超过阈值，标记更差。
- 延迟降低超过阈值，标记 candidate 更好；升高超过阈值，标记更差。
- 差异在阈值内，标记为相近。
- 核心参数不一致、任一 manifest 非 `complete`、缺 profile/轮次或汇总带数据警告时，标记不可直接比较，不应据表中百分比做生产结论。

10% 是工程判读阈值，不是统计显著性检验。应同时查看各自 CV、最差轮次和 iostat。推荐判定方法：

1. 先确认 `comparable=true` 和环境/参数一致。
2. 再按实际业务选择最相关 profile，不先看无关指标。
3. 同时检查吞吐与 p99/p99.9。吞吐更高但尾延迟显著变差时，结论应写成具体取舍，而不是“整体更快”。
4. 至少在第二个时间窗口复测一次。若胜负反转或 CV 很高，先查后台任务和云盘额度。
5. 对多 data_path，逐盘对比后再结合每台 storaged 的磁盘数量和 partition 分布判断整机能力。

## 7. 与 NebulaGraph 运行态指标联合分析

fio 结果说明文件系统和介质能提供什么，不说明当前 RocksDB 是否已成为瓶颈。建议在业务压测或低峰观测中同步检查：

- Nebula `append_wal_latency_us`、`commit_log_latency_us` 等直方图。
- RocksDB `rocksdb.bytes.read`、`rocksdb.bytes.written`、block cache hit/miss、stall、L0 文件数、pending compaction bytes。
- full compaction 前后的数据量、TTL/删除比例、压缩比和 compaction backlog。
- 业务 query p99、Raft 心跳/复制状态、磁盘利用率和 queue depth。

若已启用 `enable_rocksdb_statistics` 且 stats level 足够，可读取 storaged HTTP 端点（端口以实际配置为准）：

```bash
curl -sS http://127.0.0.1:19779/rocksdb_stats
curl -sS 'http://127.0.0.1:19779/rocksdb_property?space=<space-id>&property=rocksdb.stats'
```

工具不会为了采集指标自动修改配置或重启服务。

## 8. 常见问题

### `fio` 版本输出不是 `fio-*`

系统命中了同名的 Fiona GIS CLI。安装 Flexible I/O Tester，并通过 `--fio-bin /usr/bin/fio` 指定正确二进制。

### direct-I/O probe 返回 `EINVAL` 或不支持

目标文件系统、加密层或网络文件系统可能不支持 `O_DIRECT`，也可能存在块对齐限制。不要用 `--allow-non-disk-fs` 绕过真实生产问题；应选择与 Nebula 数据相同且支持 direct I/O 的普通磁盘文件系统。

### 提示目标目录包含 Nebula 数据树

不要把测试目录放到实际 `data_path` 内。在同一个 mount 上创建兄弟目录后重试。

### 提示活动 Nebula 进程

推荐摘流并停止该节点的 storaged/metad/standalone。只有明确需要测试在线争用且已经评估业务影响时，才使用 `--allow-active-nebula`。

### 空间不足

减小 `--size` 仅适用于流程验证；正式对比应在两边使用相同大小。不要降低 Nebula 的 `minimum_reserved_bytes`，也不要让测试触发生产磁盘高水位。

### 两套结果波动很大

检查 compaction、snapshot、balance、备份、日志轮转、其他租户 I/O、云盘 burst credit、虚拟机热迁移和 RAID rebuild。清除干扰后使用至少 3 轮重测，不要在生产节点执行全局 `drop_caches`。

## 9. 设计依据

源码位置：

- `src/kvstore/RocksEngine.cpp`：Get/MultiGet、prefix/range/scan、WriteBatch、flush 和 compact。
- `src/kvstore/RocksEngineConfig.cpp`：WAL、direct read、block cache 和 RocksDB 配置。
- `src/kvstore/wal/FileBasedWal.cpp`：Raft WAL append、16 MiB rollover 和 `fsync` 策略。
- `src/daemons/StorageDaemon.cpp`：多 `data_path` 与独立 `wal_path`。
- `conf/nebula-storaged.conf.production`：8 KiB block、64 MiB write buffer、4 个 background job/subcompaction。

官方资料：

- [NebulaGraph 3.6 Storage Service](https://docs.nebula-graph.io/3.6.0/1.introduction/3.nebula-graph-architecture/4.storage-service/)
- [NebulaGraph 3.6 Storage 配置](https://docs.nebula-graph.io/3.6.0/5.configurations-and-logs/1.configurations/4.storage-config/)
- [NebulaGraph Compaction](https://docs.nebula-graph.io/3.6.0/8.service-tuning/compaction/)
- [RocksDB Overview](https://github.com/facebook/rocksdb/wiki/RocksDB-Overview)
- [RocksDB Write Stalls](https://github.com/facebook/rocksdb/wiki/Write-Stalls)
- [RocksDB Tuning Guide](https://github.com/facebook/rocksdb/wiki/RocksDB-Tuning-Guide)
- [fio documentation](https://fio.readthedocs.io/en/master/fio_doc.html)
