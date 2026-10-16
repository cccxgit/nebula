# 开发启动前的复现实验核对与安全复跑说明

核对日期：2026-10-11。本文核对已有文件，未启动数据库、未修改产品源码、未修改任何原始实验结果。适用源码基线为 `de9b3ed800a6627d9845e9289b6bbc5b6faf460a`；服务实验仅加入 `observation.patch` 中的日志和计数观测，缓存算法没有修复性修改。

**直接结论：已有证据证明，在没有业务数据的 50 个空间中，后台空日志仍使 WAL 缓存和 RocksDB 写缓冲增长；不能把全部 RSS 增长归给 WAL。原始 WAL 组件另已证明，正常释放读者时，小容量会形成更低的空日志缓存平台。这支持空间级容量配置的需求，但尚未证明新机制的正确性、混部性能或生产收益。** 开发者应先离线复查证据，再在独立部署中做服务验证。

## 1. 已执行的拓扑与参数

| 对象 | 已执行的配置或操作 | 依据 |
|---|---|---|
| 进程 | 同一台机器上 1 个 metad、1 个 graphd、3 个 storaged | `process-manifest.json`、`cluster.py` |
| 安装目录 | `/home/sch/nebula-run/nebula-memory` | `cluster.py` 的 `INSTALL`；实际进程 cwd |
| 控制台 | `/home/sch/nebula-run/nebula-console` | `cluster.py` 的 `CONSOLE` |
| Meta 端口 | 服务 29559，HTTP 39559 | 保存的配置 |
| Graph 端口 | 服务 29669，HTTP 39669 | 保存的配置 |
| Storage 端口 | 服务 29779/29789/29799，HTTP 39779/39789/39799 | 保存的配置 |
| 空间 | `wal_idle_01` 到 `wal_idle_50`，各 20 分区、3 副本、`FIXED_STRING(32)` | `create-spaces.ngql`、`cluster-inspect.log` |
| 本地副本 | 每个 storaged 1000 个，集群合计 3000 个 | 完整采样和 WAL 归档 |
| Leader 分布 | 一次检查为 370/514/116；不是三台各 1000 个 Leader | `cluster-inspect.log` 中完整 `SHOW HOSTS` |
| 业务数据 | 没有创建 Tag/Edge，没有执行业务 INSERT | 创建与全空间检查日志；归档内所有日志载荷为空 |
| Raft 日志容量 | 每副本 8,388,608 B | flags、内部观测中的 `buffer_capacity` |
| Raft 心跳 | 初始 30 秒，实验中临时改为 1 秒，再恢复 30 秒 | `events.jsonl`、flags 快照 |
| Meta 心跳 | `heartbeat_interval_secs=1`；与上述 Raft 参数不同 | 保存的配置 |
| RocksDB WAL | `rocksdb_disable_wal=false` | 保存的配置 |
| 文件数限制 | 子进程启动前设为 65536，实际进程 limits 已确认 | `cluster.py::limit_files`、`process-manifest.json` |
| 构建 | Debug，测试关闭，jemalloc 与 MemoryTracker 开启 | `environment.txt`、配置和构建日志 |

三个 storage 进程同机运行用于观察机制，不能代表跨机器网络、独立磁盘或生产容器限额的性能。完整工程复编译继续遵循用户要求 **`make -j5`**；AGENTS 中示例的 `-j8` 不能用于本任务。

## 2. 因果关系如何被证据区分

空闲空间没有新增业务数据，不等于分区没有日志。后台周期活动仍追加空日志；每条空日志在容量计算中只计 16 B，但内存里仍占用完整记录槽，64 个槽组成一个节点。因此，即使每分区新增很少的计账字节，1000 个本地副本也会持续增加节点。

空日志提交还会推进分区的提交位置。RocksDB 会写入同一个系统键的新版本，所以“逻辑键的数量没变”不等于写缓冲没有增长。执行 FLUSH 后写缓冲明显下降，而 WAL 节点仍增加，正好把两个贡献分开。

这里的 RSS 是操作系统观察到的进程驻留内存；MemTable 是 RocksDB 尚未刷盘的写缓冲；WAL Node 是 Raft 日志缓存中的分配对象。三者不是同一个统计口径。`node_object_bytes` 是节点数乘 `sizeof(Node)`，不含分配器取整；组件中的 `node_malloc_usable_bytes` 才是逐个查询分配器可用大小后的和。不能把它们直接凑成完全闭合的 RSS 账。

## 3. 服务实验的准确窗口与结果

### 3.1 正常参数窗口

`phase-03-default30` 为正常 Raft 间隔 30 秒下的完整定量窗口：命令申请采样 180 秒，实际有 12 个点，首尾相隔约 165 秒。采样时刻间隔为 15 秒，不能把命令的 duration 直接当作首尾跨度。

| 实例 | RSS 起点 → 终点（MiB） | RSS 增量（MiB） | 磁盘 WAL 记录估计增量 | MemTable 合计 |
|---|---:|---:|---:|---:|
| s1 | 147.707 → 148.516 | 0.809 | 16,059 | 52,531,200 B，不变 |
| s2 | 147.617 → 148.332 | 0.715 | 16,041 | 52,531,200 B，不变 |
| s3 | 147.336 → 148.320 | 0.984 | 16,029 | 52,531,200 B，不变 |

内部观测的相邻窗口中，每实例 Node 都保持 1000 个，表明此时每副本仍在使用首个 64 槽节点。不能凭这 165 秒的节点不变推出长时间不增长。

### 3.2 加速窗口

`phase-05-accelerated1` 在 Raft 参数改为 1 秒并经过过渡段后开始。命令申请 900 秒；实际 60 个点，首尾约 885 秒，三实例都在 59 个相邻区间中 RSS 上升。

| 实例 | RSS 起点 → 终点（MiB） | RSS 增量（MiB） | MemTable 增量（MiB） |
|---|---:|---:|---:|
| s1 | 128.871 → 286.633 | 157.762 | 86 |
| s2 | 129.645 → 286.734 | 157.090 | 94 |
| s3 | 129.148 → 286.797 | 157.648 | 100 |

该段中共享 block cache 仅为 96 B，SST table-reader 属性为 0，未出现不可变 MemTable。每实例磁盘 WAL 记录估计增加约 151.8 万条。

内部 WAL 诊断在上述窗口中的覆盖时间约 **870 秒**，每实例新增 1,492,158 条空日志，Node 从 3000 增到 26005，增加 23005 个，即 `23005 × 3200 = 73,616,000 B`（70.206 MiB）的节点对象。期间 `freed/gc_runs/resets` 都为 0，但读者释放计数持续增加。采样到的单缓存计账字节仍远低于 8 MiB，结合组件与源码，这支持“有效缓存仍在填充”，不支持将该阶段定性为“已经淘汰但被读者卡住”。

**不能将 870 秒节点增量与 885 秒 MemTable/RSS 增量强行做精确总和。** 内部计数是低频、跨线程采样，也不是所有副本同时刻的原子快照。

1 秒参数实际从北京时间 2026-10-10 07:07:08.775 左右持续到 07:28:01.075，约 20 分 52 秒。其中 07:23:09 到 07:28:01 没有连续的外部 RSS CSV；不能画成持续采样完整覆盖的曲线。WAL 内部日志和前后磁盘扫描提供了其他证据。

切换到 1 秒后的 `phase-04-transition1` 曾先下降约 25 MiB RSS，原始记录保留，尚无独立证据闭合这次下降的来源，不能从曲线中隐藏。

### 3.3 FLUSH 对照

使用 `phase-08-beforeflush` 的单点作为操作前基准，以 `phase-09-afterflush` 的最后一行作为操作后观测，后者距离其第一行约 170 秒。**不能把 phase-09 的第一行叫作 FLUSH 前或全部任务完成后**：提交动作此前已发生，而且 FLUSH 是异步任务。

| 实例 | 操作前 RSS（MiB） | 后续终点 RSS（MiB） | 差值（MiB） | MemTable 下降 |
|---|---:|---:|---:|---:|
| s1 | 344.641 | 240.734 | -103.906 | 123 MiB |
| s2 | 344.648 | 241.215 | -103.434 | 100 MiB |
| s3 | 345.492 | 240.801 | -104.691 | 124 MiB |

50 个 FLUSH 作业后来均确认 `FINISHED`，见 `flush-job-verification.json`。终点的 MemTable 为每实例 52,531,200 B；中间曾降至 102,400 B，之后空日志提交又为各空间分配初始写缓冲。同期 WAL Node 继续增加，FLUSH 没有淘汰有效 WAL 缓存。

### 3.4 停机归档提供的事实

`shutdown-verification.json` 确认五个实验进程在北京时间 2026-10-10 07:39:05 已全部退出；随后归档和核验了 3000 个 WAL 文件，合计 215,378,368 B、6,730,574 条记录，全部 payload 为空，无半截记录、ID 缺口或 term 回退。归档 SHA-256：

```text
acb2e767bf72b9b715879c6a911e53ef115e30469f2185e8e4b3205ad988fd55
```

term 为 1 和 2，不能说全程没有换届；`term-stability.json` 只证明加速段的两次快照之间，3000 个对应文件的最后 term 没有变化。这是对“增长段是否由反复选举主导”的约束，不是任意时刻选举次数的完整审计。

## 4. 组件实验：支持方案的量化证据

### 4.1 不同容量

`production-tuning-evidence/capacity-*.csv` 使用固定提交的原始 `AtomicLogBuffer`，每次 push 后用真实 iterator 读取最新日志并立即释放。下表选取四倍填充记录数时的采样，此时 dirty 为 0，refs 为 0。

| 逻辑容量 | 累计空记录数 | 活节点 | 节点分配器可用字节 | 换算 MiB |
|---|---:|---:|---:|---:|
| 8 MiB | 2,097,152 | 8192 | 29,360,128 | 28 |
| 4 MiB | 1,048,576 | 4096 | 14,680,064 | 14 |
| 2 MiB | 524,288 | 2048 | 7,340,032 | 7 |
| 1 MiB | 262,144 | 1024 | 3,670,016 | 3.5 |
| 256 KiB | 65,536 | 256 | 917,504 | 0.875 |

证明：正常读者退出时，容量减小会形成较低的空日志平台；8 MiB 是计账容量，不是 8 MiB 的实际堆内存上限。不证明：并发大批次的硬内存上限、整个 storage/cgroup 的上限，或某档值的吞吐和延迟安全性。

### 4.2 空闲与持续有内容的日志

`write-idle-comparison-20261011` 也是原始组件，不是完整服务。8 MiB 计账容量下，空日志充分填充约占 28 MiB 节点分配；每条固定 1 KiB 内容时，样本占约 10.4375 MiB 节点加字符串分配。停止有内容写入，继续追加 600,000 条空日志后，约为 28.010 MiB。

驱动填充字符串 `x` 仅模拟缓存层 payload 大小，不是可直接提交给图状态机的业务编码。该结果说明“空日志每条计账少，单位计账容量容纳更多对象”；不能说所有真实写负载都恰好占 10.44 MiB，更不能称作“50 个空间持续写入的服务实测”。

### 4.3 独立 RocksDB 对照

`rocks-commit-probe.csv` 使用同工程 RocksDB 7.5.3，5 个独立 DB、每 DB 20 个固定系统键，累计 100 万次覆盖写，最终仍只有 100 个逻辑键。MemTable 从 10,240 B 增至 52,439,040 B，FLUSH 后回到 10,240 B；allocated 明显下降而 RSS 未立即下降。此实验隔离证明提交位置覆盖写本身可以使写缓冲增长，不能用 WAL 容量参数把这部分消掉。

## 5. 最优先的复查方式：不启动任何服务

以下命令从工程根目录执行，属于**给开发者的建议复查步骤**；本次文档核对没有重跑它们。文件输出均使用新目录，避免覆盖证据。源码读取、归档解压、离线解析不接触现网。

### 5.1 复查归档

先执行 `sha256sum task/wal-investigation/wal-evidence.tar.gz` 并核对上面的摘要；查看 `tar -tzf` 结果，归档成员应位于相对路径 `data/storage1..3/...`。

```bash
cd /home/sch/nebula/nebula-3.6-memory
audit_root=$(mktemp -d task/wal-investigation/design-evidence/offline.XXXXXX)
tar -xzf task/wal-investigation/wal-evidence.tar.gz -C "$audit_root"
python3 task/wal-investigation/parse_wal.py \
  --data-dir "s1=$audit_root/data/storage1" \
  --data-dir "s2=$audit_root/data/storage2" \
  --data-dir "s3=$audit_root/data/storage3" \
  --output "$audit_root/reparsed" > "$audit_root/parse.log" 2>&1
```

这些 `parse_wal.py` 参数已逐项核对实际 argparse。`--output` 不带后缀时生成 `.json/.csv`；已有报告会拒绝覆盖。检查退出状态、`has_anomalies=false` 和 `total`，应与第 3.4 节一致。解析器核查记录边界、ID/term 顺序，但该文件格式没有 payload checksum，不能将结果解释成密码学级内容完整性校验。

### 5.2 复跑一个原始 WAL 容量样本

本机现成的二进制可直接运行；换机器需按 `run-atomic-buffer-probe.sh` 的编译选项重新构建。二进制接口已经确认是 `PARTITIONS CAPACITY each|none|held ROUND,ROUND,...`。

```bash
cd /home/sch/nebula/nebula-3.6-memory
probe_run=$(mktemp -d task/wal-investigation/design-evidence/capacity.XXXXXX)
task/wal-investigation/atomic-buffer-probe-jemalloc \
  1 1048576 each 0,1,64,65,65536,65537,65856,131072,262144 \
  > "$probe_run/capacity-1m.csv" 2> "$probe_run/capacity-1m.meta"
```

这会启动一个独立组件进程，不启动数据库，不读取服务数据。应检查最后一个 `push` 样本 `node_malloc_usable_bytes=3670016`，`destroy_all` 样本节点归零；RSS 不要求立即回零。每条记录后立即释放读者是此预测成立的前提。

完整五档矩阵的实际命令保存在 `production-tuning-evidence/run-capacity-probe.sh`。它对原结果有覆盖保护，直接重跑会因已有 8 MiB CSV 而停止。需要复制脚本并修改 `out` 指向新目录；不要删除旧结果来绕过保护。该脚本仅核对并复用原二进制，没有重新构建。

### 5.3 原有脚本的覆盖风险

| 文件 | 可以读取的内容 | 原位执行需注意 |
|---|---|---|
| `cluster.py` | 服务实验流程和真实参数接口 | 有配置写入、服务启停、元数据操作；只适用于新的专用实验部署 |
| `run-atomic-buffer-probe.sh` | 原始源码提取、完整编译和组件矩阵 | 会覆盖 baseline、环境记录、二进制和原 CSV；不能原位复跑 |
| `production-tuning-evidence/run-capacity-probe.sh` | 五档容量矩阵 | 对关键已有结果拒绝覆盖；复制并换 `out` |
| `write-idle-comparison-20261011/run.sh` | payload 与停写对照 | 对已有结果拒绝覆盖；复制目录并修改 `probe_out` |
| `analyze_results.py` | 服务日志与采样汇总逻辑 | `ROOT` 为自身目录，会覆盖 summary、wal-observe 和图；先复制完整证据到新目录 |
| `test_parse_wal.py` | 解析器构造边界测试 | 会覆盖固定 `wal-parser-selftest` 内人工数据；复制脚本和解析器到新目录再运行 |
| `sample_runtime.py` | 活服务的只读采样 | 不修改内核状态，但有 HTTP/文件读取开销；输出 CSV 已存在会拒绝覆盖 |
| `rocks-commit-probe` | 真实 RocksDB 系统键覆盖写对照 | 会创建新数据库；驱动拒绝复用已有数据目录，CSV/meta 必须也给新名字 |

## 6. 完整服务复现的移交步骤

下面是根据实际 CLI 整理的**建议复跑顺序**，不是本轮执行记录。该流程会创建实验元数据、改变实验心跳并提交 FLUSH，只能在独立测试环境运行。先确认主机资源足够，且没有通过目录、端口、PID、数据路径指向业务实例。

### 6.1 准备独立部署，而不是复用历史安装目录

1. 将构建产物安装或复制到一个新的专用安装目录，包含 `bin` 和 `scripts`；安装前核对路径。若重编译，最大 `make -j5`。新构建应记录 commit、配置、编译器、分配器及二进制散列。
2. 在仓库 `task/` 下新建与 `wal-investigation` 同级的新目录，复制 `cluster.py`、`sample_runtime.py` 到其中。**保持这一级关系**，因为 `cluster.py` 用 `HERE.parent.parent` 定位仓库；若放到 `task/wal-investigation/子目录`，必须显式修正 `REPO`。
3. 修改复制版脚本的 `INSTALL` 为新安装目录，核对 `CONSOLE`。选择一组未使用且互不冲突的端口。除 `META_PORT/GRAPH_PORT/PORTS/STORAGE_PORTS` 外，`prepare()` 中还直接写了 HTTP 39559、39669、39779，必须一并检查和替换；同时预留服务实际使用的 Raft/内部派生端口，不能只检查 HTTP 端口。
4. 所有实验日志、配置副本、事件、CSV 都由复制版脚本写到它所在的新目录。不要把输出复制回原证据目录。
5. 若启用 `wal_memory_observe`，二进制必须包含已有观测补丁；原始发布二进制不认识该新增 flag。观测补丁只是诊断，不应作为新功能已经实现的证据。
6. 启动前执行 `ulimit -n 65536`；脚本对子进程另有同样设置。核对 `prepare()` 生成的所有配置后才启动。

`cluster.py prepare` 仅在 `INSTALL/data` 非空时拒绝继续，不检查自定义外部 data_path、已有配置和运行中的服务。`prepare` 随后会覆盖指定安装目录的 `etc` 配置，以及当前脚本目录内 `configs/spaces.txt/create-spaces.ngql`。它不能当作通用生产部署工具。

### 6.2 命令顺序

先在 shell 中将 `repro_script` 设置为**已完成上述核查的复制版**脚本路径，例如仓库 `task/` 下的新实验目录。以下参数都来自现有脚本的实际 argparse，不包含尚未实现的新空间级 flag。

```bash
ulimit -n 65536
python3 "$repro_script" prepare
python3 "$repro_script" start
python3 "$repro_script" register
```

启动命令只有固定等待，不是就绪屏障。必须用新部署的控制台确认三个 `SHOW HOSTS` 均 ONLINE，再继续；nGQL 输出要逐条检查，不能只看控制台退出码。

```bash
python3 "$repro_script" sample --name baseline-empty --duration 60 --interval 10 --empty
python3 "$repro_script" create
python3 "$repro_script" inspect
```

`create` 执行的 nGQL 来自生成文件：

```ngql
CREATE SPACE IF NOT EXISTS wal_idle_01(partition_num=20, replica_factor=3, vid_type=FIXED_STRING(32));
```

同样建到 `_50`。确认 50 个空间，各 20 分区、完整副本及已选出的 Leader；`inspect` 当前实际实现会检查全部 50 个空间的 PARTS/TAGS/EDGES。早期 `cluster-review.txt` 对“只检查第一个空间”的意见针对当时版本，该条已经不适用于当前脚本，不能照抄。

元数据传播和引擎创建结束后才采集定量窗口。检查 CSV 中每实例 `http_spaces_ok=50`、`http_spaces_failed=0`、`http_engines=50`、`wal_part_dirs=1000`、`wal_parse_errors=0`、PID 与启动时刻稳定。

```bash
python3 "$repro_script" sample --name normal30 --duration 180 --interval 15
python3 "$repro_script" flags --heartbeat 1
python3 "$repro_script" sample --name transition1 --duration 60 --interval 10
python3 "$repro_script" sample --name accelerated1 --duration 900 --interval 15
python3 "$repro_script" flags --heartbeat 30
python3 "$repro_script" sample --name transition30 --duration 30 --interval 10
python3 "$repro_script" sample --name restored30 --duration 180 --interval 15
python3 "$repro_script" sample --name beforeflush --duration 0 --interval 10
python3 "$repro_script" flush
python3 "$repro_script" jobs
python3 "$repro_script" sample --name afterflush --duration 180 --interval 10
python3 "$repro_script" jobs
```

`flags` 逐实例更新，不是三实例原子切换；若任一失败，必须读回全部值并恢复一致状态。`flush` 只提交，`jobs` 也只查询一次；需记录 50 个本次作业 ID 并复查至全部 FINISHED。采样中的早期点应明确属于执行中。

结束时，使用复制版 `cluster.py stop` 仅停止新实验部署；它不等待实际退出，也不将停止失败视为命令错误。必须核实新实验所有 PID 已退出，才能对数据目录做静态归档。保存进程、配置、操作日志、CSV、原始 WAL、校验和及退出核对记录。

### 6.3 加速参数的解释边界

`raft_heartbeat_interval_secs=1` 只用于让本地复现更快出现明显变化。已有代码的一些计时值每轮重读，另有 `static const` 在首次调用时锁存，运行中修改不能让所有计时点瞬间同步。它会改变 Raft 工作频率和时间判定，不能当作生产推荐值，也不能把 1 秒结果按恰好 30 倍换算成生产时间。

正常配置 30 秒时，一次轮询的代码延时约为 `30秒/3 + 随机0..499毫秒`；空闲日志节奏因此不能简单理解为“30 秒恰好一条”。复现应记录实际 WAL 增量和 term 稳定性，不用理论倍数替代观测。

## 7. 新需求验收还缺哪些证据

以下内容目前均未被已有实验完成，需求设计应列为开发后的验证项，不能写成已验证结论：

- 同一进程内不同空间选择不同容量、未覆盖空间保持旧默认，且跨节点配置一致。
- 新机制关闭时与旧版本行为一致；启动参数无效、空间身份不匹配、重复项等错误处理符合设计。
- 高频/中频/低频业务混部中的 p99 延迟、吞吐、错误率、磁盘读取、复制滞后及完整恢复时间。
- 最大写入批次、大 payload、长时间读者、内存高水位下的压力与崩溃恢复。
- 50 个空间持续真实业务写入的完整服务 RSS，以及停写数周后的长期平台。
- 生产 Docker/cgroup 限额下的真实内存高水位错误链路；本次没有在生产复现 OOM 或限流拒绝。
- 空间级容量配置是否能单独满足现网总内存预算：现网本地冷副本数、有效参数和 MemTable 等构成仍需采证。

最可靠的既有结论是“空闲后台活动会同时增加 WAL 缓存与写缓冲，WAL 容量缩小能降低其正常填充平台”。空间级配置是隔离冷热参数的候选机制，不能代替进程内存预算和混部性能验收。下一步应按需求定义完成配置正确性测试，再在相同业务负载下对比基线与候选实现。

## 8. 文件索引

- 简明机制：[REPORT.md](../REPORT.md)；完整历史实验：[EXPERIMENT-DETAILS.md](../EXPERIMENT-DETAILS.md)。
- 样本：[phase-03-default30](../phase-03-default30/manifest.json)、[phase-05-accelerated1](../phase-05-accelerated1/manifest.json)、[phase-08-beforeflush](../phase-08-beforeflush/manifest.json)、[phase-09-afterflush](../phase-09-afterflush/manifest.json)。
- 内部计数：[wal-observe-s1.csv](../wal-observe-s1.csv)、[wal-observe-s2.csv](../wal-observe-s2.csv)、[wal-observe-s3.csv](../wal-observe-s3.csv)。
- 完整解析：[wal-full.json](../wal-full.json)；归档核对：[wal-archive-verification.json](../wal-archive-verification.json)。
- 服务流程：[cluster.py](../cluster.py)；采样器：[sample_runtime.py](../sample_runtime.py)；离线解析器：[parse_wal.py](../parse_wal.py)。
- 容量实验：[production-tuning-evidence/README.md](../production-tuning-evidence/README.md)。
- payload 对照：[write-idle-comparison-20261011/README.md](../write-idle-comparison-20261011/README.md)。
- RocksDB 独立复现：[rocks-commit-probe-reproduction.txt](../rocks-commit-probe-reproduction.txt)。
