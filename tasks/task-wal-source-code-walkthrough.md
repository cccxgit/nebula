# Nebula Graph 3.6 WAL 内存问题：系统源码讲解

> 面向读者：了解基本 C/C++ 语法，但对 Nebula Graph、Raft、WAL、LSM/RocksDB 和并发内存管理还不熟悉。
>
> 目标：读完后能够从 storaged 启动入口一路跟踪到空 Raft 日志、三副本 WAL、
> <code>AtomicLogBuffer</code> Node 和 RocksDB commit key，并能独立解释实验指标。
>
> 范围：解释机制和根因，不讨论修复方案。
>
> 源码分支：<code>3.6-w-1</code>；产品逻辑基线：
> <code>de9b3ed800a6627d9845e9289b6bbc5b6faf460a</code>。

配套文档：

- [根因总结](./task-wal-root-cause-summary.md)
- [完整根因分析](./task-wal-root-cause-analysis.md)
- [调查运行时间线](./task-wal-investigation.log)

## 0. 阅读范围和行号口径

本文所谓“有关的所有源码”，指能够参与本问题因果链、对象生命周期、内存计费、持久化或观测验证的
代码；不会逐行解释与该问题无关的图查询算子、索引、事务和 compaction 实现。

Raft、<code>FileBasedWal</code>、<code>Part</code>、<code>NebulaStore</code> 和
<code>RocksEngine</code> 的链接对应产品基线。当前工作区为实验增加了只读 WAL 指标，因此
<code>AtomicLogBuffer</code> 和 storage HTTP stats 的正文链接使用当前工作区行号。

未插桩基线中，Atomic 主要位置为：

- <code>Record/Node</code>：<code>AtomicLogBuffer.h:23-39,44-111</code>；
- buffer 状态字段：<code>AtomicLogBuffer.h:353-366</code>；
- <code>push/releaseRef/析构</code>：
  <code>AtomicLogBuffer.cpp:71-119,167-214,56-69</code>。

## 1. 先记住最终心智模型

本问题不是从 RocksDB 查询或业务 DML 开始，而是从每个稳定 Raft Leader 的周期任务开始：

~~~mermaid
flowchart TD
    A["每个 Raft Part 的 statusPolling"] --> B["稳定 Leader 调 sendHeartbeat"]
    B --> C["空 NORMAL/no-op 日志"]
    B --> D["真正 Heartbeat RPC"]
    D --> E["只校验 Leader/term<br/>不追加日志"]
    C --> F["Leader FileBasedWal"]
    F --> G["Leader AtomicLogBuffer"]
    F --> H["AppendLog RPC 到 peers"]
    H --> I["Follower FileBasedWal"]
    I --> J["Follower AtomicLogBuffer"]
    C --> K["Leader/Follower 最终 commit"]
    K --> L["RocksDB systemCommitKey"]
~~~

最重要的根因可以拆成三层：

| 层次 | 源码事实 | 在问题中的作用 |
|---|---|---|
| 触发源 | 稳定、健康空载且复制空闲的轮次，Leader 仍向本地 WAL 文件追加空 <code>NORMAL</code> 日志 | 让完全空载的集群持续产生 Raft 日志 |
| 主要放大器 | 每个本地 replica-part 有独立 Atomic buffer，空记录逻辑计费低于对象成本 | 形成按本地副本数和时间增长的 live Node |
| 次路径 | 空 payload 仍更新每个 Part 的 RocksDB commit key | 形成额外 active-memtable entry/近似字节增长 |

对应的关键入口是：

- 周期任务：[RaftPart.cpp:1401-1434，statusPolling](../src/kvstore/raftex/RaftPart.cpp#L1401-L1434)
- 异常空日志：
  [RaftPart.cpp:2041-2049，sendHeartbeat](../src/kvstore/raftex/RaftPart.cpp#L2041-L2049)
- WAL 后进入内存：
  [FileBasedWal.cpp:442-500，appendLogInternal](../src/kvstore/wal/FileBasedWal.cpp#L442-L500)
- Node 追加：
  [AtomicLogBuffer.cpp:119-172，push](../src/kvstore/wal/AtomicLogBuffer.cpp#L119-L172)
- 空 payload 的状态机提交：
  [Part.cpp:215-364，commitLogs](../src/kvstore/Part.cpp#L215-L364)

### 1.1 先用一张“源码文件地图”定位职责

阅读时不要把所有文件都看成同一层。它们从进程编排到存储引擎依次分层：

| 文件/模块 | 负责什么 | 本问题中要追的对象或函数 |
|---|---|---|
| [StorageDaemon.cpp:115-185](../src/daemons/StorageDaemon.cpp#L115-L185) | storaged 进程入口、解析地址和路径 | 创建并启动 <code>StorageServer</code> |
| [StorageServer.cpp:95-114](../src/storage/StorageServer.cpp#L95-L114) | 组装进程级存储服务 | 创建 <code>NebulaStore</code> |
| [NebulaStore.h:31-39](../src/kvstore/NebulaStore.h#L31-L39)、[NebulaStore.cpp:354-515](../src/kvstore/NebulaStore.cpp#L354-L515) | 管理 Space、Engine 和本地 Part | 本机已物化的 <code>space × data_path</code> 的 RocksEngine；每个 replica-part 的 Part |
| [Part.h:22-25](../src/kvstore/Part.h#L22-L25)、[Part.cpp:215-364](../src/kvstore/Part.cpp#L215-L364) | Nebula KV 状态机，继承 RaftPart | 解释日志 payload、更新 commit key |
| [RaftPart.h:69-99](../src/kvstore/raftex/RaftPart.h#L69-L99)、[RaftPart.cpp:786-1127](../src/kvstore/raftex/RaftPart.cpp#L786-L1127) | 单个本地 Raft 副本的共识状态机 | role、term、WAL、复制、commit |
| [Host.h:31-42](../src/kvstore/raftex/Host.h#L31-L42)、[Host.cpp:287-405](../src/kvstore/raftex/Host.cpp#L287-L405) | Leader 本地的远端 peer 代理 | 构造并发送 AppendLog RPC |
| [RaftexService.cpp:128-175](../src/kvstore/raftex/RaftexService.cpp#L128-L175) | 进程级 Raft Thrift 接收服务 | 按 <code>(space, part)</code> 路由到本地 RaftPart |
| [FileBasedWal.h:42-80](../src/kvstore/wal/FileBasedWal.h#L42-L80)、[FileBasedWal.cpp:442-536](../src/kvstore/wal/FileBasedWal.cpp#L442-L536) | 每个本地副本的 Raft 持久日志 | 磁盘 record 与 Atomic buffer 双写 |
| [AtomicLogBuffer.h:40-136](../src/kvstore/wal/AtomicLogBuffer.h#L40-L136)、[AtomicLogBuffer.cpp:119-268](../src/kvstore/wal/AtomicLogBuffer.cpp#L119-L268) | 最近 Raft 日志的内存缓存 | Record、Node、逻辑容量、dirty、GC |
| [RocksEngine.h:181-217](../src/kvstore/RocksEngine.h#L181-L217)、[RocksEngine.cpp:120-140](../src/kvstore/RocksEngine.cpp#L120-L140) | Nebula KVEngine 到 RocksDB 的适配 | batch 最终进入 <code>rocksdb::DB::Write</code> |

先记住边界：<code>RaftPart</code> 决定“哪条日志达成共识”，<code>Part</code> 决定“日志如何应用到
Nebula KV”，<code>FileBasedWal/AtomicLogBuffer</code> 保存 Raft 日志，
<code>RocksEngine</code> 保存已应用后的状态机结果。

## 2. 读源码前必须掌握的术语

### 2.1 数据和副本术语

| 术语 | 初学者理解 | 本问题中的粒度 |
|---|---|---|
| Space | 一个图空间/数据命名空间 | 对本机至少持有一个 Part 的 Space，每条 <code>data_path</code> 建立一个 RocksDB engine |
| Logical Part | Space 中的逻辑分片 | 一个独立 Raft Group |
| Replica Part | Logical Part 在某台 storaged 的一份副本 | 一个本地 C++ <code>Part</code> 对象 |
| RF | 每个 Logical Part 的副本数 | RF=3 表示集群内三份 replica-part，通常分布在三个 placement host；每份各有 WAL/buffer |
| Raft Group | 同一 logical part 的所有副本 | 任一时刻通常一个 Leader，其余 Follower |
| term | Raft 任期/Leader 世代号 | 换 Leader 时递增 |
| accepted/replicated | Leader 本机和足够多远端副本已接受日志 | 尚不能等同本机状态机已经应用 |
| committed | 满足 Raft 提交规则后成为安全提交点 | 当前 term 日志复制到多数派时，可连带确定此前日志；共识概念上已经可以应用 |
| applied | 本地状态机已经执行该日志 | 在本项目中由 <code>Part::commitLogs()</code> 最终写 RocksDB |

标准概念上 committed 和 applied 是两个阶段。Nebula 这段实现先调用 <code>commitLogs()</code>，成功后
才推进 <code>committedLogId_</code>，见 Leader 路径
[RaftPart.cpp:1068-1083](../src/kvstore/raftex/RaftPart.cpp#L1068-L1083)；因此这个字段在本实现中更接近
“本地已经 applied 的最高水位”，但分析 Raft 协议时仍应保留两个概念。

Space 的 DDL 字段 <code>partition_num</code> 和 <code>replica_factor</code> 定义在
[meta.thrift:108-117](../src/interface/meta.thrift#L108-L117)。metad 为每个 logical part 选择 RF 个
Host 的主循环位于
[CreateSpaceProcessor.cpp:236-265](../src/meta/processors/parts/CreateSpaceProcessor.cpp#L236-L265)。

因此：

~~~text
20 logical parts × RF3 = 60 replica-parts
~~~

这里的 60 是整个 storage 集群的物理副本对象数，不是单台一定有 60 个。

### 2.2 C++ 所有权术语

| 写法 | 含义 | 本源码中的例子 |
|---|---|---|
| <code>unique_ptr</code> | 唯一所有者，所有者销毁时对象销毁 | Space 独占每条 data path 上的 RocksEngine |
| <code>shared_ptr</code> | 多个所有者共同延长生命周期 | maps、RaftexService、异步任务共同持有 Part |
| 裸指针 | 通常不负责销毁，只引用别人拥有的对象 | Part 的 <code>engine_</code> |
| <code>std::move</code> | 转移资源，避免复制 | 日志字符串、队列、Promise |
| <code>std::atomic</code> | 不加普通 mutex 也能原子读写 | head、tail、refs、replicating 状态 |
| CAS | “只有当前值仍等于期望值才修改” | 保证每个 Part 只有一条主动复制流水线 |
| <code>Future/Promise</code> | 异步结果的接收端/完成端 | 调用者等待 Raft 日志最终成功或失败 |
| <code>folly::via</code> | 把任务调度到指定 executor/EventBase | WAL、RPC 和响应处理间切换线程 |
| <code>FLAGS_xxx</code> | gflags 配置的进程级值 | heartbeat、WAL 大小、线程数 |

### 2.3 内存术语

| 术语 | 含义 | 不能混淆为 |
|---|---|---|
| 逻辑计费 | 容器自己统计的业务字节 | C++ 对象真实大小 |
| <code>sizeof</code> requested | 程序向 allocator 请求的对象大小 | allocator 实际 size class |
| allocator usable | jemalloc 为请求分配的可用块大小 | 进程 RSS |
| live object | 仍可从根对象访问、尚未 delete | 传统失去引用泄漏 |
| RSS | 进程当前驻留物理页 | 某一个容器的精确内存 |
| page cache | 内核缓存文件页 | 进程匿名堆 |
| memtable | RocksDB 的内存写入结构 | Raft AtomicLogBuffer |

本案最核心的区别是：

~~~text
Atomic size_ 的 8MiB 逻辑容量
≠ 8MiB C++ Node 对象
≠ 8MiB jemalloc usable
≠ 8MiB RSS
~~~

## 3. 三种“心跳”必须先分清

源码里 heartbeat 一词跨了不同层，混在一起会直接读错调用链。

| 名称 | 配置/入口 | 目的 | 会否新增 Raft WAL |
|---|---|---|---:|
| Storage → Meta 心跳 | <code>heartbeat_interval_secs</code> | 上报 Host、角色、leader parts、磁盘信息 | 否 |
| 真正 Raft Heartbeat RPC | <code>Host::sendHeartbeat</code> | 维持 Leader 身份、重置 Follower 选举计时 | 否 |
| 空 <code>NORMAL</code>/no-op 日志 | <code>appendLogAsync(..., "")</code> | 设计上用于新 Leader 首次提交当前-term 日志；当前实现却在稳定 term 反复创建 | 是 |

Storage → Meta 心跳由
[MetaClient.cpp:162-190](../src/clients/meta/MetaClient.cpp#L162-L190) 定时，
完整请求构造和发送位于
[MetaClient.cpp:2605-2668](../src/clients/meta/MetaClient.cpp#L2605-L2668)。发布模板对应
[nebula-storaged.conf.default:40-48](../conf/nebula-storaged.conf.default#L40-L48) 的两个不同 flag：

- <code>heartbeat_interval_secs=10</code>：MetaClient；
- <code>raft_heartbeat_interval_secs=30</code>：每个 RaftPart。

真正的 Raft Heartbeat RPC 发送逻辑位于
[RaftPart.cpp:2051-2122](../src/kvstore/raftex/RaftPart.cpp#L2051-L2122)，Follower handler 明确写明
“不 append log”，见
[RaftPart.cpp:1895-1951，processHeartbeatRequest](../src/kvstore/raftex/RaftPart.cpp#L1895-L1951)。

空日志虽然在 [Part.cpp:229-230](../src/kvstore/Part.cpp#L229-L230) 的日志里被口语化称为 heartbeat，
但它走的是 AppendLog RPC，必须写 WAL。本文后续统一称它为“空 NORMAL/no-op 日志”。

标准 Raft 文献有时也把“不携带新 entry 的 AppendEntries”称为 heartbeat；本文“真正 Raft Heartbeat
RPC”特指 Nebula 单独定义的 Thrift <code>heartbeat()</code>。理解代码时应以具体 RPC/调用链为准，
不能只按单词 heartbeat 判断是否写 WAL。

还有一条容易被搜索结果误导的空日志：
[Part.cpp:115-117，Part::sync](../src/kvstore/Part.cpp#L115-L117) 会显式发送空
<code>COMMAND</code>。它只有调用 <code>sync()</code> 时才发生，与本案由
<code>statusPolling → sendHeartbeat</code> 周期产生的空 <code>NORMAL</code> 日志不是同一入口。
同样，<code>LogType::NORMAL/COMMAND</code> 用于 Leader 本地批处理边界；RPC 的
[RaftLogEntry/AppendLogRequest，raftex.thrift:53-70](../src/interface/raftex.thrift#L53-L70) 并不序列化
该枚举。entry 自身携带 term/source 和长度为 0 的 payload；Follower 根据请求中的
<code>last_log_id_sent</code> 加列表偏移推导 LogID。

## 4. 从 storaged 进程启动到每个 Part 的对象树

### 4.1 进程入口

storaged 入口读取地址、Meta 地址和 data paths，然后创建 <code>StorageServer</code>：

- [StorageDaemon.cpp:115-156](../src/daemons/StorageDaemon.cpp#L115-L156)
- [StorageDaemon.cpp:179-185](../src/daemons/StorageDaemon.cpp#L179-L185)

<code>StorageServer::start()</code> 创建 IO/worker 线程池和 MetaClient：

- [StorageServer.cpp:178-205](../src/storage/StorageServer.cpp#L178-L205)

随后 <code>getStoreInstance()</code> 组装 <code>KVOptions</code>，创建并初始化
<code>NebulaStore</code>：

- [StorageServer.cpp:95-114](../src/storage/StorageServer.cpp#L95-L114)

<code>StorageServer::start()</code> 还会等待 MetaClient 就绪，创建 Schema/Index manager，再建立主
KVStore，位置见
[StorageServer.cpp:236-282](../src/storage/StorageServer.cpp#L236-L282)。同一 storaged 另有一个
<code>spaceId=0</code> 的 admin-task RocksEngine：
[StorageServer.cpp:152-156](../src/storage/StorageServer.cpp#L152-L156)、
[272-282](../src/storage/StorageServer.cpp#L272-L282)。它不承载 50 个数据 Space 的周期 commit-key，
所以不能把这个 admin DB 混入“本机已物化的每个数据 space × data_path 一个 RocksEngine”的对象数。

<code>NebulaStore::init()</code> 做四件与本题直接相关的事：

1. 建 background workers；
2. 启动 Raftex RPC 服务；
3. 从磁盘和 Meta 分配加载 Space/Part；
4. 安排 WAL 清理任务。

代码见 [NebulaStore.cpp:47-77](../src/kvstore/NebulaStore.cpp#L47-L77)。

### 4.2 Space、RocksEngine 与 Part

<code>SpacePartInfo</code> 同时保存：

- <code>parts_[partId]</code>：本机拥有的 replica-part；
- <code>engines_</code>：该 Space 在每条 data path 上的 KV engine。

定义见 [NebulaStore.h:31-39](../src/kvstore/NebulaStore.h#L31-L39)，进程级
<code>spaces_</code> map 位于
[NebulaStore.h:868-884](../src/kvstore/NebulaStore.h#L868-L884)。

析构顺序先清 <code>parts_</code>、再清 <code>engines_</code> 很重要：Part 中的
<code>engine_</code> 是不拥有对象的裸指针，因此必须先销毁 Part。

对当前 storaged 已经物化的 Space，每条 <code>data_path</code> 创建一个 RocksEngine：

- [NebulaStore.cpp:354-370，newEngineAsync](../src/kvstore/NebulaStore.cpp#L354-L370)
- [NebulaStore.cpp:395-421，addSpace](../src/kvstore/NebulaStore.cpp#L395-L421)

“已物化”是重要限定：MetaClient 只把分配给本机的 Part 组成 part map，见
[MetaClient.cpp:999-1017，doGetPartsMap](../src/clients/meta/MetaClient.cpp#L999-L1017)；某个 Space
第一次出现在本机 part map 时才触发 <code>onSpaceAdded()</code>，见
[MetaClient.cpp:1020-1039](../src/clients/meta/MetaClient.cpp#L1020-L1039)。因此 storaged 数大于 RF 时，
一台机器不一定物化集群中的所有 Space；本实验因为 RF=3 且恰好三台 storaged，三台才各自物化全部
50 个数据 Space。

添加本地副本时，<code>addPart()</code> 在该 Space 的 engines 中选择当前 Part 最少的一个，
然后创建 Part：

- [NebulaStore.cpp:437-479，addPart](../src/kvstore/NebulaStore.cpp#L437-L479)
- [NebulaStore.cpp:484-515，newPart](../src/kvstore/NebulaStore.cpp#L484-L515)

<code>Part</code> 继承 <code>RaftPart</code>，并保存指向共享 RocksEngine 的
<code>engine_</code>：

- [Part.h:22-25](../src/kvstore/Part.h#L22-L25)
- [Part.cpp:23-50，Part::Part](../src/kvstore/Part.cpp#L23-L50)

<code>RaftPart</code> 构造时创建独立 FileBasedWal：

- [RaftPart.cpp:330-372，RaftPart::RaftPart](../src/kvstore/raftex/RaftPart.cpp#L330-L372)

FileBasedWal 构造时再创建独立 AtomicLogBuffer：

- [FileBasedWal.cpp:40-60，FileBasedWal::FileBasedWal](../src/kvstore/wal/FileBasedWal.cpp#L40-L60)

注意 [AtomicLogBuffer.h:262-271](../src/kvstore/wal/AtomicLogBuffer.h#L262-L271) 的
<code>instance()</code> 每次都会 <code>new AtomicLogBuffer</code>，它不是全局单例。

### 4.3 最终对象所有权

~~~mermaid
flowchart TD
    S["storaged"] --> NS["NebulaStore"]
    NS --> SP["SpacePartInfo(spaceId)"]
    SP --> E["engines_[data_path]<br/>unique_ptr RocksEngine"]
    SP --> P["parts_[partId]<br/>shared_ptr Part<br/>内含 RaftPart 基类子对象"]
    P -. "非拥有 engine_ 指针" .-> E
    P --> W["shared_ptr FileBasedWal"]
    P --> H["hosts_<br/>shared_ptr Host"]
    H -. "part_ shared_ptr<br/>stop 时打破环" .-> P
    W --> B["shared_ptr AtomicLogBuffer"]
    B --> N["Node 链"]
    NS --> RS["RaftexService"]
    RS --> P
~~~

<code>RaftPart::hosts_</code> 持有 <code>shared_ptr&lt;Host&gt;</code>，而 Host 的
<code>part_</code> 又是 <code>shared_ptr&lt;RaftPart&gt;</code>，见
[Host.h:243-248](../src/kvstore/raftex/Host.h#L243-L248)。正常停止时
[RaftPart.cpp:456-483](../src/kvstore/raftex/RaftPart.cpp#L456-L483) 把 <code>hosts_</code> 移出、停止并
清空，从而打破这条环形所有权。固定运行期内，这也是 Part 不会因某个临时调用结束而析构的原因之一。

这棵树说明为什么固定拓扑中 Node 会保持可达：

~~~text
NebulaStore
  → Part
  → RaftPart::wal_
  → FileBasedWal::logBuffer_
  → AtomicLogBuffer::head_
  → Node
~~~

### 4.4 Part 的启动、恢复和销毁

进程恢复不是只信磁盘，也不是只信 Meta。<code>NebulaStore::init()</code> 先调用
[loadPartFromDataPath，NebulaStore.cpp:80-289](../src/kvstore/NebulaStore.cpp#L80-L289)：

1. 扫描每条 <code>&lt;data_path&gt;/nebula/&lt;spaceId&gt;</code>，打开已有 RocksEngine；
2. 从 engine 的 system part key 恢复本地 part，并和 Meta/正在 balance 的 peer 信息核对；
3. 对仍有效的副本调用 <code>newPart()</code> 重建 C++ Part/Raft/WAL 对象。

随后
[loadPartFromPartManager，NebulaStore.cpp:291-306](../src/kvstore/NebulaStore.cpp#L291-L306)
遍历 Meta 认为本机应持有的副本，补建磁盘上尚不存在的 Space/Part。运行期 Meta cache 变化则通过
[PartManager.cpp:87-93，onSpaceAdded](../src/kvstore/PartManager.cpp#L87-L93) 和
[165-168，onPartAdded](../src/kvstore/PartManager.cpp#L165-L168) 回到同一
<code>NebulaStore::addSpace/addPart</code> 路径。

[RaftPart.cpp:400-454，start](../src/kvstore/raftex/RaftPart.cpp#L400-L454) 启动一份本地副本时：

1. 从 FileBasedWal 恢复末端 LogID/term；
2. 调用派生类接口从 RocksDB 恢复已提交水位；
3. 为每个远端 peer 创建 <code>Host</code>；
4. 计算 quorum，把状态改成 RUNNING；
5. 安排第一次 <code>statusPolling()</code>。

RocksDB commit key 的恢复实现是
[Part.cpp:52-67，lastCommittedLogId](../src/kvstore/Part.cpp#L52-L67)。这也解释了为什么本题的次路径
必须持久化 commit key：重启后需要用它区分“WAL 中已有”与“状态机已应用”。

Raftex RPC 服务还会保存同一 Part 的 <code>shared_ptr</code>，添加和查找位置见
[RaftexService.cpp:81-115](../src/kvstore/raftex/RaftexService.cpp#L81-L115)。删除副本时，
[NebulaStore.cpp:576-595，removePart](../src/kvstore/NebulaStore.cpp#L576-L595) 先从 RaftexService 移除并
停止 Part，再从 Space map 和 RocksEngine 删除；RaftexService 的异步停止细节见
[RaftexService.cpp:88-103](../src/kvstore/raftex/RaftexService.cpp#L88-L103)。

因此固定拓扑下 Part/WAL/buffer 会一直存活。drop、rebalance 或进程退出会改变对象生命周期，reset
会改变 buffer 状态；这些都是后续做指标外推时必须声明的边界。

### 4.5 Meta 如何把 logical part 变成多份 replica-part

DDL 中的 <code>partition_num</code> 决定 logical Raft group 数，<code>replica_factor</code> 决定每组
选几个 placement host。未显式指定时，Meta 会在
[CreateSpaceProcessor.cpp:44-77](../src/meta/processors/parts/CreateSpaceProcessor.cpp#L44-L77)
补默认值；没有指定 zone 时使用所有 zone，见
[115-137](../src/meta/processors/parts/CreateSpaceProcessor.cpp#L115-L137)。

普通 <code>ADD HOSTS</code> 为每台 host 自动生成一个 default zone：
[AddHostsProcessor.cpp:69-92](../src/meta/processors/zone/AddHostsProcessor.cpp#L69-L92)。创建 Space 时，
[CreateSpaceProcessor.cpp:236-266](../src/meta/processors/parts/CreateSpaceProcessor.cpp#L236-L266)
对每个 logical part 选择 <code>replica_factor</code> 个 zone/host，并写入 part placement。

本实验恰好是三个单-host zone、RF=3，所以每个 logical part 必然在三台 storaged 各有一份本地
<code>Part</code>。因此：

~~~text
50 spaces × 20 logical parts = 1000 Raft groups
1000 groups × RF3 = 3000 cluster-wide replica-parts
3000 / 3 storaged = 1000 local replica-parts/storaged
~~~

这个 placement 事实是“每台 1000 个 FileBasedWal/AtomicLogBuffer”的上游来源，不是根据 RSS
反推出来的假设。

## 5. Raft 只学本题需要的最小集合

### 5.1 正常写入的五步

一条写入从 Leader 到可见状态，可以简化为：

~~~text
1. Leader 分配 LogID
2. Leader 先写自己的 WAL
3. Leader 用 AppendLog RPC 复制到 peers
4. 达到多数派后成为 committed
5. Part::commitLogs 把日志应用到 RocksDB 状态机
~~~

Raft 层只把 payload 当字节串；<code>Part</code> 才解释 PUT、REMOVE 等业务操作。
抽象状态机接口和实现分别位于：

- [RaftPart.h:487-500](../src/kvstore/raftex/RaftPart.h#L487-L500)
- [Part.cpp:215-364](../src/kvstore/Part.cpp#L215-L364)

Raft 日志的批处理类型定义在
[RaftPart.h:37-57](../src/kvstore/raftex/RaftPart.h#L37-L57)：<code>NORMAL</code> 是可出现在普通批次中的
日志，<code>ATOMIC_OP</code> 和 <code>COMMAND</code> 有更严格的批次边界。本案 no-op 采用
<code>NORMAL + 空 payload</code>，因此它仍走普通复制链，只是在状态机解释 payload 时被跳过。

### 5.2 核心状态字段

字段集中在 [RaftPart.h:789-860](../src/kvstore/raftex/RaftPart.h#L789-L860)：

| 字段 | 含义 |
|---|---|
| <code>status_</code> | 对象生命周期：STARTING/RUNNING/STOPPED/WAITING_SNAPSHOT |
| <code>role_</code> | Raft 身份：Leader/Follower/Candidate/Learner |
| <code>term_</code> | 当前 Leader 世代 |
| <code>lastLogId_</code> | Leader 上是多数派确认边界；Follower 上是本地收到的日志末端 |
| <code>committedLogId_</code> | 已应用到本地状态机的最高日志 |
| <code>logs_</code> | 新进入、尚未组成发送批的日志 |
| <code>sendingLogs_</code> | 当前复制流水线正在处理的批 |
| <code>replicatingLogs_</code> | 当前是否已有复制流水线 |
| <code>commitInThisTerm_</code> | 当前 Leader 是否已在本 term 真正 commit 过日志 |
| <code>wal_</code> | 每 Part 专属的一份 Raft WAL 实例，字段类型为 <code>shared_ptr</code> |

<code>replicatingLogs_</code> 是瞬时“忙/闲”状态；<code>commitInThisTerm_</code> 是整个 term 的
语义状态。后面会看到，本问题正是把这两种状态混用。

### 5.3 为什么新 Leader 需要一条 no-op

新 Leader 不能仅凭“旧日志已存在于多数副本”就随意提交前一 term 的日志。工程实现通常让新 Leader
先提交一条属于当前 term 的 no-op；当该条提交成功时，前面的日志也获得安全提交边界。

Nebula 用 payload 为空的 <code>NORMAL</code> 日志承担这个角色。

正式当选后：

- <code>commitInThisTerm_=false</code>：
  [RaftPart.cpp:1370-1395](../src/kvstore/raftex/RaftPart.cpp#L1370-L1395)
- 立即调用一次 <code>sendHeartbeat()</code>：
  [RaftPart.cpp:1390-1395](../src/kvstore/raftex/RaftPart.cpp#L1390-L1395)
- 首次成功 commit 后置 true：
  [RaftPart.cpp:1068-1095](../src/kvstore/raftex/RaftPart.cpp#L1068-L1095)
- 多副本 Leader 的 lease 也要求该字段为 true：
  [RaftPart.cpp:2254-2267](../src/kvstore/raftex/RaftPart.cpp#L2254-L2267)

所以“当选后提交一次空日志”本身有明确用途；问题不是 no-op 的存在，而是稳定 term 中仍反复创建它。

历史设计背景可从 [PR #606](https://github.com/vesoft-inc/nebula/pull/606) 看到：早期实现把 heartbeat
设计成会写 WAL 的特殊空日志，以帮助新 Leader 提交前任 term 的遗留日志。当前官方仓库中的
[Issue #6156](https://github.com/vesoft-inc/nebula/issues/6156) 也独立指出
<code>sendHeartbeat()</code> 没有用 <code>commitInThisTerm_</code> 限制 no-op；截至本次分析日期该 issue
仍是 Open、无维护者确认，所以它只作外部交叉验证，根因仍以当前分支源码和运行数据为准。

### 5.4 Leader 是怎样选出的，为什么当选后还不能立即视为 ready

Role 和本地生命周期 Status 是两套正交状态：
[raftex.thrift:10-23](../src/interface/raftex.thrift#L10-L23) 定义
Leader/Follower/Candidate/Learner 与 STARTING/RUNNING/STOPPED/WAITING_SNAPSHOT。
<code>RaftPart::start()</code> 从 WAL 与 commit key 恢复水位，建立 peers，切换为 RUNNING，并在
100～999ms 后启动第一次轮询，见
[RaftPart.cpp:400-453](../src/kvstore/raftex/RaftPart.cpp#L400-L453)。

以后每轮 <code>statusPolling()</code> 大致执行：

~~~text
Follower 超过完整 H 未收到 Leader 消息，或处于 blind-follower 状态
  → role 变为 Candidate；已经是 Candidate 时也会继续发起选举
  → pre-vote 探测是否可能获胜（不先增加本地 term）
  → 正式 vote 才增加 term 并给自己投票
  → 多数派同意后 role=Leader、leader_=本机
  → commitInThisTerm_=false、重置各 Host 复制进度
  → 立即调用一次 sendHeartbeat()
  → 若此时复制流水线空闲，异步调度新 term no-op
~~~

源码位置：

- 选举超时与 Candidate 转换：
  [RaftPart.cpp:1143-1155，needToStartElection](../src/kvstore/raftex/RaftPart.cpp#L1143-L1155)；
- pre-vote/正式 vote 的 term 处理和日志新旧水位：
  [RaftPart.cpp:1157-1195，prepareElectionRequest](../src/kvstore/raftex/RaftPart.cpp#L1157-L1195)；
- 多数派结果把角色切成 Leader：
  [RaftPart.cpp:1268-1285，processElectionResponses](../src/kvstore/raftex/RaftPart.cpp#L1268-L1285)；
- 当选后的状态重置与第一次 <code>sendHeartbeat()</code>：
  [RaftPart.cpp:1370-1395，handleElectionResponses](../src/kvstore/raftex/RaftPart.cpp#L1370-L1395)。

因此要区分四层语义：

| 问题 | 主要判断 |
|---|---|
| 本地对象能否工作？ | <code>status_ == RUNNING</code> |
| 本机是否已经当选？ | <code>role_ == LEADER</code> |
| 当前 term 是否已有安全提交？ | <code>commitInThisTerm_</code> |
| 对外是否有有效 Leader lease？ | 通常检查 <code>role_ == LEADER &amp;&amp; leaseValid()</code> |

<code>NebulaStore::checkLeader()</code> 通常同时检查角色和 lease；若调用者显式允许 follower read，
则可绕过这两个检查：
[NebulaStore.cpp:1289-1291](../src/kvstore/NebulaStore.cpp#L1289-L1291)。多副本的
<code>leaseValid()</code> 又要求当前 term 已 commit；单副本因 <code>hosts_.empty()</code> 直接返回 true：
[RaftPart.cpp:2254-2267](../src/kvstore/raftex/RaftPart.cpp#L2254-L2267)。
这说明“新 Leader 需要一次 no-op”是合理的协议阶段；根因是完成这一阶段后仍反复产生 no-op。

## 6. 周期触发器与真正缺陷

### 6.1 每个 Part 都有自己的 statusPolling 链

Part 启动恢复 WAL、commit 水位、peers 和 quorum 后，安排第一次轮询：

- [RaftPart.cpp:400-454，start](../src/kvstore/raftex/RaftPart.cpp#L400-L454)

<code>statusPolling()</code> 每轮检查选举、Leader heartbeat 和 snapshot，结束时只安排一个
one-shot 后继：

- [RaftPart.cpp:1401-1434](../src/kvstore/raftex/RaftPart.cpp#L1401-L1434)

计划延迟公式是：

~~~text
delay_ms = raft_heartbeat_interval_secs × 1000 / 3
         + random_integer(0..499)
~~~

这里还有两个“默认值”层次：C++ gflag 在
[RaftPart.cpp:30](../src/kvstore/raftex/RaftPart.cpp#L30) 定义为 5 秒，但发行版 storage 配置模板在
[nebula-storaged.conf.default:44-50](../conf/nebula-storaged.conf.default#L44-L50) 覆盖为 30 秒；
进程最终使用启动时实际加载的配置值。本文的生产条件估算使用发布模板 H=30，实验配置则显式使用
H=1，不能把三者混为一谈。

发布模板 H=30，因此固定部分是 10 秒，随机均值是 249.5ms，名义计划延迟均值是 10.2495 秒。
这不是严格 wall-clock 周期；实际调用间隔还包含函数执行、worker 排队和 timer 抖动。

稳定 Leader 的判断
[needToSendHeartbeat，RaftPart.cpp:1138-1141](../src/kvstore/raftex/RaftPart.cpp#L1138-L1141)
只检查 <code>RUNNING + LEADER</code>。

### 6.2 一个 sendHeartbeat 中存在两条分支

[RaftPart.cpp:2041-2122](../src/kvstore/raftex/RaftPart.cpp#L2041-L2122) 可以简化为：

~~~text
sendHeartbeat():
    if not replicatingLogs:
        在 executor 上 append 空 NORMAL 日志

    向所有 peers 发送真正 Heartbeat RPC
    统计 term 和多数派响应，更新 lease 时间
~~~

前半段是有 LogID、WAL、复制和 commit 的数据路径；后半段是不写日志的活性 RPC。
空日志通过 <code>folly::via</code> 异步投递，真正 heartbeat RPC 随后直接发起；两条路径没有保证
严格的跨网络先后顺序。根因只要求前者最终反复进入 WAL，不依赖它一定先于或晚于 heartbeat RPC
到达 Follower。

### 6.3 为什么它会在稳定 term 中一直重复

空日志的条件只有 <code>!replicatingLogs_</code>：

- 源码注释说的是“当前 term 尚未 commit 时，需要 append 一条日志”；
- 实际代码没有读取 <code>commitInThisTerm_</code>；
- 健康复制完成后，
  [RaftPart.cpp:1103-1127](../src/kvstore/raftex/RaftPart.cpp#L1103-L1127)
  会把 <code>replicatingLogs_</code> 重新置为 false；
- 下一轮 <code>statusPolling()</code> 因而再次满足条件。

两者的时间尺度完全不同：

~~~text
commitInThisTerm_  ：一个 term 内从首次 commit 起长期为 true
replicatingLogs_   ：一次 AppendLog 流水线期间为 true，完成后恢复 false
~~~

全仓中 <code>commitInThisTerm_</code> 的产品逻辑引用只有声明、换届重置、首次 commit 置真和
lease 检查，位置分别是
[RaftPart.h:855-857](../src/kvstore/raftex/RaftPart.h#L855-L857)、
[RaftPart.cpp:1388](../src/kvstore/raftex/RaftPart.cpp#L1388)、
[1091-1094](../src/kvstore/raftex/RaftPart.cpp#L1091-L1094) 和
[2254-2267](../src/kvstore/raftex/RaftPart.cpp#L2254-L2267)。

因此根因的最精确表述是：

> “当前 term 是否已经完成首次 commit”的长期语义，被错误地用“此刻是否有复制流水线”的瞬时
> 状态控制；已有的 <code>commitInThisTerm_</code> 没有参与 no-op 决策。

## 7. Leader 空日志的完整调用链

本节把一条空日志从函数调用还原为对象、线程和状态的变化。

### 7.1 进入待发送队列

<code>sendHeartbeat()</code> 在 executor 上调用：

~~~cpp
appendLogAsync(clusterId_, LogType::NORMAL, "")
~~~

完整实现位于
[RaftPart.cpp:786-872，appendLogAsync](../src/kvstore/raftex/RaftPart.cpp#L786-L872)。它依次完成：

1. 建立一个 <code>Promise</code>，把空字符串和 Promise 放入 <code>logs_</code>；
2. 用 CAS 把 <code>replicatingLogs_</code> 从 false 改成 true；
3. 读取当前 term，并分配 <code>firstId=lastLogId_+1</code>；
4. 把 <code>logs_</code> 转移到 <code>sendingLogs_</code>；
5. 构造 <code>AppendLogsIterator</code>，进入 <code>appendLogsInternal()</code>。

<code>logs_</code> 的 <code>FLAGS_max_batch_size</code> 检查在
[RaftPart.cpp:811-817](../src/kvstore/raftex/RaftPart.cpp#L811-L817)，CAS 在
[827-835](../src/kvstore/raftex/RaftPart.cpp#L827-L835)。这说明它是有界的瞬时批队列，不是本案随
时间增长的主要容器。

还有一个容易误读的名字：
[AppendLogsIterator::commit，RaftPart.cpp:101-109](../src/kvstore/raftex/RaftPart.cpp#L101-L109)
只是完成调用者的 Promise；它不是把数据提交到 RocksDB。真正的状态机 commit 是
<code>Part::commitLogs()</code>。

### 7.2 Leader 先写自己的 Raft WAL

[RaftPart.cpp:874-915，appendLogsInternal](../src/kvstore/raftex/RaftPart.cpp#L874-L915) 在
<code>raftLock_</code> 下保存四个关键水位：

| 值 | 含义 |
|---|---|
| <code>prevLogId/prevLogTerm</code> | 当前 Leader 的前置匹配点，用于 Follower 一致性校验；复制在途时不保证等于 WAL 文件物理尾部 |
| <code>committed</code> | 发送这批以前 Leader 已提交到哪一条 |
| <code>currTerm</code> | 本次复制所属 term |
| <code>lastId</code> | 写完 Leader WAL 后的新尾部 |

随后 <code>wal_->appendLogs(iter)</code> 先把日志追加到 Leader 本机 WAL 文件。
[FileBasedWal.cpp:515-527，appendLogs](../src/kvstore/wal/FileBasedWal.cpp#L515-L527) 对 iterator
逐条调用 <code>appendLogInternal()</code>。即使 payload 长度为 0，也不会跳过。底层先执行
<code>write()</code>；是否每条再 <code>fsync()</code> 由 <code>policy_.sync/wal_sync</code> 决定，见
[FileBasedWal.cpp:480-488](../src/kvstore/wal/FileBasedWal.cpp#L480-L488)。因此本文说“WAL 接受”时，不
额外假定默认配置已经对每条执行 fsync。

### 7.3 复制请求怎样保留“空 payload”

[RaftPart.cpp:918-999，replicateLogs](../src/kvstore/raftex/RaftPart.cpp#L918-L999) 对 peers 调用
<code>Host::appendLogs()</code>，并等待足够多非 learner peer 成功。

请求构造位于
[Host.cpp:287-345，prepareAppendLogRequest](../src/kvstore/raftex/Host.cpp#L287-L345)：它从 Leader
WAL iterator 按 LogID 顺序读取记录，但每个 <code>RaftLogEntry</code> 只携带 source、term 和
<code>log_str</code>；结构定义见
[raftex.thrift:53-70](../src/interface/raftex.thrift#L53-L70)。请求另带 <code>last_log_id_sent</code>，
Follower 用它加列表偏移推导每条 LogID。<code>logMsg()</code> 即使是空字符串，仍占据列表中的一个
entry，因而仍是一条有推导 LogID、term 和 source 的合法日志，不是“没有日志”。

本实验没有 listener，因此 RF=3 Leader 的 <code>hosts_</code> 只保存另外两个普通 peer；产品定义中
<code>hosts_</code> 还会包含 listeners，见
[RaftPart.h:795-800](../src/kvstore/raftex/RaftPart.h#L795-L800)。
[RaftPart.cpp:416-417](../src/kvstore/raftex/RaftPart.cpp#L416-L417) 计算
<code>quorum_=(peers+1)/2=1</code>。这表示 Leader 自己已经写 WAL 后，再有一个远端非 learner 成功，
就达到三副本多数派 2/3。

### 7.4 多数派成功后 Leader 立即应用

[RaftPart.cpp:1002-1101，processAppendLogResponses](../src/kvstore/raftex/RaftPart.cpp#L1002-L1101)
完成三件事：

1. 统计成功响应并检查是否出现更高 term；
2. 达到 quorum 后更新 <code>lastLogId_/lastLogTerm_</code>；
3. 取得 <code>wal_->iterator(committedId+1,lastLogId)</code>，调用
   <code>Part::commitLogs()</code> 应用这一批。

成功后 <code>committedLogId_</code> 前移，并在本 term 第一次成功时将
<code>commitInThisTerm_</code> 置 true。然后
[RaftPart.cpp:1103-1127](../src/kvstore/raftex/RaftPart.cpp#L1103-L1127) 完成 Promise；若没有新业务
日志排队，就把 <code>replicatingLogs_</code> 置回 false。这正是下一轮空日志能够再次进入的条件。

### 7.5 一张 Leader 时序图

~~~mermaid
sequenceDiagram
    participant T as "statusPolling timer"
    participant L as "RaftPart Leader"
    participant LW as "Leader FileBasedWal"
    participant H as "Host/AppendLog RPC"
    participant F as "Follower RaftPart"
    participant DB as "Leader RocksDB"
    T->>L: "sendHeartbeat()"
    L->>L: "appendLogAsync(NORMAL, empty)"
    L->>LW: "append log N"
    LW->>LW: "disk write + Atomic push"
    L->>H: "replicate log N, committed=N-1"
    H->>F: "AppendLogRequest"
    F-->>H: "WAL accepted"
    H-->>L: "quorum success"
    L->>DB: "commitLogs(N)"
    L->>L: "commitInThisTerm=true"
    L->>L: "replicatingLogs=false"
~~~

## 8. Follower 路径和“滞后一轮”的提交语义

### 8.1 RPC 到本地 WAL

Raftex 服务按 <code>spaceId/partId</code> 找到本地 Part 并路由请求，入口见
[RaftexService.cpp:139-147](../src/kvstore/raftex/RaftexService.cpp#L139-L147)。Follower 的核心处理函数是
[RaftPart.cpp:1610-1827，processAppendLogRequest](../src/kvstore/raftex/RaftPart.cpp#L1610-L1827)。

它先校验：

- term 是否过期；
- Leader 身份是否一致；
- <code>last_log_id_sent/last_log_term_sent</code> 是否能与本地 WAL 对上；
- 必要时是否应 rollback。

通过后，[RaftPart.cpp:1757-1777](../src/kvstore/raftex/RaftPart.cpp#L1757-L1777) 把请求中的
<code>log_str_list</code> 包装为 iterator，再执行本地 <code>wal_->appendLogs(logIter)</code>。在本次
两个 Follower 均健康可达并成功跟随的 RF3 实验中，两者都会经历同样的磁盘编码和 Atomic buffer
push；Raft 达成多数派本身只要求其中一个远端成功。

### 8.2 为什么 Follower 通常下一轮才提交这一条

Leader 构造第 N 条请求时，传入的 <code>committedId</code> 是“写第 N 条之前”的水位，见
[RaftPart.cpp:874-914](../src/kvstore/raftex/RaftPart.cpp#L874-L914)。稳定空闲时通常是 N-1。

Follower 在
[RaftPart.cpp:1785-1804](../src/kvstore/raftex/RaftPart.cpp#L1785-L1804) 根据请求携带的
<code>committed_log_id</code> 提交本地旧日志。所以在“健康、稳定 term、空载且通常每批一条
no-op”的条件下，时序是：

~~~text
第 N 轮：
  Leader/Follower WAL 都追加 N
  Follower 最多提交到请求携带的 N-1
  Leader 收到多数响应后提交 N

第 N+1 轮：
  请求携带 committed=N
  Follower 在追加 N+1 时提交 N
~~~

真正 Heartbeat RPC 的 Follower handler
[RaftPart.cpp:1895-1951](../src/kvstore/raftex/RaftPart.cpp#L1895-L1951) 不追加日志，也不执行这段
<code>commitLogs()</code>。因此空闲场景中，重复 no-op AppendLog 同时承担了把前一条 commit 水位带给
Follower 的作用。

在本次所有副本均健康跟随的固定拓扑中，长期看三副本的 WAL push 速率相同，Follower 的 RocksDB
commit-key 更新只比 Leader 约晚一个轮次。这解释了为何“Leader 数量不均”并不会让某台 storaged
的 Atomic 增长明显更快。

## 9. FileBasedWal：磁盘 WAL 与内存缓存是两个并行结果

### 9.1 接口和每 Part 实例

<code>Wal</code> 抽象定义 append、iterator、rollback、reset 和 clean，见
[Wal.h:15-103](../src/kvstore/wal/Wal.h#L15-L103)。本分支的 Raft Part 使用
<code>FileBasedWal</code>。

默认策略在
[FileBasedWal.cpp:15-18](../src/kvstore/wal/FileBasedWal.cpp#L15-L18)：

| 参数 | 默认值 | 管什么 |
|---|---:|---|
| <code>wal_ttl</code> | 4h | 过期磁盘 Raft WAL 文件 |
| <code>wal_file_size</code> | 16MiB | 单个 Raft WAL 文件滚动大小 |
| <code>wal_buffer_size</code> | 8MiB | Atomic buffer 的逻辑计费容量 |
| <code>wal_sync</code> | false | 是否每次 append 后执行 <code>fsync</code>；不改变 Atomic push |

[FileBasedWal.cpp:40-75](../src/kvstore/wal/FileBasedWal.cpp#L40-L75) 每次构造都会创建一个新的
<code>AtomicLogBuffer</code>，扫描已有磁盘文件并恢复 <code>firstLogId_</code>、
<code>lastLogId_</code> 和 <code>lastLogTerm_</code>。扫描不会把整个历史 WAL 回灌到 Atomic buffer；
进程重启后，新的内存 buffer 从空开始，后续读取若未命中内存再读文件。

### 9.2 空日志在磁盘上为什么是 32B

[FileBasedWal.cpp:442-500，appendLogInternal](../src/kvstore/wal/FileBasedWal.cpp#L442-L500) 的顺序是：

~~~text
1. preProcess
2. 编码 LogID + TermID + len + ClusterID + payload + len
3. write()/可选 fsync 到磁盘文件
4. 更新 FileBasedWal 首尾元数据
5. logBuffer_->push(...)
~~~

三种 ID 均为 <code>int64_t</code>，定义见
[ThriftTypes.h:12-18](../src/common/thrift/ThriftTypes.h#L12-L18)。空 payload 的编码大小因此为：

~~~text
LogID 8 + TermID 8 + length 4 + ClusterID 8 + payload 0 + length 4 = 32B
~~~

这个 32B 是 Raft WAL 文件 record 编码，不是内存 <code>Record</code> 的 <code>sizeof</code>。

### 9.3 Atomic buffer 的用途

读取 WAL 时，
[FileBasedWal.cpp:530-536，iterator](../src/kvstore/wal/FileBasedWal.cpp#L530-L536) 先尝试
<code>logBuffer_->iterator()</code>；若起始 LogID 未命中、返回的 iterator 无效，才创建
<code>WalFileIterator</code> 读盘。
所以 Atomic buffer 是近期 Raft 日志的读优化，也是复制和 commit 的快速来源。

要特别区分两种 WAL：

| 名称 | 所属层 | 本题角色 |
|---|---|---|
| <code>FileBasedWal</code> | Nebula Raft 层 | 每 replica-part 一份；空日志必写 |
| RocksDB 自己的 WAL | RocksDB engine 层 | 由 <code>WriteOptions.disableWAL</code> 控制 |

[RocksEngine.cpp:124-133](../src/kvstore/RocksEngine.cpp#L124-L133) 设置的是 RocksDB WAL 选项；它不会
改变前面的 Raft <code>FileBasedWal</code> 和 Atomic buffer。

### 9.4 文件轮转、关闭同步与重启恢复

磁盘文件名是该文件第一条 LogID 的 19 位补零形式
<code>%019ld.wal</code>，创建位置见
[FileBasedWal.cpp:292-309，prepareNewFile](../src/kvstore/wal/FileBasedWal.cpp#L292-L309)。
已有当前文件时，若 <code>current_file_size + record_size &gt; wal_file_size</code>，就在
[FileBasedWal.cpp:469-478](../src/kvstore/wal/FileBasedWal.cpp#L469-L478) 关闭当前文件并轮转。
第一次写入则直接创建文件；单条 record 若自身大于配置值，也可能让新文件超过目标大小，所以
<code>wal_file_size</code> 是滚动阈值，不是任何文件都绝不超过的硬限制。

当 <code>wal_sync=false</code> 时，不是“永不 fsync”：每条 append 不单独 fsync，但关闭/轮转文件时
[FileBasedWal.cpp:263-290，closeCurrFile](../src/kvstore/wal/FileBasedWal.cpp#L263-L290)
仍会 fsync，并把关闭时间写入 mtime。TTL 以滚动文件为单位判断，且至少保留最后两个文件，所以
“wal_ttl=4h”不能解释成“每条日志严格四小时后删除”。

启动时
[FileBasedWal.cpp:85-260，scanAllWalFiles](../src/kvstore/wal/FileBasedWal.cpp#L85-L260)
从文件名和文件内容恢复首尾水位；最后一个文件还会由
[scanLastWal，FileBasedWal.cpp:371-437](../src/kvstore/wal/FileBasedWal.cpp#L371-L437)
顺序检查连续 LogID、前后长度，并截断无效尾部。构造函数在扫描之前已经创建一个新的空
AtomicLogBuffer，扫描不会把全部历史 record 回灌到 Node 链。因此重启会让旧 live Node 随旧进程
消失，而磁盘历史仍可在内存 iterator 未命中时读取。

## 10. AtomicLogBuffer：为什么逻辑 8MiB 会对应更大的 live 内存

### 10.1 Record：逻辑计费遗漏了对象成本

[AtomicLogBuffer.h:40-56，Record](../src/kvstore/wal/AtomicLogBuffer.h#L40-L56) 有三个字段：

~~~cpp
ClusterID clusterId_;  // 8B
TermID termId_;        // 8B
std::string msg_;      // C++ 对象本身也占空间
~~~

但 <code>Record::size()</code> 只返回：

~~~text
sizeof(ClusterID) + sizeof(TermID) + msg.size()
~~~

空日志因此只记 16B。<code>std::string</code> 对象本体没有计入；即使空字符串使用 SSO、没有单独堆
payload，它仍是嵌在 Record 里的 C++ 对象。

### 10.2 Node：一次预留 64 个完整 Record

[AtomicLogBuffer.h:18](../src/kvstore/wal/AtomicLogBuffer.h#L18) 定义
<code>kMaxLength=64</code>；[Node，AtomicLogBuffer.h:61-128](../src/kvstore/wal/AtomicLogBuffer.h#L61-L128)
内联一个 cacheline-aligned 的 <code>array&lt;Record,64&gt;</code>。它不是“来一条日志 new 一个
Record”，而是每当 head 为空或已满时，一次 <code>new Node</code>，预留 64 个 Record 槽。

链表方向是：

~~~text
head_：最新 Node
head_->next_：更旧 Node
tail_：当前最旧、仍有效的 Node
prev_：从旧 Node 指回更新的 Node，供 iterator 正向扫描 LogID
tail_->next_ 之后：已标脏、等待安全删除的旧 Node
~~~

类注释
[AtomicLogBuffer.h:130-136](../src/kvstore/wal/AtomicLogBuffer.h#L130-L136) 明确它按“单 writer、多个
reader”设计。原子字段和 reader 引用计数的目标是避免 iterator 读到悬空 Node。

### 10.3 push 的两个互斥分支

[AtomicLogBuffer.cpp:119-172，push](../src/kvstore/wal/AtomicLogBuffer.cpp#L119-L172) 先计算
<code>recSize</code>，然后只走下列一个分支：

1. **需要新 Node**：head 为空、已满或已标脏时，<code>new Node</code>，写入本条、增加
   <code>size_</code>、发布新 head，然后直接 <code>return</code>。本次不会执行容量检查；
2. **复用当前 head**：head 仍可写时，才检查 <code>size_+recSize&gt;capacity_</code>。即使超限，也只有
   <code>tail != head</code> 时才能标脏旧 tail、扣除其账面大小；最后再增加 <code>size_</code> 并写入
   当前 head。

所以 <code>capacity_=8MiB</code> 是软的逻辑阈值：新 Node 分支会跳过一次检查，只有一个 Node 时也不能
把它标脏。这个控制流直接解释了第 10.6 节为何第 524,290 条才首次标脏。容量比较只看
<code>size_</code>；它不知道 <code>sizeof(Node)</code>，也不知道 jemalloc 为 Node 选择了多大的 size class。

本次 Debug ABI 的实际输出见
[abi-sizes.txt](./wal-lab/evidence/abi-sizes.txt)：

| 对象/分配 | 大小 |
|---|---:|
| <code>sizeof(Record)</code> | 48B |
| <code>sizeof(Node)</code> | 3200B |
| jemalloc usable/Node | 3584B |
| 满 Node 后长期摊销 requested/空日志 | 3200/64 = 50B |
| 满 Node 后长期摊销 usable/空日志 | 3584/64 = 56B |

50B/56B 是 Node 填满后的长期摊销；短窗口中每个 buffer 最多还有一个 partial head。3200B 和
3584B 依赖本机编译器 ABI/allocator，跨构建稳定的源码事实是“空日志逻辑计 16B”和“一 Node 64
条”。按本构建布局，8MiB/16B 对应 8192 个满 Node，即每 Part 约 25MiB requested、28MiB
jemalloc usable；这是达到逻辑容量附近的结构量级，不是现网任意时刻的无条件 RSS。

### 10.4 iterator、引用计数与 GC

Iterator 构造时 <code>addRef()</code>，析构时 <code>releaseRef()</code>，见
[AtomicLogBuffer.h:145-260](../src/kvstore/wal/AtomicLogBuffer.h#L145-L260)。因此运行期淘汰分两步：

1. <code>push()</code> 超过逻辑容量时只把旧 tail 标记为 deleted/dirty；
2. <code>releaseRef()</code> 观察到自己是最后一个旧快照 reader 时，才可能真正 <code>delete</code>
   dirty Node；此后即使有新 reader，它看到的有效区间也不老于函数预先捕获的 tail。

真正的运行期删除位于
[AtomicLogBuffer.cpp:220-268，releaseRef](../src/kvstore/wal/AtomicLogBuffer.cpp#L220-L268)，触发条件是：

~~~text
releaseRef 观察到减一前 refs==1
并且（dirtyNodes > 5 或逻辑 size_ > max_log_buffer_size）
~~~

<code>dirtyNodesLimit_=5</code> 在
[AtomicLogBuffer.h:380-388](../src/kvstore/wal/AtomicLogBuffer.h#L380-L388)，第二个阈值默认 16MiB，
见 [AtomicLogBuffer.cpp:12-14](../src/kvstore/wal/AtomicLogBuffer.cpp#L12-L14)。整体 buffer 析构也会
删除全链，见 [AtomicLogBuffer.cpp:79-96](../src/kvstore/wal/AtomicLogBuffer.cpp#L79-L96)。

本案早期阶段 <code>dirty_nodes=0</code>、普通采样时 <code>refs=0</code>。这说明不是 iterator 长期卡住
已经可回收的 dirty Node；而是按 16B 逻辑计费，Node 还没有进入标脏阶段。

### 10.5 为什么它更像“泄漏”，但严格说不是无引用泄漏

在固定拓扑、不调用 reset/drop/析构、尚未触及逻辑容量的条件下：

~~~text
每 64 次 push
  → new 1 个 Node
  → Node 仍在 head/next 链上
  → 没有满足标脏条件
  → 不会进入运行期 delete
~~~

这些对象都能从 <code>NebulaStore→Part→wal_→logBuffer_→Node</code> 找到，所以它们不是“地址丢失
且永远无法释放”的传统泄漏；它们是产品逻辑长期保留的 live object。对生产监控而言，两者都会表现
为 RSS 按时间上升且短期不下降。

### 10.6 8MiB 的名义保留时间

空日志账面 16B，8MiB 逻辑容量约对应：

~~~text
8 × 1024 × 1024 / 16 = 524,288 条
~~~

第 524,288 次恰好填满逻辑 8MiB；第 524,289 次因“head 已满→新建 Node→提前 return”没有执行容量
检查；第 524,290 次才首次标脏旧 tail。相对天级估算，这两条额外记录可忽略。若使用发布模板 H=30，
忽略执行和调度开销，名义计划延迟均值为 10.2495s，则：

~~~text
524,288 × 10.2495 / 86,400 ≈ 62.2 天
~~~

这 62.2 天只在 H=30、buffer=8MiB、固定拓扑且不 reset/drop/析构的条件下成立；不是所有现网配置
的无条件常数。

## 11. RocksDB 次路径：空 payload 为什么仍写状态机

### 11.1 Part::commitLogs 的控制流

[Part.cpp:215-364，commitLogs](../src/kvstore/Part.cpp#L215-L364) 开始时创建一个 batch，并遍历 WAL
iterator。对每条日志先更新 <code>lastId/lastTerm</code>，然后才检查 payload：

~~~text
payload 非空：解析 PUT/REMOVE/成员变更等操作，写入 batch
payload 为空：跳过业务操作，但 lastId/lastTerm 已经前移
~~~

遍历结束后，只要 <code>lastId&gt;=0</code>，
[Part.cpp:349-358](../src/kvstore/Part.cpp#L349-L358) 仍调用 <code>putCommitMsg()</code>，最后提交整个
batch。

[Part.cpp:403-410，putCommitMsg](../src/kvstore/Part.cpp#L403-L410) 把 committed LogID 和 term 编成
16B value，并使用 <code>NebulaKeyUtils::systemCommitKey(partId_)</code> 作为 key；key 编码见
[NebulaKeyUtils.cpp:101-108](../src/common/utils/NebulaKeyUtils.cpp#L101-L108)。这里的 key 只编码
<code>partId</code>，不编码 <code>spaceId</code>：同一 Space/RocksDB 内的不同 Part 有不同 key；
不同 Space 中相同 partId 的 key 字节可以相同，但因位于不同 RocksDB 实例而不会冲突。

### 11.2 从 Nebula batch 到 RocksDB DB::Write

具体桥接链是：

~~~text
Part::putCommitMsg
  → WriteBatch::put（抽象接口）
  → RocksWriteBatch::put
  → rocksdb::WriteBatch::Put
  → RocksEngine::commitBatchWrite
  → rocksdb::DB::Write
~~~

[RocksWriteBatch，RocksEngine.h:181-217](../src/kvstore/RocksEngine.h#L181-L217) 完成抽象 batch 到
RocksDB batch 的转换；[RocksEngine.cpp:120-140](../src/kvstore/RocksEngine.cpp#L120-L140) 设置
<code>disableWAL/sync/no_slowdown</code> 后调用 <code>db_->Write()</code>。

准确语义是“每个 commit iterator/batch 最多写一次 commit key”，不是源码保证每条 WAL 都单独
<code>DB::Write</code>。稳定空闲链通常一批只有一条 no-op，所以实验中长期接近 1:1；业务并发、批量
合并或 Follower 滞后时不能无条件套用。

### 11.3 为什么同一个 user key 仍会增加 active memtable entries

本次 [build/CMakeCache.txt:462-465](../build/CMakeCache.txt#L462-L465) 只能证明 include/lib 指向
<code>/opt/vesoft/third-party/3.3</code>；该前缀实际 installed header
<code>/opt/vesoft/third-party/3.3/include/rocksdb/version.h:14-16</code> 才给出版本 7.5.3。这里用
[版本匹配的 upstream 7.5.3 参考源码](../../rocksdb-7.5.3/include/rocksdb/version.h#L14-L16) 解释内部
路径，但没有证明已链接静态库与 sibling 源码逐文件 bit-identical，最终以运行 property 为准。

实验配置没有覆盖 <code>inplace_update_support</code>，因此使用默认 false，定义见
[advanced_options.h:328-340](../../rocksdb-7.5.3/include/rocksdb/advanced_options.h#L328-L340)。在
这条路径上：

- [write_batch.cc:1950-1989](../../rocksdb-7.5.3/db/write_batch.cc#L1950-L1989) 把 Put 交给
  <code>MemTable::Add(sequence,...)</code>；
- [memtable.cc:535-637](../../rocksdb-7.5.3/db/memtable.cc#L535-L637) 把 sequence 编入 internal key、
  分配/插入新的 entry，并把 entry 计数递增；
- [write_batch.cc:2075-2081](../../rocksdb-7.5.3/db/write_batch.cc#L2075-L2081) 在成功后推进
  sequence。

所以 user key 相同并不表示 active memtable 中永远只有一个版本；新的 sequence 形成新的 internal
entry，直到 memtable 生命周期推进到 flush 等后续阶段。

### 11.4 一个 Space 有多少 RocksDB 写缓冲

[NebulaStore.cpp:354-370](../src/kvstore/NebulaStore.cpp#L354-L370) 和
[395-421](../src/kvstore/NebulaStore.cpp#L395-L421) 表明，对本机至少持有一个 Part、因而已物化的
Space，粒度是 <code>space × data_path</code> 一个 RocksEngine，不是每 Part 一个 RocksDB。一个
Space 内的多个 Part 共享这个 engine，但它们写不同的 <code>systemCommitKey(partId)</code>。

发布模板的 RocksDB 选项位于
[nebula-storaged.conf.default:98-104](../conf/nebula-storaged.conf.default#L98-L104)：

~~~text
write_buffer_size=64MiB
max_write_buffer_number=4
~~~

RocksDB 对应语义分别见
[options.h:172-188](../../rocksdb-7.5.3/include/rocksdb/options.h#L172-L188) 和
[advanced_options.h:249-261](../../rocksdb-7.5.3/include/rocksdb/advanced_options.h#L249-L261)。64MiB
是一个 active memtable/write buffer 的目标大小，不是每 Space 的严格总内存上限；最多可同时保留
多个 write buffer，实际还受 flush、arena 和其他结构影响。

本次采样读取的 property 在
[db.h:920-934](../../rocksdb-7.5.3/include/rocksdb/db.h#L920-L934) 定义为 active memtable entry 总数和
active memtable 近似字节数。当前短跑直接证明的是两个采样点间这些 property 聚合值增长；没有覆盖
完整 flush 周期，不能据此断言 flush 后 RSS 必然怎样变化。

### 11.5 运行时 property 是怎样从 RocksDB 读出来的

这组数据不是根据 RSS 猜出来的，而是沿着一条只读查询链获取：

~~~text
sample-rocksdb-memtables.sh
  → HTTP /rocksdb_property?space=...&property=...
  → StorageHttpPropertyHandler::onRequest
  → NebulaStore::getProperty(spaceId, property)
  → 该 Space 的每个 RocksEngine::getProperty
  → rocksdb::DB::GetProperty
~~~

代码位置：

- HTTP 参数解析和调用 KVStore：
  [StorageHttpPropertyHandler.cpp:23-70](../src/storage/http/StorageHttpPropertyHandler.cpp#L23-L70)；
- 遍历该 Space 的全部 data-path engines：
  [NebulaStore.cpp:1411-1429，getProperty](../src/kvstore/NebulaStore.cpp#L1411-L1429)；
- 进入 RocksDB：
  [RocksEngine.cpp:517-525，getProperty](../src/kvstore/RocksEngine.cpp#L517-L525)；
- 实验逐台、逐 Space 读取 active bytes/entries：
  [sample-rocksdb-memtables.sh:15-30](./wal-lab/sample-rocksdb-memtables.sh#L15-L30)。

本实验每 Space 只有一条 data path，所以 HTTP JSON 只有 <code>Engine 0</code>。若生产有多条
data path，handler 会返回同一 Space 的多个 Engine，采样与外推都必须逐一聚合，不能照搬单路径对象数。

## 12. 为什么 WAL TTL 不会清掉 Atomic Node

### 12.1 定时清理调用链

<code>clean_wal_interval_secs</code> 默认 600s，定义和初次调度位于
[NebulaStore.cpp:24](../src/kvstore/NebulaStore.cpp#L24) 与
[NebulaStore.cpp:72](../src/kvstore/NebulaStore.cpp#L72)。周期函数
[NebulaStore.cpp:1293-1321，cleanWAL](../src/kvstore/NebulaStore.cpp#L1293-L1321) 遍历 Part，并调用
<code>part->cleanWal()</code>。

[RaftPart.cpp:492-495，cleanWal](../src/kvstore/raftex/RaftPart.cpp#L492-L495) 把当前
<code>committedLogId_</code> 传给 <code>FileBasedWal::cleanWAL()</code>。

### 12.2 cleanWAL 只操作文件集合

[FileBasedWal.cpp:640-706](../src/kvstore/wal/FileBasedWal.cpp#L640-L706) 的两个 overload 只：

- 遍历 <code>walFiles_</code>；
- 按 TTL/LogID 判断；
- <code>unlink</code> 旧文件；
- 从 <code>walFiles_</code> map 删除元数据；
- 至少保留最后两个文件。

这段代码完全不访问 <code>logBuffer_</code>。无参 overload 按旧滚动文件的 mtime/TTL 判断；普通 Part
调用的带 LogID overload 还要求文件末端低于目标 committed LogID。两者都至少保留最后两个文件。
即使某个符合相应条件的磁盘 WAL 文件被删除，也不意味着 Atomic Node 同步回收。

真正会显式重置、使现有 Node 链逻辑失效的 FileBasedWal 路径是：

- rollback：[FileBasedWal.cpp:569-620](../src/kvstore/wal/FileBasedWal.cpp#L569-L620)；
- reset：[FileBasedWal.cpp:622-638](../src/kvstore/wal/FileBasedWal.cpp#L622-L638)。

其中会调用 <code>logBuffer_->reset()</code>；
[AtomicLogBuffer.cpp:174-189](../src/kvstore/wal/AtomicLogBuffer.cpp#L174-L189) 只是先标记整链并清逻辑水位，
它本身不 delete。运行期删除还要等后续 <code>releaseRef()</code> 观察到旧 reader 条件并满足
<code>dirtyNodes&gt;5 || size_&gt;max_log_buffer_size</code>。仅仅出现更多 dirty Node 并不会主动执行
delete；达到阈值后仍须有后续 Iterator 析构进入 <code>releaseRef()</code>，且它观察到减一前
<code>refs==1</code>，才会删除 dirty 链。否则可能一直保留到满足这些条件或整体 buffer 析构。

<code>NebulaStore::cleanWAL()</code> 在 <code>rocksdb_disable_wal=true</code> 时另会 flush RocksEngine
（[NebulaStore.cpp:1298-1303](../src/kvstore/NebulaStore.cpp#L1298-L1303)）；这与
<code>FileBasedWal::cleanWAL</code> 的 Atomic buffer 仍是不同对象和不同动作。

## 13. 把源码换算成 3+3+3 实验中的对象数和斜率

### 13.1 先算对象数

实验有 50 个空 Space，每个 20 logical parts，RF=3，三台 storaged：

~~~text
logical Raft groups = 50 × 20 = 1000
cluster replica-parts = 1000 × 3 = 3000
local replica-parts/storaged = 3000 / 3 = 1000
~~~

因为 RF 等于 storaged 数，每台都有每个 logical part 的一份副本。因此每台有：

- 1000 个本地 <code>Part/RaftPart</code>；
- 1000 个 <code>FileBasedWal</code>；
- 1000 个 <code>AtomicLogBuffer</code>；
- 实验只有一个 data_path，所以有 50 个数据 Space RocksEngine；此外还有 1 个不参与本次 Part 周期
  commit-key 链的 admin-task RocksEngine。

完整配置见 [tasks/wal-lab/conf](./wal-lab/conf)，创建 Space 的 DDL 见
[create-spaces-01-10.ngql](./wal-lab/create-spaces-01-10.ngql) 和
[create-spaces-11-50.ngql](./wal-lab/create-spaces-11-50.ngql)。

### 13.2 为什么三台斜率接近而 Leader 数不同

[show-hosts-50-spaces.out](./wal-lab/evidence/show-hosts-50-spaces.out) 中三台 Leader 数是
325/145/530；但每台都有 1000 份本地 replica-part。每个 logical Leader 生成一条日志后，三个副本
都 push 一次，所以决定单台 Atomic 斜率的是本地副本数，而不是本机 Leader 数。

这也是一个很强的反事实检查：如果只有 Leader 写 buffer，第三台应比第二台快很多；实测并没有。

### 13.3 H=1 加速实验如何命中源码指纹

实验将 <code>raft_heartbeat_interval_secs</code> 设为 1，其他目标持久化参数保持发布语义；端口、路径、
Meta heartbeat、线程池等为同机九实例做了显式调整，完整值以配置文件为准。

同机 loopback 九实例能够验证对象拓扑、调用链、提交次数和随 Space/Part/副本数变化的比例关系；它
不能等同验证跨主机网络延迟、丢包、拥塞、重试及相应的在途 RPC 内存，也不能直接外推生产环境的
绝对 RSS。Debug 构建、观测计数和本机 allocator 同样会影响绝对值。

H=1 的名义计划延迟均值为：

~~~text
floor(1000/3)ms + mean(0..499)ms = 333 + 249.5 = 582.5ms
1000 local parts / 0.5825s = 1716.738 pushes/s/storaged
~~~

原始数据
[wal-runtime-samples.csv](./wal-lab/evidence/wal-runtime-samples.csv) 的 1000 副本、184 秒窗口为：

| storaged | push 增量 | empty 增量 | 逻辑 B/push | WAL B/push | Node 增量 | RSS 增量 |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 315,991 | 315,991 | 16.000 | 约 32 | 4,989 | 32.09MiB |
| 2 | 316,084 | 316,084 | 16.000 | 约 32 | 4,989 | 33.31MiB |
| 3 | 316,074 | 316,074 | 16.000 | 约 32 | 4,989 | 32.58MiB |

这组数字分别对应：

- 所有 payload 为空：<code>empty_pushes == pushes</code>；
- 逻辑 16B：<code>Record::size()</code>；
- 磁盘约 32B：FileBasedWal 编码；
- 每 64 条一个 Node：315,xxx/64 与 Node 增量吻合；
- 三台近似相同：Follower WAL 路径确实发生。

CSV 使用整数秒时间戳且三台顺序采样，严谨结论是与名义公式在约 1% 内吻合，不是万分级精度。
复算脚本见
[analyze-runtime-samples.py:1-83](./wal-lab/analyze-runtime-samples.py#L1-L83)。

### 13.4 发布默认下的条件估算

先估算尚未触及 buffer 容量时的早期日斜率。假设 H=30、每台 1000 replica-parts，且 Node ABI 与
本实验相同；jemalloc usable 一项还要求 allocator size class 相同：

~~~text
每 part 每天名义空日志 = 86,400 / 10.2495 = 8,429.679
每 storaged pushes/day = 8,429,679
长期摊销 Node requested/day = pushes / 64 × 3200 = 401.958MiB
长期摊销 jemalloc usable/day = pushes / 64 × 3584 = 450.193MiB
~~~

<code>wal_buffer_size=8MiB</code> 不决定触顶前的每日 Node 斜率，只决定这段线性增长约持续多久以及
何时进入标脏/GC；按第 10.6 节条件，名义约 62.2 天。data_path 数也不决定 Atomic 斜率，它影响的是
本机已物化 <code>space × data_path</code> 的 RocksEngine/memtable 数。

用户没有提供现网实际 H、wal buffer、data paths、编译器 ABI 和 allocator，所以日斜率、持续时间和
RocksDB 次路径规模都只能分别按真实参数重算，不能把上述数字当成已经测得的生产常数。可稳定外推的
是调用链、每 64 条一个 Node，以及触顶前斜率与本地 replica-part 数近似成正比。

## 14. 为什么其他常见嫌疑不是本次主因

以下结论限定在“拓扑固定、RPC 健康、空载”的实验条件；故障、拥塞或动态 rebalance 可有额外在途
内存。

| 嫌疑 | 相关源码 | 为什么不符合本次主斜率 |
|---|---|---|
| RocksDB block cache | [RocksEngineConfig.cpp:307-313](../src/kvstore/RocksEngineConfig.cpp#L307-L313) | 函数内 static、进程内共享且有配置上限；可随读取形成有界过渡增长，但不能解释 Atomic 每 64 个空日志出现一个 Node 的直接指纹 |
| jemalloc 不归还 | [AtomicLogBuffer.cpp:119-172](../src/kvstore/wal/AtomicLogBuffer.cpp#L119-L172)、[220-268](../src/kvstore/wal/AtomicLogBuffer.cpp#L220-L268) | Node 仍是 live、dirty=0，尚未执行 delete；allocator 是 3200→3584 的放大器，不是上游生成器 |
| 文件 page cache | [FileBasedWal.cpp:457-500](../src/kvstore/wal/FileBasedWal.cpp#L457-L500) | 确有 write 文件页，但实测匿名/Private_Dirty 与 Node 同增；page cache 解释不了 Atomic 计数不变量 |
| iterator 卡 GC | [AtomicLogBuffer.h:149-155](../src/kvstore/wal/AtomicLogBuffer.h#L149-L155)、[RaftPart.cpp:1068-1078](../src/kvstore/raftex/RaftPart.cpp#L1068-L1078) | refs 在采样时回 0，且还没有 dirty Node；不是 reader 阻止已标脏节点删除 |
| timer 泄漏 | [RaftPart.cpp:1428-1434](../src/kvstore/raftex/RaftPart.cpp#L1428-L1434)、[GenericWorker.h:209-235](../src/common/thread/GenericWorker.h#L209-L235)、[GenericWorker.cpp:124-143](../src/common/thread/GenericWorker.cpp#L124-L143) | <code>addDelayTask</code> 注册 interval=0 的 one-shot；每轮只安排一个后继，执行后 purge，数量随 Part 而非运行时长 |
| Raft logs 队列 | [RaftPart.cpp:811-869](../src/kvstore/raftex/RaftPart.cpp#L811-L869)、[1103-1127](../src/kvstore/raftex/RaftPart.cpp#L1103-L1127)、[2154-2172](../src/kvstore/raftex/RaftPart.cpp#L2154-L2172) | 有 batch 上限，成功时转移/清空，异常时复位；健康空载下是有界瞬态 |
| Append RPC pending | [Host.cpp:22](../src/kvstore/raftex/Host.cpp#L22)、[100-284](../src/kvstore/raftex/Host.cpp#L100-L284)、[498-529](../src/kvstore/raftex/Host.cpp#L498-L529) | Append 有显式 outstanding 上限，成功、异常或 drain 路径会终结 |
| Heartbeat RPC pending | [Host.cpp:408-489](../src/kvstore/raftex/Host.cpp#L408-L489) | 没有套用 Append 的同一队列上限；但 RPC 健康且 timeout 生效时，其在途量取决于频率×延迟，不按墙钟时间累积 |
| Thrift client cache | [ThriftClientManager.h:34-40](../src/common/thrift/ThriftClientManager.h#L34-L40)、[ThriftClientManager-inl.h:30-96](../src/common/thrift/ThriftClientManager-inl.h#L30-L96) | 每调用线程一张 map，map 内按远端 host×EventBase 复用；固定线程池和拓扑下有界 |
| topology maps | [RaftexService.h:139-140](../src/kvstore/raftex/RaftexService.h#L139-L140)、[NebulaStore.h:868-884](../src/kvstore/NebulaStore.h#L868-L884)、[RaftPart.h:795-800](../src/kvstore/raftex/RaftPart.h#L795-L800) | 随 Space/Part/peer 拓扑固定，不按时间每轮新建永久条目 |
| <code>walFiles_</code> map | [FileBasedWal.h:270-282](../src/kvstore/wal/FileBasedWal.h#L270-L282)、[FileBasedWal.cpp:640-706](../src/kvstore/wal/FileBasedWal.cpp#L640-L706) | 按文件而非按日志保存元数据，且 TTL 清理；无法产生“每 64 条精确一个 Node”的指纹 |

## 15. 观测补丁：每个指标究竟证明什么

为避免只看 RSS 猜测，本次工作区给 Atomic buffer 添加了只读聚合指标。它们没有参与 Raft、容量或
GC 决策。

### 15.1 代码位置

- 指标结构：
  [AtomicLogBuffer.h:20-35](../src/kvstore/wal/AtomicLogBuffer.h#L20-L35)；
- live buffer registry、构造/析构登记与聚合：
  [AtomicLogBuffer.cpp:19-117](../src/kvstore/wal/AtomicLogBuffer.cpp#L19-L117)；
- push/empty/Node 计数：
  [AtomicLogBuffer.cpp:119-128](../src/kvstore/wal/AtomicLogBuffer.cpp#L119-L128)；
- GC 删除时 Node 计数回减：
  [AtomicLogBuffer.cpp:249-260](../src/kvstore/wal/AtomicLogBuffer.cpp#L249-L260)；
- HTTP 输出：
  [StorageHttpStatsHandler.cpp:50-68](../src/storage/http/StorageHttpStatsHandler.cpp#L50-L68)；
- <code>/rocksdb_stats</code> 路由：
  [StorageServer.cpp:125-149](../src/storage/StorageServer.cpp#L125-L149)；
- 采样脚本：
  [sample-metrics.sh:1-55](./wal-lab/sample-metrics.sh#L1-L55)。

观测字段在
[AtomicLogBuffer.h:390-394](../src/kvstore/wal/AtomicLogBuffer.h#L390-L394)，没有放入 Node，因此没有
改变 <code>sizeof(Node)</code>。补丁不是零开销：每个 buffer 多 3 个 atomic 和一个 registry 条目；
每次 push 多 1～2 次 atomic RMW（新建 Node 时再增加计数）；HTTP 聚合会持 registry mutex 遍历所有
live buffer。固定拓扑下新增对象内存主要是固定项，且它不改变 Node 布局、容量或 GC 决策，但解释
绝对性能/RSS 时仍要承认这些 instrumentation 开销。

### 15.2 指标语义

| 指标 | 源码含义 | 可验证什么 |
|---|---|---|
| <code>instances</code> | 当前 live Atomic buffer 数 | 是否约等于本地 replica-part 数 |
| <code>capacity_bytes</code> | 所有 buffer 的逻辑容量和 | 配置规模，不代表已用物理内存 |
| <code>accounted_bytes</code> | 所有 <code>size_</code> 之和 | 空日志是否按 16B/push 计费 |
| <code>nodes</code> | 当前 live Node 数 | 是否约每 64 pushes 增 1 |
| <code>node_size_bytes</code> | 本构建 <code>sizeof(Node)</code> | ABI requested 大小 |
| <code>node_bytes</code> | <code>nodes×sizeof(Node)</code> | Node requested live bytes |
| <code>pushes</code> | 当前 live buffers 的累计 push 和 | WAL 进入 Atomic 的数量 |
| <code>empty_pushes</code> | payload 为空的 push 和 | 是否为空记录；单独不能证明来源一定是 heartbeat |
| <code>dirty_nodes</code> | 已标脏、等待 GC 的 Node | 当前是否已进入容量淘汰阶段 |
| <code>refs</code> | 活跃 iterator 引用和 | 是否存在 reader 阻塞 GC 的迹象 |
| <code>max_nodes_per_buffer</code> | 当前 live buffers 中当前 Node 数的最大值 | 当前填充是否严重偏斜；不是历史 high-water |

<code>node_bytes</code> 已经包含 Node 内联的 Record 对象，不能再与 <code>accounted_bytes</code> 相加
当成“总内存”；后者由 <code>Record::size()</code> 汇总，包含 <code>msg_.size()</code>，但不等于 string
对象/容量的真实堆成本。<code>node_bytes</code> 不包含非 SSO string 的独立堆缓冲，两个指标也都不反映
jemalloc 元数据和 size-class 余量、RocksDB 或 page cache。本案 payload 为空，通常走 SSO，不产生
独立字符串堆分配。

<code>pushes/empty_pushes</code> 是“当前仍存活 buffer 的累计和”；只有 buffer 析构并移出 registry
（例如 Part drop 后对象真正销毁、随后另建新对象）时，进程聚合值才可能下降。
<code>FileBasedWal::reset()</code> 还会关闭/删除磁盘 WAL 并重置文件水位；但它对
<code>AtomicLogBuffer</code> 的 <code>reset()</code> 只标脏 Node、清逻辑水位，当前补丁不会清这两个
counter。它们因此也不是无条件的进程生命周期 counter。采样使用 relaxed atomic，不是事务一致
快照，瞬时字段可有极小错位；秒/分钟级斜率不受实质影响。

### 15.3 一条可独立复核的不变量

在当前 live buffers 自构造以来没有 reset/rollback、没有 marked-deleted 旧链、没有发生 Node GC，且
每个 buffer 最多一个 partial head 时：

~~~text
ceil(pushes / 64) <= nodes <= floor(pushes / 64) + instances
~~~

三台首末样本都满足这个区间。这比“RSS 似乎一直涨”更有辨识力，因为它同时命中
<code>kMaxLength=64</code>、<code>new Node</code> 分支和本地 buffer 实例数。

## 16. 推荐的源码阅读顺序

不要从 2000 多行的 <code>RaftPart.cpp</code> 第一行硬读。按下面顺序，每一步只回答一个问题。

### 第零遍：先定位进程入口和拓扑来源

1. [StorageDaemon.cpp:115-185](../src/daemons/StorageDaemon.cpp#L115-L185)：storaged 从哪里启动？
2. [StorageServer.cpp:95-114](../src/storage/StorageServer.cpp#L95-L114)：NebulaStore 从哪里创建？
3. [CreateSpaceProcessor.cpp:236-266](../src/meta/processors/parts/CreateSpaceProcessor.cpp#L236-L266)：
   logical part 的 RF 份副本由谁选择？
4. 手算 <code>space × partition_num × RF / storaged</code>。

读完应能回答：为什么“50 Space、20 分片、RF3、三台 storage”会在每台形成 1000 个本地 Part？

### 第一遍：只建立对象和粒度

1. [NebulaStore.h:31-39](../src/kvstore/NebulaStore.h#L31-L39)：一个 Space 持有哪些对象？
2. [NebulaStore.cpp:395-421](../src/kvstore/NebulaStore.cpp#L395-L421)：engine 的粒度是什么？
3. [NebulaStore.cpp:437-515](../src/kvstore/NebulaStore.cpp#L437-L515)：一个本地 Part 怎样创建？
4. [RaftPart.cpp:330-372](../src/kvstore/raftex/RaftPart.cpp#L330-L372)：WAL 在哪里创建？
5. [FileBasedWal.cpp:40-60](../src/kvstore/wal/FileBasedWal.cpp#L40-L60)：buffer 在哪里创建？

读完应能回答：RF3 的一条 logical part 为什么会有三份 Atomic buffer？

### 第二遍：只跟一条空日志

1. [RaftPart.cpp:1401-1434](../src/kvstore/raftex/RaftPart.cpp#L1401-L1434)；
2. [RaftPart.cpp:2041-2049](../src/kvstore/raftex/RaftPart.cpp#L2041-L2049)；
3. [RaftPart.cpp:786-915](../src/kvstore/raftex/RaftPart.cpp#L786-L915)；
4. [Host.cpp:287-345](../src/kvstore/raftex/Host.cpp#L287-L345)；
5. [RaftPart.cpp:1757-1804](../src/kvstore/raftex/RaftPart.cpp#L1757-L1804)；
6. [RaftPart.cpp:1002-1127](../src/kvstore/raftex/RaftPart.cpp#L1002-L1127)。

读完应能回答：Leader 第 N 条 no-op 何时写本机、何时写 Follower、两边何时 commit？

### 第三遍：只研究内存

1. [FileBasedWal.cpp:442-500](../src/kvstore/wal/FileBasedWal.cpp#L442-L500)；
2. [AtomicLogBuffer.h:40-128](../src/kvstore/wal/AtomicLogBuffer.h#L40-L128)；
3. [AtomicLogBuffer.cpp:119-172](../src/kvstore/wal/AtomicLogBuffer.cpp#L119-L172)；
4. [AtomicLogBuffer.h:145-260](../src/kvstore/wal/AtomicLogBuffer.h#L145-L260)；
5. [AtomicLogBuffer.cpp:220-268](../src/kvstore/wal/AtomicLogBuffer.cpp#L220-L268)。

读完应能自己算出 16B、64 条、3200B/3584B 的区别。

再用单元测试反向确认结构：基本容量行为见
[AtomicLogBufferTest.cpp:29-54](../src/kvstore/wal/test/AtomicLogBufferTest.cpp#L29-L54)，单写多读见
[56-110](../src/kvstore/wal/test/AtomicLogBufferTest.cpp#L56-L110)，reset 后再 push 见
[112-147](../src/kvstore/wal/test/AtomicLogBufferTest.cpp#L112-L147)。

### 第四遍：再看状态机和 RocksDB

1. [Part.cpp:215-364](../src/kvstore/Part.cpp#L215-L364)；
2. [Part.cpp:403-410](../src/kvstore/Part.cpp#L403-L410)；
3. [RocksEngine.h:181-217](../src/kvstore/RocksEngine.h#L181-L217)；
4. [RocksEngine.cpp:120-140](../src/kvstore/RocksEngine.cpp#L120-L140)；
5. [NebulaStore.cpp:354-421](../src/kvstore/NebulaStore.cpp#L354-L421)。

读完应能回答：空 payload 为什么没有业务 KV，却仍有 commit-key Write？

FileBasedWal 的 TTL 测试位于
[FileBasedWalTest.cpp:328-401](../src/kvstore/wal/test/FileBasedWalTest.cpp#L328-L401)，可用来验证
“磁盘文件清理”和“Atomic buffer 容量/GC”是两套机制。

### 第五遍：用运行证据反查源码

依次打开：

1. [ABI 大小](./wal-lab/evidence/abi-sizes.txt)；
2. [WAL/Node/RSS 样本](./wal-lab/evidence/wal-runtime-samples.csv)；
3. [RocksDB memtable 样本](./wal-lab/evidence/rocksdb-memtable-samples.csv)；
4. [复算脚本](./wal-lab/analyze-runtime-samples.py)。

每看到一个数，回到源码找它的结构来源：16→<code>Record::size</code>，32→磁盘编码，64→Node
槽数，3200→ABI，约 1717/s→timer 计划延迟与 1000 local parts。

## 17. 建议做的纸面练习

这些练习不修改产品逻辑，只用于检验是否真正理解。

1. **对象数练习**：10 spaces、100 parts、RF3、5 storaged，均匀分布时每台约有多少
   replica-parts 和 Atomic buffers？
2. **频率练习**：H=15 时忽略执行开销，名义计划延迟均值是多少？本地 600 parts 每秒约多少 push？
3. **内存练习**：一个初始为空且尚未 GC 的 buffer 连续新增 64,000 个空 push；若本构建
   <code>sizeof(Node)=3200</code>，会创建多少 Node、对应多少 requested bytes？
4. **时序练习**：画出第 N/N+1 轮，标出 Leader/Follower 的 WAL 尾部与 committed 水位。
5. **证伪练习**：如果实测 <code>empty_pushes</code> 增长但 <code>nodes</code> 长期完全不满足每 64 条
   一级台阶，应优先检查哪些前提（GC、Part 重建、指标采样、ABI/代码版本）？

参考答案：

1. 集群副本 10×100×3=3000，均匀时约 600/台；
2. <code>floor(15000/3)ms+249.5ms=5.2495s</code>，约 600/5.2495=114.30 push/s；
3. 正好 1000 个满 Node，即 3,200,000B requested；多 buffer 或非空起点还需分别考虑 partial head；
4. 第 N 轮 Follower 写 N 但通常只 commit N-1，Leader quorum 后 commit N；第 N+1 轮 Follower
   才从请求携带水位 commit N；
5. 先检查是否已越过容量并发生 GC、是否 drop/rebalance/reset、聚合 buffer 是否变化、观测补丁是否
   对应当前二进制，再检查源码版本和 Node 布局。

## 18. 最终把整条链压缩成八句话

1. 一个 logical part 是一个 Raft group；RF3 会在集群内产生三份本地 replica <code>Part</code>。
2. 每份 Part 都有自己的 <code>FileBasedWal</code> 和 <code>AtomicLogBuffer</code>。
3. 稳定 Leader 的 <code>statusPolling()</code> 周期调用 <code>sendHeartbeat()</code>。
4. 该函数不仅发送真正 heartbeat RPC，还在每次复制空闲时追加空 <code>NORMAL</code> 日志。
5. <code>commitInThisTerm_</code> 已经记录“本 term 是否提交过”，却没有限制这条 no-op。
6. 空日志走普通 AppendLog；在本次三副本均健康的实验中，Leader 和两个 Follower 都写磁盘 WAL 并
   push Atomic buffer。
7. 空 Record 只按 16B 计容量，但每 64 条分配一个包含 64 个完整 Record 的 Node，形成主要 live
   内存增长；WAL TTL 只删文件，不清这条 Node 链。
8. 状态机虽跳过空业务 payload，仍更新每 Part 的 RocksDB commit key，形成可观测的次级
   active-memtable 增长。

如果只能记住一条调用链，请记住：

~~~text
RaftPart::statusPolling
  → RaftPart::sendHeartbeat
  → RaftPart::appendLogAsync(NORMAL, "")
  → FileBasedWal::appendLogInternal
  → AtomicLogBuffer::push                 [主内存路径]
  → Host::appendLogs / Follower WAL push  [三副本放大]
  → Part::commitLogs
  → systemCommitKey / RocksDB::Write      [次内存路径]
~~~

本文只解释根因与相关内核机制；结论证据边界、外部社区交叉验证和完整排除过程分别见
[根因总结](./task-wal-root-cause-summary.md) 与
[完整根因分析](./task-wal-root-cause-analysis.md)。
