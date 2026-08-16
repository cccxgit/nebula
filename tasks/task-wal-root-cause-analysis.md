# Nebula Graph 3.6 storaged 内存持续增长：完整根因分析

> 源码入门导读：如果对 Raft、WAL、AtomicLogBuffer 或 RocksDB 尚不熟悉，建议先阅读
> [task-wal-source-code-walkthrough.md](./task-wal-source-code-walkthrough.md)，再回到本文核对实验和证据。
>
> 分析范围：只分析根因，不研究或评价解决方案。
>
> 源码分支：<code>3.6-w-1</code>
>
> 源码基线提交：<code>de9b3ed800a6627d9845e9289b6bbc5b6faf460a</code>
>
> 实验日期：2026-08-16，Asia/Shanghai
>
> 结论置信度：高

## 1. 文档约定和代码位置口径

本文所有源码结论均同时标出：

- 源码文件；
- 当前工作区精确行号；
- 关键根因入口同时标出函数或数据结构名称。

Raft、FileBasedWal、Part 和 RocksEngine 的因果代码与上述基线提交一致。为了运行观测，本工作区对
<code>AtomicLogBuffer</code> 和 storage HTTP stats 增加了只读计数，因此这两个文件的行号以
**当前观测版工作区**为准。观测改动的位置单列在第 8 节，不能与产品原始逻辑混为一谈。

便于对照未插桩基线：原提交中 Record/Node 位于 <code>AtomicLogBuffer.h:23-39,44-111</code>，
buffer 状态字段位于 <code>:353-366</code>；push/运行期 GC/析构分别位于
<code>AtomicLogBuffer.cpp:71-119,167-214,56-69</code>。正文链接统一指向当前可直接审阅的工作区；
关键根因入口同时附带符号名，避免仅凭会漂移的裸行号判断。

RocksDB 内部语义绑定到本次构建使用的 7.5.3：
[build/CMakeCache.txt:462-465](../build/CMakeCache.txt#L462-L465) 指向
<code>/opt/vesoft/third-party/3.3</code> 的 include/lib；实际 installed header
<code>/opt/vesoft/third-party/3.3/include/rocksdb/version.h:14-16</code> 标明 7.5.3。下文使用本机
版本匹配的 upstream 参考源码
[rocksdb-7.5.3/version.h:14-16](../../rocksdb-7.5.3/include/rocksdb/version.h#L14-L16)
定位内部路径；没有对静态库与该 sibling 源码做逐文件 bit-identical 证明，因此内部源码用于解释，
实际构建行为仍以 active-entry/property 运行数据为主。

用户提供的原始问题说明 [tasks/task-wal.md](./task-wal.md) 不属于分析产物，未改写；原始 CSV 和
GDB 证据也未改动。本轮没有人为改写服务日志或 data/WAL，但当前九个服务仍在运行，其运行目录会由
服务继续更新。

## 2. 最终结论

生产现象的共同上游是：Nebula Graph 3.6 的稳定 Raft Leader 在没有业务写入时，仍周期性追加一条
payload 为空的 <code>NORMAL</code> Raft 日志。

关键缺陷位于
[RaftPart.cpp:2041-2049，RaftPart::sendHeartbeat](../src/kvstore/raftex/RaftPart.cpp#L2041-L2049)：
注释说明空日志只用于“Leader 在当前 term 尚未 commit”时提交前一 term 的遗留日志，但实际 guard
只有瞬态条件 <code>!replicatingLogs_</code>，没有检查代码中已经维护的
<code>commitInThisTerm_</code>。

这条空日志进入两条内存路径：

1. **主路径：每个本地 replica-part 的 AtomicLogBuffer。** 空记录只按 16B 计入容量，但
   <code>Node</code> 内联 64 个含 <code>std::string</code> 的 <code>Record</code>。当前实验二进制
   中，一个满 Node 的长期摊销是约 50B requested、56B jemalloc usable/空日志。在固定拓扑且该
   buffer 未 reset/析构、part 未 drop/重建时，达到默认 8MiB 逻辑容量前，Node 是仍被链表引用的
   live object，不会因容量淘汰而回收。代码位置：
   [FileBasedWal.cpp:499](../src/kvstore/wal/FileBasedWal.cpp#L499)、
   [AtomicLogBuffer.h:40-128](../src/kvstore/wal/AtomicLogBuffer.h#L40-L128)、
   [AtomicLogBuffer.cpp:119-172](../src/kvstore/wal/AtomicLogBuffer.cpp#L119-L172)。
2. **次路径：每个 space × data_path 的 RocksDB memtable。** 空 payload 虽跳过业务 KV 操作，
   commit batch 仍更新该分片的 <code>systemCommitKey</code>。健康空载实测近似一条空日志一个
   batch，并观察到 active-memtable entry 持续累积；这是本次运行结果，不把 RocksDB 内部实现写成
   仅凭 Nebula 调用点即可证明的绝对语义。代码位置：
   [Part.cpp:215-358](../src/kvstore/Part.cpp#L215-L358)、
   [Part.cpp:403-410](../src/kvstore/Part.cpp#L403-L410)、
   [RocksEngine.h:181-216](../src/kvstore/RocksEngine.h#L181-L216)、
   [RocksEngine.cpp:120-140](../src/kvstore/RocksEngine.cpp#L120-L140)。

因此它不是典型的“对象已失去引用”的泄漏，而是由不必要空日志触发的、按分片和副本放大的超长周期
live-object 保留。从生产视角看，其表现与内存泄漏相同：空载持续增长、短中期不下降、分片越多越快。

## 3. 根因代码位置总表

| 因果环节 | 代码位置 | 代码事实 |
|---|---|---|
| 轮询计划延迟 | [RaftPart.cpp:1401-1434，statusPolling](../src/kvstore/raftex/RaftPart.cpp#L1401-L1434) | 计划延迟为 <code>H×1000/3 + rand32(500)</code>，稳定 Leader 每轮调用 <code>sendHeartbeat()</code>，本轮结束时只安排一个后继；实际 start-to-start 周期另含执行与调度开销 |
| Leader 进入条件 | [RaftPart.cpp:1138-1141，needToSendHeartbeat](../src/kvstore/raftex/RaftPart.cpp#L1138-L1141) | 只判断 RUNNING + LEADER，不判断当前 term 是否已 commit |
| 发布配置 H=30 | [nebula-storaged.conf.default:44-50](../conf/nebula-storaged.conf.default#L44-L50) | 发布模板设置 <code>raft_heartbeat_interval_secs=30</code> |
| 缺陷触发点 | [RaftPart.cpp:2041-2049，sendHeartbeat](../src/kvstore/raftex/RaftPart.cpp#L2041-L2049) | 只检查 <code>!replicatingLogs_</code>，每轮异步追加空 <code>NORMAL</code> 日志 |
| 复制完成后重新满足触发条件 | [RaftPart.cpp:1103-1127](../src/kvstore/raftex/RaftPart.cpp#L1103-L1127) | 当前批完成且无新日志时把 <code>replicatingLogs_</code> 复位为 false |
| term 状态已维护 | [RaftPart.h:855-857](../src/kvstore/raftex/RaftPart.h#L855-L857)，[RaftPart.cpp:1388](../src/kvstore/raftex/RaftPart.cpp#L1388)，[1091-1094](../src/kvstore/raftex/RaftPart.cpp#L1091-L1094)，[2254-2261](../src/kvstore/raftex/RaftPart.cpp#L2254-L2261) | 字段语义明确；当选时置 false，当前 term 首次成功 commit 后置 true，lease 会读取；<code>sendHeartbeat</code> 却不读取 |
| 真正 Heartbeat RPC | [RaftPart.cpp:2051-2122](../src/kvstore/raftex/RaftPart.cpp#L2051-L2122) | RPC 与空日志 append 是同一函数内两段独立逻辑 |
| Follower 的 Heartbeat RPC | [RaftPart.cpp:1895-1951，processHeartbeatRequest](../src/kvstore/raftex/RaftPart.cpp#L1895-L1951) | 注释和实现明确该 RPC 不追加日志 |
| Leader 写 WAL | [RaftPart.cpp:786-915](../src/kvstore/raftex/RaftPart.cpp#L786-L915) | 空日志进入队列、写 Leader WAL，再进入复制 |
| 复制到 peers | [RaftPart.cpp:918-999，replicateLogs](../src/kvstore/raftex/RaftPart.cpp#L918-L999)，[Host.cpp:287-345，prepareAppendLogRequest](../src/kvstore/raftex/Host.cpp#L287-L345) | 正常 AppendLog RPC 发给 peers，leader WAL iterator 的空 <code>logMsg()</code> 被装入 RPC entry |
| Follower 写 WAL | [RaftPart.cpp:1757-1777](../src/kvstore/raftex/RaftPart.cpp#L1757-L1777) | Follower 对收到的日志执行 <code>wal_->appendLogs()</code> |
| 每 part 创建 WAL | [NebulaStore.cpp:437-515](../src/kvstore/NebulaStore.cpp#L437-L515)，[Part.cpp:23-50](../src/kvstore/Part.cpp#L23-L50)，[RaftPart.cpp:330-372](../src/kvstore/raftex/RaftPart.cpp#L330-L372) | 每个本地 Part 继承/构造 RaftPart，并为其创建独立 FileBasedWal |
| 每 WAL 创建 buffer | [FileBasedWal.cpp:40-60](../src/kvstore/wal/FileBasedWal.cpp#L40-L60) | 构造函数创建一个独立 <code>AtomicLogBuffer</code> |
| WAL 默认参数 | [FileBasedWal.cpp:15-18](../src/kvstore/wal/FileBasedWal.cpp#L15-L18) | TTL=14400s、文件=16MiB、buffer=8MiB |
| 空记录逻辑计费 | [AtomicLogBuffer.h:40-56，Record::size](../src/kvstore/wal/AtomicLogBuffer.h#L40-L56) | 只计两个 ID 和 payload 长度；空 payload 为 16B |
| Node 的 64 条内联记录 | [AtomicLogBuffer.h:18](../src/kvstore/wal/AtomicLogBuffer.h#L18)，[61-128，Node](../src/kvstore/wal/AtomicLogBuffer.h#L61-L128) | 每个 Node 内联 64 个 Record |
| Node 分配和容量判断 | [AtomicLogBuffer.cpp:119-172，push](../src/kvstore/wal/AtomicLogBuffer.cpp#L119-L172) | 每 64 条分配新 Node；是否标脏只看逻辑 <code>size_</code> 与 <code>capacity_</code> |
| 存活 buffer 的运行期旧 Node 删除 | [AtomicLogBuffer.cpp:220-268，releaseRef](../src/kvstore/wal/AtomicLogBuffer.cpp#L220-L268)；整体析构见 [79-96，~AtomicLogBuffer](../src/kvstore/wal/AtomicLogBuffer.cpp#L79-L96) | 运行期只有已标脏节点且满足 GC 条件，iterator 释放引用时才 delete；buffer 析构会释放全部 Node |
| 磁盘记录后 push | [FileBasedWal.cpp:442-500，appendLogInternal](../src/kvstore/wal/FileBasedWal.cpp#L442-L500) | 写完 32B 空记录后无条件调用 <code>logBuffer_->push()</code> |
| WAL TTL 清理 | [NebulaStore.cpp:1293-1321](../src/kvstore/NebulaStore.cpp#L1293-L1321)，[RaftPart.cpp:492-495，cleanWal](../src/kvstore/raftex/RaftPart.cpp#L492-L495)，[FileBasedWal.cpp:640-706](../src/kvstore/wal/FileBasedWal.cpp#L640-L706) | 调用链最终只遍历和删除磁盘 WAL 文件及 <code>walFiles_</code> 条目，不访问 <code>logBuffer_</code> |
| 空日志 commit | [Part.cpp:215-232，Part::commitLogs](../src/kvstore/Part.cpp#L215-L232)，[349-358](../src/kvstore/Part.cpp#L349-L358) | 先更新 lastId/term，空 payload 跳过业务 op，但 batch 末尾仍写 commit message 并提交 |
| commit key 组成 | [Part.cpp:403-410，putCommitMsg](../src/kvstore/Part.cpp#L403-L410)，[NebulaKeyUtils.cpp:101-108](../src/common/utils/NebulaKeyUtils.cpp#L101-L108) | 每个分片写自己的 system commit key |
| Nebula batch 桥接 | [RocksEngine.h:181-216，RocksWriteBatch](../src/kvstore/RocksEngine.h#L181-L216) | <code>put()</code> 调用 <code>rocksdb::WriteBatch::Put()</code>，<code>data()</code> 将同一 concrete batch 交给 engine |
| RocksDB Write | [RocksEngine.cpp:120-140，commitBatchWrite](../src/kvstore/RocksEngine.cpp#L120-L140) | WriteBatch 最终进入 <code>db_->Write()</code> |
| RocksEngine 粒度 | [NebulaStore.cpp:354-370](../src/kvstore/NebulaStore.cpp#L354-L370)，[395-421](../src/kvstore/NebulaStore.cpp#L395-L421) | 每个 space × data_path 创建一个 RocksEngine |
| memtable 配置 | [nebula-storaged.conf.default:98-104](../conf/nebula-storaged.conf.default#L98-L104) | active memtable 目标 64MiB，最多 4 个 write buffer；64MiB 不是空间总内存硬上限 |
| RocksDB 7.5.3 write-buffer 语义 | [options.h:172-188](../../rocksdb-7.5.3/include/rocksdb/options.h#L172-L188)，[advanced_options.h:249-261](../../rocksdb-7.5.3/include/rocksdb/advanced_options.h#L249-L261) | write_buffer_size 是转为有序磁盘文件前的单 buffer 目标；最多可同时持有 max_write_buffer_number 个 |
| RocksDB 7.5.3 entry/sequence | [advanced_options.h:328-340](../../rocksdb-7.5.3/include/rocksdb/advanced_options.h#L328-L340)，[write_batch.cc:1950-1989](../../rocksdb-7.5.3/db/write_batch.cc#L1950-L1989)，[2075-2081](../../rocksdb-7.5.3/db/write_batch.cc#L2075-L2081)，[memtable.cc:535-637](../../rocksdb-7.5.3/db/memtable.cc#L535-L637) | 实验配置未覆盖该选项，因而走默认 <code>inplace_update_support=false</code> 路径：Put 进入 <code>MemTable::Add</code>，entry 编码 sequence 并在插入时更新 entry 计数；成功后推进 sequence |

## 4. 源码因果链

~~~mermaid
flowchart TD
    A["RaftPart::statusPolling<br/>H/3 + random(0..499ms)"] --> B["稳定 Leader: sendHeartbeat"]
    B --> C["appendLogAsync<br/>空 NORMAL 日志"]
    B --> D["真正 Heartbeat RPC"]
    D --> E["Follower 只校验状态<br/>不追加日志"]
    C --> F["Leader FileBasedWal"]
    F --> G["Leader AtomicLogBuffer"]
    F --> H["AppendLog 复制到 peers"]
    H --> I["Follower FileBasedWal"]
    I --> J["Follower AtomicLogBuffer"]
    C --> K["各副本 Part::commitLogs"]
    K --> L["RocksDB systemCommitKey"]
~~~

### 4.1 为什么默认不是 30 秒一条，而是约 10.25 秒一条

[RaftPart.cpp:1411-1423](../src/kvstore/raftex/RaftPart.cpp#L1411-L1423) 先计算：

~~~text
delay_ms = raft_heartbeat_interval_secs * 1000 / 3 + random_integer(0..499)
~~~

[RaftPart.cpp:1428-1434](../src/kvstore/raftex/RaftPart.cpp#L1428-L1434) 在本轮末尾用该 delay 安排下一次
<code>statusPolling</code>。发布模板 H=30 的固定部分为 10000ms，随机项均值为 249.5ms，因此
**计划延迟均值**为 10.2495 秒。实际 start-to-start 周期还包含本轮函数执行、线程调度和 timer
抖动；10.2495 秒是忽略这些开销的名义值。
[RaftPart.cpp:1138-1141](../src/kvstore/raftex/RaftPart.cpp#L1138-L1141) 的
<code>needToSendHeartbeat()</code> 只判断 RUNNING + LEADER，不检查当前 term 是否已经 commit。该
结论限定在 Part 处于 RUNNING/稳定 Leader 的条件下。

### 4.2 为什么这是一条不必要的持久化空日志

[RaftPart.cpp:2041-2049](../src/kvstore/raftex/RaftPart.cpp#L2041-L2049) 的注释描述了标准 no-op 的目的：
新 Leader 在当前 term 尚无已提交日志时，需要通过当前 term 的一条日志推进 commit。但实现没有使用
这个语义条件，只要采样瞬间 <code>replicatingLogs_ == false</code> 就追加空日志。

代码中用于表达“当前 term 已经提交”的状态并非缺失：

- 字段本身的语义声明位于
  [RaftPart.h:855-857](../src/kvstore/raftex/RaftPart.h#L855-L857)；
- 新 Leader 当选时在 [RaftPart.cpp:1370-1395](../src/kvstore/raftex/RaftPart.cpp#L1370-L1395)
  将 <code>commitInThisTerm_</code> 置为 false；
- commit 成功时在 [RaftPart.cpp:1068-1095](../src/kvstore/raftex/RaftPart.cpp#L1068-L1095)
  将其置为 true；
- lease 判断在 [RaftPart.cpp:2254-2261](../src/kvstore/raftex/RaftPart.cpp#L2254-L2261)
  读取该字段；
- 但 <code>sendHeartbeat</code> 的空日志分支完全不读取它。

健康复制结束后，[RaftPart.cpp:1103-1127](../src/kvstore/raftex/RaftPart.cpp#L1103-L1127) 会在没有
新日志时把 <code>replicatingLogs_</code> 重新置为 false。因此下一轮 polling 又满足空日志分支；
这补全了“稳定 term 后续每轮仍继续追加”的状态闭环。

真正的 Heartbeat RPC 从
[RaftPart.cpp:2051-2122](../src/kvstore/raftex/RaftPart.cpp#L2051-L2122) 才开始。Follower handler
[RaftPart.cpp:1912-1914](../src/kvstore/raftex/RaftPart.cpp#L1912-L1914) 明确说明“不做 log appending”。
所以增长源不是 RPC heartbeat 本身，而是 RPC 之前附加的空 <code>NORMAL</code> append。

### 4.3 为什么 RF=3 时三个 storaged 都增长

Leader 的空日志依次经过：

- [RaftPart.cpp:786-869，appendLogAsync](../src/kvstore/raftex/RaftPart.cpp#L786-L869)；
- [RaftPart.cpp:874-915，appendLogsInternal](../src/kvstore/raftex/RaftPart.cpp#L874-L915)，其中
  891-901 行先写 Leader WAL；
- [RaftPart.cpp:918-999，replicateLogs](../src/kvstore/raftex/RaftPart.cpp#L918-L999)，复制给 peers。

[Host.cpp:287-345，Host::prepareAppendLogRequest](../src/kvstore/raftex/Host.cpp#L287-L345) 从 Leader
WAL iterator 读取 <code>logMsg()</code> 并写入 <code>RaftLogEntry.log_str</code>，因此空 payload
本身确实进入 AppendLog RPC；服务端路由见
[RaftexService.cpp:139-147，RaftexService::appendLog](../src/kvstore/raftex/RaftexService.cpp#L139-L147)。

Follower 收到 AppendLog 后在
[RaftPart.cpp:1757-1777](../src/kvstore/raftex/RaftPart.cpp#L1757-L1777) 同样写本地 WAL。
[FileBasedWal.cpp:515-527](../src/kvstore/wal/FileBasedWal.cpp#L515-L527) 会逐条进入
<code>appendLogInternal</code>；后者在
[FileBasedWal.cpp:457-499](../src/kvstore/wal/FileBasedWal.cpp#L457-L499) 先编码并写磁盘，然后无条件
push 到该 WAL 的 AtomicLogBuffer。

空 payload 的磁盘编码为：

~~~text
LogID 8 + TermID 8 + length 4 + ClusterID 8 + payload 0 + trailing length 4 = 32B
~~~

三个 Raft ID 的 8B 类型定义见
[ThriftTypes.h:12-18](../src/common/thrift/ThriftTypes.h#L12-L18)。

因此一个逻辑 Raft group 在健康空载时约每轮由 Leader 产生一条空日志，RF=3 时会在三个本地副本各产生一次 WAL
写和一次 buffer push。单机增长速率取决于该机持有的 replica-part 数，而不是该机 Leader 数。

### 4.4 为什么每个分片都有独立缓冲

[NebulaStore.cpp:437-515](../src/kvstore/NebulaStore.cpp#L437-L515) 为本地每个
<code>(space, part)</code> 创建一个 Part；[Part.cpp:23-50](../src/kvstore/Part.cpp#L23-L50) 将
walPath 传入 RaftPart 基类；RaftPart 构造逻辑在
[RaftPart.cpp:330-372](../src/kvstore/raftex/RaftPart.cpp#L330-L372) 创建 FileBasedWal；FileBasedWal
又在 [FileBasedWal.cpp:40-60](../src/kvstore/wal/FileBasedWal.cpp#L40-L60) 创建一个独立
AtomicLogBuffer。

所以 [FileBasedWal.cpp:17](../src/kvstore/wal/FileBasedWal.cpp#L17) 的 8MiB 默认值是**每个本地
replica-part 的逻辑容量**，不是每进程 8MiB。

### 4.5 为什么 16B 逻辑计费会变成更大的 live Node

[AtomicLogBuffer.h:40-56](../src/kvstore/wal/AtomicLogBuffer.h#L40-L56) 中 Record 含
<code>ClusterID</code>、<code>TermID</code> 和一个 <code>std::string</code> 对象，但
<code>Record::size()</code> 只返回两个 ID 与字符串 payload 的长度。空 payload 因而只计：

~~~text
8B ClusterID + 8B TermID = 16B
~~~

[AtomicLogBuffer.h:18](../src/kvstore/wal/AtomicLogBuffer.h#L18) 和
[61-128](../src/kvstore/wal/AtomicLogBuffer.h#L61-L128) 表明 Node 内联 64 个 Record。
[AtomicLogBuffer.cpp:124-142](../src/kvstore/wal/AtomicLogBuffer.cpp#L124-L142) 在 head 不存在或已满时
分配新 Node。

对本次精确 Debug 二进制，GDB/DWARF 和 jemalloc 结果保存在
[abi-sizes.txt](./wal-lab/evidence/abi-sizes.txt)：

| 项 | 本次实验值 |
|---|---:|
| <code>sizeof(std::string)</code> | 32B |
| <code>sizeof(Record)</code> | 48B |
| <code>sizeof(Node)</code> | 3200B |
| <code>nallocx(3200, 0)</code> | 3584B |
| 满 Node 后每条空日志长期摊销 requested | 50B |
| 满 Node 后每条空日志长期摊销 jemalloc usable | 56B |

50B/56B 是 Node 填满后的长期摊销，不是每次 push 都独立分配 50B/56B。每个 buffer 始终可能有一个
未填满 head，因此短窗口会有 partial-head 误差。3200B/3584B 也只适用于本次 C++ ABI 和 allocator；
跨编译器或商业构建必须重新测量。源码层面稳定的事实是：空记录逻辑计费 16B、每 Node 64 条。

[AtomicLogBuffer.cpp:98-116，AtomicLogBuffer::metrics](../src/kvstore/wal/AtomicLogBuffer.cpp#L98-L116)
分别将 <code>size_</code> 聚合为 accountedBytes，并把 <code>nodes × sizeof(Node)</code> 聚合为
nodeBytes。后者已包含 Node 内联 Record，不能再与 accountedBytes 相加作为总内存，否则会重复计算
ID/Record 内容。

### 4.6 为什么短中期看不到回收

[AtomicLogBuffer.cpp:119-172](../src/kvstore/wal/AtomicLogBuffer.cpp#L119-L172) 的容量判断只比较
<code>record.size()</code> 累加出的逻辑 <code>size_</code>。在约 8MiB 前，没有 tail 被标记删除。
8MiB / 16B 约为 524,288 条；由于“满 head 时先建新 Node 并提前 return”的分支，首次标脏约比该
算术阈值晚 2 条，量级差异可忽略。

对一个仍存活的 buffer，运行期容量淘汰的 delete 位于
[AtomicLogBuffer.cpp:220-268](../src/kvstore/wal/AtomicLogBuffer.cpp#L220-L268) 的
<code>releaseRef()</code>；需要节点已经 dirty，并满足
<code>dirtyNodes > 5</code> 或逻辑 <code>size_ > 16MiB</code>。dirty limit 字段见
[AtomicLogBuffer.h:387-388](../src/kvstore/wal/AtomicLogBuffer.h#L387-L388)，16MiB flag 定义见
[AtomicLogBuffer.cpp:12-14](../src/kvstore/wal/AtomicLogBuffer.cpp#L12-L14)。正常 Leader/Follower commit iterator
分别位于 [RaftPart.cpp:1068-1078](../src/kvstore/raftex/RaftPart.cpp#L1068-L1078) 和
[1793-1797](../src/kvstore/raftex/RaftPart.cpp#L1793-L1797)，iterator 析构在
[AtomicLogBuffer.h:149-155](../src/kvstore/wal/AtomicLogBuffer.h#L149-L155) 释放引用。
buffer 整体析构时还会由
[AtomicLogBuffer.cpp:79-96](../src/kvstore/wal/AtomicLogBuffer.cpp#L79-L96) 释放其所有 Node；固定
拓扑空载实验没有触发分片/buffer 析构。

按发布默认 H=30 且未覆盖参数、忽略函数执行和调度额外延迟时，达到 8MiB 账面量名义约需：

~~~text
524,288 × 10.2495 / 86,400 = 62.20 天
~~~

在这个阶段，Node 仍由链表引用，是实际 live memory；并不是已 free 后 allocator 不向 OS 归还。
当前 ABI 下，约 8192 个满 Node 已是约 25MiB requested/28MiB jemalloc usable/分片，边界处另有少量
partial/dirty Node。

### 4.7 为什么 WAL TTL 不会清掉这块内存

[NebulaStore.cpp:24](../src/kvstore/NebulaStore.cpp#L24) 定义默认清理间隔 600 秒，
[NebulaStore.cpp:72](../src/kvstore/NebulaStore.cpp#L72) 首次调度，
[1293-1321](../src/kvstore/NebulaStore.cpp#L1293-L1321) 执行并重新安排 Part 的 WAL 清理。
[RaftPart.cpp:492-495，RaftPart::cleanWal](../src/kvstore/raftex/RaftPart.cpp#L492-L495) 把
<code>committedLogId_</code> 传给 FileBasedWal。
[FileBasedWal.cpp:640-706](../src/kvstore/wal/FileBasedWal.cpp#L640-L706) 的两个 cleanWAL 重载只
<code>unlink</code> 过期文件并从 <code>walFiles_</code> 删除元数据，至少保留两个 WAL 文件；整个
函数没有访问 <code>logBuffer_</code>。这里“只删磁盘”特指 FileBasedWal 的两个重载；
NebulaStore 在 <code>rocksdb_disable_wal</code> 分支还可能调用 engine flush
（[NebulaStore.cpp:1298-1303](../src/kvstore/NebulaStore.cpp#L1298-L1303)），但同样不回收
AtomicLogBuffer。

因此 [FileBasedWal.cpp:15](../src/kvstore/wal/FileBasedWal.cpp#L15) 的 4 小时 TTL 只控制磁盘文件，
不控制 AtomicLogBuffer live Node。

### 4.8 RocksDB 第二条内存路径

[Part.cpp:215-232](../src/kvstore/Part.cpp#L215-L232) 在读取 payload 前先更新 <code>lastId/lastTerm</code>；
空 payload 只跳过业务 KV op。[Part.cpp:349-358](../src/kvstore/Part.cpp#L349-L358) 在 iterator/batch
结束后仍执行 <code>putCommitMsg</code> 并提交 WriteBatch。

[Part.cpp:403-410](../src/kvstore/Part.cpp#L403-L410) 把 committed id/term 写入
<code>systemCommitKey(partId)</code>；key 的编码见
[NebulaKeyUtils.cpp:101-108](../src/common/utils/NebulaKeyUtils.cpp#L101-L108)。具体实现
[RocksWriteBatch::put/data，RocksEngine.h:181-216](../src/kvstore/RocksEngine.h#L181-L216) 将抽象
<code>WriteBatch::put</code> 桥接为 <code>rocksdb::WriteBatch::Put</code>，并把同一 concrete batch
交给 [RocksEngine::commitBatchWrite，RocksEngine.cpp:124-140](../src/kvstore/RocksEngine.cpp#L124-L140)
调用 <code>db_->Write()</code>。

严格语义是“每个 commit iterator/batch 写一次 commit key”，不是源码保证“每条日志一次 Write”。
Leader 在 [RaftPart.cpp:1068-1095](../src/kvstore/raftex/RaftPart.cpp#L1068-L1095) 提交当前批；
AppendLog 带的是写当前批前读取的 committed id
（[RaftPart.cpp:874-914](../src/kvstore/raftex/RaftPart.cpp#L874-L914)），Follower 在下一次 AppendLog
中按该值提交上一批
（[RaftPart.cpp:1785-1804](../src/kvstore/raftex/RaftPart.cpp#L1785-L1804)）。因此稳定空载时 Follower
通常滞后一轮，但长期 commit-entry 速率相同。在本次路径中，运行数据验证日志与 batch 近似 1:1；
若日志批量或 follower 滞后加大，多条日志可能合并到同一 batch。

[NebulaStore.cpp:354-370](../src/kvstore/NebulaStore.cpp#L354-L370) 与
[395-421](../src/kvstore/NebulaStore.cpp#L395-L421) 表明每个 space × data_path 创建一个
RocksEngine。本次实验每台只有一条 data path，因此 50 个空间正好有 50 个 DB。
[nebula-storaged.conf.default:98-104](../conf/nebula-storaged.conf.default#L98-L104) 的 64MiB 是
单个 active memtable 的配置目标，且 <code>max_write_buffer_number=4</code>；它不是每空间 RocksDB
内存的硬上限。对应 RocksDB 7.5.3 语义见
[options.h:172-188](../../rocksdb-7.5.3/include/rocksdb/options.h#L172-L188) 和
[advanced_options.h:249-261](../../rocksdb-7.5.3/include/rocksdb/advanced_options.h#L249-L261)。

本实验 ColumnFamily options
([storaged-1.conf:31](./wal-lab/conf/nebula-storaged-1.conf#L31)) 未覆盖
<code>inplace_update_support</code>，RocksDB 7.5.3 默认值为 false
([advanced_options.h:328-340](../../rocksdb-7.5.3/include/rocksdb/advanced_options.h#L328-L340))。
在这条实际路径上，对同一个 commit user key 的 Put，
[write_batch.cc:1950-1989](../../rocksdb-7.5.3/db/write_batch.cc#L1950-L1989) 调用
<code>MemTable::Add(sequence_, ...)</code>；
[memtable.cc:535-637](../../rocksdb-7.5.3/db/memtable.cc#L535-L637) 将 sequence 编入 internal key，
分配 entry，并覆盖串行和并发插入路径的 entry 计数；
[write_batch.cc:2075-2081](../../rocksdb-7.5.3/db/write_batch.cc#L2075-L2081) 在成功后推进 sequence。
因此连续 commit Write 在 flush 前会形成新的 memtable entry；这与本次 active-entry property 计数
实测一致。

## 5. 3 metad + 3 storaged + 3 graphd 运行验证

### 5.1 控制条件和配置位置

本机实际启动 9 个服务进程：

| 服务 | service ports | HTTP ports |
|---|---|---|
| metad | 9559 / 9569 / 9579 | 19559 / 19569 / 19579 |
| graphd | 9669 / 9679 / 9689 | 19669 / 19679 / 19689 |
| storaged | 9779 / 9789 / 9799 | 19779 / 19789 / 19799 |

九份配置位于 [tasks/wal-lab/conf](./wal-lab/conf)。三个 storaged 的加速及被测参数位置分别为：

- [storaged-1.conf:19-31](./wal-lab/conf/nebula-storaged-1.conf#L19-L31)；
- [storaged-2.conf:19-31](./wal-lab/conf/nebula-storaged-2.conf#L19-L31)；
- [storaged-3.conf:19-31](./wal-lab/conf/nebula-storaged-3.conf#L19-L31)。

目标因果链的关键持久化参数中，只将 H 从发布模板的 30 改为 1，以便把数周行为压缩到分钟级；
以下参数保持发布模板语义：

- <code>wal_ttl=14400</code>；
- <code>wal_file_size=16MiB</code>；
- <code>wal_buffer_size=8MiB</code>；
- <code>rocksdb_block_cache=4MiB</code>；
- <code>write_buffer_size=64MiB</code>、<code>max_write_buffer_number=4</code>。

目标持久化参数之外，九份单机运行配置还显式设置或调整了端口、路径、storage 到 meta 的 heartbeat
（实验配置第 18 行从模板 10s 改为 2s）、metad 的 <code>default_parts_num=20</code>、内存相关
flags 和线程池（[storaged-1.conf:33-44](./wal-lab/conf/nebula-storaged-1.conf#L33-L44)）。
这不是九份配置相对模板的穷举清单，完整取值以上述配置文件为准；实验 DDL 显式指定了
<code>PARTITION_NUM=20</code>，所以 metad 默认分片数没有决定实验拓扑。线程池缩小理论上可能增加
调度延迟；实测频率与公式约 1% 内吻合，说明在本次空载负荷下未造成可见机制偏移。不能把这些运行性
调整表述成“只有 H 一项配置发生变化”。

创建空间的语句保存在
[create-spaces-01-10.ngql](./wal-lab/create-spaces-01-10.ngql) 和
[create-spaces-11-50.ngql](./wal-lab/create-spaces-11-50.ngql)：50 个空空间，每空间 20 分片、
RF=3。没有创建 tag/edge，也没有执行图数据 DML。

在 RF 等于 storaged 数时，每台本地副本数为：

~~~text
50 spaces × 20 parts × RF3 / 3 storaged = 1000 replica-parts/storaged
~~~

### 5.2 观测指标和语义

观测代码只增加计数，没有改变 Raft、容量或 GC 条件：

- 指标结构：[AtomicLogBuffer.h:20-35](../src/kvstore/wal/AtomicLogBuffer.h#L20-L35)；
- 进程内 registry/聚合：[AtomicLogBuffer.cpp:19-117](../src/kvstore/wal/AtomicLogBuffer.cpp#L19-L117)；
- Node/push/empty 计数：[AtomicLogBuffer.cpp:119-172](../src/kvstore/wal/AtomicLogBuffer.cpp#L119-L172)；
- HTTP 暴露：[StorageHttpStatsHandler.cpp:50-68](../src/storage/http/StorageHttpStatsHandler.cpp#L50-L68)；
- 观测字段位于 buffer 而非 Node：
  [AtomicLogBuffer.h:380-394](../src/kvstore/wal/AtomicLogBuffer.h#L380-L394)，所以未改变 Node ABI。

指标边界：

- pushes/empty_pushes 是“当前仍存活 buffer 的累计和”；drop part 后进程聚合值可能下降。本次采样期
  固定拓扑，没有 drop/rebalance；
- empty_pushes 只证明 payload 为空；heartbeat 归因来自空载控制、稳定 term、源码调用链和频率共同
  构成的证据；
- relaxed atomic 聚合不是事务快照，单次采样可能有极小跨字段错位；
- registry 的 raw pointer 方案用于固定拓扑诊断，非长期产品化指标设计。

### 5.3 空日志频率与源码公式

原始数据：[wal-runtime-samples.csv](./wal-lab/evidence/wal-runtime-samples.csv)。
复算逻辑：[analyze-runtime-samples.py:1-83](./wal-lab/analyze-runtime-samples.py#L1-L83)。

| 阶段 | 本地副本/进程 | 名义窗口 | 名义理论 push/s | 三台名义实测 push/s | 严谨结论 |
|---|---:|---:|---:|---:|---|
| 10 空间 | 200 | 122s | 343.348 | 345.984～346.213 | 约 1% 内吻合 |
| 50 空间 | 1000 | 184s | 1716.738 | 1717.342～1717.848 | 约 1% 内吻合 |

H=1 忽略执行/调度额外开销的源码名义值为：

~~~text
floor(1000 / 3)ms + mean(0..499)ms = 333 + 249.5 = 582.5ms
1000 / 0.5825 = 1716.738 pushes/s
~~~

CSV 时间戳来自整数秒，三台又是顺序采集。184 秒窗口仅时间量化不确定度就约 0.54%，所以不能把
0.035%～0.065% 的名义差写成万分级实验精度；可支持的结论是名义公式与实测在约 1% 内吻合。

本地副本从 200 增到 1000 时，名义 push 速率约增 4.96 倍，接近 replica-part 数的 5 倍。

同机九实例与生产分布式九实例的可外推部分是调用链、计数比例、对象结构和 replica-part 线性关系；
绝对调度延迟和总 RSS 仍受同机资源竞争、Debug 构建及观测 instrumentation 影响。loopback 也不能
代表跨主机网络延迟、丢包、拥塞、重试及其在途内存。

### 5.4 16B、32B、64 条一个 Node 的源码指纹

1000 副本、184 秒阶段：

| storaged | push 增量 | empty 增量 | 逻辑 B/push | 磁盘 WAL B/push | Node 增量 | Node requested 增量 | RSS 增量 |
|---|---:|---:|---:|---:|---:|---:|---:|
| 1 | 315,991 | 315,991 | 16.000 | 约 32 | 4,989 | 15,964,800B | 32.09MiB |
| 2 | 316,084 | 316,084 | 16.000 | 约 32 | 4,989 | 15,964,800B | 33.31MiB |
| 3 | 316,074 | 316,074 | 16.000 | 约 32 | 4,989 | 15,964,800B | 32.58MiB |

这些数分别对应：

- 16B： [Record::size](../src/kvstore/wal/AtomicLogBuffer.h#L49-L51)；
- 32B： [FileBasedWal 磁盘编码](../src/kvstore/wal/FileBasedWal.cpp#L457-L467)；
- 每 64 条一个 Node： [kMaxLength=64](../src/kvstore/wal/AtomicLogBuffer.h#L18) 和
  [push 新建 Node](../src/kvstore/wal/AtomicLogBuffer.cpp#L124-L142)。

所有首末样本满足：

~~~text
pushes / 64 <= nodes <= pushes / 64 + live buffer instances
~~~

右侧余量来自每个 buffer 最多一个未填满 head。该阶段 <code>dirty_nodes=0</code>，refs 在普通采样中
回到 0；因此主增长不是 reader 卡住已标脏节点，而是节点根本还没进入可回收阶段。

<code>/proc/pid/smaps_rollup</code> 的 RSS 增量几乎全部落在 Private_Dirty/Anonymous。RSS 高于 Node
requested，说明 RocksDB arena、jemalloc size class 和其他短期分配也有贡献；本报告没有把全部 RSS
错误归因给 Node。

### 5.5 Leader 数不均而三台增长相同，验证 Follower 路径

[show-hosts-50-spaces.out](./wal-lab/evidence/show-hosts-50-spaces.out) 显示三台 Leader 数分别为
325 / 145 / 530，但同一窗口 push 增量为 315,991 / 316,084 / 316,074。

若只有 Leader 写 buffer，第三台应显著快于第二台。实际三台近似相同，直接支持源码中的
[Leader replicateLogs](../src/kvstore/raftex/RaftPart.cpp#L918-L999) 与
[Follower wal append](../src/kvstore/raftex/RaftPart.cpp#L1757-L1777)：决定每台斜率的是本地
replica-part 数。

### 5.6 RocksDB 第二路径的运行证据

原始数据：[rocksdb-memtable-samples.csv](./wal-lab/evidence/rocksdb-memtable-samples.csv)。
对三台各 50 个 RocksDB 顺序采样，两次约隔 105 秒：

| storaged | active entries 增量 | 约 entries/s | active memtable bytes |
|---|---:|---:|---:|
| 1 | 178,548 | 1700 | 52,531,200 → 63,016,960 |
| 2 | 178,843 | 1703 | 52,531,200 → 97,619,968 |
| 3 | 178,950 | 1704 | 54,628,352 → 63,016,960 |

entry 生成率紧跟空 WAL 的约 1717/s。结合
[Part::commitLogs 空 payload 路径](../src/kvstore/Part.cpp#L215-L232)、
[batch 末尾 commit key](../src/kvstore/Part.cpp#L349-L358) 和
[RocksDB Write](../src/kvstore/RocksEngine.cpp#L124-L140)，可归因于健康空载路径中近似每条空日志
一次的 commit-key 更新。

采样脚本读取的两个 property 位于
[sample-rocksdb-memtables.sh:25-27](./wal-lab/sample-rocksdb-memtables.sh#L25-L27)；RocksDB 7.5.3
将其定义为 active memtable 的 entry 总数与近似字节数
（[db.h:920-934](../../rocksdb-7.5.3/include/rocksdb/db.h#L920-L934)）。因此 CSV 直接证明的是：
两采样点之间聚合 active entries/近似 active-memtable bytes 增长，且单 DB 的字节 property 取值呈
1,050,624B/2,099,200B 等离散档位；两个时点不足以证明一条完整时间曲线。RocksDB 7.5.3 的
[MemTable::ShouldFlushNow，memtable.cc:153-200](../../rocksdb-7.5.3/db/memtable.cc#L153-L200)
按 arena block 的已分配量和 over-allocation 比例判断 flush；将离散档位归因于 arena 分块是
结合版本匹配源码作出的解释，不是 CSV 单独证明的事实。短跑期间没有观察到
<code>Write Buffer Full</code>，也未覆盖完整 flush 周期，因此不推断 flush 后 active bytes 或 RSS
是否以及如何回落。

## 6. 生产量级外推及边界

用户没有提供现网实际：

- <code>raft_heartbeat_interval_secs</code>；
- <code>wal_buffer_size</code>；
- data_path 数；
- 商业二进制的编译器、C++ ABI 和 allocator。

所以本节只回答“发布默认配置能否解释每天数百 MB”，不是现网参数实测。假设：

- H=30；
- wal_buffer_size=8MiB；
- 每台 1000 replica-parts；
- Node=3200B、jemalloc usable=3584B，与本次 Debug 二进制一致。

则：

~~~text
计划延迟均值（名义周期） = 10.2495s
每分片每天名义空日志 = 86,400 / 10.2495 = 8,429.679
每台空 pushes/day = 8,429,679
Node requested/day = pushes / 64 × 3200 = 401.958MiB
jemalloc usable/day = pushes / 64 × 3584 = 450.193MiB
~~~

将 H=1 的当前实测速率仅按
[statusPolling 的计划延迟公式](../src/kvstore/raftex/RaftPart.cpp#L1411-L1411) 缩放到 H=30，得到约
402.175MiB/day requested。考虑整数秒采样精度，只能表述为“在约 1% 内与静态公式吻合”。

现网参数应代入：

~~~text
名义 T(H) = floor(H × 1000 / 3) / 1000 + 0.2495 秒
Node bytes/day = local replica-parts / T(H) / 64 × sizeof(Node) × 86,400
触及逻辑容量天数 ≈ (wal_buffer_size / 16) × T(H) / 86,400
~~~

RocksDB RSS 不能用单一常数线性外推：它受 space × data_path 的 DB 数、
[arena/flush 判断](../../rocksdb-7.5.3/db/memtable.cc#L153-L200)、write-buffer 数和 flush 错峰影响。
本次两点采样直接观测到的是附加 active-memtable property 聚合增长及单 DB 字节值的离散档位；
完整 flush 后 active bytes/RSS 的形态没有在当前短跑验证，因此不作锯齿或回落幅度断言。这不改变
Atomic Node 的确定性线性基线。

## 7. 替代解释的排除

### 7.1 不是 block cache 的线性膨胀

实验配置把 Storage block cache 保持为 4MiB，位置见
[storaged-1.conf:28](./wal-lab/conf/nebula-storaged-1.conf#L28)。实现使用函数内 static cache，
[RocksEngineConfig.cpp:307-313](../src/kvstore/RocksEngineConfig.cpp#L307-L313) 表明它在进程内共享，
可解释固定基线，不能解释与 replica-part 数和空日志频率成比例的 live Node。

### 7.2 不是单纯 jemalloc “free 后不归还”

[AtomicLogBuffer.cpp:124-142](../src/kvstore/wal/AtomicLogBuffer.cpp#L124-L142) 持续 new Node，而
[220-268](../src/kvstore/wal/AtomicLogBuffer.cpp#L220-L268) 的 delete 条件尚未满足。实测
<code>nodes/node_bytes</code> 本身持续上升且 <code>dirty_nodes=0</code>，对象还活着。jemalloc
3584B size class 会放大斜率，但不是原始触发源。

### 7.3 不是 WAL page cache

[FileBasedWal.cpp:480](../src/kvstore/wal/FileBasedWal.cpp#L480) 使用普通 <code>write()</code> 写 WAL；
文件 page cache 不能解释进程匿名 RSS 与观测到的 live Node 同步增长。运行中的 smaps_rollup 又显示
增量主要是 Private_Dirty/Anonymous。

### 7.4 不是正常 iterator 长期卡住 GC

iterator 在 [AtomicLogBuffer.h:149-155](../src/kvstore/wal/AtomicLogBuffer.h#L149-L155) 析构时释放
引用；Leader/Follower commit iterator 位置分别为
[RaftPart.cpp:1068-1078](../src/kvstore/raftex/RaftPart.cpp#L1068-L1078) 和
[1793-1797](../src/kvstore/raftex/RaftPart.cpp#L1793-L1797)。实验 refs 回到 0 且 dirty=0，说明当前
早期主路径不是旧 reader 阻止 GC。长 reader 在容量阶段以后可能放大保留，但没有证据表明它是本案
当前主因。

### 7.5 定时器、Raft 临时队列、future 和 client cache 不是按时间无界增长

| 候选 | 代码位置 | 排除依据 |
|---|---|---|
| statusPolling timer | [RaftPart.cpp:1428-1434](../src/kvstore/raftex/RaftPart.cpp#L1428-L1434)，[GenericWorker.h:209-235，addDelayTask](../src/common/thread/GenericWorker.h#L209-L235)，[GenericWorker.cpp:124-143](../src/common/thread/GenericWorker.cpp#L124-L143)，[176-181](../src/common/thread/GenericWorker.cpp#L176-L181) | addDelayTask 以 interval=0 注册；每轮只安排一个 one-shot 后继，执行后 purge/erase，活动量为 O(parts) |
| Raft logs_ 队列 | [RaftPart.cpp:34](../src/kvstore/raftex/RaftPart.cpp#L34)，[786-869](../src/kvstore/raftex/RaftPart.cpp#L786-L869)，[1103-1127](../src/kvstore/raftex/RaftPart.cpp#L1103-L1127)，[2154-2172](../src/kvstore/raftex/RaftPart.cpp#L2154-L2172) | max_batch_size=256；成功继续/清空，失败明确 clear |
| Append RPC 在途对象 | [Host.cpp:22](../src/kvstore/raftex/Host.cpp#L22)，[100-284](../src/kvstore/raftex/Host.cpp#L100-L284)，[498-529](../src/kvstore/raftex/Host.cpp#L498-L529)，[CollectNSucceeded-inl.h:24-67](../src/common/base/CollectNSucceeded-inl.h#L24-L67) | caching promise/outstanding request 有显式上限，并在成功、异常或 drain 路径终结；健康空载时不按墙钟时间线性累积 |
| Heartbeat RPC 在途对象 | [Host.cpp:408-489](../src/kvstore/raftex/Host.cpp#L408-L489) | 不套用 Append 队列上限；RPC 健康且 timeout 生效时，在途量取决于频率×延迟，未见按运行时长累积的路径 |
| Thrift client cache | [ThriftClientManager.h:35-38](../src/common/thrift/ThriftClientManager.h#L35-L38)，[ThriftClientManager-inl.h:31-49](../src/common/thrift/ThriftClientManager-inl.h#L31-L49)，[52-96](../src/common/thrift/ThriftClientManager-inl.h#L52-L96)，[NebulaStore.h:69-82](../src/kvstore/NebulaStore.h#L69-L82)，[StorageServer.cpp:56](../src/storage/StorageServer.cpp#L56)，[178-186](../src/storage/StorageServer.cpp#L178-L186) | folly::ThreadLocal 表示每调用线程一张 map，map 内按 remote host × EventBase 复用；共享 manager、固定线程池和固定拓扑下有界，不按分片或墙钟时间新增 |
| 固定拓扑容器 | [NebulaStore.h:31-37](../src/kvstore/NebulaStore.h#L31-L37)，[868-870](../src/kvstore/NebulaStore.h#L868-L870)，[RaftPart.h:795-807](../src/kvstore/raftex/RaftPart.h#L795-L807) | parts/spaces/hosts 随拓扑固定，可解释基线，不能解释空载按时间斜率 |

这些候选可贡献启动基线或瞬时波动，但都不能同时命中实测的调度频率、16B 逻辑记录、32B 磁盘记录和
每 64 条一个 live Node 的四个源码指纹。

## 8. 观测代码与仪器效应边界

观测改动只位于：

- [AtomicLogBuffer.h:20-35](../src/kvstore/wal/AtomicLogBuffer.h#L20-L35)：指标结构；
- [AtomicLogBuffer.h:390-393](../src/kvstore/wal/AtomicLogBuffer.h#L390-L393)：三个 buffer 级 atomic；
- [AtomicLogBuffer.cpp:19-117](../src/kvstore/wal/AtomicLogBuffer.cpp#L19-L117)：live buffer registry 与聚合；
- [AtomicLogBuffer.cpp:119-172](../src/kvstore/wal/AtomicLogBuffer.cpp#L119-L172)：push/empty/Node 计数；
- [AtomicLogBuffer.cpp:249-260](../src/kvstore/wal/AtomicLogBuffer.cpp#L249-L260)：Node delete 时扣减；
- [StorageHttpStatsHandler.cpp:50-68](../src/storage/http/StorageHttpStatsHandler.cpp#L50-L68)：只读 HTTP 输出。

这些字段不参与 <code>capacity_</code>、<code>size_</code>、<code>dirtyNodes_</code>、
<code>replicatingLogs_</code> 或 RocksDB Write 决策。每次 push 多了少量 atomic 操作，会造成固定热路径
仪器开销，但不会生成额外 Raft 日志，也不会改变每 64 条一个 Node 的结构。固定拓扑诊断期间 registry
生命周期可控；raw pointer registry 在对象析构并发下存在生命周期语义边界，因此本报告只把它作为
本次固定拓扑实验指标，不评价其长期产品化方案。

观测版 storaged 构建并安装成功；实验所用二进制 SHA-256：

~~~text
3996570eda9bac9ed76a2dd1063167849ba2a4b6711799eb56e7ae46b9465fcf
~~~

## 9. 外部一手资料和公开案例

### 9.1 历史设计和精准 issue

- [PR #606](https://github.com/vesoft-inc/nebula/pull/606) 于 2019-07-11 合入，说明把 heartbeat 作为
  “写 WAL、commit 时跳过的特殊空日志”，用于新 Leader 提交前一 term 的日志。它解释 no-op 的
  历史目的，但不证明稳定 term 每轮都应写。
- [Issue #6156](https://github.com/vesoft-inc/nebula/issues/6156) 于 2026-06-29 在官方仓库提出，
  精确指出 <code>sendHeartbeat</code> 缺少 <code>commitInThisTerm_</code> 条件。截至 2026-08-16
  仍为 Open、0 评论、无关联 PR；它是独立一致的问题报告，不等于维护者已确认或已修复。
- [PR #4386](https://github.com/vesoft-inc/nebula/pull/4386) 修的是“大 payload 让少量 dirty Node
  占用数百 MB”的 GC 触发条件。3.6 已包含该改动，但它没有改变周期空日志，也没有把 Record/Node
  固定对象开销计入 <code>Record::size()</code>。
- 官方 [v3.8.0 RaftPart.cpp:2041-2049](https://github.com/vesoft-inc/nebula/blob/v3.8.0/src/kvstore/raftex/RaftPart.cpp#L2041-L2049)
  仍保留相同空日志逻辑。社区“可能在 3.7 修”的表述不是公开交付证据。

### 9.2 社区案例

| 案例 | 可核实事实 | 与本根因关系 |
|---|---|---|
| [topic 13075](https://discuss.nebula-graph.com.cn/t/topic/13075) | 3.1；几乎无读写 5 天 5.7G→6.1G；jeprof 增量主要在 FileBasedWal append | 命中 WAL/Atomic 上游与量级 |
| [topic 13677](https://discuss.nebula-graph.com.cn/t/topic/13677) | 12,700 本地 parts；jeprof 中 AtomicLogBuffer::push 约 160.5MB；降低分片后用户称稳定 | 命中具体分配栈和 partition 依赖 |
| [topic 14898](https://discuss.nebula-graph.com.cn/t/topic/14898) | 3.6 单机完全空载仍涨；约 310 parts 降到约 40 后约 10～12MB/日 | 斜率随 parts 减小，量级与公式接近 |
| [topic 14718](https://discuss.nebula-graph.com.cn/t/topic/14718) | 5 storaged、405×100×RF3，平均约 24,300 replica-parts/台，RSS 到 90% | 大量本地副本放大每 part buffer |
| [topic 16594](https://discuss.nebula-graph.com.cn/t/topic/16594) | 明确 3.6；每台约 4000+ replica-parts；用户展示多日持续爬升，并明确称 drop caches 后整体内存仍持续增加 | 与非单纯 page cache 的判断一致 |

公开帖子不是受控实验，也没有官方最终结论，只作为外部交叉验证。根因判断以当前分支源码和本次
3+3+3 运行证据为主。

## 10. 最终根因表述和置信边界

### 10.1 根因

Nebula Graph 3.6 在稳定 term 的健康空闲 Leader 上，几乎每轮
[statusPolling](../src/kvstore/raftex/RaftPart.cpp#L1401-L1434) 都会通过
[sendHeartbeat](../src/kvstore/raftex/RaftPart.cpp#L2041-L2049) 追加一条空 <code>NORMAL</code> 日志。
实现没有执行注释表达的“当前 term 只需一次”的状态限制。

大量 replica-part 把这一事件放大后：

- 每副本 AtomicLogBuffer 通过
  [FileBasedWal.cpp:499](../src/kvstore/wal/FileBasedWal.cpp#L499) 收到空记录，按
  [Record::size](../src/kvstore/wal/AtomicLogBuffer.h#L49-L51) 的 16B 计费，却以
  [64 Record Node](../src/kvstore/wal/AtomicLogBuffer.h#L61-L128) 保留真实对象；
- 每个 space × data_path 的 RocksDB 又经
  [Part.cpp:349-358](../src/kvstore/Part.cpp#L349-L358) 近似随每条空日志更新 commit key，在 active
  memtable 中积累 entry。本实验配置未覆盖 <code>inplace_update_support</code>，因此在 RocksDB
  7.5.3 默认 false 路径下，该 entry 带 sequence；Nebula batch 桥接和内部位置见
  [RocksEngine.h:181-216](../src/kvstore/RocksEngine.h#L181-L216)、
  [write_batch.cc:1950-1989](../../rocksdb-7.5.3/db/write_batch.cc#L1950-L1989) 和
  [memtable.cc:535-637](../../rocksdb-7.5.3/db/memtable.cc#L535-L637)。

Atomic Node 是主要确定性线性项；RocksDB 是同一触发源下两点采样观测到的附加 active-memtable
property 聚合增长，单 DB 字节值呈离散档位。本次短跑没有验证完整 flush 后 active bytes 或 RSS
的形态。

### 10.2 高置信度依据

- 源码调度公式与三台 1000 replica-part 的空 push 速率在约 1% 内吻合；
- <code>pushes == empty_pushes</code>；
- 16B/push、约 32B WAL/push、每 64 条一个 Node 三个结构指纹同时命中；
- Node 数在增长，dirty=0、refs 回到 0，证明增长的是仍存活对象；
- Leader 数 325/145/530 严重不均，三台 push 增量却近似相同，验证 Follower WAL 路径；
- RocksDB active entries 以约 1700/s 同步增长，验证第二路径；
- 按发布默认 H=30 和本次 ABI/allocator 的 401.958MiB/day 静态估算，与 H=1 实测缩放在约 1% 内；
- 多个公开案例的 WAL/Atomic 堆栈、分片依赖和空载现象一致。

### 10.3 不应扩大解释的范围

- 现网参数和商业二进制 ABI 未提供，401.958/450.193MiB/day 与 62.2 天是条件估算，不是现网直接
  测量；
- 本报告不声称全部 RSS 都来自 Atomic Node；RocksDB、allocator 和其他固定缓存也有贡献；
- 本报告不声称无限增长。默认容量附近开始标脏/GC，但每分片此前可形成约 25～28MiB 的 Node 量级，
  1000 分片即约 24.4～27.3GiB，足以造成生产故障；
- 当前短跑没有覆盖所有 RocksDB flush 周期，不对 flush 后 RSS 回落量作断言；
- Issue #6156 尚未得到维护者评论或合入，不表述为官方已确认修复。

## 11. 可复核材料

- 总结报告：[task-wal-root-cause-summary.md](./task-wal-root-cause-summary.md)
- 调查过程记录：[task-wal-investigation.log](./task-wal-investigation.log)
- WAL/Node/RSS 原始 CSV：[wal-runtime-samples.csv](./wal-lab/evidence/wal-runtime-samples.csv)
- RocksDB 逐空间 CSV：[rocksdb-memtable-samples.csv](./wal-lab/evidence/rocksdb-memtable-samples.csv)
- ABI/jemalloc 输出：[abi-sizes.txt](./wal-lab/evidence/abi-sizes.txt)
- Leader/副本分布：[show-hosts-50-spaces.out](./wal-lab/evidence/show-hosts-50-spaces.out)
- 九实例配置：[tasks/wal-lab/conf](./wal-lab/conf)
- 启停脚本：[lab-control.sh](./wal-lab/lab-control.sh)
- WAL 采样脚本：[sample-metrics.sh](./wal-lab/sample-metrics.sh)
- RocksDB 采样脚本：[sample-rocksdb-memtables.sh](./wal-lab/sample-rocksdb-memtables.sh)
- 复算脚本：[analyze-runtime-samples.py](./wal-lab/analyze-runtime-samples.py)

原始根因实验结束时曾按 graph → storage → meta 顺序停止 9 个进程，历史动作保留在调查时间线。
随后这些配置对应的 9 个进程已于 2026-08-16 12:41:49～12:41:50 被重新启动；本次文档重写校验时
仍在运行。本轮只读检查了进程/端口，没有启动、停止或重启服务。配置、data、WAL、服务日志和原始
证据均保留。
