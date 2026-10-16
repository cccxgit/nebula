# 50 个图空间持续写入，与保持空闲时的内存有什么不同

分析对象：`3.6-memory` 分支，产品代码基线 `de9b3ed800a6627d9845e9289b6bbc5b6faf460a`。当前文档提交 `194958131042df77301072eb99dccbc64fb34246` 没有改变这些产品实现。本机 `AtomicLogBuffer.cpp` 另有观测补丁，因此涉及原始淘汰实现时引用固定保存的基线文件。

本文遵循工程根目录的 [可读性与证据规范](../../nebula_graph_readable_analysis_prompts.md)。沿用前一场景：3 个 storaged、50 个空间、每空间 20 个分片、每分片 3 个副本，每实例一个数据目录。本文讨论 storaged 内存；没有把 graphd 的请求内存计入其中。

## 一、直接结论

如果 50 个空间中的分片持续收到足够多的业务写入，Raft 日志缓存通常会更快填满，并开始反复替换旧记录；RocksDB 也更快经历“积累内存数据、刷盘、释放旧内存”的循环。空闲场景的记录很小，反而可能长时间填不满缓存，表现为每天上涨、一直看不到平台。但这不等于持续写入时进程内存一定更低：业务内容、待刷盘数据、请求及复制中的临时副本、磁盘文件的读取管理信息都会占内存。现有完整服务实验只测过空闲场景，不能给出持续写入时每台 RSS 必然为多少 GiB 的结论。

先把最容易混淆的差别放在一起。下面的“通常”均要求写入分布、消息大小相对稳定，复制及刷盘能跟上，缓存读者能够正常释放。

| 比较项 | 50 个空间保持空闲 | 50 个空间持续有新数据写入 |
|---|---|---|
| Raft 日志是否新增 | 仍新增空日志，来自空闲心跳追加路径 | 新增业务日志；复制间隙也可能新增空日志 |
| 单条日志容量计账 | 空 payload，每条 16B | 每条为 `16B + 编码后 payload 长度` |
| 到达缓存淘汰阶段 | 很慢；模板心跳节奏下，纯空日志约需 62 天填满 | 取决于每分片字节速率；较大消息、较高写速通常更快到达 |
| WAL 缓存的长期形态 | 先长时间上涨，健康条件下最终也会进入平台 | 通常更早进入“追加新记录、淘汰旧记录”的阶段 |
| 相同 8MiB 逻辑容量的实际内存 | 需要容纳很多小记录，固定对象开销占比很高 | 较大消息用较少记录即可填满；还要分配消息字符串 |
| RocksDB 内存 | 主要写系统提交位置，积累慢，也可能长期上涨 | 业务键值与提交位置一起写，通常更频繁刷盘；多空间合计可形成较高的平台或波动 |
| 其他进程内存 | 请求、复制中的业务内容较少 | 请求、复制、刷盘及文件管理等开销通常增加 |
| 总 RSS 谁更低 | 不能只由“空闲”二字判断 | 不能只由“触发淘汰更频繁”判断 |

## 二、只需先理解这两个存放位置

一次业务写入会经过两个不同的内存存放位置，不能把它们当成同一个缓存。

**第一个是 Raft 日志缓存。** 它保存最近写入的日志，方便分片复制或提交时读取。每个本地分片副本有一份：这里每台存着 `50 × 20 = 1000` 个本地副本，因此每台有 1000 份这样的缓存。Follower 也有缓存，不能只按本机 Leader 数量计算。

**第二个是 RocksDB 的 MemTable。** 它保存已经应用到数据库、暂时还没有形成磁盘数据文件的键值记录。这个场景中，同一空间的 20 个本地分片共用一个 RocksDB 实例；每台有 50 个目标空间实例。MemTable 主要按这些实例累计，而不是按 1000 个分片各配一份。

MemTable 写到需要刷盘时，旧表停止接收新写入，称为 immutable MemTable；后台把它写成 SST，即磁盘上的排序数据文件。新写入可以进入新 MemTable。旧表写完且不再被引用之后，才能释放它的内存。

本文的三个内存数字含义不同：

| 名称 | 实际在量什么 | 不能用来替代什么 |
|---|---|---|
| WAL 逻辑字节 | 缓存为判断是否淘汰而计算的 `16 + payload` | 不是分配器实际分配，更不是 RSS |
| WAL 对象/字符串分配字节 | 缓存中的 Node 和消息字符串占用的堆分配 | 不包含所有线程、数据库、网络对象与分配器内部空间 |
| 进程 RSS | 操作系统统计的当前驻留物理页，本次主报告取 `/proc/<pid>/status` 的 `VmRSS` | 不能直接当作仍存活对象的精确总和 |

后文引用的 RocksDB MemTable 属性也是近似分配统计。释放对象以后，分配器可能先保留内存供下次复用，所以对象减少与 RSS 下降不必同时发生。操作系统文件页缓存则是另一个统计对象，不能把主机或容器的文件缓存增长直接记作 storaged RSS 增长。

## 三、为什么持续写入后的曲线会不同

### 3.1 空闲时：每条日志很小，缓存要存很多条才认为自己“满了”

空闲 Leader 仍周期性追加一条没有业务内容的日志，Follower 也复制这条日志。缓存对它只记 16B，因此单分片默认 8MiB 可以计入约 524,288 条空记录。

可是真正存储记录的对象不只有这 16B。源码把每 64 个记录槽装在一个 Node 中。本机构建实测每个 Node 对象为 3200B，jemalloc 实际分配档位为 3584B。即使没有业务内容，也要保存这些槽位、字符串对象以及链表和状态字段。满 Node 平均就是 `3584 / 64 = 56B/条`。

于是 8MiB 的逻辑容量，纯空记录需要约 28MiB 的 Node 分配。缓存尚未到容量时，这些都是“仍保留的最近日志”，没有理由进入代码中的淘汰分支。前一场景的长期上涨主要就在这个填充阶段。

### 3.2 持续写入时：业务内容也算容量，缓存更早开始替换旧日志

业务记录的 payload 是编码后的操作内容，里面包含操作类型、键、值等。假设一条这样的内容长 1024B，那么缓存对它记 `16 + 1024 = 1040B`。同样的 8MiB，大约 8066 条就能填满，已经不是 52 万条。

这并不是“写入操作会清空 WAL”。真正发生的是：持续写入让逻辑大小越过阈值，代码把最老的一组记录标记为可淘汰；新的记录继续加入，读者离开之后再释放被淘汰的对象。新写入提供了到达阈值的速度，释放动作仍由原有淘汰和回收条件控制。

较大的 payload 需要单独分配字符串内存，所以不能只看到 Node 少就认为缓存几乎不占内存。区别在于：纯空日志大量消耗固定记录槽；大消息的主要成本转为消息内容，固定槽成本在总量中的比例更小。在消息大小适中且稳定时，大消息组成的满缓存，实际内存可以低于纯空记录组成的满缓存。

这里至少有四个成立条件：写速足以填满缓存；关注的分片确实收到这些写入；缓存读者有机会全部释放；消息没有大到单个 Node 就明显超过容量。源码并没有提供严格的“每缓存实际内存绝不超过 8MiB”承诺。

### 3.3 RocksDB 更频繁释放旧 MemTable，但不保证总量更小

空闲时，分片主要反复覆盖自己的“已经提交到第几条日志”这个系统键。虽然键的数量基本不变，写入仍会进入 MemTable，旧版本不会简单地在同一块 16B 内存上被原地替换。

有业务写入后，还会加入真实业务键值。MemTable 的填充速度通常变快，达到刷盘条件也更快。刷盘完成后旧 MemTable 能退出，所以经常可以看到上涨与回落交替；50 个空间的周期错开后，进程整体也可能看起来像一个较平稳的平台，而不是整齐的锯齿。

但是，50 个空间都活跃，意味着 50 个数据库可能同时持有较大的 active MemTable，部分还在刷上一张旧表。单个空间更频繁地释放，并不能推出 50 个空间的总量更低。

本次配置的 `write_buffer_size=64MiB` 是每个 column family 的配置；当前场景使用普通默认列族，可以按每个目标 DB 一个来理解。`50 × 64MiB = 3.125GiB` 只是“每个 DB 各有一份接近该大小缓冲”的量级。`max_write_buffer_number=4` 允许多份写缓冲并存，粗乘得到 `12.5GiB`，也不能当作整个进程的严格上限、恒定值或必然峰值。

“一直有新数据”还意味着磁盘 SST 的数量和内容持续演化。RocksDB 会在后台合并磁盘文件，称为 compaction；刷盘、合并以及打开 SST 所需的读取管理信息，会带来额外内存。前次空空间实验中 table-reader memory 为 0，不能据此推断持续写入后也为 0，更不能假定整个进程最终永远保持完全水平。

块缓存用于复用读取过的数据块。当前 Nebula 初始化代码把它设成进程内多个 RocksDB 实例共享的对象，所以前次实验的 4MiB 配额不能再乘 50 次。纯追加写也不保证一定把块缓存填满；是否读旧数据、是否有索引和存在性检查、索引及过滤块如何缓存，都影响它的实际占用。

### 3.4 “每个空间都在写”还不够，要看空间内部的分片

假设每个空间的 20 个分片中只有 1 个持续收到业务，其他 19 个几乎没有业务。每台此时是 50 个热分片加 950 个冷分片。热分片的 WAL 很快开始替换；冷分片仍然按空日志缓慢填充。因此完全可能出现“50 个空间都有写入，但进程仍每天上涨”的现象。

同一个空间的 20 个分片共享 RocksDB，热分片可以促使这个空间的 MemTable 刷盘，连同冷分片的系统提交记录一起落盘。可是它不能顺带淘汰那 19 个冷分片各自独立的 Raft 日志缓存。这是两种内存的管理单位不同带来的直接差别。

### 3.5 从持续写入转为空闲后，WAL 内存也可能继续上涨

停止业务写入时，缓存不会立即删除已经保留的业务日志。之后空心跳继续加入空记录，超过容量时逐步移除最老的业务 Node。此后缓存中的业务 payload 越来越少，空记录越来越多。

若旧业务 payload 为 1024B，一条业务日志计账 1040B，相当于 65 条空日志的 16B 计账。释放同样多的逻辑容量后，可以容纳更多记录槽。极端到最后全是空日志时，Node 数量会从约 127 个靠近 8192 个。即使业务内容逐渐释放，固定槽分配仍可能增加更多。

所以“停写之后，缓存里的业务字节在减少，进程内存却继续涨”在这套实现中有可能成立；需要同时看 Node、payload、逻辑大小和 RSS。它不自动证明有对象泄漏，也不说明每个停写现场都会如此。是否已经填满、停写前消息大小、淘汰和读者状态、分配器是否复用旧页都会影响曲线。

## 四、一个完整的数字例子

### 4.1 先用假设把量级算清楚

以下是教学假设，不是业务压测结果：每个逻辑分片每秒写入 10 条 Raft 日志，每条编码后的 payload 恰为 1024B；1000 个分片均匀写入，正常复制和回收，先忽略间隙中的少量空心跳。

写入开始前，每个本地副本的缓存近似为空。开始以后，每秒增加 `10 × 1040 = 10400B` 逻辑计账，约 `8388608 / 10400 = 806.6秒`，即 13.4 分钟接近 8MiB，然后进入淘汰阶段。这段时间包含约 8066 条记录，装入约 127 个 Node。

作为比较，空闲场景使用模板心跳参数 30 秒，实际空日志平均基础间隔约 10.2495 秒。填入 524,288 条空记录，需要约 62.2 天。因此在一天的观察窗口中，前者可能已经反复替换多轮，后者仍处在填充初期。

| 同一个本地分片缓存 | 纯空日志 | 假设每条 payload=1024B |
|---|---:|---:|
| 每条逻辑计账 | 16B | 1040B |
| 8MiB 附近的有效记录量级 | 524,288 条 | 约 8066 条 |
| 容纳这些记录的 Node 数量级 | 8192 个 | 约 127 个 |
| Node 的 jemalloc 分配量级 | 28MiB | 约 0.434MiB |
| 消息内容原始字节合计 | 0 | 约 7.88MiB |
| 填充时间 | 约 62.2 天，按模板空闲节奏 | 约 13.4 分钟，按假设的每秒 10 条 |

消息内容的 7.88MiB 仍不是字符串真实分配：需要考虑末尾字节、分配档位，以及暂留的脏节点；不能把表中两行相加就声称持续写场景的真实上限为 8.31MiB。Node 的数量也会随 64 槽边界及淘汰批次略有波动。

1000 个分片都满足该假设时，三副本会把同一写入复制到各台。每台观察到的本地追加速率约为 10000 条/秒，而不是只算本机发起写入的 Leader。实际客户端“每秒写几个顶点”不能直接代入这个公式：一个业务批次可以包含多个顶点或多个键值，也可以被分成多个分片请求。

### 4.2 已完成的空闲服务实验说明了什么

前次完整集群实验在这 50 个空间没有业务写入的条件下，将 Raft 心跳参数临时加速到 1 秒。885 秒的外部采样窗口中，三台 storaged 的 RSS 分别增加约 **157.762、157.090、157.648MiB**。这里的 1 秒配置是加速复现条件，不能当作模板 30 秒场景的真实时间速度。

相同阶段的服务内部计数中，Node 从每台 3000 增至 26005；观测区间约 870 秒，与外部 885 秒窗口并非完全重合。没有 Node 释放，采样到的缓存仍远未到默认 8MiB。RocksDB 的目标空间 MemTable 同时增加了 86、94、100MiB。因此这里已经证实的是“空闲内部写入可以使 WAL 与 RocksDB 同时增长”，而不是“持续写入的 RSS 比空闲更低”。

随后显式刷盘，50 个作业全部完成；目标空间 MemTable 从约 150–174MiB 降到约 50MiB，三台 RSS 降了约 103–105MiB，但 WAL Node 没有释放。这说明 RocksDB 的刷盘与 WAL 缓存淘汰是两套独立过程，也为前面的持续写分析提供了一个受控对照。

原始证据：完整过程见 [实验明细](EXPERIMENT-DETAILS.md)；空闲主窗口见 [s1](phase-05-accelerated1/s1.csv)、[s2](phase-05-accelerated1/s2.csv)、[s3](phase-05-accelerated1/s3.csv)；刷盘前后见 [刷盘前 s1](phase-08-beforeflush/s1.csv)、[刷盘后 s1](phase-09-afterflush/s1.csv) 和 [作业完成校验](flush-job-verification.json)。

### 4.3 原始缓存组件的不同 payload 对照

2026-10-11 已用同一提交的原始 `AtomicLogBuffer` 执行四组独立程序。它们每条写入后都创建真实 iterator，核对读到的内容，再立即析构；容量固定 8MiB。它不包含 RocksDB、RPC 或整个 50 空间集群的业务写入；非空 payload 是用于组件测试的定长字节串，不是交给业务状态机执行的合法日志编码。

**实验一：对比填满后的同一个缓存。** 程序把仍保留的 Node 和字符串堆分配都计算在内，包括已标删但尚未真正释放的少量节点。以下“分配合计”均为这两类对象的 `malloc_usable_size` 之和，不是进程 RSS。

| 载荷 | 累计追加记录 | 当前逻辑字节 | 尚未删除的 Node（其中脏节点） | Node 分配 | 字符串分配 | 分配合计 |
|---|---:|---:|---:|---:|---:|---:|
| 空 payload | 524,288 | 8,388,608B | 8192（0） | 29,360,128B | 0 | **28MiB** |
| 空 payload，继续写 | 1,100,000 | 8,388,096B | 8194（2） | 29,367,296B | 0 | **约 28.007MiB** |
| 每条 256B | 131,072 | 8,373,248B | 482（1） | 1,727,488B | 9,871,360B | **约 11.062MiB** |
| 每条 1024B | 32,768 | 8,386,560B | 128（2） | 458,752B | 10,485,760B | **10.4375MiB** |
| 每条 1024B，继续写 | 131,072 | 8,386,560B | 128（2） | 458,752B | 10,485,760B | **10.4375MiB** |

这组结果证实了前面的解释：在固定消息大小、读者正常退出的组件条件下，继续写入并没有让这份缓存按累计记录数一直增长。纯空记录填满后的实际分配高于 1024B 消息填满后的分配。1024B 组在 65,536 条的中间样本为 130 个 Node、其中 4 个脏节点，分配约 10.60MiB，说明“平台”仍有回收批次造成的波动，并非每个时刻都严格等于 10.4375MiB。

实测字符串的 jemalloc 分配档位解释了数字差距：256B 内容占 320B，1024B 内容占 1280B。1024B 组在表中保留 8192 条记录（包括脏节点内的记录），所以字符串分配为 `8192 × 1280 = 10485760B`；它不是上一节理想计算中的 7.88MiB。这个组件实验因此也否定了“业务消息缓存实际只占 8MiB”或“直接把 payload 原始字节当真实分配”的说法。

边界样本也与代码一致：1024B 组在第 8065 条时尚无脏节点，逻辑大小 8,387,600B；第 8066 条进入淘汰，把最老 Node 的逻辑大小扣除，脏节点变为 1，真正释放稍后才发生。这把“较大的消息更早到达容量”落实到原始实现的实际执行。

**实验二：停止 1024B 写入，只保留空日志。** 另一个独立进程先追加 32,768 条 1024B 载荷，再持续追加空记录。表中“后续空日志”不含前面 32,768 条；“保留非空记录”包含脏节点中暂留的记录。

| 后续空日志 | 仍保留的非空记录 | 尚未删除的 Node | 当前逻辑字节 | Node＋字符串分配 |
|---|---:|---:|---:|---:|
| 0 条 | 8192 | 128 | 8,386,560B | **10,944,512B，10.4375MiB** |
| 32,768 条 | 7808 | 634 | 8,378,368B | 12,266,496B，约 11.698MiB |
| 262,144 条 | 4352 | 4164 | 8,387,584B | 20,494,336B，约 19.545MiB |
| 600,000 条 | 0 | 8195 | 8,388,608B | **29,370,880B，约 28.010MiB** |

这里并非“GC 没执行”：旧的非空记录确实逐步消失，最后已经全部释放。只是空记录占据了更多槽位，Node 增长超过了消息释放所减少的内存。这个进程的 RSS 同期从 15,736KiB 增至 43,900KiB；其中还包含分配器保留过往释放页的影响，不能把这两个 RSS 数字乘 1000，当成集群结果。

对应回用户的问题：持续写业务时一份缓存可较早停在约 10MiB 的分配量级；停写并持续很久后，同一份缓存又可能向纯空日志的约 28MiB 演化。该具体数值只对本次 1024B 组件、构建和分配器成立；真实业务需带入实际载荷分布，并单独测 RocksDB 等其余部分。

复现方法、驱动和环境见 [组件实验说明](write-idle-comparison-20261011/README.md)。原始表分别为 [空日志](write-idle-comparison-20261011/empty.csv)、[256B](write-idle-comparison-20261011/payload256.csv)、[1024B](write-idle-comparison-20261011/payload1024.csv)、[停写后](write-idle-comparison-20261011/after-stop.csv)。这是按记录数快速推进的实验，没有模拟 13.4 分钟或 62 天真实经过时间。

## 五、源码如何证明这些解释

以下均对应前述产品基线。片段只保留与结论有关的部分，省略处明确标出；详细逐函数版本见 [WAL 解读](CODE-WALKTHROUGH-WAL.md) 与 [RocksDB 解读](CODE-WALKTHROUGH-ROCKSDB.md)。

### 5.1 业务内容真实进入容量计账，并保留自己的字符串

[AtomicLogBuffer.h：Record 构造与 size，29–38 行](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/wal/AtomicLogBuffer.h#L29)：

```cpp
Record(ClusterID clusterId, TermID termId, folly::StringPiece msg)
    : clusterId_(clusterId), termId_(termId), msg_(msg.toString()) {}

int32_t size() const {
  return sizeof(ClusterID) + sizeof(TermID) + msg_.size();
}
```

它证明：缓存保存的是一个拥有内容的 `std::string`，逻辑容量随 payload 长度增加；不是只保存外部字符串的指针。`kMaxLength=64` 和 Node 内的 `std::array<Record, kMaxLength>` 分别见同文件 [18 行](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/wal/AtomicLogBuffer.h#L18)、[104 行](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/wal/AtomicLogBuffer.h#L104)。它不能单独证明字符串实际用了多少堆内存：短字符串可能内联，长字符串的分配档位必须结合本机实现观测。

[FileBasedWal.cpp：appendLogInternal，499 行](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/wal/FileBasedWal.cpp#L499) 在成功追加文件后执行 `logBuffer_->push(id, term, cluster, msg)`；没有按“空消息/业务消息”选择不同缓存。这证明两种场景使用同一套缓存机制，不证明两者整个进程的内存组成相同。

### 5.2 超容量后按最老 Node 淘汰，但不是严格的字节硬上限

[原始 AtomicLogBuffer.cpp：push，71–118 行](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/wal/AtomicLogBuffer.cpp#L71) 的行为可以写成以下**伪代码**：

```text
若当前 Node 不存在、已有 64 条或已标删：
    新建 Node，加入本条日志，增加逻辑大小，直接返回

若追加本条后逻辑大小超过容量，且最老 Node 不是当前 Node：
    把最老 Node 标为删除，逻辑大小扣掉整个 Node
    dirtyNodes 加 1                 # 仍未真正释放内存

把本条日志追加到当前 Node，增加逻辑大小
```

这证明：它按一组最多 64 条的 Node 淘汰，每次该分支最多处理一个旧 Node；不是每次追加都循环删到 8MiB 以下。新 Node 的路径会提前返回；只剩一个 Node 时也不会把正在填充的 Node 淘汰。若单条消息极大，或者消息大小突然变化，超出量可能明显，前面按平均消息大小估算的健康平台不能当成严格保证。

[原始 releaseRef，167–204 行](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/wal/AtomicLogBuffer.cpp#L167) 的关键条件：

```cpp
auto readers = refs_.fetch_sub(1, std::memory_order_relaxed);
if (readers > 1) {
  return;
}
CHECK_EQ(1, readers);
// ... 获取 dirtyNodes，尝试进入唯一 GC 执行区 ...
if (dirtyNodes > dirtyNodesLimit_ || size_ > FLAGS_max_log_buffer_size) {
  // ... 只从 tail 后取出已标删节点 ...
  // ... 遍历这些节点并 delete ...
}
```

完整代码还使用原子标志避免同时执行多个回收过程；GC 指释放已经标删的节点。上面证明最后一个缓存迭代器离开，是尝试 GC 的时机；`max_log_buffer_size=16MiB` 是这一条件的一部分，不是定时任务，也不是缓存或进程的绝对上限。仍被视为有效的最近记录不会因为 GC 执行就全部清掉。

一个长期不退出的 reader，或始终重叠而没有归零机会的 reader，可能推迟脏节点释放。前次组件测试已证明可构造该行为，但没有证据表明持续业务写现场一定发生。即使没有阻塞读者，少量未过 `dirtyNodes > 5` 门槛的脏节点也可以暂留，不能据此直接定性异常。

### 5.3 有业务时空心跳可能减少，但没有“有写入就禁用空日志”的开关

[RaftPart.cpp：sendHeartbeat，2041–2048 行](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/raftex/RaftPart.cpp#L2041)：

```cpp
if (!replicatingLogs_.load(std::memory_order_acquire)) {
  folly::via(executor_.get(), [this] {
    std::string log = "";
    appendLogAsync(clusterId_, LogType::NORMAL, std::move(log));
  });
}
```

它判断的是当时是否有复制任务，不是最近一分钟有没有业务。持续复制可以减少进入该分支的机会；业务写入之间仍有空档时，空日志仍可能产生。所以持续写负载实际是业务日志与空日志的混合，不能总按“两种速率机械相加”，也不能宣称空日志完全消失。

### 5.4 一个业务请求不等于一条日志，一条日志也不等于一个键

[Part.cpp：asyncMultiPut，83–86 行](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/Part.cpp#L83)：

```cpp
std::string log = encodeMultiValues(OP_MULTI_PUT, keyValues);
appendAsync(FLAGS_cluster_id, std::move(log))
    // ... 省略随后连接的 thenValue 回调及语句结束符 ...
```

[LogEncoder.cpp：encodeMultiValues，92–119 行](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/LogEncoder.cpp#L92) 遍历整个 KV 列表，把所有键值及长度一起编码到这个字符串。容量公式中的 `payload` 指这一整条编码的长度，包含编码开销；不是一个顶点的属性长度。

顶点无索引写路径 [AddVerticesProcessor::doProcess，77–146 行](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/storage/mutate/AddVerticesProcessor.cpp#L77) 按分片收集多个顶点、tag 对应的键值；[BaseProcessor::doPut，111 行](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/storage/BaseProcessor-inl.h#L111) 再把这一组传给 `asyncMultiPut`。有索引的路径另用原子操作，涉及更多键值。这证明仅凭顶点数、请求数或 nGQL 语句数，不能直接算出缓存日志条数与消息长度；负载验证必须记录编码后数据。

### 5.5 两种场景都会写提交位置，业务场景还写业务键值

[Part.cpp：commitLogs，225–232 行](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/Part.cpp#L225) 先记住当前 logId，再对空 payload 跳过业务解码；[247–259 行](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/Part.cpp#L247) 对非空 `OP_MULTI_PUT` 遍历 KV，逐个加入 RocksDB batch。最后的 [349–358 行](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/Part.cpp#L349) 无论业务 payload 是否为空，都会把本批最后的提交位置加入 batch，再调用 `commitBatchWrite`。

它证明空闲与业务写入都能更新 MemTable，而业务写入增加了业务键值。它也说明提交位置通常是每次 `commitLogs` 写一次，不是任何负载下每条 Raft 日志都严格写一次；一批中可能有多条日志。业务场景不能照抄空闲低速实验中的“WAL 条数与 MemTable entries 近似一对一”。

[RocksEngine.cpp：commitBatchWrite，124–137 行](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/RocksEngine.cpp#L124) 调用 `db_->Write(options, b->data())`；它不是每个请求都把 MemTable 刷成 SST。所用 RocksDB 7.5.3 的本机公共头文件中，write_buffer_size 注释（原实验机头文件 `/opt/vesoft/third-party/3.3/include/rocksdb/options.h:172`） 解释刷盘前内存量级和按列族配置；max_write_buffer_number 注释（原实验机头文件 `/opt/vesoft/third-party/3.3/include/rocksdb/advanced_options.h:250`） 解释多份缓冲可并存。本文没有冒称已经逐行核对该第三方库全部 flush/compaction 内部实现。

### 5.6 空间与缓存的共享范围，以及负载超出处理能力时的边界

[NebulaStore::addSpace，395–421 行](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/NebulaStore.cpp#L395) 为每个空间的每个数据路径建立 engine；[RocksEngineConfig.cpp，310–312 行](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/RocksEngineConfig.cpp#L310) 则用函数内 `static shared_ptr` 建立共享 block cache。它们分别证明“50 个目标 DB”与“进程内共享块缓存”的计算单位。多个数据路径或不同列族配置时，要重新计算，不能固定套 50。

复制与请求也占内存。[RaftPart::appendLogAsync，800–835 行](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/raftex/RaftPart.cpp#L800) 在复制进行中继续暂存请求；队列达到 `max_batch_size` 等条件会返回 `E_RAFT_BUFFER_OVERFLOW`。这是按日志数量的控制，不是整个进程的字节上限。[Host::prepareAppendLogRequest，323–342 行](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/raftex/Host.cpp#L323) 把日志内容复制到 RPC 请求中，所以业务 payload 在传输阶段可能存在额外副本。

慢 Follower 不等于某个缓存 iterator 必定一直活着：上述 Host 中 iterator 是局部对象，构造 RPC 时复制字符串，不能把网络等待时间直接当成 iterator 生命周期。慢复制可能增加等待和临时数据，但要证明它阻塞脏节点回收，必须另查实际 reader 计数和持续时间。

当刷盘跟不上，旧 MemTable 可以并存，写入可能等待或失败。[RocksEngine::commitBatchWrite](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/RocksEngine.cpp#L124) 设置 `no_slowdown = !wait`，并把不允许等待情况下的 `Incomplete` 映射为 `E_WRITE_STALLED`。这些压力分支说明“稳定平台”有前提，不能承诺任意写速都保持有界、低延迟且无失败。当前没有持续业务写场景的 MemoryTracker 拒绝、OOM 或重启时间线证据，本报告不推断这些事件已经发生，也不把写入错误等同于进程被杀。

## 六、如何验证这个对比，什么结果会推翻判断

已有空闲复现和缓存组件可以证明机制；如果要回答“我的业务持续写时到底占多少内存”，最有价值的是以下三个有区分度的检查。以下完整集群业务对照属于**建议验证，尚未执行**。

1. **先核实写到了哪些分片、日志有多大。** 在隔离测试数据上统计每个 space/part 的业务日志条数、空日志条数、payload 字节，保留客户端成功数、失败数、批量大小及索引配置。预期均匀业务负载会让 1000 个本地缓存更快达到逻辑容量；若只有少数热分片，大部分仍未到容量，就否定了“50 空间持续写等于全部分片已经进入淘汰阶段”的前提。只看总 QPS 不能完成这个区分。

2. **同时看 WAL 的有效量与真正释放量。** 对比 `live_nodes`、`live_logical_bytes`、dirty/reader、GC/释放次数，并为新试验补充消息字符串分配观测。原服务的 `node_object_bytes` 不包含业务 payload，不能拿它代表活跃场景全部 WAL 内存。预期稳定大小负载在越过容量后，追加与释放都持续发生，缓存分配在范围内波动；若逻辑量已稳定但 dirty 与物理节点不断上升，就应转查读者是否归零；若 WAL 全部对象稳定而 RSS 仍涨，就不能继续把主要增量归给这份缓存。

3. **同时核对 RocksDB 的刷盘循环与进程 RSS。** 对每个 engine 采集 `cur-size-all-mem-tables`、`size-all-mem-tables`、immutable 数量、flush 状态、table-reader memory，块缓存按共享对象统计一次；RSS 沿用同一 `/proc` 字段并确认 PID 未变。预期活跃场景能看到多轮 MemTable 产生与退出；若无新表退出，应查是否其实仍未填满或刷盘受阻。若 MemTable、WAL 已稳定而 reader/其他对象增长，就否定“总 RSS 平台完全由这两个缓存决定”的简化推断。最后停止业务、保留心跳，观察 payload 下降与 Node 增加能否同时出现。

完整对照应保留相同空间/分片/副本配置、相同心跳参数和同类起始状态，分别跑空闲、均匀持续写、只写热点分片、业务停写后的阶段。比较持续时间至少覆盖多个缓存淘汰和 MemTable 刷盘周期；一分钟没有平台既不能证明无限泄漏，一分钟出现平台也不能证明长期总 RSS 不再增加。现有加速空闲实验的心跳为 1 秒，不能与另一个心跳为 30 秒的业务试验不加说明地横向比较。

业务测试会生成真实数据和磁盘 IO，应使用独立测试数据目录和端口，不覆盖已归档空闲证据；测试结束停掉测试实例并保留日志。沿用本项目要求：启动前 `ulimit -n 65536`，如确需重新编译，最大 `make -j5`。本文没有启动新的数据库服务，也没有更改业务逻辑或实施修复。

## 七、最终结论

两个场景最核心的差别是：空闲用很小的内部日志缓慢填缓存；持续业务写入通常以更大的字节速率填缓存，所以更早进入替换旧日志和刷盘的循环。对于同样的 WAL 逻辑容量，纯空记录可能比较大业务消息占用更多实际缓存内存；这与“单条业务消息更大”并不矛盾，因为能保留的记录条数不同。

整个 storaged 谁占内存更多仍取决于消息大小、每分片写速、冷热分布、刷盘和复制状态，不能只从“50 个空间都在写”得出唯一数值。最值得补齐的现场证据是每分片的日志大小与速率，以及 WAL 实际分配和各 RocksDB MemTable 的同步曲线；前次空闲服务数据和本次组件对照各自证明的范围必须保留。
