# Nebula Graph 图空间级 WAL 缓存配置需求设计

开发启动与团队需求澄清文档

文档版本：0.9 评审稿｜编写日期：2026 年 10 月 11 日｜目标分支：`3.6-memory`

适用读者：Storage、Raft、Meta、测试和运维开发人员，以及本需求的设计责任人。

本文给出可用于拆分开发任务的首期设计。当前只完成机制分析、既有实验复核和方案设计，**空间级配置功能尚未实现，也未通过生产性能验收**。文中的“必须”是拟定的开发与验收契约；明确标为“建议、待确认”的数值需要设计评审签署，不能当作已经批准的现网参数。

## 1 先明确要解决什么

建议开发“全局默认值加图空间覆盖值”的 WAL 缓存配置能力。首期通过 gflag 输入配置，在 storaged 启动时完成校验并冻结；每个业务分片构造 WAL 时取得所在空间的容量。高频空间沿用原容量，只给确认空闲且性能要求较宽松的空间配置较小容量。这样可以降低空闲分片长期积累的 WAL 缓存平台，同时避免直接缩小同进程所有空间的缓存。

这项能力不能直接保证容器内存降到某个百分比。已经确认：空闲空间仍会产生内部日志；WAL 缓存按逻辑字节计账，实际分配可能更大；内部日志提交还会占用 RocksDB 写缓冲。尚未确认：生产环境超过 80% 的内存中，各类对象各占多少，以及空间级配置对生产混合负载的收益。因此，开发完成的标准必须同时包含“参数确实按空间生效”和“在生产等价负载下内存、查询、写入及恢复均达标”。

### 1.1 用户背景和业务分类

生产环境已经商业化运行。用户反馈 storaged 内存达到 Docker memory limit 的 80% 以上，查询、写入受到影响，并经常出现内存水位线相关错误；初步定位与大量空闲图空间有关。这是用户提供的现场情况，本次实验没有采集对应生产容器，也没有复现容器 OOM 或生产错误码。

| 空间类型 | 用户提供的业务特征 | 首期处理原则 |
| --- | --- | --- |
| 高频读写 | 点边合计约 5 亿，读写频繁，实时性能要求高 | 保持该实例现有有效 WAL 容量，作为性能保护对象 |
| 中频读写 | 百万级数据，读写频率中等，实时性能要求一般 | 第一轮维持原容量，后续单独评估 |
| 低频读写 | 几千条模型资产，读取较少，批量写入后可数月无新写入 | 经空间归因后选择小批量试点，逐档缩小 WAL 容量 |

“低频空间”由运维和业务确认。首期不根据访问次数自动分类，也不自动调参。空间的数据量小，不代表分片对象少；即使一个空间只有几千条数据，若仍分配了较多分片和副本，就仍会创建相应的 WAL 与 RocksDB 对象。

### 1.2 首期目标与明确边界

首期目标是让同一个 storaged 内的不同业务图空间使用不同的 `wal_buffer_size`，并且能准确查询实际值、发现配置错误、进行滚动发布和回退。作用单位是“空间”，分配单位仍是“本机该空间的每个分片副本”。例如空间配置 1 MiB，本机有 20 个分片副本，就有 20 个独立的 1 MiB 逻辑容量，而不是整个空间共享 1 MiB。

首期只改变容量的来源，不改空日志产生条件、日志格式、Raft 协议、提交语义、缓存淘汰和回收算法、磁盘 WAL 保留及持久化策略。不支持在线缩容、空间自动分级、空闲自动释放，也不承诺限制整个进程的 RSS。

RocksDB 写缓冲的空间化是后续独立需求。共享 block cache、row cache、线程池、MemoryTracker 和容器水位线不是简单增加空间参数映射就能正确拆分的对象，均不纳入首期。

### 1.3 事实和设计的标记方式

本文按三种层次叙述：源码事实说明目标版本在什么条件下做什么；实验事实说明本地实际观察到什么；设计要求说明将要开发和测试什么。数学推算、配置示例及发布阈值都明确给出适用条件。不要把“源码支持某种机制”直接替换成“生产内存全部由该机制造成”。

源码基线固定为 `de9b3ed800a6627d9845e9289b6bbc5b6faf460a`。当前文档工作树 HEAD 为 `194958131042df77301072eb99dccbc64fb34246`。现有 `AtomicLogBuffer.cpp` 的修改只有观测日志，见 `observation.patch`；分析原始回收算法时使用固定提交或 `baseline/` 中的原文件，避免日志补丁导致行号错位。

## 2 空间没有业务写入 为什么内存还会增加

### 2.1 只需理解的五个对象

图空间是用户看到的数据隔离单位。一个空间分成多个分片；每个分片按副本数部署到 storage 实例。Raft 负责这些副本之间复制日志和决定提交顺序，Leader 发起复制，Follower 保存并应用日志。

这里的 WAL 指 Raft 使用的预写日志。每个本地分片副本既有磁盘 WAL 文件，也有保存近期日志的内存缓存。缓存帮助读取近期日志；未命中时，代码有从磁盘 WAL 读取的路径。

RocksDB 是应用已提交日志后的存储引擎。它的 MemTable 是先接收写入、随后刷成磁盘 SST 文件的内存结构。Nebula 的 Raft WAL 与 RocksDB 自己的 WAL 是不同对象，不能因为都叫 WAL 就把参数混用。

### 2.2 第一条因果链 空日志进入 WAL 缓存

当分片 Leader 需要发送心跳、且当时没有正在复制的日志时，当前实现会提交一条内容为空的正常 Raft 日志。这条日志虽然没有业务 payload，仍有日志序号、任期等信息，仍会写入 WAL 并复制给副本。因此，没有 INSERT、UPDATE、DELETE，不等于 WAL 不增长。

WAL 缓存把日志放在链表节点中，每个 Node 固定能放 64 条 Record。空日志在容量计账时只算 16 字节，但 Record 对象还包含字符串对象等结构，Node 也有链表和管理字段。本实验构建下 Record 为 48 字节，Node 的结构大小为 3200 字节，jemalloc 实际为一个 Node 分配 3584 字节。

当逻辑字节尚未超过容量时，旧节点仍然有效，不会因为业务空闲或经过一段时间而自动删除。日志不断加入，节点就逐渐增加。达到容量后，追加路径才开始把旧尾节点标为删除；真正释放还要等待读者退出时满足回收条件。因此 `wal_buffer_size` 是影响近期日志保留规模的逻辑容量，不是立即生效的物理内存硬上限。

### 2.3 第二条因果链 空日志也会更新 RocksDB 提交位置

应用空日志时，业务操作分支确实被跳过，但代码在跳过之前已经记录了这批日志的最后序号和任期。批次结束后，仍会把最新提交位置写进 RocksDB。

每个分片的提交位置是固定键，但同一个键反复覆盖写入，仍会在 MemTable 中形成写入占用，等待 flush 等机制处理。“只有少量系统键”不等于“只有这些键最终值大小的内存”。这解释了为什么实验中 WAL 节点和 MemTable 会同时上涨，也解释了为什么单调 WAL 容量只能处理其中一部分。

### 2.4 第三条因果链 释放对象不等于 RSS 立即下降

RSS 表示进程当前驻留在物理内存中的页；Node 分配字节表示某一类对象的分配量；RocksDB 属性表示该引擎自己的统计。这些不是同一个指标。对象释放后，分配器可能保留内存页供以后复用；容器统计又可能包含文件缓存等项目。

所以需要同时观测 WAL 对象、MemTable、进程 RSS、分配器统计和容器原始统计。不能把 Node 字节与 RSS 直接相加，也不能把某次 RSS 下降等同于某一条回收路径已经发生。

### 2.5 一个与生产接近的最小例子

以下为教学假设：一个低频模型空间有 20 个分片、3 副本，均匀放在 3 个 storaged 上。每台实例有该空间的 20 个分片副本，即有 20 个 WAL 缓存。

如果每个缓存都主要积累空日志，8 MiB 逻辑容量可以容纳约 `8 × 1024 × 1024 ÷ 16 = 524288` 条空记录。按每 Node 64 条计算，需要约 8192 个 Node；按本实验 jemalloc 分配大小计算，约为 28 MiB Node 分配量。20 个本地副本的这一部分约为 560 MiB。

把这个空间配置为 1 MiB 后，同样条件下每个缓存的平台约为 3.5 MiB，20 个副本约为 70 MiB，差额约 490 MiB。以上仅是“已经填满、正常读者释放、主要为空日志”的 Node 分配推算，不是实测生产 RSS，也不包含 MemTable、查询执行内存、待回收节点和其他开销。

如果生产缓存尚未填到 1 MiB，缩小参数也不会立刻产生上述差额。需要区分“降低长期平台”和“降低当前用量”。首期通过重启构造新缓存生效；重启后的短期低内存本身不能证明空间配置有长期收益。

## 3 源码如何支持这些判断

本节列出理解需求必需的代码。完整逐函数讲解见本目录三篇 `CODE-WALKTHROUGH` 文章，固定提交链接和职责清单见附录 A。

### 3.1 心跳路径会追加空日志

目标文件为 `src/kvstore/raftex/RaftPart.cpp`，`sendHeartbeat()`，基线约第 2041 行。以下为保留关键条件的真实代码节选：

```cpp
if (!replicatingLogs_.load(std::memory_order_acquire)) {
  folly::via(executor_.get(), [this] {
    std::string log = "";
    appendLogAsync(clusterId_, LogType::NORMAL, std::move(log));
  });
}
```

它证明当前实现存在“心跳期间向正常复制路径追加空日志”的行为，并且受复制状态条件约束。它不证明每个心跳必定产生一条日志，也不能单独证明生产中所有空 payload 都来自这里。实验还需要结合命令记录、日志速率、分片任期和业务写入情况判断。

Leader 轮询、心跳间隔与复制状态共同决定真实频率，不能简单把参数 30 秒解释成每个副本严格每 30 秒一条。当前源码 gflag 默认值与实验使用的配置模板值也不同；本次常速实验使用 30 秒，后续用 1 秒加速，按真实采样速率分析。

### 3.2 容量计算没有包含 Record 的全部结构开销

目标文件为 `src/kvstore/wal/AtomicLogBuffer.h`，`Record::size()`，基线第 32 行附近：

```cpp
int32_t size() const {
  return sizeof(ClusterID) + sizeof(TermID) + msg_.size();
}
```

这段代码证明计账包含 clusterId、termId 和消息长度。空消息在本构建下计账 16 字节，没有把整个 `std::string` 对象、Node 固定槽位、链表字段和分配器取整算进去。结构大小与分配器结果仍需用实际二进制验证；换编译器、ABI 或 allocator 后不能直接照搬 28 MiB。

### 3.3 容量触发标删 读者退出才有机会释放

目标文件为原始 `AtomicLogBuffer.cpp`，`push()` 第 71 至 119 行、`releaseRef()` 第 167 至 213 行。下列是解释用伪代码，不是可以替换源码的实现：

```text
追加一条记录
  如果需要新建 Node
    分配 Node 并插入记录 更新逻辑字节 然后返回
  否则 如果追加后超过容量 且尾节点不是头节点
    将一个旧尾节点标删 从有效逻辑字节中扣除
  将记录加入当前头节点

一个迭代器释放引用
  如果仍有其他读者 则返回
  如果待回收节点数超过阈值 或逻辑字节超过另一阈值
    在回收互斥条件满足时 断开并删除已标删节点
```

关键点是：新建 Node 的分支先返回；单次追加最多推动一个尾节点；只有一个 Node 时不能立即淘汰自身；标删与实际释放不是同一步。原实现默认待回收节点阈值为 5，另有 `max_log_buffer_size` 条件，但它也不是整个缓存实际分配的硬上限。

这些条件说明容量附近允许波动，也说明把 `capacity_` 简单改成原子变量并在线调小，不等于完成安全缩容。持有读者、批量大日志、头尾边界、并发访问都需要额外设计。首期通过构造时固定容量避免引入这组新语义；保留原回收算法，也就保留原算法的边界。

本次服务实验中有效逻辑字节尚小，节点没有被标删和释放，且存在大量正常读者退出记录。因此该次增长不需要用“读者永久占用导致 GC 失效”来解释。独立组件实验确实展示了长期持有读者可能延迟回收，但不能把那个独立场景写成这次服务实验的原因。

### 3.4 空消息不做业务操作 仍提交位点

目标文件为 `src/kvstore/Part.cpp`，`commitLogs()` 第 215 至 358 行。以下是真实代码的两段节选，中间省略了非空业务日志解析：

```cpp
while (iter->valid()) {
  lastId = iter->logId();
  lastTerm = iter->logTerm();
  auto log = iter->logMsg();
  if (log.empty()) {
    ++(*iter);
    continue;
  }
  // 此处省略非空业务日志解析与循环推进
}
```

```cpp
if (lastId >= 0) {
  auto code = putCommitMsg(batch.get(), lastId, lastTerm);
  if (code != nebula::cpp2::ErrorCode::SUCCEEDED) {
    return {code, kNoCommitLogId, kNoCommitLogTerm};
  }
}
// 随后调用 engine_->commitBatchWrite(...)
```

它证明空日志不改变点边业务数据，但可以推进持久提交位置。本版本提交位置键为 8 字节、值为 16 字节；MemTable 的实际占用还受 RocksDB 版本、分配方式、已有版本和刷盘节奏影响，不能用键值长度直接估算 RSS。

### 3.5 缓存变小之后 为什么仍能读日志

`FileBasedWal.cpp` 的追加路径先处理文件写入，再把记录放进内存缓存；`iterator()` 在内存未覆盖目标日志范围时创建文件迭代器。`WalFileIterator.cpp` 会打开对应文件、扫描记录位置并读取内容。`Host.cpp` 在复制时使用日志迭代器构造请求。

正常提交也使用同一个 WAL 迭代器：Leader 和 Follower 提交路径在 `RaftPart.cpp` 第 1069、1794 行附近先调用 `wal_->iterator()`，再把结果交给 `commitLogs()`。因此，小缓存引起的磁盘回退不只关系落后副本；批次较大或提交进度落后时，也可能影响正常写入的提交耗时。这是待测风险，不是本次已经测到的性能损失。

因此缩小缓存主要改变近期日志的内存命中范围，不直接删掉磁盘 WAL 或改变日志内容。这是首期选择容量参数的正确性依据，但不是“性能必然不变”的依据。副本落后时更容易走磁盘读取；若所需日志已经不在磁盘保留范围，复制路径还可能进入快照恢复流程。磁盘读取会与查询、flush、compaction 竞争，必须做混合负载和故障恢复验证。

`cleanWAL()` 的 TTL 处理对象是磁盘文件，并非内存活跃链表。调短磁盘 TTL 不能替代缩小内存缓存，还可能缩小副本追赶窗口。首期不修改 TTL。

## 4 已有实验说明了什么

### 4.1 服务实验的环境和操作

实验使用本机独立端口的一组服务：1 个 metad、1 个 graphd、3 个 storaged。构建为 Debug，使用 jemalloc 和 MemoryTracker，测试目标关闭。数据库安装目录为 `/home/sch/nebula-run/nebula-memory`；按用户要求，完整源码构建最大并行度为 `make -j5`。启动前把文件描述符上限设为 65536。

创建 `wal_idle_01` 至 `wal_idle_50`，每空间 20 个分片、3 副本、FIXED_STRING(32)。没有建立业务 schema，也没有写入点边。同一机器的 3 个 storaged 进程各有 1000 个本地分片副本，共 3000 份 WAL；Leader 分布为 370、514、116，但统计缓存必须数所有本地副本，不能只数 Leader。

服务端口为 graph 29669、meta 29559、storage 29779/29789/29799；HTTP 端口为 39669、39559、39779/39789/39799。先观察 30 秒配置下的常速阶段，再切到 1 秒加速，恢复 30 秒后执行 FLUSH 对照。所有原始命令和时间记录在 `events.jsonl`，服务已于 2026 年 10 月 10 日 07:39:05 中国标准时间停止。

### 4.2 关键观测和对应判断

| 实验窗口 | 实际观测 | 可以支持的判断 |
| --- | --- | --- |
| 常速 30 秒阶段，约 165 秒 | 各实例 RSS 增约 0.7 至 1 MiB；各约新增 1.6 万条空记录；Node 数保持 1000；MemTable 总量稳定约 50.1 MiB | 空闲仍产生内部写入；短窗口不一定出现新 Node 或明显 RSS 增长 |
| 加速后稳定阶段，60 点，首尾约 885 秒 | 三实例 RSS 分别增加 157.762、157.090、157.648 MiB；59 个相邻区间均上涨；MemTable 分别增加 86、94、100 MiB | 无业务写入也能观察到持续上涨；上涨包含 RocksDB 成分 |
| 加速阶段内的 WAL 观测窗口，约 870 秒 | 每实例 Node 从 3000 增至 26005，增加 23005 个；每实例结构字节增约 70.206 MiB、空记录增 1492158；没有 free、GC、reset | 缓存节点增加有直接对象证据；不是只用 RSS 推测 WAL 增长 |
| 恢复 30 秒，约 165 秒 | 日志产生回到约每实例 100 条每秒，RSS 仍缓慢净增约 1.8 至 2 MiB | 降低产生速度不会自动清空已有缓存 |
| FLUSH 前至约 170 秒后 | RSS 从约 344.6/344.6/345.5 降至 240.7/241.2/240.8 MiB；各下降约 104 MiB；WAL Node 仍由 35076 增至 35501 | RocksDB 刷盘可释放一部分占用，但没有消除 WAL 节点增长 |

885 秒 RSS 窗口与 870 秒内部 WAL 窗口不同，不能把两者精确相减得到“其他内存”。整个加速设置持续约 20 分 52 秒，但外部 CSV 在 07:23 至 07:28 有采样缺口；连续上涨结论只针对上述完整 60 点窗口。

FLUSH 的 50 个作业编号为 51 至 100，已核验均为 FINISHED。应以 phase-08 为刷盘前基线、phase-09 尾部为刷盘后观察；phase-09 第一条采样时作业未全部完成，不能把它当作稳态。MemTable 刷盘后从约 100 KiB 再到约 50.1 MiB，而 RSS 没有等额增加，也说明属性值与驻留页不能简单相加。

### 4.3 WAL 文件提供的独立证据

停机后归档了 3000 个 WAL 文件，解析到 6730574 条记录，全部 payload 为空；原始文件共 215378368 字节，未发现不完整记录、长度不匹配、日志 ID 异常等结构问题。归档 `wal-evidence.tar.gz` 的 SHA-256 为：

```text
acb2e767bf72b9b715879c6a911e53ef115e30469f2185e8e4b3205ad988fd55
```

这组文件说明“没有点边写入时仍积累大量空 WAL”不是采样图的假象。空 payload 本身不能唯一识别产生来源，要与没有业务写入的命令记录及前述心跳源码一起使用。它也不能证明生产环境全部 WAL 都为空。

### 4.4 容量缩小的组件实验

使用固定提交的原始 `AtomicLogBuffer`，每档独立进程、一个缓存，每次追加空日志后读取最新记录并释放迭代器，推进到容量对应记录量的 4 倍。结果如下；这里统计的是存活 Node 的 `malloc_usable_size` 总和，不是 RSS。

| 逻辑容量 | 首次填满所需空记录 | 首次填满和四倍填充采样的 Node 分配 |
| --- | --- | --- |
| 8 MiB | 524288 | 28 MiB |
| 4 MiB | 262144 | 14 MiB |
| 2 MiB | 131072 | 7 MiB |
| 1 MiB | 65536 | 3.5 MiB |
| 256 KiB | 16384 | 0.875 MiB |

容量附近仍有节点级波动，待回收节点和读者条件不能忽略。析构后 Node 总量归零，RSS 未必同步回到起点。结果支持“正常读者释放的空日志缓存，容量更小会形成更低平台”；不证明 1 MiB 或 256 KiB 已适合生产。

### 4.5 与持续业务写入场景的对比

同样 8 MiB 逻辑容量，独立组件中空消息填满约需要 28 MiB 分配，固定 1024 字节 payload 的对照约为 10.4375 MiB，256 字节对照约为 11.0615 MiB。这里统一比较存活 Node 与消息字符串堆分配的合计，包含尚未释放的标删节点；空消息没有额外 payload 堆分配，非空组包含它。原因是非空消息更快用完逻辑字节，保留的记录条数和 Node 数更少；完整口径见 `WRITE-VS-IDLE.md`。

这些固定 payload 不是实际编码的点边业务日志。不能由此宣称“持续写入的 storaged 总内存更小”：业务写入还会增加 MemTable、索引、批次、请求和缓存占用。并且“50 个空间持续写入”不代表每个分片都热；每空间若只热 1 个分片，本机仍可能是 50 个热分片、950 个冷分片。

持续写入还会更快到达缓存平台；空闲空间可能每天缓慢增长、很久才达到平台。停止业务写入后，旧业务日志逐步被空日志替换，Node 数还可能再次增长。这正是本需求选择显式空间策略、并要求长期稳态验证的原因。

### 4.6 目前没有证据支持的承诺

当前没有完整的“50 空间持续真实业务写入”服务 RSS 对照，没有新空间配置功能的服务实验，没有生产错误码与容器 OOM 的联合采样，也没有生产 SLA 压测。后续文档、代码注释和发布说明不得把这些写成已经验证。

## 5 团队如何复核和复现

先做只读证据复核，再做独立组件实验，最后在隔离环境重复服务实验。只读复核可以确认原结论的来源，组件实验可以确认容量机制，完整服务实验用于验证 WAL 与 RocksDB 同时增长。三者不能互相替代。

### 5.1 只读重新解析停机 WAL

下列命令在工程根目录执行，只读取原归档，输出到新建任务目录，不启动服务：

```bash
cd /home/sch/nebula/nebula-3.6-memory
sha256sum task/wal-investigation/wal-evidence.tar.gz
replay_dir=$(mktemp -d task/wal-investigation/replay-check.XXXXXX)
tar -xzf task/wal-investigation/wal-evidence.tar.gz -C "$replay_dir"
python3 task/wal-investigation/parse_wal.py \
  --data-dir s1="$replay_dir/data/storage1" \
  --data-dir s2="$replay_dir/data/storage2" \
  --data-dir s3="$replay_dir/data/storage3" \
  --output "$replay_dir/reparsed" > "$replay_dir/parse.log" 2>&1
```

预期解析退出码为 0，结果中记录数为 6730574、非空数为 0、文件数为 3000、异常为空。若散列不符、记录不完整或计数不同，先排查证据版本与解析器，不能继续引用原汇总冒充复核通过。原始汇总在 `wal-full.json`；解析器会同时生成 JSON 和 CSV。

### 5.2 独立复跑 WAL 容量对照

本机已有组件二进制，可在工程根目录执行下列命令。每档一个独立进程，输出使用新目录；不运行会写入原结果目录的旧脚本。若换机器，需要先按 `run-atomic-buffer-probe.sh` 中的依赖和编译方式构建，不能假设现成 ELF 可移植。

```bash
set -euo pipefail
cd /home/sch/nebula/nebula-3.6-memory
probe=task/wal-investigation/atomic-buffer-probe-jemalloc
probe_dir=$(mktemp -d task/wal-investigation/capacity-check.XXXXXX)
sha256sum "$probe" > "$probe_dir/binary.sha256"
for capacity in 8388608 4194304 2097152 1048576 262144; do
  records=$((capacity / 16))
  checkpoints="0,1,64,65,$records,$((records+1)),$((records+2))"
  checkpoints="$checkpoints,$((records+320)),$((records*2)),$((records*4))"
  "$probe" 1 "$capacity" each "$checkpoints" \
    > "$probe_dir/capacity-$capacity.csv" \
    2> "$probe_dir/capacity-$capacity.meta"
done
```

核对 `refs`、`dirty_nodes`、`logical_bytes`、`nodes`、`node_malloc_usable_bytes` 和 `rss_kib`，不要只看 RSS。`each` 表示每次追加后创建并释放读者；`held` 和 `none` 属于不同回收条件，需另建实验，不应混入这张容量表。

### 5.3 复跑完整服务前的环境准备

**完整服务复现只在隔离测试环境执行。** 原 `cluster.py` 硬编码了安装路径、端口以及相对于脚本的证据路径；`prepare` 仅检查安装目录中的 data 非空情况，并非全面的覆盖保护，仍会写配置和生成文件。不得直接在当前保留证据的目录重新执行 `prepare`，也不得为了通过检查删除原数据。

测试负责人需要建立独立 checkout 或完整实验副本，保留仓库目录层级；修改副本中的 INSTALL、CONSOLE、服务/HTTP 端口、配置和采样路径，使它们均指向新目录。单独把脚本复制到任意目录可能改变它推导的 REPO 根路径，必须核对。确认目标端口无占用、数据和证据目录为空、控制台版本正确后再准备。

完整源码构建方式沿用 `AGENTS.md`，但并行数遵守用户更严格的 `make -j5`。下列示例使用新的安装目录，先核验它确为本次专用路径，且与已修改的脚本 INSTALL 一致；在独立 build 目录执行。目录已存在时检查命令会失败，不应删除旧目录强行通过：

```bash
set -euo pipefail
repro_install=/home/sch/nebula-run/nebula-space-wal-repro
test ! -e "$repro_install"
cmake -DCMAKE_INSTALL_PREFIX="$repro_install" \
  -DENABLE_TESTING=OFF -DCMAKE_BUILD_TYPE=Debug ..
make -j5
make install
ulimit -n 65536
```

服务控制必须针对本次配置和本次 PID。不要按进程名批量停止机器上的其他数据库。此次文档编写没有重新构建或启动数据库。

### 5.4 完整服务的操作顺序和检查点

以下命令是完成上一节隔离改造后，在新 checkout 根目录运行的复现流程；不能跳过隔离准备直接粘贴到原目录。日志统一保存在新任务目录。原脚本的 `start` 会设置进程文件描述符限制，外层仍保留 `ulimit` 检查。

```bash
ulimit -n 65536
python3 task/wal-investigation/cluster.py prepare
python3 task/wal-investigation/cluster.py start
python3 task/wal-investigation/cluster.py register
```

核查本次 `register-hosts.log` 和 `SHOW HOSTS`，等待 3 个 storage 均 ONLINE 后继续；脚本固定等待时间不代替这个检查。

```bash
python3 task/wal-investigation/cluster.py sample \
  --empty --name repeat-no-spaces --duration 60 --interval 10
python3 task/wal-investigation/cluster.py create
python3 task/wal-investigation/cluster.py inspect
```

确认 50 个空间的 20 分片和 3 副本均就绪，每实例有 1000 个本地副本，然后开始采样。

```bash
python3 task/wal-investigation/cluster.py sample \
  --name repeat-default30 --duration 180 --interval 15
python3 task/wal-investigation/cluster.py flags --heartbeat 1
python3 task/wal-investigation/cluster.py sample \
  --name repeat-transition1 --duration 60 --interval 10
python3 task/wal-investigation/cluster.py sample \
  --name repeat-accelerated1 --duration 900 --interval 15
python3 task/wal-investigation/cluster.py flags --heartbeat 30
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

检查本次 50 个 FLUSH 作业全部 FINISHED；`jobs` 只是查询一次，不会自动等待所有作业完成。若未完成，继续记录作业状态和采样，保留原样本，然后再停止本次服务：

```bash
python3 task/wal-investigation/cluster.py stop
```

建空间后先确认 50 空间、每空间 20 分片、3 副本就绪；采样应包含三实例有效响应和实际分片数。心跳切换是实验加速手段，不是建议的生产调参；已有排队任务不会瞬时重排，`checkAlive()` 还有首次读取后锁存超时的细节，必须单独保留过渡期并检查任期稳定性。

停止后确认本次 5 个 PID 已退出，再做静止 WAL 解析、归档和散列。FLUSH 必须核对各作业完成，不能只看提交命令返回。若出现任期频繁变动、部分实例失联、业务写入混入或采样缺口，应标注该窗口，不得为了得到相同斜率而隐藏失败样本。

### 5.5 能推翻或收窄判断的观察

若生产主要增量来自查询执行内存、block cache 或其他对象，而 WAL 节点很少，空间 WAL 配置的收益就有限。若配置缩小后逻辑缓存已经转入较低平台，但 RSS 基本不变，需要排查分配器驻留、MemTable 与文件缓存，不能立即认定参数未生效。

若 WAL 已标删但长期无法释放，需单独分析读者和回收条件；首期容量配置不处理所有这类问题。若冷空间配置后热点 P99 明显恶化，应检查共享磁盘 I/O 和复制追赶，不能因为热点参数数值未变就排除影响。

## 6 为什么选择这个方案

gflag 在现有系统中本质上是进程级变量。把参数名字改成“空间级”并不会自动改变作用范围。需要让 gflag 承载配置数据，再由新解析层按空间查找不可变策略，最后把得到的值显式传入分片构造链。

| 方案 | 收益 | 主要代价和结论 |
| --- | --- | --- |
| 直接缩小全局 WAL 容量 | 无需新配置模型 | 同时影响热点空间；保留为旧版本受控验证方案，不是本需求目标 |
| 冷热空间分部署 | 隔离共享内存和 I/O | 运维与容量成本较高；空间参数不能消除干扰时仍可采用 |
| 全局默认加空间覆盖，重启生效 | 同实例差异化；构造时语义清楚；易于兼容旧行为 | 需要配置分发和滚动重启；作为首期方案 |
| 在线缩容并立即回收 | 无需重启，响应快 | 要重做并发、回收、限额与失败语义，超出首期 |
| 修改空日志或提交位点机制 | 可能减少增长源头 | 涉及一致性、恢复等更深机制；不与参数能力混在一次发布 |

本次把此前生产方案修订为两个阶段：开发完成前仍按旧版本参数边界操作；开发完成并验收后，全局和热点维持实际基线，仅给指定冷空间覆盖。中频空间及 RocksDB 参数另行开展单变量评估。

## 7 功能需求和配置契约

### 7.1 首期需求编号

| 编号 | 必须交付的行为 | 验收要点 |
| --- | --- | --- |
| FR01 | 默认关闭功能，关闭时沿用原容量路径 | 旧配置可启动，现有行为无新依赖 |
| FR02 | 开启后支持按空间覆盖 WAL 容量 | 同进程两个空间的实际对象容量不同 |
| FR03 | 未覆盖空间使用启动时冻结的全局值 | 运行中新增分片也使用同一份默认值 |
| FR04 | 配置严格校验，开启时错误明确失败 | 不忽略拼写错误，不悄悄回退为默认 |
| FR05 | 配置绑定集群和空间身份 | 错集群、错 ID、名称不匹配不能应用 |
| FR06 | 首期运行期间配置不可变 | HTTP 和 Meta 路径不能改变有效策略 |
| FR07 | 配置进入所有业务分片创建路径 | 启动、迁移、重新建副本结果一致 |
| FR08 | 提供只读实际生效诊断 | 展示实际对象容量与期望策略是否一致 |
| FR09 | 支持灰度、混合版本和回退 | 配置分发顺序明确，回退容量有保障 |
| FR10 | 输出可复核的收益和性能结果 | 内存分项、业务 SLA 和恢复同时通过 |

### 7.2 新增 gflag 和保留参数

拟新增两个 gflag：`enable_space_kernel_options`，布尔型，默认 false；`space_kernel_options`，字符串型，默认 `{}`。后者承载版本化 JSON，必须使用 STRING，不复用现有面向 RocksDB 的 NESTED 配置广播语义。

保留 `wal_buffer_size` 的名称、单位和原默认值 8388608 字节。开启新功能时，把启动流程确定的全局有效值复制到策略表，之后未覆盖空间都继承这份冻结值。不能在每次创建分片时重新读一个可能被其他入口改变的全局变量。

首期覆盖值建议限制在 262144 至 8388608 字节之间，含边界，单位为整数 bytes。这是为了约束发布范围的产品建议，**不是算法证明的安全区间**，需评审确认。全局既有有效值可能大于 8 MiB，不能为了这个覆盖范围把旧全局值静默改小；开启时至少要求全局值为正且可无损放入底层 int32 容量，超出原字段范围必须报错。

### 7.3 配置示例和逐字段解释

以下 ID、名称和 revision 都是假设，仅用于说明格式。当前二进制尚不支持这两个新增 flag，不可直接投放现网。

```json
{
  "format_version": 1,
  "revision": "cold-trial-01",
  "cluster_id": "12345",
  "spaces": {
    "303": {
      "expected_name": "asset_models",
      "wal_buffer_size": 1048576
    }
  }
}
```

| 字段 | 拟定约束 | 目的 |
| --- | --- | --- |
| format_version | 必填整数，首期仅接受 1 | 区分结构版本，不把发布批次当格式版本 |
| revision | 必填非空 ASCII 标识，建议 1 至 64 字符，限字母数字及点横线下划线 | 关联运维发布和审计 |
| cluster_id | 必填规范十进制字符串，范围 1 至 INT64_MAX，无符号和前导零 | 防止跨集群误投及 JSON 工具丢失大整数精度 |
| spaces | 必填对象，可为空 | 空对象表示全部继承冻结的全局值 |
| 空间键 | 无前导零的十进制正整数，须落在 GraphSpaceID 的有效范围 | 明确按 ID 匹配，不按名字自动追随 |
| expected_name | 必填且与 Meta 当前名称精确相同 | 防止认错 ID 或误用旧清单 |
| wal_buffer_size | 必填整数 bytes，按评审确认的覆盖范围校验 | 为该空间所有本地业务副本提供容量 |

开启且需要“全部继承”时，仍要提供带版本、revision、cluster_id 和空 spaces 的完整对象；默认 `{}` 只适用于开关关闭。每个覆盖项首期只接受 `expected_name` 与 `wal_buffer_size`，未支持字段要拒绝，不能为未来兼容而无声忽略。

解析器必须在构建 map 之前发现重复 JSON 键。容量和格式版本拒绝浮点、字符串化数字、布尔值、负数、零、null 和溢出；空间 ID 键与 cluster_id 则严格按约定的十进制字符串解析。cluster_id 使用字符串，是对此前可行性稿中数字示例的接口收紧：真实 ID 可以超过部分 JSON 管理工具的 53 位精确整数范围。首期尚未实现，不需要兼容那份示意输入。建议配置文本上限 1 MiB、覆盖项上限 4096，作为异常输入保护；这两个规模上限同样在评审时确认。

### 7.4 开关和错误的行为矩阵

| 输入状态 | 规定行为 |
| --- | --- |
| 开关关闭，配置为默认空对象 | 完全采用旧路径，不新增 Meta 空间校验要求 |
| 开关关闭，配置非空或格式错误 | 不解析、不应用；启动时只记录一次配置被忽略的提示，诊断标明 disabled |
| 开关开启，格式或字段错误 | 在创建业务 KV/Raft 对象前失败，给出字段路径及原因 |
| 开关开启，空间 ID 不存在或名称不符 | 失败，禁止悄悄继承默认容量 |
| 开关开启，集群 ID 不符 | 失败，禁止启动业务服务 |
| 开关开启，Meta 暂时不可用 | 有界重试，超时失败并保留错误；不能跳过身份检查 |
| 开关开启，合法空 spaces | 冻结全局值，所有业务空间继承 |

关闭时忽略无效文本是为了让关闭开关本身可作为回退手段；开启时严格失败是为了避免运维误以为冷空间已降低容量。新增 flag 的非法布尔字面量仍由 gflags 自身报错，不属于“忽略 JSON”的例外。

### 7.5 需要校验的身份和范围

校验必须使用 Meta 的完整空间清单，而不是仅用本机已有分片。一个空间当前可能不在该机器，随后可能通过迁移或扩容来到这里。不存在的“未来空间 ID”首期不允许预配；先创建空间，取得 ID，再更新下一次启动配置。

正式启动时的集群 ID，应来自成功心跳确认的 `MetaClientOptions::clusterId_`。现有 `FLAGS_cluster_id` 默认可能为 0，不可直接拿它当目标集群身份；也不能随意使用同名但未校验的成员。需提供明确的只读 accessor，并在调用约定中要求已完成身份握手。

策略只应用到普通业务 storage 分片。Meta 使用的 KVStore、管理空间 0、listener 的 Raft 路径等不能仅凭 `spaceId > 0` 或共享基类就自动套用。建议首期只在正常 storaged 模式允许开启；listener 或非目标 KV 模式若显式开启则返回“不支持的运行角色”，关闭时保持原行为。

## 8 内核结构和启动流程

### 8.1 数据结构和依赖方向

新增独立的严格解析器与不可变策略对象，例如 `SpaceKernelOptionsParser`、`SpaceKernelOptionsSnapshot`。名称可在代码评审时按项目规范调整，但依赖边界必须保持：解析器只负责输入校验；策略对象只读；容量选择不在日志追加热路径查 JSON 或访问 Meta。

建议的类型职责如下，属于接口伪代码：

```text
WalBufferOptions
  capacityBytes        构造时固定的 int32
  source               global 或 space_override
  revision             发布标识
  policyHash           规范化后的策略摘要

SpaceKernelOptionsSnapshot
  enabled              固定开关状态
  formatVersion        固定格式版本
  clusterId            已校验的集群身份
  frozenGlobalBytes    启动时冻结的默认值
  overrides            以 GraphSpaceID 为键的只读映射
  resolve(spaceId)      返回类型化 WalBufferOptions
```

策略表每个进程共享一份，分片只保存所需容量和轻量追踪信息，避免每个分片复制整张 JSON。revision/hash 可共享持有，不应为了诊断让 1000 个副本各自存一大份重复字符串。

容量沿明确构造链传递：

```text
storaged 配置输入
  → 严格解析和 Meta 身份校验
  → 冻结 SpaceKernelOptionsSnapshot
  → StorageServer 创建 KVOptions
  → NebulaStore::newPart 按业务 spaceId 解析
  → Part 与 RaftPart 接收类型化构造选项
  → FileBasedWalPolicy 设置容量
  → AtomicLogBuffer 构造并固定 capacity
```

`RaftPart` 是共享基础设施，不能在其内部按 spaceId 自行读取全局映射；调用方必须明确标识本次是业务 storage 还是其他角色。功能关闭时保持旧容量来源和默认路径，不要求其他角色构造一个假空间策略。

### 8.2 正式启动的先后顺序

第一步，解析本地 flags。若功能关闭，走旧启动流程；若开启，先做严格语法、字段、数值和角色检查，并保存本地 wal_buffer_size 作为候选默认。必须在 Meta 首次 loadCfg 之前安装受保护参数的修改限制，防止历史 Meta 条目先把本地值改掉再被误称为本地冻结值。此时不创建业务 RocksDB、RaftPart 或对外业务监听。

第二步，建立 Meta 客户端，完成现有必要的身份握手，取得被确认的 cluster ID，并获取全量空间清单。现有 `waitForMetadReady()` 会涉及心跳、版本/配置注册和后台活动，不能把它宣传成没有外部副作用的预检查。

第三步，验证 cluster ID、所有覆盖空间 ID 和名称，将第一步保存的候选默认与覆盖表发布为不可变快照，生成规范化摘要。失败时输出结构化原因并退出。这里允许已经发生 Meta 启动握手，但必须保证还没有加载业务引擎、开放 Raft 或接收业务请求。

第四步，把策略传入 KVOptions，再创建 KVStore 和业务分片。需要特别注意：当前 `NebulaStore::init()` 会启动 Raft 服务并加载引擎，因此不能把校验放在它的后半段。校验屏障必须位于调用它之前；业务 readiness 必须等实例所有启动所需部分完成。

第五步，每次业务 `newPart` 创建出 WAL 后、注册到 Raft 服务并 start 之前，核验最终对象容量与解析结果一致；失败不允许把该对象投入运行。这个检查同时覆盖启动加载和运行中迁入、新建。启动期间再汇总已构造对象的实际容量，发现不一致则启动失败并退出，不允许仅报警后宣称配置已成功。若某空间当前无本地分片，诊断展示“已校验策略、无本地对象”，不伪造已生效副本数。

### 8.3 只读预检查的真实边界

首期需交付一个复用解析器的离线校验入口，可为独立工具 `nebula-space-options-check`。该名称是拟定交付项，不是当前可执行命令。离线模式只检查文件、结构、数值和规范化摘要；联网只读模式可通过现有只读 Meta 接口核对空间 ID 和名称，不调用 `waitForMetadReady()` 作为捷径。

**现有已核查接口不足以让这个只读工具权威确认 cluster ID。** 因此首期工具输出必须把 `cluster_identity_check` 标为 `not_verified`，不能返回“所有校验均通过”。可接受经过受控流程导出的身份作为人工核对材料，但仍由正式启动握手做权威校验。若团队要求预检查也完成权威集群身份验证，需增加只读 Meta 身份 API，并作为单独评审的范围扩展。

工具成功解析的退出码建议为 0，校验失败为非零；输出须区分 `syntax_valid`、`spaces_valid`、`cluster_identity_check` 和检查模式。独立检查模式始终严格解析映射，不因待发布配置的开关关闭而跳过。不能用一个绿色“成功”掩盖没有执行的校验。预检查不得写 pid、注册配置、写版本、改元数据、创建引擎或开启业务监听，只有指定输出文件和日志可写。

### 8.4 摘要和一致性

`policy_hash` 根据确定的格式版本、集群 ID、冻结全局值和按数字 ID 排序的覆盖项计算；字段顺序、空白、JSON 缩进不应改变它。revision 单独展示，不参与有效策略的语义摘要，避免相同配置只换发布名称就被误判为参数不一致。

同一个集群所有可能承载该空间的 storaged 都要具备同一版策略，包括尚未承载分片的新机器。首期依赖现有运维配置分发，不新增 Meta 中央策略存储，也不自动向其他实例同步本地 gflag。滚动期间不同节点容量不同不直接改变日志协议，但必须可观测、限定持续时间，并在发布结束核对收敛。

## 9 运行中更新和空间生命周期

### 9.1 为什么只写 mutable 属性还不够

当前 HTTP `SetFlagsHandler` 直接调用 `SetCommandLineOption`，不会自动遵守 Meta 配置表中的 MUTABLE 约束，也不保证调用空间对象更新逻辑。MetaClient 的配置刷新又是独立路径。因此仅把新 flag 标为不可变，不能阻止所有入口改动。

新增两项 flag 在进程运行期间，无论功能当前开或关，都禁止通过 HTTP 修改；错误信息明确提示需要修改启动配置并重启。功能开启时，HTTP 对 `wal_buffer_size` 的修改也必须在执行 SetCommandLineOption 之前被拒绝；关闭时保留其原有行为，并继续说明旧对象未必随 flag 字符串改变。

HTTP 请求若同时包含普通可修改字段和受保护字段，必须先检查整份请求，整单拒绝且不写任何字段；建议响应 409 和 `RESTART_REQUIRED`。即使请求的新旧值相同也拒绝，以保持启动参数语义一致。没有受保护字段的原有请求继续使用既有行为。

### 9.2 Meta 配置的处理契约

新 flags 不得注册为可变 Meta 配置。开启时 `wal_buffer_size` 不得继续作为可在线应用的容量源；用户自定义 mutable 列表不能绕过该限制。若现场以前通过 Meta 更新过全局 WAL 值，发布前必须导出实际有效值并迁移到本地基线，避免新进程意外使用安装模板默认值。

对于历史残留或错误下发的 Meta 配置，storage 必须在 SetCommandLineOption 之前拒绝受保护字段，保持原 gflag 和有效策略不变，也不触发对应更新回调；记录 revision、来源、拒绝原因和时间。现有 Meta 配置更新是异步传播；Meta 接受命令后，某个 storage 拒绝应用，不等于原命令能同步返回失败。首期必须如实显示“请求值”和“实际值”的差异，不能把 Meta 更新成功当作所有实例已应用。

若要求所有 Meta 写入口都同步拒绝此类配置，需要额外修改 Meta 侧准入协议，不在当前“本地启动策略”默认范围内。即使有人在未来代码中直接改动全局变量，新策略对象也不会改变；这不免除对现有外部更新入口的明确拦截。

### 9.3 创建 删除 重建和迁移

未配置的新空间使用冻结全局值。已有空间新增副本、故障重建、迁移到新主机时，必须通过相同的业务分片构造入口解析策略；不能只在进程启动加载已有分片时应用。

空间删除时，沿用既有分片销毁流程；对应策略条目可在本次进程内保留为无本地对象的历史配置，不自动关联到其他 ID。同名重建通常会获得新 ID，新空间使用默认值，并在诊断中提示配置里的旧 ID 已不存在。下一次开启功能启动时，陈旧 ID 会导致严格校验失败，所以运维必须同步清理或替换配置。

本地已有分片对象一旦构造，容量保持不变。运行中“期望配置文件已改”不代表对象已改；只有新的进程启动才采用新的整份策略。分片内部的 reset 或角色切换不得重新读取磁盘上的配置文件，也不得偷偷使用新的全局值。

启动存在性校验只是一个时刻的快照，不与之后所有空间 DDL 构成分布式事务。校验后若发生删除，必须沿用已有成员和销毁检查，不得因冻结表仍有条目就复活空间。发布期间建议暂停目标空间的 DROP/重建，但仍需验证相关竞态和诊断读取的安全性。

## 10 可观测性和故障处理

### 10.1 必须能回答的运维问题

运维要能查到：当前功能是否开启；集群身份是否校验；全局冻结值是多少；每个空间来自全局还是覆盖；本机有几个对应副本；这些实际对象的容量是否一致；最近是否拒绝过动态更新。

拟增加只读管理接口 `/space_kernel_options`，沿用现有管理端口的访问控制、网络限制和发布规范，不新增面向业务公网的入口。路径和最终字段名可在接口评审中调整，但下列语义不可省略。该接口是设计稿，当前版本不存在。

```json
{
  "schema_version": 1,
  "instance_id": "<本机实例标识>",
  "binary_version": "<二进制版本>",
  "started_at": "<进程启动时间>",
  "sampled_at": "<本次采样时间>",
  "enabled": true,
  "role": "storage",
  "cluster_id": "12345",
  "cluster_identity_verified": true,
  "revision": "cold-trial-01",
  "policy_hash": "<规范化策略摘要>",
  "frozen_global_wal_buffer_size": 8388608,
  "spaces": [{
    "space_id": 303,
    "space_name": "asset_models",
    "source": "space_override",
    "configured_wal_buffer_size": 1048576,
    "local_partition_count": 20,
    "actual_capacity_distribution": {"1048576": 20},
    "consistent": true
  }],
  "last_rejected_update": null
}
```

`actual_capacity_distribution` 必须来自实际已构造的 WAL/分片对象或与其构造原子登记的只读记录，不能再次调用策略解析器然后假装读取了对象。接口读取需考虑分片删除并发，使用安全快照或受保护引用，不能遍历悬空指针。

无本地对象的配置项明确返回 0 和空分布，状态为 `VALIDATED_NO_LOCAL_PART`，`consistent` 为 null，不能用空集合直接宣称实际一致。响应是本实例的近时快照，不保证与其他实例或并发 DDL 处于同一时刻。分页或空间过滤要保证能导出完整清单。不要默认向每个分片持续打详细日志，1000 分片规模下那会改变被测负载。启动摘要、拒绝事件、空间级计数和按需诊断已经足以支持首期。

### 10.2 需要关联的指标

| 观测对象 | 最少记录项 | 使用方式 |
| --- | --- | --- |
| 空间策略 | revision、hash、来源、实际容量分布 | 判断配置是否真正落到对象 |
| WAL 缓存 | 逻辑字节、活跃/标删 Node、读者、释放计数 | 区分未达容量、正常平台和回收延迟 |
| 复制与磁盘 | 日志读取命中/回退、复制落后量、追赶耗时、快照次数、磁盘时延 | 判断小缓存是否增加不可接受的读取和恢复成本 |
| RocksDB | MemTable、immutable、block cache、flush/compaction | 解释非 WAL 内存和共享 I/O 影响 |
| 进程与容器 | RSS、匿名页、allocator allocated/resident、cgroup 原始统计、limit | 核对物理内存和容器水位口径 |
| 业务 | 分空间或分等级的吞吐、P95/P99、超时、内存错误 | 验证热点和冷空间偶发业务均正常 |

其中 WAL 详细计数和读取命中统计可通过受控诊断构建或低开销计数实现，需单独评估开销。不存在的指标不能在报告里写成已经可直接从原版接口取得。生产长期指标保留周期和基数限制由监控评审确定。

### 10.3 错误分类和恢复动作

本地校验至少区分：格式/重复字段、非法数值、不支持格式、角色不支持、集群不匹配、空间不存在、名称不匹配、Meta 不可达、运行中更新被拒绝、实际容量不一致。可采用本模块状态枚举和结构化日志，不要求把这些都扩充成用户查询的全局 ErrorCode。

错误内容给出字段路径、空间 ID、期望/实际身份、revision 和修正方式；不得只报“invalid config”。启动错误通过进程非零退出和日志报告，动态拒绝通过对应 HTTP 响应或异步应用状态报告。日志打印精简片段，避免整份巨大配置反复刷屏。

配置错误时的恢复方式是修正启动配置或关闭开关，再按副本健康流程重启。不能在失败路径继续用大缓存启动并宣称策略成功，因为这可能让内存保护目标悄悄失效。

## 11 测试和验收设计

### 11.1 三组对照必须分开

A 组是旧版本加原配置；B 组是新版本但功能关闭；C 组是新版本开启且只覆盖目标冷空间。A 对 B 用于发现新代码本身的兼容性和开销；B 对 C 用于评估空间策略。三组保持业务数据、分片、副本、磁盘、容器额度、负载发生器和其他参数一致。

冷空间候选按 8→4→2→1 MiB 逐档测试，不在一次对照中同时修改 RocksDB 或心跳参数。256 KiB 已有组件结果，但尚不足以成为首发推荐值。实际高频空间全局基线可能不是 8 MiB，应使用现场已确认的有效值。

### 11.2 开发自动化验证矩阵

| 编号 | 测试输入或场景 | 必须观察到的结果 |
| --- | --- | --- |
| T01 | 默认关闭、旧 flags；关闭且带坏 JSON | 原路径启动；坏 JSON 未应用且有单次提示 |
| T02 | 开启、空覆盖、两个不同覆盖空间 | 继承正确；三个空间实际对象容量分别符合预期 |
| T03 | 重复键、未知字段、浮点、null、溢出、边界值 | 严格拒绝或按已确认边界接受，无截断无最后值覆盖 |
| T04 | 错 cluster、错 ID、名称不符、Meta 不可达 | 未创建业务引擎/监听前失败；原因准确 |
| T05 | 离线及只读预检查 | 无 Meta 写入、版本注册、Raft 服务和数据目录变更；未校验项明确 |
| T06 | HTTP 改两项新 flag；开启时改 wal_buffer_size；混合多个参数 | 在写全局变量前整单拒绝，实际对象和所有请求字段均不变 |
| T07 | Meta 自定义 mutable 列表、残留更新、反复刷新 | 不能绕过保护；拒绝可查询；冻结值不变 |
| T08 | 新空间、DROP、同名重建、迁移、新机器加副本；校验后创建前删除 | 按 ID 解析；默认和覆盖正确；旧 ID 不误套新对象、不复活已删除空间 |
| T09 | listener、Meta、空间 0、非目标运行模式 | 关闭时旧行为；非法启用明确拒绝，不误用业务策略 |
| T10 | 并发查询诊断与删除分片 | 无悬空访问；实际分布与可用快照一致 |
| T11 | 空日志、真实业务日志、大记录、跨 Node 边界、held 读者 | 日志顺序与内容正确；容量波动有解释；无新的竞态 |
| T12 | 新旧程序滚动、配置回退、二进制回退 | 数据一致、可启动、参数分布符合阶段预期 |

T11 中有些回收延迟是原机制已有的行为，应记录与 A/B 组的差异，不为了让测试“看起来硬限额”而顺手修改算法。所需单测目标可在测试构建中单独开启；生产复现构建仍沿用既有开关和 `make -j5` 上限。

### 11.3 生产等价场景

场景一：50 个空闲空间、20 分片、3 副本，重复基线实验，并观察新配置是否形成更低 WAL 平台。组件加速填充覆盖容量边界；服务层同时保留真实心跳频率的长期样本。不能为方便把 1 秒实验心跳带入生产性能验收。

场景二：高频空间持续读写，中频空间正常负载，低频空间空闲；随后低频空间突发导入和查询。记录热点 P99、吞吐、错误率以及冷空间导入耗时，观察共享磁盘是否成为瓶颈。高频数据规模和工作集必须足以代表用户的 5 亿点边场景，不能拿很小全内存数据集代替。

场景三：50 个空间持续真实业务写入，分别测试所有分片均匀写入和仅部分分片热点。采集每空间/分片实际分布，验证“空间热不等于全部分片热”的影响。与空闲组使用相同内存口径，不把固定 payload 组件实验冒充此场景结果。

场景四：Follower 短时离线和长时离线、重启追赶、Leader 切换、冷副本升主、新机器接收副本和磁盘压力。检查追赶时间、快照次数、日志/数据一致性及业务恢复时间。小容量减少命中后仍应通过既有恢复机制完成，不能只验证稳态读写。

场景五：按正式发布流程改小、改回和关闭开关，覆盖重启中断、配置分发遗漏、错误集群文件、新旧程序共存。检验诊断是否能定位哪台实例、哪个空间没有按计划生效。

### 11.4 通过标准如何确定

正确性与配置行为没有可放宽的百分比：配置解析、身份匹配、实际容量、数据一致性、异常拒绝和回退用例必须全部通过；不能出现新增数据丢失、损坏、持续不可用或错误套用空间。

性能门槛必须以用户已有 SLA 为主。为便于测试准备，可采用以下**待评审的初始门槛**：A/B 对照中热点 P99 与吞吐变化不超过 3%；B/C 对照中热点 P99 退化不超过 5%、吞吐下降不超过 5%；不得新增持续性超时或内存错误。低频空间导入、查询和副本恢复分别使用业务确认的绝对时间上限。相对比例只作为辅助，不能替代绝对 SLA，也不能用均值掩盖尾延迟。

每组建议至少 3 次独立重复，记录波动区间和负载实际完成量；若环境噪声超过门槛，应延长或稳定环境，不能仅挑最好的一次。建议至少 72 小时混合负载稳定性测试，并结合容量填充实验；72 小时不是“默认 8 MiB 空闲缓存已到平台”的保证，原条件下填满可能需要数十天。

内存目标由生产归因决定。首期必须证明被覆盖空间的实际 WAL 平台显著降低，并按同一 cgroup 口径展示稳定期和峰值的绝对字节变化。可把“峰值与现网实际保护阈值之间至少保留 10 个百分点”作为待评审的运行余量目标，但需结合容器 limit、突发查询与回退预算确定。不得直接把 Docker 展示的 80% 当成源代码中所有内存保护的共同阈值。

### 11.5 内存和水位线需要单独验收

现有 MemoryUtils、MemoryTracker、容器统计和 graph/storage 的错误路径有不同口径。上线前必须保留具体错误码、报错组件和时间，关联当时原始 cgroup 指标、进程 RSS 和 tracker 值，确认先后关系。内存保护拒绝请求不等于内核 OOM 杀进程。

不通过提高水位线、关闭 tracker、关闭持久化、减少必要副本或延长心跳来“达成”本需求内存目标。这些动作会改变保护或功能约束，掩盖容量调整的实际效果，且不是本次方案的一部分。

## 12 发布 灰度和回退

### 12.1 发布前必须补齐的生产信息

上线前由业务、运维和测试共同补齐：容器 limit 和 cgroup 版本；各等级空间 ID、名称、分片数、副本数及本机分布；各实例当前有效 WAL 值及配置来源；数据目录数量；实际内存错误和 SLA；commercial 分支与本基线的相关差异；当前复制落后和磁盘余量。

这些缺失不妨碍解析器、构造链、诊断和测试框架开发，但妨碍选择现网参数与批准上线。特别需要先确认大量空闲空间的 WAL 占比：若主要压力来自其他对象，本需求仍有配置价值，却可能不足以解决 80% 痛点。

### 12.2 两阶段灰度

第一阶段只部署新二进制，功能保持关闭，确认 A/B 兼容与开销。新增 flag 不能提前下发给不识别它们的旧二进制。记录所有实例版本、flags、空间分布和基线内存，确保同一副本组能够保持健康仲裁。

第二阶段选择少量确认低频的空间，先以较保守的一档覆盖，生成带 revision 的整份策略，完成离线和只读空间校验。按实际副本布局逐台重启；每台恢复健康并追平后，再操作下一台。不能因为示例是 3 副本就假设生产所有空间都能按同样节奏滚动。

发布过程中持续核对实际容量分布、hash 和空间级性能；完成后检查所有可能承载目标空间的机器都收到相同策略。观察窗口同时覆盖低频导入、峰值读写及一次恢复测试，再扩大冷空间名单或降低下一档。

### 12.3 停止扩大和回退条件

实际容量不一致、热点 SLA 越界、异常快照或追赶时间增长、数据一致性失败、新增不可解释错误、内存收益达不到已批准目标，任一出现即停止扩大。先保留日志、策略、实际分布和同窗口指标，再执行预定回退；不能在一次回退中同时改其他参数导致原因不可区分。

参数回退：把受影响空间改回上一档，更新 revision，按同样的滚动重启生效。功能回退：关闭开关并恢复原全局有效值，重新启动后核验所有对象采用旧路径。二进制回退：在换旧程序前移除旧程序不认识的新增 flag；仅把 enable 设为 false、却保留未知 flag，仍可能导致旧程序启动失败。

回退到较大容量会让缓存随后再次增长，不能把“重启后暂时很低”当作回退容量充足。发布前需为旧平台、查询突发、恢复和滚动过程保留资源，或准备扩容/部署隔离。若只有开启新功能才能勉强不超限，应先解决回退期间的容量安排再上线。

## 13 开发任务拆分和交付物

| 工作包 | 主要负责人角色 | 输出和依赖 |
| --- | --- | --- |
| W1 配置模型 | Storage 基础设施 | 严格解析器、不可变快照、数值及重复键测试；先确认格式与边界 |
| W2 启动和构造链 | Storage 与 Raft | Meta 身份读取、初始化屏障、类型化容量传递、角色隔离；依赖 W1 |
| W3 更新入口保护 | 配置与 MetaClient | HTTP 拦截、Meta 注册/应用保护、拒绝状态；依赖冻结语义 |
| W4 诊断和预检查 | Storage 与运维工具 | 离线/只读校验、实际对象接口、摘要、运行手册；依赖 W1/W2 |
| W5 功能和恢复验证 | 测试与 Raft | T01 至 T12 自动化、故障恢复、混合版本；与 W2/W3 并行准备 |
| W6 性能和容量验证 | 性能测试与业务 | A/B/C、冷热混合、50 空间持续写、长期填充、原始证据包 |
| W7 发布与回退 | 运维与设计责任人 | 空间清单、灰度策略、SLA 签署、回退资源和上线记录 |

开发顺序建议是先合入关闭状态下可独立验证的配置模型，再接构造链和更新保护，最后开放诊断及试点。W2 与 W3 必须一起达到验收状态才能允许启用；否则可能出现部分对象新值、部分对象旧值。

每个工作包交付代码、必要测试、设计差异记录和错误处理说明。测试证据应包含二进制/源码版本、配置、命令、时间、原始采样、分析程序、失败样本及校验清单。正式报告至少按空间分类给出收益和性能，不能只贴一张整机 RSS 曲线。

## 14 评审需要确认的事项

以下是拟定建议，不代表设计责任人已批准。评审可以调整数值，但不能删掉身份校验、冻结语义、真实生效观测和兼容回退这些核心约束。

| 决策项 | 本稿建议 | 最晚确认时间 |
| --- | --- | --- |
| D1 首期参数和生效方式 | 仅业务 storage 的 wal_buffer_size，重启生效 | 开发启动评审 |
| D2 覆盖范围和规模限制 | 256 KiB 至 8 MiB；JSON 1 MiB；4096 项 | 解析器接口冻结前 |
| D3 初次试点值 | 按现场基线逐档，优先少量冷空间；不直接推荐 256 KiB | 生产等价测试后 |
| D4 预检查 cluster 身份 | 首期显式未验证，启动握手权威校验；若要预检查全验证则增 Meta API | 启动设计评审 |
| D5 性能和内存门槛 | 以现有 SLA 为主，采用第 11 节比例作初始讨论值 | 性能测试执行前 |
| D6 非目标运行模式 | 默认关闭兼容，显式启用则拒绝 | 构造链评审 |
| D7 生效接口权限与监控基数 | 复用管理面控制，空间聚合，按需分片明细 | 运维接口评审 |
| D8 后续 RocksDB 空间参数 | 单独需求和单独验收，不搭车首期 | 后续版本立项 |

## 15 后续扩展为什么要另立需求

RocksDB 的部分 CF 参数，例如 `write_buffer_size`、`max_write_buffer_number`、`min_write_buffer_number_to_merge`，在每个空间的引擎对象上有实现空间。当前代码也存在空间 option 更新入口，但现有全局配置广播、异步更新结果、多个引擎部分成功和删除覆盖后的恢复默认，都需要重新定义。

尤其不能把“删除覆盖项”实现为传一个空 map：现有 SetOptions 只修改提供的项，空 map 不会自动恢复继承值。需要明确合成全局基线与空间覆盖、显式重新应用继承值，并报告各引擎实际应用状态。全局更新时也必须重新合成，不能冲掉空间覆盖。

block cache 和 row cache 在目标版本相关实现中是进程静态共享对象。若在第一次初始化时读某个空间的值，可能让第一个空间决定全进程缓存，造成伪空间化。它们需要独立的总预算、空间配额和公平性设计。未来若由 Meta 集中存储策略，还要增加版本、能力协商、期望/实际状态、备份恢复和旧版本兼容，而不是把本地 JSON 简单搬进现有 NESTED 配置。

## 16 最终结论

源码和实验已证明：空闲分片可以持续产生内部日志，同时推动 WAL 节点和 RocksDB MemTable 占用；WAL 容量按逻辑字节计账，小空消息会使实际节点分配明显大于配置值。空间级 WAL 容量能给低频空间更低的平台，并保留热点空间的原容量，因此具备明确的开发价值和可实施路径。

首期采用严格校验、启动冻结、显式构造传递和真实生效诊断。它是否足以解决生产 80% 水位问题，要由生产归因、冷热混合负载及恢复验收共同确认；当前不能承诺任一缩容数值零性能影响，也不能承诺整个容器的硬内存上限。

## 附录 A 源码定位和阅读职责

以下链接固定到分析基线，不跟随分支漂移。行号是定位起点，阅读时保留完整条件和相邻调用，不能只看单行。

| 机制或改造位置 | 固定版本源码 | 阅读重点 |
| --- | --- | --- |
| 心跳追加 | [RaftPart.cpp 2041](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/raftex/RaftPart.cpp#L2041) | 未在复制时追加空 NORMAL 日志 |
| 当前容量来源 | [RaftPart.cpp 330](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/raftex/RaftPart.cpp#L330) | 构造 WAL policy 时读取全局 flag |
| 缓存对象和计账 | [AtomicLogBuffer.h 18](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/wal/AtomicLogBuffer.h#L18) | Record 大小、64 槽 Node、固定 capacity |
| 标删和回收 | [AtomicLogBuffer.cpp 71](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/wal/AtomicLogBuffer.cpp#L71) | push、releaseRef 和 reader 条件 |
| 文件与内存路径 | [FileBasedWal.cpp 457](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/wal/FileBasedWal.cpp#L457) | append、iterator 的磁盘回退、cleanWAL |
| 文件读取 | [WalFileIterator.cpp 30](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/wal/WalFileIterator.cpp#L30) | 打开文件、定位记录和 pread |
| 复制请求取日志 | [Host.cpp 306](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/raftex/Host.cpp#L306) | 日志范围、迭代器与快照判定 |
| 空日志提交 | [Part.cpp 215](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/Part.cpp#L215) | 先取 lastId/lastTerm，后跳空消息，批末更新提交位点 |
| 业务分片创建 | [NebulaStore.cpp 484](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/NebulaStore.cpp#L484) | newPart 作为策略解析的关键入口 |
| 引擎作用域 | [NebulaStore.cpp 354](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/NebulaStore.cpp#L354) | 每空间、每数据目录的引擎创建 |
| Storage 启动 | [StorageServer.cpp 194](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/storage/StorageServer.cpp#L194) | skipConfig、Meta ready 与 KVStore 创建顺序 |
| 集群身份 | [MetaClient.cpp 2671](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/clients/meta/MetaClient.cpp#L2671) | 心跳成功确认 options 中的 clusterId |
| 动态 Meta 配置 | [MetaClient.cpp 3185](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/clients/meta/MetaClient.cpp#L3185) | mutable 检查、SetFlag 与空间广播 |
| HTTP 改 flag | [SetFlagsHandler.cpp 79](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/webservice/SetFlagsHandler.cpp#L79) | 直接写 flag 的独立入口 |
| 共享缓存 | [RocksEngineConfig.cpp 307](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/RocksEngineConfig.cpp#L307) | static cache 不是天然按空间独立 |
| RocksDB option 更新 | [RocksEngine.cpp 489](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/RocksEngine.cpp#L489) | SetOptions 只更新指定项，需处理返回结果 |
| 内存保护口径 | [MemoryUtils.cpp 75](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/common/memory/MemoryUtils.cpp#L75) | cgroup、tracker、purge 与系统指标区别 |

## 附录 B 证据文件和复查入口

| 材料 | 用途 |
| --- | --- |
| [REPORT.md](REPORT.md) | 通俗根因解释与核心服务观测 |
| [EXPERIMENT-DETAILS.md](EXPERIMENT-DETAILS.md) | 原始窗口、命令、失败阶段、完整复现说明 |
| [CODE-WALKTHROUGH-RAFT.md](CODE-WALKTHROUGH-RAFT.md) | Raft 生命周期、心跳、复制、提交、读者 |
| [CODE-WALKTHROUGH-WAL.md](CODE-WALKTHROUGH-WAL.md) | 文件、缓存、标删、回收、恢复、磁盘 TTL |
| [CODE-WALKTHROUGH-ROCKSDB.md](CODE-WALKTHROUGH-ROCKSDB.md) | 提交位点、MemTable、共享缓存、RSS 和保护 |
| [WRITE-VS-IDLE.md](WRITE-VS-IDLE.md) | 空闲与持续 payload 写入的组件对照及边界 |
| [PRODUCTION-PARAMETER-TUNING.md](PRODUCTION-PARAMETER-TUNING.md) | 生产分级调参与水位线归因 |
| [SPACE-LEVEL-KERNEL-PARAMETERS.md](SPACE-LEVEL-KERNEL-PARAMETERS.md) | 本文之前的可行性与方案比较 |
| [production-tuning-evidence/README.md](production-tuning-evidence/README.md) | 五档容量实验、原始 CSV 和独立校验 |
| events.jsonl 与 phase-00 至 phase-09 | 时间线、原始 RSS/RocksDB/HTTP 采样 |
| wal-observe-s1.csv 至 wal-observe-s3.csv | 原服务 Node、空记录和 GC 计数 |
| wal-evidence.tar.gz 与 wal-full.json | 停机 WAL 原件和完整解析 |
| runtime-metrics.json 与 flush-job-verification.json | 实际窗口复算及 50 个 FLUSH 作业完成证据 |
| observation.patch 与 baseline/kvstore/wal | 观测改动和未经改动的组件源码 |

原始日志和二进制主要保存在实验机器本地，历史 GitHub 文档提交不包含整个证据包。新成员仅拉取远端文章后，若缺少文件，必须取得受控证据副本或在隔离环境复跑，不能把缺失材料写成已经核验。旧 `SHA256SUMS` 对应历史交付，不能用它验证当前新文档的字节；本稿有独立的设计交付校验记录。

## 附录 C 需求与证据的对应关系

| 需求原因 | 已有依据 | 对应开发与验收 |
| --- | --- | --- |
| 空闲仍产生长期占用 | 空 WAL 归档、心跳源码、Node 计数 | FR02、T02、空闲长稳态 |
| 容量不等于物理内存 | Record 计账、Node 分配、容量组件实验 | FR08、分项指标和预算 |
| 热点与冷空间需要不同策略 | 用户业务分类；小缓存存在磁盘回退成本 | FR02、冷热混合与恢复 SLA |
| 现有 gflag 是全局来源 | RaftPart 构造和配置入口源码 | W1/W2、FR03/FR07 |
| 在线改 flag 不等于改对象 | 固定 capacity、HTTP/Meta 独立入口 | FR06、T06/T07 |
| 空间身份可能误投或重建 | 集群/空间 ID 的作用域与生命周期 | FR05、T04/T08 |
| 重启后下降不能证明长期有效 | 启动不预热缓存、慢速填充和 allocator 行为 | A/B/C、容量填充与长期采样 |
| 整机内存还有 RocksDB 成分 | 提交位点源码、FLUSH 对照 | 单变量 WAL 验收与后续独立需求 |
