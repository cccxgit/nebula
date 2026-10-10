# Nebula Graph 空闲空间内存持续上涨调查报告

对应任务：[task-wal.md](../task-wal.md)。分析基线：`3.6-memory`，提交 `de9b3ed800a6627d9845e9289b6bbc5b6faf460a`。

本次工作只做问题解释、复现和观测。产品源码仅在 `src/kvstore/wal/AtomicLogBuffer.cpp` 增加开关控制的计数与限频日志；没有修改心跳、日志复制、容量计账、缓存淘汰、GC 或 RocksDB 写入行为。完整差异见 [observation.patch](observation.patch)，统计边界见 [instrumentation-notes.txt](instrumentation-notes.txt)。

## 1. 核心结论与阅读入口

空闲业务空间仍有内部写入。稳定运行的 Leader 在周期轮询中调用 `sendHeartbeat()`；当前没有复制任务时，它会追加一条空字符串日志。Leader 与 Followers 都把这条日志写入各自的 `FileBasedWal` 和 `AtomicLogBuffer`，随后在 RocksDB 中更新该分片的提交位点。

这里有三个需要分开的内存现象：

1. **WAL 有效缓存长期填充。** 每个本地分片副本各有一个缓存，默认 8MiB 上限只按自定义逻辑大小计算。空记录只计 16B，但本机构建的满节点平均分配为 56B/条。达到容量之前，没有按空闲时长或 WAL TTL 驱逐这些有效节点的路径。
2. **RocksDB 提交位点覆盖写。** 空业务 payload 会跳过业务操作，却仍执行 `putCommitMsg()` 和 batch commit。同一批系统键反复覆盖，MemTable 仍会积累版本、申请 arena 内存并触碰物理页。
3. **释放对象与 RSS 下降不同步。** 已归还给分配器的内存可能保留复用；`purge` 也不能释放仍由有效 WAL 节点或 MemTable 持有的内存。单看 RSS 无法判断是哪一层在增长、是否发生对象泄漏。

按配置模板 `raft_heartbeat_interval_secs=30`，基础轮询平均约 `30/3+0.2495=10.2495` 秒。50 空间×20 分片×3 副本部署在 3 个 storage 时，每个实例有 1000 个本地副本，不能只计算本机 Leader 的数量。缓存尚未填满时，仅 WAL 节点分配的条件推算约为 **450.2MiB/实例/天**；默认逻辑容量约需 **62.2 天**填满。这与任务描述的量级相符，但属于基于本分支配置、布局和节奏的推算，不能冒充生产实测。

正常读者生命周期下，原始缓存代码在达到容量后会进入平台，单分片约 28MiB 的 Node 分配；因此“长期增长”不自动等于“数学上无限的泄漏”。持有长期不释放的迭代器会阻止脏节点 GC，组件实验也验证了这种额外条件，但本次服务实验是否存在该条件必须另看计数，不能从现象倒推。

深入阅读请按以下顺序：

- [源码因果、定量模型与原始组件实验](COMPONENT-ANALYSIS.md)：原始代码直接运行的复现命令、完整数据表、默认 5 秒与模板 30 秒的区别。
- [Raft 逐函数解读](CODE-WALKTHROUGH-RAFT.md)：对象生命周期、选举、两条心跳路径、队列、复制与提交。
- [WAL 逐函数解读](CODE-WALKTHROUGH-WAL.md)：写盘/恢复、64 槽节点、链方向、计账、标删、迭代器、GC、reset、析构、TTL。
- [RocksDB 与内存统计逐函数解读](CODE-WALKTHROUGH-ROCKSDB.md)：系统键、batch、MemTable、flush、缓存共享、监控、purge、RSS。

## 2. 实验环境、构建与观测边界

| 项目 | 实际设置 |
|---|---|
| 系统 | Ubuntu 22.04，Linux 6.8，x86_64，24 个逻辑 CPU，约 31GiB RAM |
| 编译器/依赖 | GCC 11.4、GLIBC 2.35、CMake 3.26.5、工程 `/opt/vesoft/third-party/3.3`，RocksDB 7.5.3 |
| 构建 | Debug，`ENABLE_TESTING=OFF`，jemalloc 与 MemoryTracker 均启用 |
| 编译并行度 | 用户新增要求后停止本任务原 `-j8` 构建，使用 `make -j5` 增量完成；安装也用 `-j5` |
| 安装/工作目录 | `/home/sch/nebula-run/nebula-memory` |
| 拓扑 | 同机 1 metad、1 graphd、3 个独立 storaged 进程；每个 storage 独立数据目录/日志/PID/端口 |
| graph / meta 端口 | `29669` / `29559` |
| storage / HTTP 端口 | `29779/29789/29799` / `39779/39789/39799` |
| 空间与分片 | `wal_idle_01` 至 `wal_idle_50`；每空间 20 分片、3 副本、`FIXED_STRING(32)` VID |
| 业务负载 | 只创建空间；没有 INSERT/UPDATE/DELETE，没有创建 tag 或 edge schema |
| WAL 参数 | `wal_buffer_size=8388608`、`max_log_buffer_size=16777216`、`wal_ttl=14400`、`clean_wal_interval_secs=600` |
| RocksDB 参数 | `rocksdb_disable_wal=false`、`write_buffer_size=67108864`、`max_write_buffer_number=4`、共享 block cache 4MiB |
| 内存监控 | `memory_purge_enabled=true`、interval 10 秒、`system_memory_high_watermark_ratio=0.8` |
| 观测 | `wal_memory_observe=true`，进程级计数约每 10 秒一行；外部采样通常每 15 秒一次 |
| 文件句柄 | 启动子进程前设置 `RLIMIT_NOFILE` soft=65536，等价于要求的 `ulimit -n 65536` |

完整配置见 [configs](configs)、[flags-initial.json](flags-initial.json)；真实 PID、可执行文件和工作目录、文件句柄限制见 [process-manifest.json](process-manifest.json)。二进制 SHA-256、资源情况见 [runtime-environment.txt](runtime-environment.txt)，构建记录见 [configure.log](configure.log)、[build-j5.log](build-j5.log)、[install.log](install.log)。

元信息心跳 `heartbeat_interval_secs=1` 用于本地建空间后快速传播，它与 Raft 心跳参数不同。初始 Raft 参数使用配置模板的 30 秒；加速阶段改为 1 秒，之后恢复为 30 秒。这个实验改动只用于缩短复现等待时间。基础轮询期分别约为 10.2495 秒和 0.5825 秒，理想速率比约 17.6，**不是 30 倍**；最终比较使用实际日志计数。

## 3. 实验有效性与原始证据

三实例上线及空间数见 [hosts-before-spaces.log](hosts-before-spaces.log)、[create-spaces.log](create-spaces.log)。全量 `SHOW PARTS/TAGS/EDGES` 见 [cluster-inspect.log](cluster-inspect.log)：50 个空间各返回 20 个分片，各分片具有三个副本；不存在 tag/edge schema。最初的 Leader 分布为 370/514/116，但三台都具有 1000 个本地副本，三台空 WAL 的增长速率仍接近。

每条用于定量分析的采样行要求：`http_spaces_ok=50`、`http_spaces_failed=0`、`http_engines=50`、`wal_part_dirs=1000`、`wal_parse_errors=0`。同时核对进程 PID 与 `/proc/<pid>/stat` 的启动时钟字段保持不变，防止把重启前后内存混成一条曲线。采样器保留实际时间、耗时与 HTTP 原始响应；汇总采用首末有效样本之间的实际秒数，不把配置 duration 当作实际窗口长度。

WAL 有两种观测来源：

- 外部采样按文件首末 LogID 估算记录数，用于低开销连续记录；它是磁盘统计，不能单独等同当前内存缓存大小。
- [parse_wal.py](parse_wal.py) 逐条验证 `<LogID,TermID,length,ClusterID,payload,length>`，检查首尾长度、ID 连续性、任期回退、空/非空 payload。每个文件以打开时大小为边界，运行时扫描不是跨文件原子快照。

首次全量扫描得到 56,994 条记录，全部为空，任期均为 1，ID 缺口和不完整记录均为 0；见 [wal-before-acceleration.json](wal-before-acceleration.json)。这与无业务写的命令历史和源代码链共同支持心跳内部写入的判断。单独看到空 payload 并不足以证明唯一来源，因为源码中的 `Part::sync()` 也可以生成空命令。

观测搭建中的两类失败也保留了原始记录：本机 HTTP 代理使第一次 localhost 请求返回 502；逗号被编码为 `%2C` 后，当前 HTTP handler 按原始字符串拆分导致属性请求 400。采样脚本现直接连接 localhost 并保留参数中的逗号。`phase-01-warmup` 与 `phase-02-default30` 的失败行不参与结论。数据库未重启，缓存算法未改变。

## 4. 真实服务实测

### 4.1 时间轴与数据口径

以下时间均为 **2026-10-10，Asia/Shanghai（UTC+8）**。RSS统一采用各实例CSV的 `status_VmRSS_kib`，1KiB=1024B，1MiB=1024KiB；MemTable和Node字节则直接保留原始B单位。CSV与服务内的限频观测日志有各自的时间戳，下文分别计算，避免把不同首末时刻的计数强行相减。

| 阶段 | 条件 | 外部CSV实际首末窗口 | 每实例样本数 | 本阶段用途 |
|---|---|---|---:|---|
| `phase-00-no-spaces` | 尚未创建50个目标空间 | 07:00:31.688–07:01:21.700，约50秒 | 6 | 无目标空间的进程基线 |
| `phase-03-default30` | 50空间，Raft heartbeat=30 | 07:03:35.810–07:06:20.946，约165秒 | 12 | 模板节奏下验证内部写入及初始内存行为 |
| `phase-04-transition1` | 切换至heartbeat=1的过渡期 | 07:07:08.862–07:07:59.187，约50秒 | 6 | 保留参数切换、任期变化和RSS回落，单独观察 |
| `phase-05-accelerated1` | heartbeat=1，空闲加速阶段 | **07:08:08.994–07:22:54.118，约885秒** | **60** | 验证持续RSS增长与Node/MemTable增长 |

heartbeat=1的配置实际从 **07:07:08** 保持到 **07:28:01**，共约20分52秒；这不是约21分钟连续CSV。主分析窗口是07:08:09–07:22:54；采样任务07:23:09结束后，到07:28:01恢复参数前没有新的外部CSV采样，服务中的 `WAL_MEMORY_OBSERVE` 日志仍持续记录。不能把这段仅有服务计数日志的区间补作RSS曲线，也不能将配置持续时间替代885秒实际样本跨度。操作时间见 [events.jsonl](events.jsonl)，主要汇总见 [summary.json](summary.json)。

`phase-05`虽然配置duration为900秒，但每15秒采一次，60个采样点的首末间隔是59×15≈885秒。s1/s2/s3精确跨度分别为885.014、884.977、884.893秒。所有180行都具有50个成功空间、50个engine、1000个本地WAL分片目录，HTTP失败和WAL解析错误均为0；PID及进程启动时钟不变。本文没有将 `phase-01/02` 的失败HTTP样本填0或混入这些结果。

### 4.2 无空间基线：RSS并非完全静止，但没有目标分片WAL

`phase-00`尚未创建目标空间，每实例只有约55MiB进程RSS：

| 实例 | RSS起点（KiB） | RSS终点（KiB） | 增量（KiB） | 目标WAL分片目录 | 目标WAL记录/文件字节 |
|---|---:|---:|---:|---:|---|
| s1 | 56,636 | 56,848 | +212 | 0→0 | 0→0 / 0→0 |
| s2 | 56,764 | 56,980 | +216 | 0→0 | 0→0 / 0→0 |
| s3 | 56,904 | 57,124 | +220 | 0→0 | 0→0 / 0→0 |

这约50秒基线仍包含进程初始化、内部管理等行为，不能要求RSS绝对不动。由于查询的目标空间列表为空，MemTable属性在CSV里是缺测空字段，**不能解释为整个storage的RocksDB内存为0**；这个阶段也未触发目标分片的WAL诊断行，不能将缺失的Node观测伪装为一个已采到的计数值。

原始数据：[s1基线CSV](phase-00-no-spaces/s1.csv)、[s2基线CSV](phase-00-no-spaces/s2.csv)、[s3基线CSV](phase-00-no-spaces/s3.csv)。

### 4.3 模板30秒参数：空闲仍有写入，但这165秒没有新Node分配

50空间就绪后，在默认配置模板节奏下，每台约165秒都新增约1.6万条WAL记录，RocksDB active entries以接近相同速率增加：

| 实例 | RSS起点→终点（KiB） | RSS增量 | WAL记录估计起点→终点（增量） | active MemTable entries起点→终点（增量） |
|---|---:|---:|---:|---:|
| s1 | 151,252→152,080 | +828KiB（0.809MiB） | 11,929→27,988（+16,059） | 12,358→28,409（+16,051） |
| s2 | 151,160→151,892 | +732KiB（0.715MiB） | 11,949→27,990（+16,041） | 12,519→28,555（+16,036） |
| s3 | 150,872→151,880 | +1,008KiB（0.984MiB） | 11,962→27,991（+16,029） | 12,132→28,157（+16,025） |

三台的 `rocksdb.cur-size-all-mem-tables` 在整个窗口都为 **52,531,200B = 50×1,050,624B ≈50.098MiB**，共享block cache始终96B，table-reader memory始终0。active entries增加而MemTable字节不动，与已申请arena中的持续写入相容；字节台阶不变不代表没有内部写入。独立分析的12点拟合中，每台WAL约96.8–96.9条/秒，与配置模板下1000副本总计约97.6条/秒的量级一致；详情见 [runtime-rocks-analysis.txt](runtime-rocks-analysis.txt)。

这一阶段有一个必须明确的反证边界：**服务日志中的 `live_nodes` 始终为1000，`node_object_bytes`始终为3,200,000B，没有新增Node。** 每个分片尚在填充自己的第一个64槽Node，所以不能将上表RSS的小幅上涨直接归因为新的WAL Node分配。

| 同窗口内的服务观测（约07:03:42–07:06:15，实际约152.934秒） | s1 | s2 | s3 |
|---|---:|---:|---:|
| 观测日志条数 | 16 | 16 | 16 |
| `pushes`及`empty_pushes` | 12,001→27,001 | 12,001→27,001 | 12,001→27,001 |
| `live_nodes` | 1,000→1,000 | 1,000→1,000 | 1,000→1,000 |
| `node_object_bytes` | 3,200,000→3,200,000 | 3,200,000→3,200,000 | 3,200,000→3,200,000 |
| `live_logical_bytes` | 192,016→432,016 | 192,016→432,016 | 192,016→432,016 |
| `freed / gc_runs / gc_freed / resets` | 全部0 | 全部0 | 全部0 |

逻辑字节增加240,000B，恰对应15,000条空记录×16B；这是现有槽位填充，不是同量的新堆分配。RSS仍可能受arena内页面触碰、其它系统对象和分配器行为影响，本段证据不能给这不足1MiB的残差做唯一归因。

原始数据：[s1默认参数CSV](phase-03-default30/s1.csv)、[s2默认参数CSV](phase-03-default30/s2.csv)、[s3默认参数CSV](phase-03-default30/s3.csv)，以及 [s1观测日志CSV](wal-observe-s1.csv)、[s2观测日志CSV](wal-observe-s2.csv)、[s3观测日志CSV](wal-observe-s3.csv)。

### 4.4 加速空闲阶段：885秒内每台RSS增加约157MiB

切换后的 `phase-04` 并非单调增长：三台RSS分别下降25,344、25,356、25,388KiB，而同期观测Node由1000增加到2000、并没有Node释放。这里存在与WAL Node增长不同的进程内存变化，本报告不将它强行归给某一类对象，也不把过渡期隐藏起来。以下持续上涨结论限定为过渡后的60点主窗口。

在 `phase-05` 的每一台进程中，**59个相邻RSS采样间隔全部上涨**；同时匿名RSS增加量与总RSS增加量相等，主要增量不是文件映射页：

| 实例 | RSS起点（KiB / MiB） | RSS终点（KiB / MiB） | RSS增量（KiB / MiB） | `RssAnon`起点→终点（KiB） |
|---|---:|---:|---:|---:|
| s1 | 131,964 / 128.871 | 293,512 / 286.633 | **161,548 / 157.762** | 94,040→255,588 |
| s2 | 132,756 / 129.645 | 293,616 / 286.734 | **160,860 / 157.090** | 94,724→255,584 |
| s3 | 132,248 / 129.148 | 293,680 / 286.797 | **161,432 / 157.648** | 94,148→255,580 |

s3首个样本中 `smaps_Rss_kib=132252`，比先读取的 `status_VmRSS_kib=132248` 高4KiB，两次 `/proc` 读取不是原子快照。这里统一使用VmRSS列，避免在首尾或实例间悄悄切换统计口径。

![真实服务内存、WAL节点与MemTable采样](runtime-memory.png)

原始60点CSV：[s1](phase-05-accelerated1/s1.csv)、[s2](phase-05-accelerated1/s2.csv)、[s3](phase-05-accelerated1/s3.csv)。图中的跨阶段留白与采样窗口应按上文时间轴阅读，不应将没有CSV的07:23–07:28区间理解为已经观测到连续RSS。

#### 4.4.1 同一CSV窗口内，WAL与RocksDB计数同时增加

| 实例 | WAL记录估计起点→终点（增量） | active MemTable entries起点→终点（增量） | MemTable字节起点→终点 | MemTable增量 |
|---|---:|---:|---:|---:|
| s1 | 134,128→1,651,964（+1,517,836） | 134,190→1,652,021（+1,517,831） | 52,531,200→142,708,736 | **90,177,536B / 86MiB** |
| s2 | 134,333→1,652,071（+1,517,738） | 134,549→1,652,232（+1,517,683） | 52,531,200→151,097,344 | **98,566,144B / 94MiB** |
| s3 | 134,590→1,652,178（+1,517,588） | 134,529→1,652,024（+1,517,495） | 52,531,200→157,388,800 | **104,857,600B / 100MiB** |

每实例WAL记录首末差约1715条/秒，即每本地副本约1.715条/秒、平均约0.583秒一条，与heartbeat=1时基础轮询均值0.5825秒一致。与默认30秒阶段相比，实际速率约快17.7倍，不能按参数比值直接说成30倍。

三个实例的记录增量接近，而最初Leader分布明显不同，支持“followers也缓存并提交日志，内存倍率应按1000个本地副本计算”。WAL与MemTable entries绝对值和增量的细小差异不能当作丢日志：两类统计采样时刻不同，follower commit可略滞后，初始engine/part创建也会写系统记录。

整个主窗口中，三个实例的immutable memtable数量与entries均为0；table-reader memory保持0，共享block cache保持96B。这排除了“此窗口主要是这50个空间的SST reader或block cache在填充”的解释；同时明确观测到active MemTable增长，不能将约157MiB RSS全部归给WAL。

#### 4.4.2 服务内的Node计数直接证明WAL堆对象持续保留

取主CSV窗口内部的首末限频日志，三个实例的时间均约为 **07:08:15.607–07:22:45.607，870.001秒以内**，各有88条观测。它比CSV首末窗口约短15秒，因此以下Node计数与上一表RSS/MemTable不能构成逐字节一致的会计等式。

| 进程内观测字段 | s1 | s2 | s3 |
|---|---:|---:|---:|
| `pushes`起点→终点 | 145,419→1,637,577 | 145,419→1,637,577 | 145,419→1,637,577 |
| `empty_pushes`增量 | **1,492,158** | **1,492,158** | **1,492,158** |
| `created`及`live_nodes` | **3,000→26,005** | **3,000→26,005** | **3,000→26,005** |
| `node_object_bytes` | 9,600,000→83,216,000 | 9,600,000→83,216,000 | 9,600,000→83,216,000 |
| Node对象字节增量 | **73,616,000B（70.206MiB）** | **73,616,000B（70.206MiB）** | **73,616,000B（70.206MiB）** |
| `live_logical_bytes` | 2,326,704→26,201,232 | 2,326,704→26,201,232 | 2,326,704→26,201,232 |
| iterator `releases`增量 | 2,465,183 | 2,607,998 | 2,387,606 |
| `freed / gc_runs / gc_freed / resets` | 全程0 | 全程0 | 全程0 |

所有观测行的 `pushes==empty_pushes`；采样到的那个buffer的 `buffer_dirty_nodes=0`、`buffer_refs=0`，其逻辑size最大26,624B，远小于8MiB容量。这里的buffer字段是一行中单个buffer的瞬时状态，不能冒充对1000个buffer同时取快照；不过进程级新增23,005个Node、没有任何Node释放，以及大量iterator release共同说明，本次明显增长处于**有效缓存继续填充、未触发淘汰**的阶段，不能把它描述成“从来没有reader释放所以GC卡住”。

组件实验实测本机jemalloc的Node分配档位为3584B。按同一布局把23,005个新增Node换算为usable量级约 **82,449,920B（78.630MiB）**。这不是服务端直接采得的allocator allocated字段，表中直接观测的是Node数和 `sizeof(Node)=3200`；换算只用于评估分配量级，不能与不同窗口、不同含义的MemTable/RSS数字机械相加。

#### 4.4.3 全量WAL复核与任期边界

恢复heartbeat=30后，于07:28:02–07:28:08做了另一次逐条WAL扫描，见 [wal-accelerated-end.json](wal-accelerated-end.json)。三实例各有1000个分片文件、2,179,564条记录，合计 **6,538,692条，全部payload为空**；ID缺口、非递增ID、任期回退、不完整尾记录均为0。该扫描晚于主CSV终点约5分钟，不应将其记录总数填入07:22:54的CSV终点。

文件中同时存在term=1和term=2：每实例387个分片的最后term仍为1，613个分片的最后term为2。因此不能声称整个参数切换过程没有任期变化。所有文件最高term为2，也不能仅凭一次全量扫描推断变化的精确秒数；主分析使用过渡期之后的实际追加速率、Node计数和MemTable/RSS，不把理想定时公式当成未经验证的事实。与切换前全部term=1的扫描相比，这一差异已经保留在原始证据中。

源码、原始组件实验和这次服务运行形成了相互独立的证据：空闲空间持续追加空记录；记录跨过64槽边界后，真实进程的Node数量不断增加且尚无delete；同一阶段RocksDB提交位点覆盖写也增加MemTable；约885秒内三个进程都出现了持续的RSS上涨。该结论无需把诊断补丁当成修复：产品源码差异仍仅为124行观测新增，原始未修改实现的组件实验验证了相同的缓存机制。

### 4.5 恢复30秒与FLUSH对照

07:28:01恢复heartbeat=30，先保留 `phase-06-transition30`，再从07:28:31开始 `phase-07-restored30`。后者每实例12个有效点，首末约165秒；没有重启进程，也没有重置WAL缓存。

| 实例 | RSS起点→终点（KiB） | RSS增量 | WAL记录增量 | WAL实际速率（12点OLS，条/秒） | MemTable字节起点→终点 |
|---|---:|---:|---:|---:|---:|
| s1 | 348,912→350,924 | +2,012KiB | +16,639 | 99.674 | 157,388,800→162,631,680 |
| s2 | 349,312→351,128 | +1,816KiB | +16,679 | 99.801 | 157,388,800→157,388,800 |
| s3 | 350,124→351,936 | +1,812KiB | +16,685 | 99.864 | 182,554,624→182,554,624 |

WAL追加速率由加速阶段约1715条/秒降回约100条/秒，表明动态恢复参数确实改变了内部写入节奏。RSS没有回到加速前基线，且仍小幅净增加；恢复慢速不是停止心跳，也不会使已经填充的有效缓存失效。此阶段RSS有小幅上下波动，不再宣称每个相邻点都上涨。

在约07:28:41–07:31:14的内层WAL观测窗口，三个实例均有：

- `empty_pushes`增加15,000，`live_nodes`由34,694增加到34,967。
- `node_object_bytes`从111,020,800B增加到111,894,400B，即再增加873,600B。
- `freed / gc_runs / gc_freed / resets`均保持0；恢复参数没有释放先前节点。

这些进程的MemTable还处于不同arena填充位置：s1跨过了额外5MiB的聚合分配台阶，s2/s3尚未跨新的台阶。不能据此把不同实例的RSS差异解释成不同的WAL记录保留策略。逐行数据见 [恢复后s1](phase-07-restored30/s1.csv)、[恢复后s2](phase-07-restored30/s2.csv)、[恢复后s3](phase-07-restored30/s3.csv)；独立计算及OLS见 [runtime-metrics.json](runtime-metrics.json)。

### 4.6 FLUSH对照：MemTable可以回落，但WAL节点没有随之释放

本对照保持heartbeat=30，并继续接收空心跳提交。07:35:45先完成 `phase-08-beforeflush` 的显式刷盘前采样，再向50个空间提交FLUSH作业，随后采集 `phase-09-afterflush` 的18个点，首末约170秒。

**phase09的名字不代表它的第一个点已经处在“全部FLUSH完成后”。** 第一组点位于07:35:45.905–07:35:46.026，此时s1/s2/s3分别仍有11/11/10个immutable memtable，MemTable聚合值为89,387,304 / 66,422,912 / 58,926,768B，处于异步刷盘进行中；RSS此时也短暂高于显式刷盘前值。因此不能拿这些首点当作完成后的稳态，也不能只比较phase09首尾代替真正的刷盘前后对照。

50个作业ID为51–100。后续 [flush-job-verification.json](flush-job-verification.json) 于07:36:41核实：50个互不重复的作业全部 `FINISHED`。命令、返回与作业状态原文见 [flush.log](flush.log)、[job-status.log](job-status.log)。完成状态的核验时间与MemTable采样时间分别保留，不将作业提交成功当作作业完成。

下表对比刷盘前、phase09观测谷值、约30秒点及末点；RSS仍使用VmRSS。三个实例的MemTable谷值与RSS谷值恰好落在各自的约10秒采样点，但这不是对更高时间分辨率下绝对最低值的保证。

| 实例/阶段 | 本地采样时间（约） | MemTable聚合字节 | active entries | RSS（KiB） |
|---|---|---:|---:|---:|
| s1，显式刷盘前 | 07:35:45.404 | **181,506,048** | 2,224,560 | **352,912** |
| s1，观测谷值（约10秒） | 07:35:55.913 | **102,400** | 1,000 | **243,652** |
| s1，约30秒 | 07:36:15.914 | 52,531,200 | 2,996 | 244,352 |
| s1，末点（约170秒） | 07:38:35.913 | **52,531,200** | 16,216 | **246,512** |
| s2，显式刷盘前 | 07:35:45.464 | **157,388,800** | 2,224,676 | **352,920** |
| s2，观测谷值（约10秒） | 07:35:55.984 | **102,400** | 1,000 | **243,904** |
| s2，约30秒 | 07:36:15.984 | 52,531,200 | 2,996 | 244,888 |
| s2，末点（约170秒） | 07:38:35.992 | **52,531,200** | 16,231 | **247,004** |
| s3，显式刷盘前 | 07:35:45.529 | **182,554,624** | 2,224,367 | **353,784** |
| s3，观测谷值（约10秒） | 07:35:56.047 | **102,400** | 1,000 | **243,716** |
| s3，约30秒 | 07:36:16.038 | 52,531,200 | 2,996 | 244,580 |
| s3，末点（约170秒） | 07:38:36.063 | **52,531,200** | 16,249 | **246,580** |

显式刷盘前到末点的变化为：

| 实例 | MemTable减少 | RSS减少 | 最近刷盘前/末点WAL观测 |
|---|---:|---:|---|
| s1 | 128,974,848B，**123MiB** | 106,400KiB，**103.906MiB** | Node 35,076→35,501，未释放 |
| s2 | 104,857,600B，**100MiB** | 105,916KiB，**103.434MiB** | Node 35,076→35,501，未释放 |
| s3 | 130,023,424B，**124MiB** | 107,204KiB，**104.691MiB** | Node 35,076→35,501，未释放 |

用于这张表的WAL观测时间约为07:35:39.74和07:38:33.39，分别比对应外部采样早约5.7秒和2.5秒；它们用于确认物理释放方向，不冒充精确同时快照。三台在该观测跨度均又追加17,000条空记录，Node对象字节从112,243,200增加到113,603,200B，`freed/gc_runs/gc_freed/resets`仍为0。仅取phase09内部日志时，起点已经是35,085个Node，故增量为416；两种增量差异来自起点选择，不能混用。

这一结果明确区分了两类对象：**FLUSH使RocksDB MemTable退出并伴随进程RSS明显回落，但没有让Raft WAL的有效缓存节点退出。** 它是诊断对照，不是修改缓存算法的修复。刷盘后RSS仍远高于无空间基线，但也不能把剩余RSS全部算成WAL，因为进程还有新MemTable、管理存储、连接、线程和分配器等。

谷值102,400B恰为50×2048B；实际上约20秒点就已回到50×1,050,624B，当时active entries分别为1,999/2,000/2,000，约40条/空间；约30秒点则已增至2,996条。空心跳始终继续写提交位点，已足以让新MemTable进入1MiB arena分配档位。它与独立组件20次写仍在初始档位、60次写已经跨档的观察一致，但不能宣称“精确第60条才会扩容”，也不能把单线程组件的单条平均字节成本硬套服务。**MemTable属性重新增加约50MiB，并没有使RSS同期再增加50MiB**：谷值到30秒，三台RSS只增加700/984/864KiB。这直接说明arena分配统计与即时驻留页不是同一个计量，不能机械相加闭账；64MiB write buffer配置没有被改成1MiB或3MiB。

完成刷盘后的table-reader memory为每实例110,150B，共享block cache仍为96B。相比刷盘前，进程线程数153→163、文件驻留页增加约1.1MiB，管理作业也会使用HTTP50空间以外的对象。因此RSS下降与目标MemTable减少量不完全相等有明确的统计范围差异，不能据此反推遗漏或泄漏。

原始数据：[刷盘前s1](phase-08-beforeflush/s1.csv)、[s2](phase-08-beforeflush/s2.csv)、[s3](phase-08-beforeflush/s3.csv)；[刷盘阶段s1](phase-09-afterflush/s1.csv)、[s2](phase-09-afterflush/s2.csv)、[s3](phase-09-afterflush/s3.csv)。独立复算结果见 [runtime-metrics.json](runtime-metrics.json) 的 `flush_comparison`，机制与OPTIONS说明见 [runtime-rocks-analysis.txt](runtime-rocks-analysis.txt)。

### 4.7 任期稳定性、停止核验与原始WAL归档

进一步将07:08:38开始与07:28:02开始的两次全量WAL快照，按实例、空间、分片匹配：**3000个共同文件，最后term变化数为0**，见 [term-stability.json](term-stability.json)。独立分析还按term计数核对，两快照之间新增5,985,274条记录，全部属于各文件在起始快照时已经存在的最后term，没有其它term新增，见 [runtime-metrics.json](runtime-metrics.json) 的 `full_wal_snapshot_comparison`。

这与前面“参数切换过渡期间部分分片从term1进入term2”不矛盾：07:08:38以后，观察到的是在既有稳定任期中持续追加空日志，而不是靠不断产生新任期解释增长。两次扫描分别历时约0.7秒和6秒，并非跨3000文件的原子快照；其开始时间也不等同于phase05 CSV的07:08:09起点，因此这项任期证据覆盖的是明确给出的快照区间，不能扩大成“整个启动及切换过程完全没有选举”。

实验结束后恢复了30秒参数并停止本次独立环境的1个graphd、1个metad和3个storaged。**07:39:05核验这5个预期PID全部退出**，见 [shutdown-verification.json](shutdown-verification.json)。这里没有把进程退出后所有内存归还作为运行期间GC生效的证据。

随后07:39:28–07:39:34对静止数据做最终扫描，结果如下：

| 实例 | 分片文件数 | 最终记录数 | WAL字节数 |
|---|---:|---:|---:|
| s1 | 1,000 | 2,243,520 | 71,792,640 |
| s2 | 1,000 | 2,243,522 | 71,792,704 |
| s3 | 1,000 | 2,243,532 | 71,793,024 |
| 合计 | **3,000** | **6,730,574** | **215,378,368** |

最终 **6,730,574条全部为空payload**；非空记录、ID缺口、缺失ID、非递增ID、term回退、不完整记录、扫描时文件变化均为0。term只包含1和2。各实例最后记录数有极小差异，停止命令按进程依次发出，因此不能要求停机末刻完全相同；表中使用各自真实文件结果，没有对齐或补齐记录。

最终证据已经固化：

- [wal-full.json](wal-full.json)：逐文件、分片、实例及总体的最终静态校验；[wal-full.csv](wal-full.csv)便于重新筛选统计。
- [wal-evidence.tar.gz](wal-evidence.tar.gz)：3000个原始WAL文件，解压后总计215,378,368B；[wal-evidence-files.txt](wal-evidence-files.txt)记录归档文件清单。
- [wal-archive-verification.json](wal-archive-verification.json)：归档验证3000文件、0个不匹配，SHA-256为 `acb2e767bf72b9b715879c6a911e53ef115e30469f2185e8e4b3205ad988fd55`。

这些文件使“没有业务payload、空日志实际数量、任期范围、内存计数来源及完整运行时间”能够独立复查。源码仍只保留观测新增，没有将本次FLUSH对照或参数加速替换成产品行为修复。

## 5. 为什么这些结果不能只用“泄漏”两个字解释

`AtomicLogBuffer` 的链表包含有效节点与已经标删、尚未物理回收的节点。增长的正常填充阶段，`dirtyNodes=0`，节点仍被缓存有意持有；释放 reader 不会删除有效节点。原始组件实验中，1000 个缓存各写入 4096 条空记录，产生 64,000 个 Node，jemalloc usable 总量为 229,376,000B，仍无脏节点。一个默认缓存写到 524,288 条附近达到逻辑 8MiB；继续写至 1,100,000 条，在正常 iterator 释放条件下节点数进入平台。

如果没有任何 iterator 析构，或者总有一个 iterator 被持有，`releaseRef()` 的回收入口就无法满足条件，脏节点可以继续累积。这是另一个已经由组件构造实验验证的机制，但要用服务中的 `dirtyNodes/refs/gc` 等证据证明，不能与本次有效缓存填充混写。

RocksDB 的内存属性也是分配台阶，不等于“有效 key/value 字节”。本实验每个 DB 的 `arena_block_size=1048576`。独立组件 20 次覆盖写仍报告 2048B，60 次覆盖写已报告 `2048+1MiB=1050624B`。因此服务初始每空间约 1MiB 并非凭空出现的特殊缓存；平台期间，后续写入仍可消耗这块 arena 的剩余容量并触碰更多页面。详见 [runtime-rocks-analysis.txt](runtime-rocks-analysis.txt) 和 [实际 RocksDB OPTIONS 汇总](runtime-rocks-options-comparison.json)。

同一实例还包括隐藏的 space 0 adminStore、Raft/Thrift 连接、线程栈、统计对象、分配器 arena 元数据与已释放页等。HTTP 采样聚合的是 50 个目标空间，不能把“这 50 个空间的 MemTable + Node 对象大小”当成整个 RSS 的精确会计等式。共享 block cache 属性采用全进程最大值，不按 50 空间重复求和。

磁盘 `wal_ttl` 只参与磁盘文件回收；磁盘清理不调用 `AtomicLogBuffer::reset()`，也没有按 TTL 删除有效内存节点。当前每个分片的日志文件远小于默认 16MiB，并且清理实现至少保留两个文件，因而不能把“设了 4 小时 TTL”理解为“4 小时后所有空日志内存必然释放”。

## 6. 可复现步骤

以下命令针对本次建立的独立环境。原始数据、配置及采样证据已经保留；重新采样使用新的输出目录名。`prepare` 会拒绝覆盖已经非空的默认数据目录，本次首次运行时该安装目录不存在。

### 6.1 编译与安装

观测补丁已经保留在工作区。若另建相同提交的干净 checkout，可检查并应用 [observation.patch](observation.patch)。

```bash
cd /home/sch/nebula/nebula-3.6-memory
mkdir -p build
cd build
cmake -DCMAKE_INSTALL_PREFIX=/home/sch/nebula-run/nebula-memory \
  -DENABLE_TESTING=OFF -DCMAKE_BUILD_TYPE=Debug ..
make -j5
make -j5 install
```

三个服务二进制均由本 checkout 构建。组件实验另行从固定提交提取未加观测的原始 WAL `.cpp/.h`，用直接调用证明行为并非由服务观测补丁引入。

### 6.2 首次建立与核实空集群

```bash
cd /home/sch/nebula/nebula-3.6-memory
python3 task/wal-investigation/cluster.py prepare
python3 task/wal-investigation/cluster.py start
python3 task/wal-investigation/cluster.py register
/home/sch/nebula-run/nebula-console -addr 127.0.0.1 -port 29669 \
  -u root -p nebula -e 'SHOW HOSTS;'
```

`start` 调用 `AGENTS.md` 指定安装目录的 `scripts/nebula.service`，storage 使用 `-c` 指定三个配置文件；每次启动都设置 65536 文件句柄限制。先确认三个 HOST ONLINE，再采无空间基线并建空间：

```bash
python3 task/wal-investigation/cluster.py sample --empty \
  --name repeat-no-spaces --duration 60 --interval 10
python3 task/wal-investigation/cluster.py create
python3 task/wal-investigation/cluster.py inspect
```

建空间后等待分片/Leader 就绪；全量 `SHOW PARTS`、50 空间和每实例 1000 个 WAL 分片目录都应成立。控制台在同一行执行多条语句时只展示最后结果，所以 `inspect.ngql` 已把各个 SHOW 分开成行。

### 6.3 按实际心跳节奏采样与加速

```bash
python3 task/wal-investigation/cluster.py sample \
  --name repeat-default30 --duration 180 --interval 15
python3 task/wal-investigation/cluster.py flags --heartbeat 1
python3 task/wal-investigation/cluster.py sample \
  --name repeat-transition1 --duration 60 --interval 10
python3 task/wal-investigation/cluster.py sample \
  --name repeat-accelerated1 --duration 900 --interval 15
python3 task/wal-investigation/cluster.py flags --heartbeat 30
```

动态切换逐实例生效，已经排队的轮询不会立刻重排；`checkAlive()` 还有首次调用锁存超时的实现细节。因此过渡期单独保留，并用采样中的任期变化与日志真实速率校验稳态；不把配置切换当成瞬时、严格等比例的实验操作。

### 6.4 恢复慢速、刷盘对照、保存 WAL 与停止

```bash
python3 task/wal-investigation/cluster.py sample \
  --name repeat-transition30 --duration 30 --interval 10
python3 task/wal-investigation/cluster.py sample \
  --name repeat-restored30 --duration 180 --interval 15
python3 task/wal-investigation/cluster.py sample \
  --name repeat-before-flush --duration 1 --interval 10
python3 task/wal-investigation/cluster.py flush
python3 task/wal-investigation/cluster.py jobs
python3 task/wal-investigation/cluster.py sample \
  --name repeat-after-flush --duration 180 --interval 10
python3 task/wal-investigation/cluster.py jobs
```

FLUSH 命令只提交作业，必须结合 `SHOW JOBS/SHOW JOB` 确认 50 个作业完成，并观察 MemTable 属性；不能仅凭控制台返回就称为刷盘完成。它用于分离 RocksDB 与 WAL 两类对象，没有改变 WAL 缓存的回收逻辑。

```bash
python3 task/wal-investigation/cluster.py stop
# 核实 process-manifest.json 中的本次 PID 已经退出，再做静止文件扫描。
python3 task/wal-investigation/parse_wal.py \
  --data-dir s1=/home/sch/nebula-run/nebula-memory/data/storage1 \
  --data-dir s2=/home/sch/nebula-run/nebula-memory/data/storage2 \
  --data-dir s3=/home/sch/nebula-run/nebula-memory/data/storage3 \
  --output task/wal-investigation/repeat-wal-full
```

本次完整操作顺序和时间以 [events.jsonl](events.jsonl) 为准，采样与日志留在任务目录；运行数据留在上述独立安装目录。

### 6.5 不启动服务，独立复查原始 WAL

下面只解包本次已经停止写入的证据并运行解析器。选择新的解压和输出目录，保留交付时的原始结果：

```bash
cd /home/sch/nebula/nebula-3.6-memory
sha256sum task/wal-investigation/wal-evidence.tar.gz
# 应为 acb2e767bf72b9b715879c6a911e53ef115e30469f2185e8e4b3205ad988fd55
WAL_REPLAY_DIR=$(mktemp -d /tmp/nebula-wal-evidence.XXXXXX)
tar -xzf task/wal-investigation/wal-evidence.tar.gz -C "$WAL_REPLAY_DIR"
python3 task/wal-investigation/parse_wal.py \
  --data-dir s1="$WAL_REPLAY_DIR/data/storage1" \
  --data-dir s2="$WAL_REPLAY_DIR/data/storage2" \
  --data-dir s3="$WAL_REPLAY_DIR/data/storage3" \
  --output "$WAL_REPLAY_DIR/rechecked"
```

应得到3000个文件、6,730,574条空记录、215,378,368B，总体无异常。解析结果的路径和扫描时间会随解压目录、复查时间改变，不应逐字比较整个JSON。解析器格式与并发边界说明见 [wal-parser-notes.txt](wal-parser-notes.txt)，10项合成测试结果见 [wal-parser-selftest.log](wal-parser-selftest.log)。

### 6.6 从已有采样复算指标和图表

两个分析程序都只读取任务目录中的证据，不请求数据库。它们会更新衍生JSON/图表及生成时间；如需验证交付文件完整性，应在重算前执行校验：

```bash
cd /home/sch/nebula/nebula-3.6-memory/task/wal-investigation
sha256sum -c SHA256SUMS
python3 analyze_runtime_rocks.py
python3 -m venv /tmp/nebula-wal-plots-venv
/tmp/nebula-wal-plots-venv/bin/pip install -r plot-requirements.txt
/tmp/nebula-wal-plots-venv/bin/python analyze_results.py
```

`runtime-metrics.json` 保留逐阶段、逐实例的首末点、实际窗口、OLS斜率、RSS列选择、Node观测窗口、FLUSH选定时点和任期比较。`runtime-memory.png/.svg` 中RSS曲线读取 `smaps_Rss_kib`；正文表统一读取 `status_VmRSS_kib`，两种读数均来自原始CSV。图表在超过30秒的外部采样缺口处断线，不插值虚构观测。

## 7. 结论适用范围

本报告解释的是当前提交中能够由源码、原始组件实验和本地三实例实验串联验证的机制。没有生产进程的配置快照、heap profile、缓存填充年龄及长读者证据，所以不能宣称已经排除了生产环境的一切其它原因，也不能把短时加速实验说成连续运行了 62 天。

每天约 450MiB 是模板参数下 WAL Node usable 的条件推算，不包括 RocksDB、RPC、线程、分配器额外开销，也不是进程 RSS 必然每天增加的精确数值。真实斜率受业务复制、轮询执行耗时、已有缓存填充程度、分片数和 GC/flush 等影响。

本次没有实现修复，也没有以实验中的心跳/容量配置作为生产优化建议。调查保留完整命令、配置、源码观测补丁、原始日志、采样 CSV 和校验程序，供后续按生产实际参数验证。
