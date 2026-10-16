# Nebula Graph 3.6 空闲分片内存增长：源码与组件实验

本文对应 `task/task-wal.md`，记录代码分析、定量模型和已经完成的独立组件实验。**仅解释和复现问题，不包含业务修复或生产调参方案。** 本文的实测值来自下文明确列出的组件程序；它们不等同于完整 Nebula 服务的运行数据。

分析基线为 `de9b3ed800a6627d9845e9289b6bbc5b6faf460a`，环境为本机 x86_64、GCC 11.4、GLIBC 2.35，第三方库使用工程现有依赖。证据集中在 [task/wal-investigation](../../task/wal-investigation)。详细逐函数解读见 [CODE-WALKTHROUGH-WAL.md](../../task/wal-investigation/CODE-WALKTHROUGH-WAL.md)。

## 1. 问题是什么，分析需要回答什么

问题描述为：storage 长期运行时内存持续上涨；空间没有数据或很少使用时仍上涨；空间或分片越多越明显；例如 3 个 storage、50 个空间、每空间 20 分片、3 副本时，每天增加几百 MB，且长期不下降。

这里必须区分四个事实：业务没有写入、Raft 没有新增日志、对象没有释放、RSS 没有下降。代码和实验表明，这四件事并不等价。空闲 leader 仍会周期性生成空日志；空日志仍会形成 WAL 缓存记录，并更新 RocksDB 的分片提交位置；缓存对象的释放与分配器将内存交还操作系统，又是不同阶段。

已经确认的机制如下：

1. 空闲心跳持续生成有序号的空 WAL 日志，leader 与 followers 都保留各自的内存副本。
2. 每个本地分片副本拥有独立 `AtomicLogBuffer`。默认 `wal_buffer_size=8MiB` 约束的是自定义逻辑计账，空消息每条只记 16B。
3. 本机构建中，每 64 条记录的 `Node` 对象为 3200B；实际 jemalloc 分配档位为 3584B，满节点平均 56B/空记录。默认 8MiB 逻辑容量因此对应约 28MiB 活跃节点分配/分片，而不是 8MiB。
4. 在达到容量之前，没有基于时间或业务空闲的内存缓存驱逐。配置模板的心跳参数下，填满可以经历约两个月，足以表现为长期连续增长。
5. 正常 reader 生命周期下，该缓存机制最终会进入平台；不能将所有长期上涨直接称为“无限泄漏”。此外，GC 依赖最后一个 iterator 释放，确有可构造的脏节点延迟回收条件，但尚不能把该额外条件假定为生产事实。
6. 空日志提交还会覆盖写 RocksDB 的提交位置。独立实验已验证，逻辑键数不变时 memtable 仍可持续增加；因此全部 RSS 增量不能只归给 WAL 缓存。

## 2. 从空闲心跳到内存持有的完整链路

### 2.1 心跳参数不是实际追加间隔

[RaftPart.cpp:1401](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/raftex/RaftPart.cpp#L1401) 的 `statusPolling()` 使用以下延迟重新调度自身：

```text
delay_ms = floor(raft_heartbeat_interval_secs * 1000 / 3) + random_integer[0, 499]
```

正常 leader 的 [needToSendHeartbeat():1138](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/raftex/RaftPart.cpp#L1138) 只检查 `RUNNING && LEADER`，并不再次等待一个完整的 `raft_heartbeat_interval_secs`。

需要区分两套值：

| 来源 | heartbeat 参数 | 正常轮询基础范围 | 基础均值 |
|---|---:|---:|---:|
| [RaftPart.cpp:30](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/raftex/RaftPart.cpp#L30) 源码默认值 | 5 秒 | 1666–2165ms | 1.9155 秒 |
| [nebula-storaged.conf.default:46](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/conf/nebula-storaged.conf.default#L46) | 30 秒 | 10000–10499ms | 10.2495 秒 |
| [nebula-storaged.conf.production:46](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/conf/nebula-storaged.conf.production#L46) | 30 秒 | 10000–10499ms | 10.2495 秒 |

表中不包含执行耗时、排队、选举或复制阻塞。部署采用模板时，应按实际生效的 30 秒参数分析，不能直接套源码默认 5 秒，更不能把参数 30 秒当作每 30 秒才产生一条空日志。

### 2.2 空字符串仍然是有序号的 WAL 记录

[sendHeartbeat():2041](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/raftex/RaftPart.cpp#L2041) 包含两条并行作用路径：

- 如果当前没有复制任务，即 `!replicatingLogs_`，异步执行 `appendLogAsync(clusterId_, NORMAL, "")`。
- 发送独立的 Heartbeat RPC，用于 leader 合法性、任期和租约等处理。

Heartbeat RPC 的接收函数 [processHeartbeatRequest():1912](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/raftex/RaftPart.cpp#L1912) 明确不追加日志。增长来自同一次 `sendHeartbeat()` 发起的 **空 AppendLog 路径**，不是把所有 Heartbeat RPC 都直接当成 WAL。

函数注释提到新任期需要提交一条空日志，但实际 `if` 只判断 `replicatingLogs_`，没有判断 `commitInThisTerm_`。因此稳定任期中的空闲 leader 也会反复追加。

之后经过 [appendLogAsync():786](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/raftex/RaftPart.cpp#L786)、[appendLogsInternal():874](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/raftex/RaftPart.cpp#L874)，leader 首先 `wal_->appendLogs()`，再向 followers 复制。Followers 在 [RaftPart.cpp:1757](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/raftex/RaftPart.cpp#L1757) 同样调用本地 `wal_->appendLogs()`。

### 2.3 文件字节、逻辑计账、对象字节是三个量

[FileBasedWal::appendLogInternal():442](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/wal/FileBasedWal.cpp#L442) 先写文件，再无条件执行 `logBuffer_->push(id, term, cluster, msg)`。空消息没有特殊跳过逻辑。

| 层次 | 空消息的大小 | 含义 |
|---|---:|---|
| WAL 文件记录 | 32B | `LogID(8) + TermID(8) + len(4) + ClusterID(8) + payload(0) + len(4)` |
| `Record::size()` | 16B | 只加 `sizeof(ClusterID)+sizeof(TermID)+msg.size()` |
| `sizeof(Record)` | 48B | 本机构建：两个 8B ID，加 32B `std::string` 对象 |
| `sizeof(Node)` | 3200B/64槽 | 包含 64 个 Record、指针、原子状态和对齐 |
| `malloc_usable_size(Node)` | 3584B/64槽 | 本工程 jemalloc 在本机采用的分配档位 |

类型定义和计账见 [AtomicLogBuffer.h:23](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/wal/AtomicLogBuffer.h#L23)、[AtomicLogBuffer.h:44](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/wal/AtomicLogBuffer.h#L44)、[ThriftTypes.h:14](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/common/thrift/ThriftTypes.h#L14)。结构体尺寸与分配档位由组件程序实测，不是凭经验假定。

一个新 Node 即使只存入一条，也已经分配全部 64 槽。因此实际增长是台阶状；分片多、心跳时间错开后，进程级曲线可能看起来近似平滑。

### 2.4 为什么 8MiB 之前一直不清理

[FileBasedWal.cpp:59](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/wal/FileBasedWal.cpp#L59) 为每个分片构建一个 `AtomicLogBuffer::instance(policy_.bufferSize)`。这个 `instance()` 每次都 `new`，不是全局单例。

`AtomicLogBuffer.cpp` 的原始实现已原样保存在 [baseline/kvstore/wal/AtomicLogBuffer.cpp](../../task/wal-investigation/baseline/kvstore/wal/AtomicLogBuffer.cpp)。**本节涉及该文件的行号都以这份固定基线为准**，避免观测补丁插入行数后产生歧义。

- [原始 push():71](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/wal/AtomicLogBuffer.cpp#L71) 在 head 不存在、已满或已标删时直接分配新 Node，存入第一条并 `return`。
- 只有“向现有未满 head 追加”分支才检查 `size_ + recSize > capacity_`。超过时将最老有效 Node 标删，推进 tail，扣除它的逻辑计账。
- 标删不立即 `delete`。被标删节点仍由物理链表连接，等待安全回收。
- [原始 releaseRef():167](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/wal/AtomicLogBuffer.cpp#L167) 只在 iterator 析构时调用。必须是最后一个 reader 离开，并且 `dirtyNodes > 5` 或逻辑 `size_ > max_log_buffer_size`，才尝试回收 tail 之后的脏节点。

在最普通的填充阶段，逻辑容量尚未用满，`dirtyNodes=0`。即使 reader 每条日志后正常释放，GC 也没有可回收的有效节点；这就解释了“不需要业务查询或长读者，也能持续上涨”。

### 2.5 为什么 TTL、重启、RSS 是独立问题

[FileBasedWal::cleanWAL():640](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/wal/FileBasedWal.cpp#L640) 与带 commit-id 的重载只操作 `walFiles_` 和磁盘文件。它们遵守 TTL、commit-id 和至少保留两个文件等限制，不调用 `AtomicLogBuffer::reset()`，也不会使有效内存缓存按小时过期。

启动时 [FileBasedWal 构造函数:40](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/wal/FileBasedWal.cpp#L40) 创建空缓存，再扫描磁盘文件恢复首末 ID、term 和当前追加文件；扫描并不将历史日志灌入缓存。后续读取可以走文件 iterator。因此重启后缓存从小重新积累，不能据此直接认定历史对象泄漏。

物理 `delete` 发生以后，分配器还可能把内存留作复用。组件实验已直接观察到 jemalloc 下节点全部释放而 RSS 当时不下降；这不等于节点仍然活跃。

## 3. 数量级：为什么 50 空间能每天增加几百 MB

设某个 storage 进程有 `P` 个本地分片副本，缓存还没到容量，且每个副本已积累约 `N` 条空日志，则本机实测布局下：

```text
Node 数量 ≈ P × ceil(N / 64)
Node 对象字节 = Node 数量 × 3200
Node jemalloc usable 字节 = Node 数量 × 3584
满节点平均分配 = 3584 / 64 = 56B / 空日志
```

这里应数本地副本，不能只数 leader。题设共有：

```text
逻辑分片总数 = 50 × 20 = 1000
集群分片副本总数 = 1000 × 3 = 3000
3 台 storage，正常三副本分布：每台约 3000 / 3 = 1000 个本地副本
```

只有约三分之一在本机担任 leader，并不会把内存倍率减到三分之一，因为剩下的 followers 同样写本地 WAL 和缓存。

每个默认缓存可计账的空记录量为：

```text
N_capacity = 8388608 / 16 = 524288 条
有效 Node ≈ 524288 / 64 = 8192
对象字节 = 8192 × 3200 = 25MiB
jemalloc usable = 8192 × 3584 = 28MiB
```

GC 批次、最后一个未满 Node 和暂留脏节点会造成少量额外空间。该值也不包含 `FileBasedWal`、分配器页/arena 元数据、RocksDB 等其它对象。

| 假设 | 每副本空记录/天 | 1000 本地副本的 WAL Node 增量/天 | 单缓存从空到逻辑 8MiB |
|---|---:|---:|---:|
| 源码默认 heartbeat=5，间隔约1.9155秒 | 45105.7 | 2408.9MiB，约2.35GiB | 11.62天 |
| 配置模板 heartbeat=30，间隔约10.2495秒 | 8429.7 | **450.2MiB** | **62.20天** |

计算式为 `86400 / interval × 56 × P`，填充时间为 `524288 × interval / 86400`。

模板参数下“每天几百 MB、长时间不下降”的量级因此有直接的代码解释。1000 个副本最终仅 Node usable 就可能约 `1000 × 28MiB = 27.34GiB`。这些都是**有条件推算**：要求本地副本数、实际心跳节奏、缓存初始状态与假设一致，且没有长期复制阻塞等影响；它们不是生产环境已经测得的曲线。实际服务应以空日志计数和节点计数的增量校准。

## 4. 原始 AtomicLogBuffer 组件复现

### 4.1 实验做了什么

[atomic-buffer-probe.cpp](../../task/wal-investigation/atomic-buffer-probe.cpp) 直接调用本仓库原始 `AtomicLogBuffer` 的 `push()`、`iterator()`，不模拟它的回收算法。它在单线程静止采样点读取逻辑计账、节点数、脏节点和 reader 数，并汇总 `sizeof(Node)`、`malloc_usable_size()`、进程 RSS。

程序只在自己的翻译单元暴露 private 字段，先包含依赖头再临时切换访问控制；没有改变类布局，也不修改生产头。驱动没有 Raft、RPC、RocksDB。因此它能证明缓存组件本身的行为，不能代替完整服务归因。

[run-atomic-buffer-probe.sh](../../task/wal-investigation/run-atomic-buffer-probe.sh) 先从固定提交提取 `.cpp/.h`，再编译运行 GLIBC 和项目 jemalloc 两组，确保服务观测补丁不混进基线实验。

```bash
cd /home/sch/nebula/nebula-3.6-memory
./task/wal-investigation/run-atomic-buffer-probe.sh \
  > task/wal-investigation/atomic-buffer-probe-build.log 2>&1
```

jemalloc 版本的等价独立编译命令如下；脚本已保存完整执行记录：

```bash
/usr/bin/c++ -std=c++17 -O2 -g \
  -DENABLE_JEMALLOC -DENABLE_MEMORY_TRACKER -DS2_USE_GFLAGS -DS2_USE_GLOG \
  -D__STDC_FORMAT_MACROS \
  -I task/wal-investigation/baseline -I src -I build/src \
  -isystem /opt/vesoft/third-party/3.3/include \
  task/wal-investigation/atomic-buffer-probe.cpp \
  task/wal-investigation/baseline/kvstore/wal/AtomicLogBuffer.cpp \
  -L /opt/vesoft/third-party/3.3/lib \
  -lfolly -lglog -lgflags -ljemalloc -ldl -pthread \
  -o task/wal-investigation/atomic-buffer-probe-jemalloc
```

参数是 `分片缓存数 逻辑容量 each|none|held 采样记录数量列表`。`each` 每次 push 后创建销毁一个 iterator；`none` 不创建 iterator，最后再创建销毁一次；`held` 始终持有第一个 iterator，同时每次 push 创建销毁另一个 iterator，最后释放常驻 iterator。

```bash
# 默认容量，1000缓存，观察64条一个Node的阶梯
 task/wal-investigation/atomic-buffer-probe-jemalloc \
  1000 8388608 each 0,1,64,65,128,256,512,1024,2048,4096

# 默认容量，一个缓存跨越阈值，验证是否进入平台
 task/wal-investigation/atomic-buffer-probe-jemalloc \
  1 8388608 each 0,1,64,65,524288,524289,524290,524608,525000,600000,1100000

# 诊断性小容量，缩短达到驱逐条件所需的记录数
 task/wal-investigation/atomic-buffer-probe-jemalloc \
  100 4096 each 0,1,64,65,256,512,1024,4096,16384
 task/wal-investigation/atomic-buffer-probe-jemalloc \
  100 4096 none 0,1,64,65,256,512,1024,4096,16384
 task/wal-investigation/atomic-buffer-probe-jemalloc \
  100 4096 held 0,1,64,65,256,512,1024,4096,16384
```

小容量和加速追加只用于检查机制，不是生产修复方案。百万次循环代表百万条记录，不能把组件几秒运行时间解释为真实服务已持续运行数十天。

### 4.2 实验一：分片数线性放大，无 reader 滞留也能增长

每缓存追加4096条空日志，容量均为8MiB，模式 `each`：

| 缓存数 | 结束 Node 数 | 有效逻辑字节 | Node 对象字节 | jemalloc usable 字节 | RSS 起点→终点（KiB） |
|---:|---:|---:|---:|---:|---:|
| 1 | 64 | 65,536 | 204,800 | 229,376 | 4580→4808 |
| 100 | 6,400 | 6,553,600 | 20,480,000 | 22,937,600 | 4636→27240 |
| 1000 | 64,000 | 65,536,000 | 204,800,000 | 229,376,000 | 4716→230612 |

所有结束点 `dirty_nodes=0, refs=0`。也就是说，这一增长不依赖常驻查询、iterator 泄漏或 GC 被阻塞，完全来自仍有效的缓存积累。

1000缓存时，每缓存1条和64条都只有1000个总Node；第65条变为2000个Node。这个台阶直接对应64槽布局。

原始证据：[atomic-jemalloc-default-1.csv](../../task/wal-investigation/atomic-jemalloc-default-1.csv)、[atomic-jemalloc-default-100.csv](../../task/wal-investigation/atomic-jemalloc-default-100.csv)、[atomic-jemalloc-default-1000.csv](../../task/wal-investigation/atomic-jemalloc-default-1000.csv)。

### 4.3 实验二：默认8MiB容量有平台，但实际分配约28MiB

单缓存、`each` 模式：

| 已追加空记录 | 逻辑字节 | Node数 | dirty数 | Node usable字节 | RSS（KiB） |
|---:|---:|---:|---:|---:|---:|
| 524,288 | 8,388,608 | 8,192 | 0 | 29,360,128 | 33,512 |
| 524,289 | 8,388,624 | 8,193 | 0 | 29,363,712 | 33,516 |
| 524,290 | 8,387,616 | 8,193 | 1 | 29,363,712 | 33,516 |
| 524,608 | 8,388,608 | 8,197 | 5 | 29,378,048 | 33,532 |
| 525,000 | 8,387,712 | 8,192 | 0 | 29,360,128 | 33,536 |
| 600,000 | 8,388,608 | 8,193 | 1 | 29,363,712 | 33,536 |
| 1,100,000 | 8,388,096 | 8,194 | 2 | 29,367,296 | 33,536 |

第524289条的新Node分支先返回，所以逻辑容量短暂超出16B；第524290条才标记最老Node。dirty达到5仍未超过 `>5` 门槛，继续追加后才出现整批物理回收。超过110万条后的平台是实际观测结果。

原始证据：[atomic-jemalloc-default-threshold.csv](../../task/wal-investigation/atomic-jemalloc-default-threshold.csv)。

### 4.4 实验三：物理GC必须满足 reader 生命周期条件

100缓存、每缓存逻辑容量4096B、各追加16384条空日志：

| 模式及采样阶段 | Node数 | dirty数 | refs总数 | Node usable字节 |
|---|---:|---:|---:|---:|
| 每条销毁iterator，结束 | 400 | 0 | 0 | 1,433,600 |
| 从不创建iterator，结束 | 25,600 | 25,200 | 0 | 91,750,400 |
| 上一行结束后各创建销毁一次iterator | 400 | 0 | 0 | 1,433,600 |
| 常驻一个iterator，结束 | 25,600 | 25,200 | 100 | 91,750,400 |
| 释放常驻iterator后 | 400 | 0 | 0 | 1,433,600 |

这里有两个容易误判的地方：`refs=0` 不保证GC曾被调用；有效逻辑容量已经维持4096B，也不代表所有被标删节点已实际释放。

实际 Raft leader/follower 提交路径通常会构造并销毁 WAL iterator，因此不能把 `none/held` 的构造条件直接套到题述环境。它们用于解释容量不是严格堆上限，以及需要观测什么才能判断额外异常。

原始证据：[atomic-jemalloc-small-each.csv](../../task/wal-investigation/atomic-jemalloc-small-each.csv)、[atomic-jemalloc-small-none.csv](../../task/wal-investigation/atomic-jemalloc-small-none.csv)、[atomic-jemalloc-small-held.csv](../../task/wal-investigation/atomic-jemalloc-small-held.csv)。

### 4.5 实验四：活跃对象归零，RSS仍可保留

1000缓存实验结束时，节点 usable 为229,376,000B，RSS230612KiB。随后销毁全部缓存，遍历可见 Node 总数归零，RSS当时仍为230612KiB。GLIBC对照与jemalloc表现不同，进一步说明不能使用单一RSS数字反推对象存活。

本结论仅为“释放不保证RSS立即下降”，不是“所有长时间RSS高位都是allocator留存”。真实进程仍需结合 live allocations 和 allocator 数据分析。

编译、环境与核验：[atomic-buffer-probe-build.log](../../task/wal-investigation/atomic-buffer-probe-build.log)、[atomic-buffer-probe-environment.txt](../../task/wal-investigation/atomic-buffer-probe-environment.txt)、[atomic-buffer-probe-validation.txt](../../task/wal-investigation/atomic-buffer-probe-validation.txt)。

## 5. 另一条已确认的空闲写入链：RocksDB提交位置

本节根据独立分析代理的 [code-analysis-independent.txt](../../task/wal-investigation/code-analysis-independent.txt) 和 [rocks-commit-probe-reproduction.txt](../../task/wal-investigation/rocks-commit-probe-reproduction.txt) 整合；其组件程序、数据和WAL缓存实验互相独立。

### 5.1 “Skip the heartbeat”之后还有什么

[Part::commitLogs():225](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/Part.cpp#L225) 先更新 `lastId/lastTerm`，再对空payload执行 `continue`。它跳过的是顶点/边等业务操作。循环结束后 [Part.cpp:349](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/Part.cpp#L349) 仍调用 `putCommitMsg()`，再 `commitBatchWrite()`。

[putCommitMsg():403](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/Part.cpp#L403) 构造16B value：`int64 committedLogId + int64 committedLogTerm`；[systemCommitKey():101](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/common/utils/NebulaKeyUtils.cpp#L101) 构造8B key，将分片编号和系统键类型编码进去。同一分片不断覆盖相同key，但每次提交位置都前进。

[RocksEngine::commitBatchWrite():124](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/RocksEngine.cpp#L124) 最终调用 `rocksdb::DB::Write()`。工程所用RocksDB头的 `inplace_update_support` 默认为false，组件实际 OPTIONS 也记录了该设置。覆盖写不等于在现有memtable记录上原地替换。

空间数与分片数的影响因此不同：WAL缓存按分片副本独立；[NebulaStore::addSpace():395](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/NebulaStore.cpp#L395) 为每个space与dataPath组合创建engine，其memtable有各自的填充与flush时机。模板中相关write-buffer设置是64MiB、最多4个，但这个数也不能直接当成所有空间启动即预分配的RSS。

另外，[RocksEngineConfig.cpp:307](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/RocksEngineConfig.cpp#L307) 的block cache是进程级静态共享对象，不能把配置cache容量按空间数相乘。

### 5.2 同库独立RocksDB实验及完整复现

程序使用本机同一套 `librocksdb.a` 7.5.3和jemalloc。5个DB×每DB20个分片提交键×10000轮，每次使用一个单Put WriteBatch；key/value字节完全复刻上述系统提交位置格式。`disableWAL=false`、`sync=false`、64MiB write buffer、max4，未写业务顶点/边，flush前不做读取查询。

```bash
cd /home/sch/nebula/nebula-3.6-memory
c++ -std=c++17 -O1 -g -no-pie \
  -I/opt/vesoft/third-party/3.3/include \
  task/wal-investigation/rocks-commit-probe.cpp \
  -L/opt/vesoft/third-party/3.3/lib -L/opt/vesoft/third-party/3.3/lib64 \
  -Wl,--start-group -lrocksdb -lsnappy -lz -lbz2 -llz4 -lzstd -lgflags -ljemalloc \
  -Wl,--end-group -lpthread -ldl -lrt \
  -o task/wal-investigation/rocks-commit-probe
ulimit -n 65536
nice -n 10 task/wal-investigation/rocks-commit-probe \
  task/wal-investigation/rocks-probe-data-rerun 5 20 10000 \
  > task/wal-investigation/rocks-commit-probe-rerun.csv \
  2> task/wal-investigation/rocks-commit-probe-rerun.meta
```

复跑需采用新的data目录，驱动会拒绝已存在的目录，不会自动删除历史数据。上面的 `-rerun` 是复跑示例；原实验使用的是 `rocks-probe-data` 和无 `-rerun` 的结果文件。

| 阶段 | 累计覆盖写 | active memtable entries | memtable字节 | jemalloc allocated字节 | RSS（KiB） |
|---|---:|---:|---:|---:|---:|
| 打开5个DB | 0 | 0 | 10,240 | 2,481,912 | 11,292 |
| 写入中 | 100,000 | 100,000 | 5,253,120 | 7,516,200 | 17,200 |
| 写入完成 | 1,000,000 | 1,000,000 | 52,439,040 | 54,700,736 | 63,544 |
| 同步flush全部DB后 | 1,000,000 | 0 | 10,240 | 5,881,248 | 64,144 |

最终扫描只有100个逻辑key，所有value均为最后一轮提交值。整个过程中共享block cache始终为96B。这个实验没有Raft WAL缓存，仍观察到memtable增加50MiB；flush以后活跃memtable和allocated明显下降，RSS却没有随之下降。

本区间实测memtable增量平均52.4288B/写，是该arena分配和配置下的观测值，不能推广为所有工作负载恒定单条成本，也不能与WAL的56B/条机械相加后当作生产精确日增量。完整服务中日志合批、flush、其它元数据与写入频率均会影响比例。

原始证据：[rocks-commit-probe.cpp](../../task/wal-investigation/rocks-commit-probe.cpp)、[rocks-commit-probe.csv](../../task/wal-investigation/rocks-commit-probe.csv)、[rocks-commit-probe.meta](../../task/wal-investigation/rocks-commit-probe.meta)。

### 5.3 为什么关闭RocksDB WAL不是一个纯磁盘对照

[NebulaStore::cleanWAL():1293](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/NebulaStore.cpp#L1293) 在 `rocksdb_disable_wal=true` 时，还会定时对各engine执行flush；默认清理间隔为600秒。切换该参数既改变RocksDB磁盘WAL，也会改变memtable的时间行为，不能只将其解释为“是否写磁盘日志”这个单变量。本文没有把这个参数变化作为修复建议。

## 6. 已证明的内容、仍需服务证据的内容

| 论断 | 当前证据 | 可得结论 |
|---|---|---|
| 无业务请求仍可生成空WAL | Raft源码完整调用链 | 机制成立，实际速率应看运行数据 |
| 缓存随空记录和分片数增加 | 原始实现组件CSV，1/100/1000缓存对照 | 已直接复现 |
| 8MiB不等于真实8MiB堆 | sizeof与jemalloc usable实测，默认阈值跨越 | 本机构建约28MiB节点分配/分片 |
| 长期增长是否无限 | 正常iterator模式跨至110万条 | 此路径有平台，不能泛称无限泄漏 |
| 是否存在读者导致额外滞留 | none/held组件对照 | 条件机制成立，未证明生产存在该条件 |
| 空提交会增加memtable | 同库提交key覆盖写组件实验 | 机制成立，不能把服务RSS全部归WAL |
| RSS不降是否等于未释放 | Node归零及memtable flush实验 | 不等价，allocator留存可使RSS保持 |
| 题设每天几百MB是否解释得通 | 模板参数与副本数量模型 | 约450MiB/天的WAL量级吻合，仍非生产实测 |

为了在真实storaged中建立更强归因，需要把以下量放在同一时间轴：空日志追加数、Node创建与delete数、GC释放数、活跃/脏逻辑字节、reader数、RocksDB memtable、jemalloc allocated/resident、进程RSS。磁盘WAL解析还能独立验证payload是否为空，但磁盘字节数不能直接当作RSS。

证据边界也限定了本报告的结论：源码与组件实验已给出能够解释问题特征的完整机制，并证伪了“只有业务数据才用内存”“WAL TTL自动回收内存”“8MiB就是实际堆上限”“RSS不降必然意味着对象泄漏”这些推断；对实际部署各组件占比、是否存在异常长reader，以及最终增长平台，仍需以完整服务采样为准。
