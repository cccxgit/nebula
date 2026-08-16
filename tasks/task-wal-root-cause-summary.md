# Nebula Graph 3.6 storaged 内存持续增长：总结报告

> 范围：只总结根因，不讨论解决方案。
>
> 分支/基线：<code>3.6-w-1</code> /
> <code>de9b3ed800a6627d9845e9289b6bbc5b6faf460a</code>
>
> 代码行号：Raft 等产品逻辑按当前基线；AtomicLogBuffer 因只读观测补丁有行号偏移，按当前实验
> 工作区标注，并同时写明符号。
>
> 结论置信度：高。

完整推导、排除过程和全部证据见
[task-wal-root-cause-analysis.md](./task-wal-root-cause-analysis.md)。
如果希望先建立 Raft、WAL、AtomicLogBuffer 和 RocksDB 的源码心智模型，请按顺序阅读
[task-wal-source-code-walkthrough.md](./task-wal-source-code-walkthrough.md)。
未插桩提交中的 Atomic Record/Node、push、GC 位置分别为
<code>AtomicLogBuffer.h:23-39,44-111</code>、
<code>AtomicLogBuffer.cpp:71-119,167-214</code>；下表链接统一指向当前可直接审阅的工作区。
本次构建使用的 RocksDB 版本由
[build/CMakeCache.txt:462-465](../build/CMakeCache.txt#L462-L465) 与实际 installed header
<code>/opt/vesoft/third-party/3.3/include/rocksdb/version.h:14-16</code> 确认为 7.5.3。下表使用
[版本匹配的 upstream 7.5.3 参考源码](../../rocksdb-7.5.3/include/rocksdb/version.h#L14-L16)
解释内部路径；未证明静态库与 sibling 源码逐文件 bit-identical，实际行为以运行 property 为主。

## 1. 一句话结论

Nebula Graph 3.6 的稳定 Raft Leader 在空闲时，每轮结束仍会按
<code>H/3 + random(0..499ms)</code> 计划下一轮，并追加一条空 <code>NORMAL</code> 日志。缺陷是
[RaftPart::sendHeartbeat，RaftPart.cpp:2041-2049](../src/kvstore/raftex/RaftPart.cpp#L2041-L2049)
只判断 <code>!replicatingLogs_</code>，没有判断代码已维护的 <code>commitInThisTerm_</code>。

空日志复制到所有副本后，分别形成：

1. 每个 replica-part 同轮写 WAL/AtomicLogBuffer 并长期保留 live Node，形成主要、确定性的线性增长；
2. 每个 space × data_path 的 RocksDB 持续更新分片 commit key；Follower 通常在下一次 AppendLog
   才提交上一条。两点采样观察到附加 active-memtable property 聚合增长及单 DB 字节值离散档位；
   当前短跑未验证完整 flush 后形态。

它不是典型的无引用对象泄漏，而是仍被正常引用的对象被长时间保留；在发布默认 H=30、8MiB buffer、
固定拓扑且不 reset/drop/析构时，触及逻辑容量的名义时间约 62.2 天。生产表现与泄漏相同。

## 2. 根因源码定位表

| 编号 | 结论 | 源码位置（关键入口附符号） |
|---|---|---|
| C1 | 稳定 Leader 的计划延迟是 <code>H×1000/3+rand32(500)</code>，不是 H 秒；实际 start-to-start 周期另含执行/调度开销 | [RaftPart.cpp:1138-1141，needToSendHeartbeat](../src/kvstore/raftex/RaftPart.cpp#L1138-L1141)；[1401-1434，statusPolling](../src/kvstore/raftex/RaftPart.cpp#L1401-L1434) |
| C2 | 3.6 发布模板 H=30、WAL TTL=14400s | [nebula-storaged.conf.default:44-50](../conf/nebula-storaged.conf.default#L44-L50) |
| C3 | 每轮空闲时追加空 NORMAL 日志 | [RaftPart.cpp:2041-2049，sendHeartbeat](../src/kvstore/raftex/RaftPart.cpp#L2041-L2049) |
| C4 | 当前 term commit 状态已经存在，却未用于 C3 | [RaftPart.h:855-857](../src/kvstore/raftex/RaftPart.h#L855-L857)；[RaftPart.cpp:1388](../src/kvstore/raftex/RaftPart.cpp#L1388)；[1091-1094](../src/kvstore/raftex/RaftPart.cpp#L1091-L1094)；[2254-2261](../src/kvstore/raftex/RaftPart.cpp#L2254-L2261) |
| C5 | 上一轮复制完成会把 <code>replicatingLogs_</code> 复位，下一轮又满足 C3 | [RaftPart.cpp:1103-1127](../src/kvstore/raftex/RaftPart.cpp#L1103-L1127) |
| C6 | 真正 Heartbeat RPC 是独立路径，Follower handler 不追加日志 | [RaftPart.cpp:2051-2122](../src/kvstore/raftex/RaftPart.cpp#L2051-L2122)；[1895-1951，processHeartbeatRequest](../src/kvstore/raftex/RaftPart.cpp#L1895-L1951) |
| C7 | 空日志先写 Leader WAL，空 logMsg 装入 RPC，再写 Follower WAL | [RaftPart.cpp:786-915](../src/kvstore/raftex/RaftPart.cpp#L786-L915)；[918-999](../src/kvstore/raftex/RaftPart.cpp#L918-L999)；[Host.cpp:287-345，prepareAppendLogRequest](../src/kvstore/raftex/Host.cpp#L287-L345)；[RaftPart.cpp:1757-1777](../src/kvstore/raftex/RaftPart.cpp#L1757-L1777) |
| C8 | 每个本地 Part 有独立 FileBasedWal 和 AtomicLogBuffer | [NebulaStore.cpp:437-515](../src/kvstore/NebulaStore.cpp#L437-L515)；[Part.cpp:23-50](../src/kvstore/Part.cpp#L23-L50)；[RaftPart.cpp:330-372](../src/kvstore/raftex/RaftPart.cpp#L330-L372)；[FileBasedWal.cpp:40-60](../src/kvstore/wal/FileBasedWal.cpp#L40-L60) |
| C9 | 空 WAL 编码 32B，写完后每副本都 push buffer | [ThriftTypes.h:12-18](../src/common/thrift/ThriftTypes.h#L12-L18)；[FileBasedWal.cpp:442-500，appendLogInternal](../src/kvstore/wal/FileBasedWal.cpp#L442-L500) |
| C10 | 空 Record 只计 16B；一个 Node 内联 64 个 Record | [AtomicLogBuffer.h:18](../src/kvstore/wal/AtomicLogBuffer.h#L18)；[40-56，Record::size](../src/kvstore/wal/AtomicLogBuffer.h#L40-L56)；[61-128，Node](../src/kvstore/wal/AtomicLogBuffer.h#L61-L128) |
| C11 | push 按逻辑容量标脏；存活 buffer 的运行期旧 Node 只在 releaseRef GC 删除 | [AtomicLogBuffer.cpp:119-172，push](../src/kvstore/wal/AtomicLogBuffer.cpp#L119-L172)；[220-268，releaseRef](../src/kvstore/wal/AtomicLogBuffer.cpp#L220-L268)；整体析构另见 [79-96](../src/kvstore/wal/AtomicLogBuffer.cpp#L79-L96) |
| C12 | FileBasedWal 的 TTL 清理只删磁盘文件，不访问内存 buffer | [NebulaStore.cpp:24](../src/kvstore/NebulaStore.cpp#L24)、[72](../src/kvstore/NebulaStore.cpp#L72)、[1293-1321](../src/kvstore/NebulaStore.cpp#L1293-L1321)；[RaftPart.cpp:492-495，cleanWal](../src/kvstore/raftex/RaftPart.cpp#L492-L495)；[FileBasedWal.cpp:640-706，cleanWAL](../src/kvstore/wal/FileBasedWal.cpp#L640-L706) |
| C13 | 空 payload 跳过业务 op，但 batch 仍写 systemCommitKey；RocksWriteBatch 将抽象 put 桥接到 RocksDB Put，并把同一 batch 交给 db Write | [Part.cpp:215-232](../src/kvstore/Part.cpp#L215-L232)、[349-358](../src/kvstore/Part.cpp#L349-L358)、[403-410](../src/kvstore/Part.cpp#L403-L410)；[NebulaKeyUtils.cpp:101-108](../src/common/utils/NebulaKeyUtils.cpp#L101-L108)；[RocksEngine.h:181-216，RocksWriteBatch](../src/kvstore/RocksEngine.h#L181-L216)；[RocksEngine.cpp:120-140，commitBatchWrite](../src/kvstore/RocksEngine.cpp#L120-L140) |
| C14 | 每个 space × data_path 创建一个 RocksEngine | [NebulaStore.cpp:354-370](../src/kvstore/NebulaStore.cpp#L354-L370)、[395-421](../src/kvstore/NebulaStore.cpp#L395-L421)；memtable 配置见 [nebula-storaged.conf.default:98-104](../conf/nebula-storaged.conf.default#L98-L104) |
| C15 | 实验配置未覆盖该选项，因而走默认 <code>inplace_update_support=false</code> 路径：RocksDB 7.5.3 Put 创建带 sequence 的 memtable entry；write_buffer_size 是单 buffer 目标且可同时持有多个 buffer | [advanced_options.h:328-340](../../rocksdb-7.5.3/include/rocksdb/advanced_options.h#L328-L340)；[write_batch.cc:1950-1989](../../rocksdb-7.5.3/db/write_batch.cc#L1950-L1989)、[2075-2081](../../rocksdb-7.5.3/db/write_batch.cc#L2075-L2081)；[memtable.cc:535-637，MemTable::Add](../../rocksdb-7.5.3/db/memtable.cc#L535-L637)；[options.h:172-188](../../rocksdb-7.5.3/include/rocksdb/options.h#L172-L188)；[advanced_options.h:249-261](../../rocksdb-7.5.3/include/rocksdb/advanced_options.h#L249-L261) |

## 3. 主路径：AtomicLogBuffer 为什么持续增长

### 3.1 空闲 Leader 的实际生成频率

由 C1、C2：

~~~text
H=30 时：
固定延迟 = 30,000 / 3 = 10,000ms
随机延迟均值 = mean(0..499) = 249.5ms
计划延迟均值（名义周期） = 10.2495s
~~~

后继 timer 在本轮结束时才注册，所以实际 start-to-start 周期还包含执行、线程调度与 timer 抖动；
10.2495 秒仅是忽略这些开销的名义值。

C3 每轮在 <code>replicatingLogs_ == false</code> 时追加空日志；C5 表明健康复制完成后该标志会复位。
C4 则证明“当前 term 已经 commit”的状态存在，却没有限制 C3。因此稳定 term 并非只追加一次
no-op，而是继续周期追加。

### 3.2 三副本都会保留

C7 显示空日志走普通 AppendLog 链；C9 显示 Leader 和 Follower 每次 WAL 写后都进入本地
AtomicLogBuffer。真正 Heartbeat RPC 的 Follower 路径 C6 明确不 append，因此不能把增长归因于 RPC
消息对象本身。

决定单机斜率的是本地 replica-part 数，不是该机 Leader 数。生产示例中：

~~~text
50 spaces × 20 parts × RF3 / 3 storaged = 1000 replica-parts/storaged
~~~

### 3.3 为什么 8MiB buffer 会保留约 25～28MiB Node

默认参数位于
[FileBasedWal.cpp:15-18](../src/kvstore/wal/FileBasedWal.cpp#L15-L18)：每 part 逻辑 buffer=8MiB。
由 C10，空记录账面只计 16B，但对象中还有 <code>std::string</code>。

本次 Debug ABI/allocator 实测
([abi-sizes.txt](./wal-lab/evidence/abi-sizes.txt))：

| 项 | 大小 |
|---|---:|
| <code>sizeof(Record)</code> | 48B |
| <code>sizeof(Node)</code> | 3200B |
| jemalloc Node usable | 3584B |
| 满 Node 后长期摊销 requested/空日志 | 50B |
| 满 Node 后长期摊销 usable/空日志 | 56B |

C11 的容量判断使用 <code>Record::size()</code>，所以约 8MiB / 16B = 524,288 条以后才开始标脏；
由于满 head 分支提前新建 Node，首次标脏约晚 2 条。H=30 且忽略执行/调度额外延迟时，名义约需
62.2 天。

达到这个量级前，Node 是仍由链表引用的 live object。当前 ABI 下约 8192 个满 Node 即约 25MiB
requested/28MiB jemalloc usable/part，边界处另有少量 partial/dirty Node。C12 证明 WAL TTL 只删
磁盘文件，不回收这些 Node。

50B/56B 是满 Node 的长期摊销；短窗还包含每 buffer 的 partial head。3200B/3584B 依赖本次构建，
不是跨 ABI 常量。稳定的源码事实是 16B 逻辑计费和每 Node 64 条。

## 4. 次路径：RocksDB commit-key memtable

C13 表明：

- <code>commitLogs()</code> 先更新 lastId/term；
- 空 payload 只跳过业务数据；
- iterator/batch 结束后仍更新 <code>systemCommitKey(partId)</code>；
- [RocksWriteBatch::put/data](../src/kvstore/RocksEngine.h#L181-L216) 将抽象 batch 桥接到 RocksDB Put，
  并由 [RocksEngine::commitBatchWrite](../src/kvstore/RocksEngine.cpp#L124-L140) 最终进入
  <code>db_->Write()</code>。

严格语义是“每个 commit iterator/batch 一次 commit-key Write”，不是无条件每条日志一次。Leader 在
[RaftPart.cpp:1068-1095](../src/kvstore/raftex/RaftPart.cpp#L1068-L1095) 提交当前批；Follower 通常在
下一次 AppendLog 按 committed id 提交上一批
([RaftPart.cpp:1785-1804](../src/kvstore/raftex/RaftPart.cpp#L1785-L1804))。稳定空载时长期速率相同，
但时序滞后一轮。

C14 表明每个 space × data_path 一个 RocksEngine。本实验只有一条 data path，因此每台 50 个 DB。
发布配置的 <code>write_buffer_size=64MiB</code> 是单 active memtable 的目标值，并有
<code>max_write_buffer_number=4</code>；64MiB 不是每空间总内存硬上限。对应 RocksDB 7.5.3
语义见 C15。在本实验配置实际采用的默认 <code>inplace_update_support=false</code> 路径下，同一个
commit user key 的连续 Put 会以新的 sequence 进入 memtable；这与本次 active-entry property
运行计数一致。

## 5. 3+3+3 实验结果

完整实验启动了 3 metad、3 storaged、3 graphd；创建 50 个空空间，每空间 20 分片、RF=3，无 tag、
edge 和 DML。

三个 storage 配置的被测参数位置：

- [nebula-storaged-1.conf:18-39](./wal-lab/conf/nebula-storaged-1.conf#L18-L39)；
- [nebula-storaged-2.conf:18-39](./wal-lab/conf/nebula-storaged-2.conf#L18-L39)；
- [nebula-storaged-3.conf:18-39](./wal-lab/conf/nebula-storaged-3.conf#L18-L39)。

为加速目标链，将 Raft H 改为 1；WAL buffer/file/TTL 和 RocksDB write-buffer 保持发布语义。目标
持久化参数之外，九份同机配置还显式设置或调整了端口、路径、meta heartbeat、metad 默认分片数、
内存 flags 和线程池；这不是相对模板的穷举清单，完整取值以配置文件为准。实验 DDL 显式指定
<code>PARTITION_NUM=20</code>，metad 默认值不决定本次拓扑。线程池变化可能影响绝对调度延迟，但
实测速率与源码公式约 1% 内吻合，未显示本负载下的显著偏移。

### 5.1 空日志和 Node

原始数据：[wal-runtime-samples.csv](./wal-lab/evidence/wal-runtime-samples.csv)。

| 阶段 | 本地副本/进程 | 理论 push/s | 三台名义实测 push/s | 结论 |
|---|---:|---:|---:|---|
| 10 空间 | 200 | 343.348 | 345.984～346.213 | 约 1% 内吻合 |
| 50 空间 | 1000 | 1716.738 | 1717.342～1717.848 | 约 1% 内吻合 |

CSV 是整数秒时间戳且三台顺序采集，不能宣称万分级精度。

1000 副本、184 秒阶段：

| storaged | push/empty 增量 | 逻辑 B/push | WAL B/push | Node 增量 | RSS 增量 |
|---|---:|---:|---:|---:|---:|
| 1 | 315,991 / 315,991 | 16.000 | 约 32 | 4,989 | 32.09MiB |
| 2 | 316,084 / 316,084 | 16.000 | 约 32 | 4,989 | 33.31MiB |
| 3 | 316,074 / 316,074 | 16.000 | 约 32 | 4,989 | 32.58MiB |

所有样本满足：

~~~text
pushes / 64 <= nodes <= pushes / 64 + buffer instances
~~~

这分别命中 C10/C11 的 16B、每 64 条一个 Node 和 partial-head 结构。采样期
<code>dirty_nodes=0</code>、refs 回到 0，排除了 reader 卡住已标脏 Node 作为早期主因。

三台 Leader 数为 325 / 145 / 530，但 push 增量近似相同，运行结果与 C7 的 Follower WAL 路径一致。

### 5.2 RocksDB

原始数据：[rocksdb-memtable-samples.csv](./wal-lab/evidence/rocksdb-memtable-samples.csv)。
两次约隔 105 秒：

| storaged | active entries 增量 | 约 entries/s | active memtable bytes |
|---|---:|---:|---:|
| 1 | 178,548 | 1700 | 52,531,200 → 63,016,960 |
| 2 | 178,843 | 1703 | 52,531,200 → 97,619,968 |
| 3 | 178,950 | 1704 | 54,628,352 → 63,016,960 |

entry 速率跟随空日志频率，验证 C13 在当前稳定空载构建中近似一条空日志一个 commit batch。采样脚本
[sample-rocksdb-memtables.sh:25-27](./wal-lab/sample-rocksdb-memtables.sh#L25-L27) 读取的 property
在 RocksDB 7.5.3 中定义为 active entries 总数与 active memtable 近似字节数
（[db.h:920-934](../../rocksdb-7.5.3/include/rocksdb/db.h#L920-L934)）。两个采样点直接证明其聚合值
增长，并显示单 DB 字节 property 取值呈离散档位；不足以证明完整时间曲线。arena/flush 判断位置在
[memtable.cc:153-200，MemTable::ShouldFlushNow](../../rocksdb-7.5.3/db/memtable.cc#L153-L200)。
将离散档位归因于 arena 分块是结合版本匹配源码的解释。当前短跑未覆盖完整 flush 周期，不断言
flush 后 active bytes 或 RSS 的精确形态。

## 6. 发布默认配置下的条件估算

用户未提供现网 H、wal_buffer、data_path 数及商业二进制 ABI/allocator。以下仅在这些条件成立时：

- H=30；
- wal_buffer=8MiB；
- 1000 replica-parts/storaged；
- Node=3200B、jemalloc usable=3584B。

估算为：

~~~text
空 pushes ≈ 8,429,679/天/storaged
Node requested ≈ 401.958MiB/天/storaged
jemalloc usable ≈ 450.193MiB/天/storaged
逻辑容量触及时间 ≈ 62.2 天/part
~~~

H=1 实测按 C1 的计划延迟名义值缩放得到约 402.175MiB/day requested；考虑执行/调度额外开销和
时间戳精度，应表述为与名义静态公式在约 1% 内吻合。

同机九实例可验证调用链、对象结构和 replica-part 线性比例；绝对调度延迟与总 RSS 仍受同机资源
竞争、Debug 构建和 instrumentation 影响。loopback 结果也不能代表跨主机网络延迟、丢包、拥塞、
重试及其在途内存。现网数字必须代入实际参数并重测其 ABI。

## 7. 其他主因的排除

| 候选 | 代码位置 | 结论 |
|---|---|---|
| block cache | [RocksEngineConfig.cpp:307-313](../src/kvstore/RocksEngineConfig.cpp#L307-L313) | 函数内 static、进程内共享；解释固定基线，不能解释按 replica-part 和时间增长 |
| jemalloc 不归还 | [AtomicLogBuffer.cpp:119-172](../src/kvstore/wal/AtomicLogBuffer.cpp#L119-L172)、[220-268](../src/kvstore/wal/AtomicLogBuffer.cpp#L220-L268) | 实测 live Node 仍增长且 dirty=0，对象尚未 delete；allocator 只是放大 |
| WAL page cache | [FileBasedWal.cpp:457-500](../src/kvstore/wal/FileBasedWal.cpp#L457-L500) | WAL 用 write；运行 smaps 增量主要为 Private_Dirty/Anonymous，并与 Node 计数同步 |
| iterator 卡 GC | [AtomicLogBuffer.h:149-155](../src/kvstore/wal/AtomicLogBuffer.h#L149-L155)，[RaftPart.cpp:1068-1078](../src/kvstore/raftex/RaftPart.cpp#L1068-L1078)、[1793-1797](../src/kvstore/raftex/RaftPart.cpp#L1793-L1797) | refs 回到 0、dirty=0；当前早期增长不是 reader 阻止 GC |
| timer | [RaftPart.cpp:1428-1434](../src/kvstore/raftex/RaftPart.cpp#L1428-L1434)，[GenericWorker.h:209-235](../src/common/thread/GenericWorker.h#L209-L235)，[GenericWorker.cpp:124-143](../src/common/thread/GenericWorker.cpp#L124-L143)、[176-181](../src/common/thread/GenericWorker.cpp#L176-L181) | addDelayTask 以 interval=0 注册；每 part 只有 one-shot 后继，执行后 purge |
| Raft 日志/Append RPC 队列 | [RaftPart.cpp:811-869](../src/kvstore/raftex/RaftPart.cpp#L811-L869)、[1103-1127](../src/kvstore/raftex/RaftPart.cpp#L1103-L1127)、[2154-2172](../src/kvstore/raftex/RaftPart.cpp#L2154-L2172)；[Host.cpp:22](../src/kvstore/raftex/Host.cpp#L22)、[100-284](../src/kvstore/raftex/Host.cpp#L100-L284)、[498-529](../src/kvstore/raftex/Host.cpp#L498-L529) | 有显式上限并在成功、异常或 drain 路径转移/终结；健康空载时不按墙钟时间线性累积 |
| Heartbeat RPC 在途量 | [Host.cpp:408-489](../src/kvstore/raftex/Host.cpp#L408-L489) | 不套用 Append 队列上限；RPC 健康且 timeout 生效时由频率×延迟决定，未见按运行时长累积的路径 |
| client/topology map | [ThriftClientManager.h:35-38](../src/common/thrift/ThriftClientManager.h#L35-L38)、[ThriftClientManager-inl.h:31-96](../src/common/thrift/ThriftClientManager-inl.h#L31-L96)；[NebulaStore.h:69-82](../src/kvstore/NebulaStore.h#L69-L82)；[StorageServer.cpp:56](../src/storage/StorageServer.cpp#L56)、[178-186](../src/storage/StorageServer.cpp#L178-L186) | 每调用线程一张 map，map 内按 host×EventBase 复用；固定线程池/拓扑下有界 |

上述排除限定在本次“拓扑稳定、RPC 健康、空载”的运行条件；故障或拥塞可产生暂时在途内存，但没有
证据支持其为当前主因。

## 8. 最终判断

根因链为：

~~~text
C1/C2 周期轮询
  → C3 缺少当前 term 状态限制的空 NORMAL 日志
  → C5 每轮完成后重新满足条件
  → C7/C9 三副本 WAL 和 buffer push
  → C10/C11 每 part live Node 长期保留（主线性项）
  → C13/C14 commit-key active-memtable property 聚合增长（两点采样，单 DB 字节值为离散档位）
~~~

这一组合定性解释：

- 没有 DML 仍增长；
- storage 三台同时增长；
- 空间/分片越多越快；
- WAL TTL 后 RSS 不同步下降；
- 短中期看不到回收。

“每天数百 MB”的量级只在第 6 节所列发布默认 H=30、8MiB buffer、1000 replica-parts 且现网 ABI/
allocator 与本实验一致的条件下得到定量闭环；用户未提供的现网参数不能被当作已经测得。

## 9. 外部交叉验证

- [PR #606](https://github.com/vesoft-inc/nebula/pull/606)：空日志 heartbeat 的原始设计背景；
- [Issue #6156](https://github.com/vesoft-inc/nebula/issues/6156)：独立指出缺少
  <code>commitInThisTerm_</code> 条件；截至 2026-08-16 仍 Open、无维护者确认；
- [PR #4386](https://github.com/vesoft-inc/nebula/pull/4386)：处理大 payload dirty-node GC，不是本链；
- [topic 13075](https://discuss.nebula-graph.com.cn/t/topic/13075)、
  [topic 13677](https://discuss.nebula-graph.com.cn/t/topic/13677)、
  [topic 14898](https://discuss.nebula-graph.com.cn/t/topic/14898)、
  [topic 14718](https://discuss.nebula-graph.com.cn/t/topic/14718)、
  [topic 16594](https://discuss.nebula-graph.com.cn/t/topic/16594)：WAL/Atomic 堆栈、空载增长和分片数
  依赖与本结论一致。

外部资料只作交叉验证；根因以当前分支源码和本次 3+3+3 运行结果为准。

## 10. 复核材料

- 完整报告：[task-wal-root-cause-analysis.md](./task-wal-root-cause-analysis.md)
- 调查时间线：[task-wal-investigation.log](./task-wal-investigation.log)
- WAL/Node/RSS CSV：[wal-runtime-samples.csv](./wal-lab/evidence/wal-runtime-samples.csv)
- RocksDB CSV：[rocksdb-memtable-samples.csv](./wal-lab/evidence/rocksdb-memtable-samples.csv)
- ABI 输出：[abi-sizes.txt](./wal-lab/evidence/abi-sizes.txt)
- 九实例配置：[tasks/wal-lab/conf](./wal-lab/conf)
- 复算脚本：[analyze-runtime-samples.py](./wal-lab/analyze-runtime-samples.py)

原始实验结束时曾停止全部 9 个进程；这些配置对应的服务随后于 2026-08-16 12:41:49～12:41:50
被重新启动，本次文档重写校验时仍在运行。本轮没有启动、停止或重启服务。配置、data、WAL、服务
日志和原始证据均保留。
