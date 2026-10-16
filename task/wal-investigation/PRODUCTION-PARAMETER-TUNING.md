# 大量空闲图空间：参数调优与图空间级改造方案

分析日期：2026-10-11。目标：`3.6-memory` 分支；产品代码基线 `de9b3ed800a6627d9845e9289b6bbc5b6faf460a`。当前 HEAD `194958131042df77301072eb99dccbc64fb34246` 只增加了此前的分析文档。本次没有修改产品实现，没有启动数据库，没有操作现网。

本文遵循工程的 [可读性与证据规范](../../nebula_graph_readable_analysis_prompts.md)。结论分为源码确认、组件实测、生产待验证三类。

**方案修订：用户现已允许规划新的内核配置机制。** 主方案改为“保留全局默认，对低频图空间覆盖 WAL 容量”，详细设计见 [图空间级参数可行性与实施方案](SPACE-LEVEL-KERNEL-PARAMETERS.md)。该功能尚未实现；本文保留原版本机制和实验依据，并将新机制上线后的处理与旧版本限制分开。本轮没有修改内核代码。

## 一、直接结论

**图空间级 `wal_buffer_size` 改造可行，应作为新的主方案：保留全局和高频空间的原值，只对选定低频空间逐档缩小容量。建议第一版用 gflag JSON 输入空间覆盖，在分片创建时传入容量，滚动重启生效；不同时修改 WAL 格式、复制协议或开发在线缩容。冷热可以继续同进程运行，但共享磁盘仍可能产生性能干扰，必须验证混合负载与恢复。新机制未交付前，现有二进制仍只能统一配置。**

不能仅通过“生产内存达到 80%”断言剩余内存全被 WAL 占用。目前已经证明这种空闲增长机制存在，也复现了容量调小后的缓存平台下降；**尚未拿到现网内存分项、错误原文和热点性能数据，不能保证某个数值既解决容量问题又完全不影响性能。**

## 二、为什么优先考虑 WAL，而不是先压缩查询缓存

WAL 是用于复制、恢复的日志；本文的 WAL 缓存，是每个本地分片副本保存的近期日志对象，和磁盘上的日志文件分开管理。

你的第三类空间虽然几个月没有业务写入，分片仍在运行 Raft。后台活动会产生空日志，复制后也推进 RocksDB 中的提交位点。于是它仍占用两部分内存：每个副本的 WAL 缓存，以及每个空间的 RocksDB 写缓冲（MemTable，保存尚未刷盘的更新）。几千条资产数据并不能代表这两部分内存很小。

已有空闲实验确认：50 个空间、每空间 20 分片、3 副本、3 台 storage，每台有 1000 个本地副本。加速后台活动后，WAL 节点与 MemTable 都增加。对空间做 FLUSH 后，MemTable 和 RSS 下降，WAL 节点仍增加。因此，调小 WAL 缓存只解决其中一项，不能把原实验的全部 RSS 增长都算作 WAL。详见 [调查报告](REPORT.md) 的实验部分。

对第一类 5 亿点边、高频读写空间，RocksDB 的数据块缓存通常服务于真正的查询需求。为了处理冷空间的 WAL，却先大幅砍掉整个进程共享的数据块缓存，可能直接伤害热点查询，且冷空间的 WAL 仍继续积累。所以调参顺序应由内存归因决定。

## 三、收益依据：减小 `wal_buffer_size` 如何降低缓存

### 3.1 调的究竟是什么

该参数的源码默认值是 `8388608` 字节，即 8 MiB。每个新创建的分片副本会把当前值复制到自己的缓存容量中。容量按“日志内容 + 两个 8 字节字段”计账，**不等于实际分配内存，更不是 storage 总内存上限。**

一条空日志只记账 16 B，但缓存还要保存 C++ 对象和链表结构。本机 x86_64、jemalloc 构建中，每个容纳 64 条日志的 Node 分配 3584 B，所以填满空日志时，节点分配约为记账容量的 3.5 倍。这个系数来自本机构建实测，现网编译方式或分配器不同应重新核验。

### 3.2 本次新增容量实验：效果已经验证到哪一步

2026-10-11 使用固定提交的**原始 AtomicLogBuffer 实现**，没有使用现有日志观测补丁。每次写一条空日志，立即创建读者核对日志 ID/内容并释放；分别写到容量对应条数的四倍，验证正常回收时会进入平台。原始输出见 [实验说明与复现](production-tuning-evidence/README.md)。

| `wal_buffer_size` | 空日志填满时每副本的 Node 分配实测 | 若一台有 1000 个同类副本，条件推算 | 首次填满所需空日志/副本 |
|---|---:|---:|---:|
| 8 MiB = 8388608 B | 28 MiB | 27.34 GiB | 524288 |
| 4 MiB = 4194304 B | 14 MiB | 13.67 GiB | 262144 |
| 2 MiB = 2097152 B | 7 MiB | 6.84 GiB | 131072 |
| 1 MiB = 1048576 B | 3.5 MiB | 3.42 GiB | 65536 |
| 256 KiB = 262144 B | 0.875 MiB | 0.85 GiB | 16384 |

表中单副本值取首次恰好填满且没有待回收节点的样本；继续写入时在其附近小幅波动。1000 副本一列是乘法推算，不是本次启动 1000 副本的实测，更不是整个容器 RSS。缓存对象本身、临时批次、读者延迟回收、分配器未归还页面、RocksDB、请求执行和容器其他开销还要另外预算。大日志也可能因 64 条一组的存储粒度显著超过名义容量。

### 3.3 用你的场景举例

**以下是假设性例子，不是现网配置。** 假设第三类空间有 50 个，每空间 20 分片、3 副本，恰好放在 3 台 storage 上。每台都有 1000 个冷副本，不能再乘一次副本数 3。

原来采用 8 MiB，经过足够长时间，空日志缓存节点可达到约 27.34 GiB/台。若新建缓存使用 2 MiB，同样条件下约为 6.84 GiB/台，平台量级相差约 20.5 GiB。**这个差值只有在比较两个充分填充的缓存平台时才成立，不能据此承诺当前生产立即释放 20.5 GiB。**

若每副本平均约 10.25 秒收到一条空日志，从空缓存填到 8、4、2、1 MiB 的时间分别约 62.2、31.1、15.6、7.8 天，256 KiB 约 1.94 天。调小容量主要让它**更早进入较低平台**；填满前，每收到相同数量的空日志，分配增长速度基本相同。因此不能只观察重启后一两天的增长斜率，就宣布调参无效或已经成功。

这正对应“业务停写几个月”的问题：长期平台比启动几小时的内存更值得关注。

### 3.4 为什么不能直接设得很小

分片复制日志、将已提交日志应用到 RocksDB 时都要读取 WAL。如果近期日志还在缓存，直接读内存；如果起始日志已经淘汰，代码会读取磁盘 WAL 文件。调小缓存没有关闭文件日志，也没有修改副本数或提交规则，但会增加走文件读取路径的机会。

文件读取路径会从选中的文件起点扫描记录头，找到所需日志位置，再读取内容。因此，即使操作系统缓存了文件，也可能增加系统调用和 CPU 开销；没有命中操作系统缓存时，还会增加磁盘 I/O。高频写入、批量导入、应用提交滞后、副本落后恢复尤其需要验证。新增 I/O 又可能和查询争用磁盘，间接影响查询延迟。

**假设**某热分片每秒产生 1 MiB 的 WAL 计账数据，8 MiB 大致覆盖最近 8 秒，2 MiB 只覆盖约 2 秒；一次 3 秒滞后就更容易需要读文件。这里的吞吐量必须按日志字节统计，不能把“一条业务 INSERT”或“一个点”等同一条固定大小的 WAL。

缓存未命中**不等于直接发送全量快照**。代码首先可回退读取仍然保留的磁盘日志；需要的日志早于磁盘 WAL 最早记录时，才会走相关快照路径。所以不要同时降低 `wal_ttl`，否则既缩短内存窗口，又缩短磁盘追赶窗口，无法独立判断影响。

### 3.5 修订后的取值顺序

- **新空间机制通过开发验收后：**全局、高频空间、中频空间首先保持实际原值。只选择少量低频空间，如果当前是 8 MiB，依次验证 4、2、1 MiB；具体停在哪一档由该空间的内存预算、冷读写/追赶性能及热点受扰程度共同决定。第一轮不自动把中频空间也改小。
- **更小档位：**256 KiB 只作为进一步实验候选，不因空间级改造可行就直接推广。仍需覆盖偶发批量资产写入、冷读及副本恢复。若同进程资源干扰不能达标，才进一步评估专用部署。
- **仍使用旧二进制时：**不能使用新设计的空间覆盖 flag。原先的全局 8→4→2 MiB 测试或部署隔离仍是可选途径，但必须接受它们各自的影响范围，不能宣称已实现冷空间单独调整。
- **不要把 0 当作关闭缓存的配置使用，也不建议首次就全局设为极小值。**本实现没有“0 自动关闭缓存”的分支，小值也不是严格物理上限。

这些是工程验证起点，不是官方性能保证。新方案中高频空间虽然保持原值，若冷空间的额外文件读取使热业务不达标，也不能继续盲目缩容；应回退该冷空间档位或评估分区数量、资源和部署隔离。

## 四、三类空间如何分别处理

| 空间类别 | 内存治理重点 | 新空间机制交付后的混部建议 |
|---|---|---|
| 高频，约 5 亿点边 | 热数据缓存、写入峰值、复制/提交追赶和 P99 延迟 | 保留原 WAL 容量与 RocksDB 读写缓存，同时验收冷空间变更带来的共享资源干扰 |
| 中频，百万级 | WAL 与 MemTable 都需检查 | 第一轮保持原值；冷空间方案验收后，再按空间独立评估 |
| 低频，几千条，停写数月 | 本地副本数量 × WAL 长期平台；大量空间各自保留的写缓冲 | 首批覆盖目标；逐档验证较小 WAL，长远避免过多分区，必要时进一步隔离部署 |

**旧实现的限制仍需保留：** `wal_buffer_size` 对整个进程使用一个值；RocksDB 的标准配置下发路径也会遍历本地所有空间，不能在 `USE 冷空间` 后执行 `UPDATE CONFIGS` 就认为只修改了冷空间。拟开发的新机制使用显式 spaceId 覆盖，业务分类由经过审核的映射表达，不在首期自动推断冷热。

原先需要部署边界才能实现不同 WAL 容量的建议，现调整为**旧版本备用路径**。新机制交付后，同一进程可以让热空间继承较大默认值、冷空间使用较小覆盖值，无须仅为参数不同迁移图空间。但它不提供 CPU/磁盘或进程 OOM 隔离；如果混合负载测试仍不达标，部署隔离依然有价值。空间迁移、放置策略和故障域仍需另作变更评审。

对于后续新建的几千条模型资产空间，可以评估较少分区。例如从 20 分区变为 1 分区，WAL 副本数量这一项可降到原来的 1/20；但写入并行度、热点集中程度也改变。这里不是要求所有冷空间用 1 分区，且减少分区不会等比例减少每空间的 RocksDB 基础开销。**现有空间不能靠修改默认分区参数在线缩成更少分区，需要迁移方案；不要通过降低副本数换取内存。**

## 五、其他参数：现有语义与后续扩展顺序

新机制第一版只允许 `wal_buffer_size`。下表保留现有参数的作用和代价；RocksDB 写缓冲可作为第二版空间化候选，但共享读缓存、进程水位线、Raft 心跳和持久化开关不随首期一起改。仅增加配置 Map 不会自动改变这些对象的作用范围。

| 参数/操作 | 对本问题的作用 | 建议与代价 |
|---|---|---|
| `rocksdb_column_family_options.write_buffer_size` | 限制单个 RocksDB 写缓冲的规模；不限制 Raft WAL 缓存 | 新方案首期保持原值。第二版空间化交付且 MemTable 证据充分后，再只对选定冷空间验证；原值为 64 MiB 时可从 32 MiB 开始，8/4 MiB 为后续候选。更频繁刷盘会产生更多 SST 文件/合并工作，仍可能影响共享资源上的热写和查询 |
| `max_write_buffer_number` | 控制写缓冲个数，不会消除当前那一个缓冲 | 不作为冷空间第一刀。降低后热写可吸收的突发量减少，刷盘来不及时更易写停顿；本轮保持原值 |
| `rocksdb_block_cache` | RocksDB 数据块缓存，进程内共享 | 先看真实占用与命中率。它不是“每空间一份参数容量”；不按空间数乘，也不为解决 WAL 直接大幅缩小 |
| `memory_purge_enabled` | 在编译支持及监控路径正常时，让 jemalloc 归还可回收页面 | 核对是否保持默认 `true`。不能删除仍存活的 WAL 对象；默认 10 秒的 purge 间隔不是空闲空间回收周期，不建议首先缩到 1 秒 |
| `raft_heartbeat_interval_secs` | 改变后台日志产生节奏，也影响选举判断 | 不是首选。增大主要延缓填满，不改变最终缓存容量，并可能延长故障切换。不要为省内存大幅拉长；源代码默认 5 秒、仓库配置模板 30 秒，先查实际值 |
| `max_log_buffer_size` | 读者释放时判断是否发起垃圾回收 | 不替代 `wal_buffer_size`；它只清理已经标记淘汰的节点，不能把尚有效的冷缓存压成这个值 |
| `wal_ttl`、`wal_file_size` | 磁盘 Raft 日志保留/文件滚动 | 不是 WAL 内存容量开关。缩短保留可能让落后副本更早失去增量追赶所需日志；本轮不改 |
| `clean_wal_interval_secs` | 周期清理磁盘日志；特定配置下还会刷 RocksDB | `rocksdb_disable_wal=true` 时才额外遍历引擎 FLUSH。缩短间隔不清除有效 Raft 缓存，并可能给全库增加刷盘压力；不能当作每空间空闲超时 |
| `SUBMIT JOB FLUSH` | 可以对空间刷 MemTable | 已有实验支持它能降低该部分内存，但不能释放有效 WAL。可作为 MemTable 归因对照或经验证的临时处置；不建议把所有冷空间频繁定时 FLUSH 当长期方案 |
| `rocksdb_disable_wal`、`wal_sync` 等 | 涉及日志与持久化行为 | 保持现有策略，不作为本次节省内存的顺手改动 |
| `disable_page_cache`、过滤器/索引缓存选项 | 改变文件读取与查询的内存/I/O关系 | 没有证据表明它们是空闲 WAL 问题的直接解法。5 亿数据的读性能可能受影响，不与本轮 WAL 灰度混改 |
| 两类内存水位阈值 | 决定何时保护性拒绝请求/分配 | 提高到 0.9 只是延后保护，不释放对象；本轮保持原有保护值，先降低占用 |

**不要把 50 × 64 MiB 当作实际 MemTable 常驻值。** 参数是容量策略，实际占用取决于写入、分配和刷盘。此前实验中每空间刷盘后很快又出现约 1 MiB 的 MemTable 分配，说明不能期待一个空闲空间刷盘后永久只占几 KiB。可核对 [原始实验明细](EXPERIMENT-DETAILS.md) 和 [写入/空闲对照](WRITE-VS-IDLE.md)。

同样，64→32 MiB 也不代表初始 MemTable 分配减半。本机实际依赖 RocksDB 7.5.3 的 `advanced_options.h:477–493` 说明：`arena_block_size=0` 时通常按 `min(write_buffer_size/8, 1 MiB)` 选择分配块，并作对齐。因此 64、32、8 MiB 对应的默认分配块仍可能都是 1 MiB；到 4 MiB 才可能变成约 512 KiB。这里只核验了该依赖头文件，未将现网依赖版本或旧 MemTable 的即时行为当作已验证事实。先调 `arena_block_size` 本身也不能限制长期累计量，不列为主方案。

## 六、先确认“80%”究竟对应哪一道保护

### 6.1 三个数字不能直接互换

- **进程 RSS：**该进程实际驻留物理内存的页面，包含对象、分配器保留页面等。
- **容器内存：**由 cgroup 记账，可能包括文件缓存及容器内其他进程。Docker CLI 展示值还会扣掉一部分文件缓存；不能直接等同 `memory.current`。见 [Docker 官方统计说明](https://docs.docker.com/reference/cli/docker/container/stats/)。
- **MemoryTracker 使用量：**本实现追踪的 `new/delete` 分配计数，不等同前两者。

`memory_tracker_limit_ratio=0.8` 的静态限额为：

```text
Tracker 限额 = 0.8 ×（读取到的总内存 − untracked_reserved_memory）
```

默认未追踪内存保留值为 50 MiB，不是对所有生产 workload 都足够的安全保证。当前任务不应把这个值调小来挤出更多额度。

`containerized=true` 时才走配置指定的 cgroup 路径；否则读取 `/proc/meminfo`。必须在容器内部核对路径对应的究竟是该容器还是其他层级，以及读到的限额是否为真实 Docker limit。读取异常、`memory.max` 为 `max`、父级限制更小等情况下，不能继续拿理想公式推算；也不要伪造一份限额文件去绕过实际约束。

还有一个目标版本细节：这份代码从 `memory.stat` 查找 `total_cache` 和 `total_inactive_file` 并相加；它与 Docker CLI 的扣减方式不同。标准 cgroup v2 使用 `file/inactive_file` 等字段，并不匹配这里的 `total_` 正则，因此该路径通常不会扣除文件缓存。这是**源码确认的统计差异**，不是已经确认的现网根因。原始 cgroup 文件与日志 `sys/ usr` 必须一起看。参见 [Linux cgroup v2 文档](https://docs.kernel.org/admin-guide/cgroup-v2.html#memory-interface-files)。

### 6.2 按错误定位服务

| 实际证据 | 先定位哪里 | 不能直接推导什么 |
|---|---|---|
| `Used memory hits the high watermark(...) of total system memory` | 目标源码中的 Graph 查询执行/迭代器检查；核对 graphd 的内存、容器和配置 | 不能只凭这句话断言改 storaged 的 `system_memory_high_watermark_ratio` 就能解除错误 |
| `E_STORAGE_MEMORY_EXCEEDED` / -3600、Storage `bad_alloc` | Storage 请求分配失败及 MemoryTracker 的 `usr` 使用量/限额 | 不能仅凭错误码确定一定是 Tracker，真实分配失败也需排查；更不能等同已经 OOM kill |
| 容器退出、`OOMKilled` 或 cgroup `oom_kill` 增量 | 容器/宿主机内存限制与 OOM 事件 | 与请求被保护性拒绝是不同事件，应按时间线区分 |

不建议将 `memory_tracker_limit_ratio` 改为特殊值 3 来关闭保护，也不建议用动态模式 2 作为本问题的默认解法。后者依赖可用内存的统计，必须先解决上述口径确认。`system_memory_high_watermark_ratio >= 1` 在这份实现中还会提前返回，跳过本轮 Tracker 更新、purge 和诊断日志，不能当成无副作用的解除告警方式。

## 七、源码证据：参数为什么有效，又为什么有边界

以下均为目标固定提交；省略代码用注释标记。本文没有借其他版本实现补齐结论。

### 7.1 容量在创建分片时固定

[RaftPart.cpp:357](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/raftex/RaftPart.cpp#L357)、[FileBasedWal.cpp:59](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/wal/FileBasedWal.cpp#L59)、[AtomicLogBuffer.h:327](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/wal/AtomicLogBuffer.h#L327)：

```cpp
// RaftPart 构造：省略其他 policy 字段
policy.bufferSize = FLAGS_wal_buffer_size;
// FileBasedWal 构造：
logBuffer_ = AtomicLogBuffer::instance(policy_.bufferSize);
// AtomicLogBuffer 构造：
explicit AtomicLogBuffer(int32_t capacity) : capacity_(capacity) {}
```

证明：改变后续创建对象的容量有直接代码路径，但改 gflag 变量不会重设已经存在的缓存。生产存量分片应通过持久配置与受控滚动重启生效。**HTTP `/flags` 读到新值只证明变量改变，单独不能证明旧分片已改变。**

### 7.2 超容量先淘汰旧节点，读者退出后才能释放

[AtomicLogBuffer.h:32](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/wal/AtomicLogBuffer.h#L32)：

```cpp
int32_t size() const {
  return sizeof(ClusterID) + sizeof(TermID) + msg_.size();
}
```

[AtomicLogBuffer.cpp:91](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/wal/AtomicLogBuffer.cpp#L91) 的核心条件是 `size_ + recSize > capacity_` 且 `tail != head`，随后标记旧节点、移动尾指针、减少有效计账、增加待回收节点数。[releaseRef:167](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/wal/AtomicLogBuffer.cpp#L167) 先检查是否最后一个读者，再判断回收条件：

```cpp
if (readers > 1) {
  return;
}
// ...
if (dirtyNodes > dirtyNodesLimit_ || size_ > FLAGS_max_log_buffer_size) {
  if (gcOnGoing_.compare_exchange_strong(gcRunning, true, std::memory_order_acquire)) {
    // ... 从 tail->next_ 开始删除已标记节点
  }
}
```

证明：减小 `capacity_` 会更早淘汰旧的有效日志；`max_log_buffer_size` 不能替代这一步。不能证明：任何时刻占用都小于某个物理上限。新 Node 分支会提前返回、每 Node 固定 64 条、单次批量写入与读者延迟回收，都可能使峰值偏离表格的平台模型。

### 7.3 未命中仍有文件读取路径，性能需要实测

[FileBasedWal.cpp:530](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/wal/FileBasedWal.cpp#L530)：

```cpp
auto iter = logBuffer_->iterator(firstLogId, lastLogId);
if (iter->valid()) {
  return iter;
}
return std::make_unique<WalFileIterator>(shared_from_this(), firstLogId, lastLogId);
```

这里证明缓存缺失不等同日志丢失。写日志时先写文件再入缓存，见 [appendLogInternal:457](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/wal/FileBasedWal.cpp#L457)；文件读取的打开、扫描和 `pread` 见 [WalFileIterator.cpp:30](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/wal/WalFileIterator.cpp#L30)。复制 RPC 构建见 [Host.cpp:323](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/raftex/Host.cpp#L323)，Leader/Follower 提交读取见 [RaftPart.cpp:1069](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/raftex/RaftPart.cpp#L1069)、[1794](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/raftex/RaftPart.cpp#L1794)。这些代码不能证明特定生产磁盘和峰值负载下没有延迟回退。

### 7.4 配置入口不能误认成空间隔离

[MetaClient.cpp:3214](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/clients/meta/MetaClient.cpp#L3214)：

```cpp
// ... 构造 optionMap
for (const auto& spaceEntry : metadata.localCache_) {
  listener_->onSpaceOptionUpdated(spaceEntry.first, optionMap);
}
```

虽然内部函数带 `spaceId`，配置下发方会遍历所有空间。`write_buffer_size/max_write_buffer_number` 位于 [PartManager.cpp:103](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/PartManager.cpp#L103) 支持集合中，经现有引擎 `SetOptions` 应用；但 `local_config=true`（Storage 默认）会跳过 Meta 配置拉取，见 [StorageServer.cpp:194](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/storage/StorageServer.cpp#L194)、[MetaClient.cpp:3147](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/clients/meta/MetaClient.cpp#L3147)。

所以不能保证 `UPDATE CONFIGS` 在现网一定生效，更不能为了调一个参数贸然改变整套 `local_config` 策略。HTTP PUT `/flags` 也只修改变量，不主动更新现存 RocksDB 引擎。RocksDB 配置调整需保留现有其他键，并核验引擎实际 `OPTIONS-*`、应用日志与行为。异步更新并非所有失败都会回传给最初发起配置的客户端。

3.6 [官方 Storage 配置文档](https://docs.nebula-graph.io/3.6.0/5.configurations-and-logs/1.configurations/4.storage-config/) 表格将 RocksDB 配置整体列为非动态；这里指出的是目标源码中有条件的动态更新路径，**不是建议绕过发布流程依赖未验证的热更新**。本次 WAL 方案按滚动重启设计。

### 7.5 其他结论的代码入口

| 结论 | 固定版本源码 |
|---|---|
| block cache 是进程内静态共享对象；写缓冲选项从统一 flag 初始化 | [RocksEngineConfig.cpp:258](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/RocksEngineConfig.cpp#L258)，同文件 307–318 行 |
| 心跳参数还参与选举超时 | [RaftPart.cpp:1143](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/raftex/RaftPart.cpp#L1143)，轮询间隔见 1411 行 |
| TTL 清理的是磁盘文件 | [FileBasedWal.cpp:640](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/wal/FileBasedWal.cpp#L640) |
| 定时清理仅在禁用 RocksDB WAL 时额外刷引擎 | [NebulaStore.cpp:1293](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/NebulaStore.cpp#L1293) |
| cgroup 读取、限额、purge、日志与提前返回 | [MemoryUtils.cpp:75](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/common/memory/MemoryUtils.cpp#L75)，相关实现至 237 行 |
| 字面系统水位线错误在 Graph；Storage 捕获分配失败 | [Executor.cpp:625](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/graph/executor/Executor.cpp#L625)、[BaseProcessor.h:175](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/storage/BaseProcessor.h#L175) |

## 八、生产验证与回退：每一步解决一个不确定性

### 8.1 先做只读归因

采集三类空间在每个 storage 的本地副本数，区分 Leader 与全部副本；记录容器 limit、运行时参数、进程启动时间、内存分项、原始错误。热点服务还需有同一时段的读写吞吐、P95/P99 延迟、错误率、磁盘延迟、复制/提交落后及刷盘/合并积压。至少覆盖代表性的高峰与低峰；不要只用空载基线。

源码现有指标包括 `commit_log_latency_us`、`replicate_log_latency_us`、`append_log_latency_us`、`append_wal_latency_us`，以及 `num_start_elect`、`num_send_snapshot`；注册见 [KVStats.cpp:23](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/stats/KVStats.cpp#L23)。它们可协助发现提交/复制变慢或恢复方式变化，但通常是汇总值，不能代替按热点空间衡量的业务延迟，也不是 WAL 缓存命中率。

MemTable 应按空间和数据路径聚合；共享 block cache 不能按空间重复累加。若冷空间多、空日志持续、WAL 节点随日志数增长，支持本文方向；若增长主要是 MemTable、SST 读取器、文件缓存或请求队列，就应调整优先级。当前原生接口没有本次新增的 `WAL_MEMORY_OBSERVE` 观测日志，不应把实验补丁的指标当作现网自带指标；不能用“RSS 减去几项”当精确 WAL 计数。

### 8.2 再算容量是否有解

把冷副本的 WAL 预算单独列出来，其余常驻内存、热分片 WAL、查询/写入峰值、恢复期间开销和分配器余量另算。对本机空日志布局可先用：

```text
冷 WAL 节点平台近似值 = 本地冷副本数 N × wal_buffer_size × 3.5
可给冷 WAL 的预算 = 运维设定的目标总内存 − 其余内存预算
候选 wal_buffer_size ≤ 可给冷 WAL 的预算 / (3.5 × N)
```

该公式适用于冷副本采用相同容量的情况。新机制允许容量不同后，改为逐空间求和：`Σ(该空间本地副本数 × 该空间容量 × 3.5)`。热点空间应按真实 payload 和峰值单独估计，不能照搬空日志系数。

这是容量筛选公式，不是严格上限。若目标只是不碰 80%，却让日常基线贴着 79%，一次查询/写入突发仍会触发保护。目标值应根据现网峰值和恢复余量设定，不能把某个固定百分比称为普适安全值。

**假设例子：**容器 32 GiB，运维选择日常目标 70%，其余内存预算已占 18 GiB，1000 个冷副本只剩 4.4 GiB 预算，则冷容量约需不大于 1.29 MiB。此时 2 MiB 不能满足这一容量模型；新空间机制允许仅把冷空间的 1 MiB 列入验证，高频空间继续保留原值。如果冷读写/恢复或共享磁盘干扰仍不达标，再选择隔离或资源/分区调整。70% 和 18 GiB 都是假设，不能套用到现网。

### 8.3 在等价测试环境比较参数

只改一个参数，比较基线与候选值。必须包括：真实高频查询和写入、最大资产导入批次、业务高峰叠加后台刷盘、Follower 短时落后后的追赶、单节点滚动重启、Leader 切换后继续读写。检查已确认写入的数据能正常读取，检查业务结果与基线一致，并核对恢复用时和错误率。

新机制还必须增加“旧版本原配置 / 新版本开关关闭 / 新版本仅冷空间覆盖”三组对照，以及空间映射、角色限定、参数校验、HTTP 运行中拒绝、新增/删除/迁移分片和配置漂移验证。完整验收项见 [新设计第七节](SPACE-LEVEL-KERNEL-PARAMETERS.md#七验证发布与回退)。

压力与故障注入在测试环境执行。本次组件实验只证明空日志缓存平台降低；**没有执行上述生产等价性能/恢复测试**。如果新参数让热点 P99 超过现有 SLA、持续复制落后、增加写停顿或追赶超时，即否定该候选档位。

长期验证要覆盖冷缓存填充过程。相同重启时长、相同日志条数、相近业务负载的对照，比“变更前高、重启后低”更可信。可以在测试环境加速空日志输入，但不能把改变心跳后的整机吞吐直接当作原心跳下的性能结果。

### 8.4 修订后的灰度与回退

以下新机制流程仅在开发和测试通过后适用，目前未实施。

1. 备份持久配置、启动参数和实际值，保持心跳、磁盘 WAL 保留、水位线和 RocksDB 选项不变。先部署新功能关闭的新版本，验证旧路径兼容；不提前把新 flag 下发给不认识它的旧二进制。
2. 全局 `wal_buffer_size` 保持原值。只为少量已确认冷空间添加覆盖，并先做格式、集群身份、spaceId/名称、容量和配置一致性预检查；第一轮候选可从原 8 MiB 到冷空间 4 MiB。中高频空间不随之缩小。
3. 核对实际副本数和所有相关分片健康/追赶状态。三副本健康集群每次只变更一个 storage，另外两个相关副本必须可服务；一副本、已有故障、剩余节点无足够容量时不能套用。
4. 低峰滚动重启，每台恢复并追平后再进入下一台。遵守 `ulimit -n 65536`。虽然只覆盖冷空间参数，整个 storage 重启仍可能影响热业务 Leader、缓存和重试。
5. 同时核对配置版本/hash和实际对象的分空间容量；新设计要求新增这种诊断能力，现有 `/flags` 单独不够。监测混合负载 SLA、内存、文件 I/O 与恢复，覆盖 Leader/Follower 角色。冷空间 4 MiB 通过后，再决定是否试 2、1 MiB。
6. 若出现 SLA 越界、错误率上升、持续复制落后、写停顿或异常数据结果，停止扩展。参数回退可只恢复相关冷空间的上一档；功能回退则关闭空间机制，按健康条件逐台恢复。回退旧二进制前移除它不支持的新 flag。只恢复文件而不重建缓存，不会还原存量容量；恢复较大默认值后的长期回填内存必须预留。

如果最终选择继续用旧二进制，上述“空间覆盖”步骤不能执行，应按原全局参数方式单变量灰度。原生参数的构造时生效限制和副本健康要求仍然相同。

验收标准以现有 SLA 和同等负载基线为准。不要为了让调参“通过”而同步放宽超时、心跳或水位阈值。若当前已频繁拒绝请求，先评估健康副本和资源余量；不要在无余量状态下盲目开启全量重启或压力测试。

## 附录：建议的只读采证入口

以下是**待在现网按实际地址执行的示例**，本轮没有访问现网。执行位置为容器内或能够访问对应 storage HTTP 管理端口的运维环境；`19779` 仅为默认示例，替换为实际端口，空间名用真实名称。结果按时间保存到变更记录目录。

```bash
# 运行时参数：GET，无修改；不要把 GET 改成 PUT。
curl --fail --silent --show-error --get 'http://127.0.0.1:19779/flags' \
  --data-urlencode 'format=json' \
  --data-urlencode 'flags=wal_buffer_size,max_log_buffer_size,raft_heartbeat_interval_secs,wal_ttl,rocksdb_column_family_options,rocksdb_block_cache,rocksdb_disable_wal,local_config,containerized,system_memory_high_watermark_ratio,memory_tracker_limit_ratio,memory_tracker_untracked_reserved_memory_mb,memory_purge_enabled,cgroup_v2_controllers,cgroup_v1_memory_stat_path,cgroup_v1_memory_max_path,cgroup_v1_memory_current_path,cgroup_v2_memory_stat_path,cgroup_v2_memory_max_path,cgroup_v2_memory_current_path'

# 分空间 RocksDB 属性；大规模空间分批低频采样，避免并发扫全部空间。
curl --fail --silent --show-error --get 'http://127.0.0.1:19779/rocksdb_property' \
  --data-urlencode 'space=实际空间名' \
  --data-urlencode 'property=rocksdb.cur-size-all-mem-tables,rocksdb.size-all-mem-tables,rocksdb.num-immutable-mem-table,rocksdb.estimate-table-readers-mem,rocksdb.block-cache-usage,rocksdb.is-write-stopped'
```

某项属性不支持时分别查询并保留错误，不要填成 0。`GET /flags` 与属性接口实现分别见 [GetFlagsHandler.cpp:24](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/webservice/GetFlagsHandler.cpp#L24)、[StorageHttpPropertyHandler.cpp:23](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/storage/http/StorageHttpPropertyHandler.cpp#L23)。

在每个相关服务容器内读取实际 cgroup 文件及进程 `/proc/<pid>/status`、`smaps_rollup`；cgroup v2 另外保留 `memory.events` 的前后增量，v1 保留 `memory.failcnt` 与 OOM/容器事件。这些文件路径要结合 `/proc/self/cgroup`、挂载点与有效 flags 确认，不假定宿主机和容器路径一致。`oom_kill` 字段的含义以 [Linux 官方文档](https://docs.kernel.org/admin-guide/cgroup-v2.html#memory-interface-files) 为准。

可以在已验证的配置发布方式下临时开启 `memory_tracker_detail_log=true`、保留默认 60000 ms 间隔，采集 `sys:使用量/总量` 与 `usr:追踪量/限额`，窗口结束后恢复原值。它要求编译启用 Tracker、INFO 日志可见且监控路径正常；没有日志不等于使用量为零。不要为了采证全局打开高等级逐条 WAL 日志。

## 最后结论

允许开发新机制后，主方案调整为：保留全局默认，给低频空间单独设置较小的 `wal_buffer_size`，首期构造时生效并滚动发布。已有组件实验支持容量下降的收益，但空间映射正确性、冷热混合负载和恢复性能仍须在实现后验收；提高内存水位线依然不能替代内存治理。
