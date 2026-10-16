# 图空间级内核参数：可行性评估与方案修订

日期：2026-10-11。目标：`3.6-memory`，产品源码基线 `de9b3ed800a6627d9845e9289b6bbc5b6faf460a`。本文按工程 [分析规范](../../nebula_graph_readable_analysis_prompts.md) 编写。

**状态：设计评估，尚未实现。** 本轮只改方案与证据文档，没有新增产品代码、编译或运行数据库。下文的新 flag、类型、诊断接口与元数据结构都是建议设计，当前二进制不能直接使用。

## 一、直接结论

**可行，而且比全局缩小 WAL 缓存更适合你的冷热混部环境。建议保留现有全局 gflag 作为默认值，新增按图空间覆盖的配置，再把解析后的容量传给该空间的每个分片。第一版只覆盖 `wal_buffer_size`，启动时确定、滚动重启生效；不同时开发在线缩容或修改复制协议。这样能让高频空间保留原容量，只压缩低频空间的长期缓存。参数隔离不等于资源隔离，冷空间增加的文件读取仍可能影响共享磁盘上的热业务，需要混合负载回归。**

之前“不改内核时只能统一调参或隔离部署”的结论仍适用于旧二进制。现在允许开发新机制，方案的主线改为：**全局默认保持原值 → 新版本增加空间覆盖能力 → 先灰度低频空间 → 有证据后再扩展其他参数。** 部署隔离改为性能仍不能达标时的备选，不再是实现不同 WAL 容量的必要前提。

## 二、gflag 应怎样承担这个功能

gflags 提供进程级变量；它没有自带“当前正在访问哪个图空间”的上下文。官方支持的类型也不包含空间参数映射，可以用 string 承载 JSON，再由程序解析。参见 [gflags 官方文档](https://gflags.github.io/gflags/#define)。

因此建议区分两个对象：**gflag 保存输入配置；空间策略对象保存已经校验、能直接使用的配置。** storage 启动时解析一次，创建分片时按 `spaceId` 取值。每次写日志直接使用已经保存在该缓存对象中的容量，不在读写路径解析 JSON，也不访问 Meta 服务。

绝对不要采用这种做法：处理空间 A 前把 `FLAGS_wal_buffer_size` 改成 1 MiB，处理空间 B 前再改成 8 MiB。空间会并发创建/运行，共享变量不能代表某一个空间；加一把局部锁也不能覆盖所有潜在读取方。

### 2.1 第一版建议的配置入口

拟新增两个启动参数，名称暂定：

| 参数 | 建议职责 |
|---|---|
| `enable_space_kernel_options=false` | 新机制开关，默认关闭；关闭时保持原代码取值路径 |
| `space_kernel_options={}` | JSON 输入，包含格式版本、发布版本、目标集群与空间覆盖项；第一版仅允许 `wal_buffer_size` |

现有 `wal_buffer_size` 保留原名称、单位和默认值。第一版启用新机制时，在创建业务分片前将全局默认值与空间映射一起冻结为启动配置快照；显式覆盖优先，未覆盖空间继承该启动默认值。首期不引入热/中/冷 profile 二级继承，也不根据访问频率自动改档，避免一次分类错误影响一批空间。

**以下是设计格式示意，所有 ID/名称都是假设，1 MiB 也不是首轮上线建议值：**

```text
--wal_buffer_size=8388608
--enable_space_kernel_options=true
--space_kernel_options={"format_version":1,"revision":"cold-trial-01","cluster_id":12345,"spaces":{"303":{"expected_name":"asset_models","wal_buffer_size":1048576}}}
```

这表示只有 ID 303 的 `asset_models` 使用 1 MiB，其余空间仍按全局 8 MiB。实际第一轮应只选少量已确认低频空间，并从它们的当前值向 4 MiB 验证，而不是直接复制示例。配置里的 `wal_buffer_size` 单位为字节，且是**每个本地分片副本的目标计账容量，不是整个图空间合计 1 MiB**。

### 2.2 输入必须有明确的失败语义

第一版建议支持的覆盖范围为 256 KiB～8 MiB，作为本次“降低冷空间缓存”的产品约束；它不是现有算法天然支持范围，也不表示下限已获生产安全验证。未覆盖空间继续使用原有合法全局值。以后需要更大容量时另做范围与溢出评估，不顺手增加第一版范围。

拒绝非法 JSON、重复键、未知参数、负数、零、非整数、溢出与不支持的格式版本，不把拼错的覆盖静默当成默认。数值先以足够宽的类型解析，检查后再转成底层 `int32_t`，避免先截断再验证。

空间身份以 `(目标集群, spaceId)` 为准，`expected_name` 作为防误配检查，不按名称前缀或通配符批量匹配。空间删除再同名重建会有新的空间身份，不能自动继承旧 ID 的策略；跨集群备份恢复也需重新审核映射。集群身份应来自已确认的 Meta 连接信息，不能拿默认可能为 0 的另一个 `FLAGS_cluster_id` 变量直接当可信校验值，必要时为 MetaClient 增加只读访问接口。

启动前检查覆盖里的空间是否存在于 Meta 的完整空间列表，不能仅因为某 storage 当前没有该空间的本地分片就判错。已经删除的 ID 应从发布文件清理后再重启；启用模式下发现未知 ID 或名称不一致时，拒绝该实例使用这份策略启动并给出明确错误。这个选择能阻止误配置，但会阻止该实例上线，所以必须提供离线格式检查、连接 Meta 的预检查，并坚持逐台发布，不能先停全体服务再发现配置有误。

运行中的 DROP SPACE 沿用原对象删除流程；冻结映射中的旧 ID 不会给其他空间使用。运行中新建且未列出的空间继承启动默认值，同时输出“未覆盖”的来源信息；要调整它，重新发布策略并按重启流程生效。空间生命周期校验需考虑校验后立即删除的竞态，不得阻断合法删除或复活旧空间。

### 2.3 第一版不接受伪热更新

仅“不把新 flag 列入 MUTABLE”不足以实现启动参数限制。现有 HTTP PUT `/flags` 直接调用 `SetCommandLineOption`，可以修改未列入 Meta 可变集合的变量。

第一版应明确拒绝新开关、新映射，以及启用模式下已冻结的 `wal_buffer_size` 的运行时更新。HTTP 入口可以在写变量前同步返回“需要重启”并保留旧值；不只在写入后通知回调。后台只读策略始终不从可变的原始 JSON 字符串重新取值，避免 `/flags` 已显示 1 MiB、实际缓存还用 8 MiB。关闭新机制时保留旧全局参数行为，新开关/映射仍是启动参数。

Meta 入口要单独处理：新增两参数不进入 Meta 可变注册，启用模式下全局 `wal_buffer_size` 也禁止注册为可变项，自定义 gflags 清单不能绕过限制。若现网曾通过自定义 Meta 配置管理它，应先核对并迁移有效值，不能在启用时悄悄忽略。遇到历史或误注入的 Meta 条目，Storage 应拒绝应用、保持有效值并记录可查询的拒绝状态；**不能宣称 Meta 命令发起者会同步收到这个拒绝**，因为现有 Meta 写入与 Storage 异步应用不是同一个回执。若必须在 Meta 发起端同步拒绝，还需增加 Meta 管理入口校验，这属于额外控制面工作。

如未来产品选择“允许保存新值但等待重启”，则必须增加 desired/effective 两种状态和明确回执，这是另一种管理语义，不能只改字符串后返回普通成功。本文首期选择拒绝运行时修改。

## 三、为什么可以用较小改动实现 WAL 空间化

现有结构已经是：**一个图空间包含多个分片，每个分片副本有独立的 FileBasedWal 和 AtomicLogBuffer。** 它们不是一个全局共享 WAL 缓存，所以无需为了不同容量重建缓存架构。

现有 [RaftPart.cpp:357](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/raftex/RaftPart.cpp#L357) 在构造时设置：

```cpp
FileBasedWalPolicy policy;
policy.fileSize = FLAGS_wal_file_size;
policy.bufferSize = FLAGS_wal_buffer_size;
policy.sync = FLAGS_wal_sync;
```

该构造函数已经接收 `spaceId`。随后 [FileBasedWal.cpp:59](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/wal/FileBasedWal.cpp#L59) 执行：

```cpp
logBuffer_ = AtomicLogBuffer::instance(policy_.bufferSize);
```

这证明改变传入的容量就能创建不同大小的缓存，无需改变磁盘日志布局。它不能证明目前已经有按空间解析配置的功能，也不能证明任何候选值都满足生产 SLA。

### 3.1 推荐的改造边界

1. 在业务 Storage 启动层解析并校验配置，构造一个不可变的空间策略表。解析、校验和错误输出在创建引擎/分片前完成；不是每个分片各复制一整份 JSON。
2. 将策略以受控的 `KVOptions`/上下文传给对应业务 `NebulaStore`。普通 storage、Meta 和 Listener 的上下文需明确区分，不能仅凭数值 `spaceId > 0` 推断角色。
3. 在 `NebulaStore::newPart(spaceId, ...)` 选择实际容量，以新增的类型化选项传入 `Part → RaftPart → FileBasedWalPolicy`。保持不提供覆盖的调用方沿用原全局路径。
4. 本次只替换 `bufferSize` 的来源。`fileSize`、`sync`、TTL、心跳与 GC 算法均沿用现有行为。
5. 新增只读生效信息与启动摘要，记录空间、角色、策略版本、来源、实际容量和本地副本数量。

**建议接口的伪代码，不是已经提交的实现：**

```cpp
// Storage 启动时：一份经过验证的不可变快照
auto options = parseAndValidateSpaceOptions(flags, readyMeta);
// 创建业务 Part 时：值随对象传递，不临时改全局 FLAGS
auto capacity = options.resolveWalBufferSize(spaceId);
auto part = makePart(/* existing arguments */, WalMemoryOptions{capacity});
// RaftPart 构造时：仅改变这个字段的来源
policy.bufferSize = explicitOptions.walBufferSize;
```

`WalMemoryOptions` 等名称为拟议接口。完整实现还需提供兼容构造入口/默认路径，并覆盖直接构造 Part/RaftPart 的测试与工具调用方。

业务分片入口见 [NebulaStore.cpp:484](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/NebulaStore.cpp#L484)。`Part` 和 `Listener` 都继承 `RaftPart`，见 [Part.cpp:23](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/Part.cpp#L23)、[Listener.cpp:20](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/listener/Listener.cpp#L20)。Meta 也使用 NebulaStore，管理库还会创建 space 0 的 RocksEngine，见 [MetaDaemonInit.cpp:84](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/daemons/MetaDaemonInit.cpp#L84)、[StorageServer.cpp:152](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/storage/StorageServer.cpp#L152)。这些事实说明必须按调用上下文限定作用范围，不能直接在公共 Raft 类里把同 ID 全部改掉。

Storage 当前先等待 Meta 就绪，再创建 store，见 [StorageServer.cpp:236](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/storage/StorageServer.cpp#L236)。这提供了构造前校验的接入位置，但还需要实现错误处理与身份验证，不是已有就绪检查自动替你验证新配置。

### 3.2 为什么首期不做在线缩容

[AtomicLogBuffer.h:327](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/wal/AtomicLogBuffer.h#L327) 在构造时保存容量；`capacity_` 目前是普通 `int32_t`，没有 resize 接口。原始 [push:71](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/wal/AtomicLogBuffer.cpp#L71) 超容量后至多标记一个旧 Node，真正删除发生在 [releaseRef:167](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/wal/AtomicLogBuffer.cpp#L167) 满足读者条件时。

所以把 8 MiB 在线改成 1 MiB 至少有三个额外问题：并发写入如何安全读取新容量；既有有效日志如何逐步淘汰；仍被迭代器引用的旧节点何时释放。只把整数改成 atomic，不能自动解决后两件事，也不能承诺内存立即下降。

首期采用重建对象生效，复用已有构造/析构语义。代价是需要滚动重启，热空间参数虽保持不变，热业务仍可能经历短时 Leader 变化、冷缓存与重试。不能把“只给冷空间改参数”写成“发布过程只影响冷空间”。

## 四、一个贴近现网的例子与收益边界

**以下全部是假设配置。** 同一个 storaged 承载高频空间 101、中频空间 202、低频资产空间 303；原来都用 8 MiB。

| 阶段 | 高频 101 | 中频 202 | 低频 303 |
|---|---|---|---|
| 原版本 | 8 MiB | 8 MiB | 8 MiB |
| 新版本开关关闭 | 8 MiB，原路径 | 8 MiB，原路径 | 8 MiB，原路径 |
| 首轮候选 | 继承 8 MiB | 继承 8 MiB | 显式 4 MiB |
| 进一步验证后的示意目标 | 继承 8 MiB | 仍保持原值，单独评估 | 2 MiB 或 1 MiB，取通过验收的一档 |

假设资产空间有 20 分区、3 副本，恰好在 3 台 storage 各有 20 个本地副本。在缓存被空日志填满、读者正常释放、本机同构建的条件下，每台该空间的缓存节点可从约 `20 × 28 = 560 MiB`，降低到 1 MiB 配置时的 `20 × 3.5 = 70 MiB`。50 个这样的冷空间，对应此前的约 27.34 GiB→3.42 GiB 量级，而热空间继续使用原容量。

单缓存不同容量的数值来自 [原始组件实验](production-tuning-evidence/README.md)，不是新空间机制的集成实测。该实验已证明“容量变小→空日志缓存平台变低”，**没有证明新映射不会串空间、不会误作用于 Listener，也没有验证混合负载性能**；这些必须在开发后验证。

空间级机制解决的是“冷空间不能单独设小”的限制，不改变 64 条一组、计账量与实际分配量不同、批次/读者导致超额峰值等原有性质。它也不减少图空间数量或分片数量，不直接限制整个容器内存。

更准确的预算应为各空间相加：

```text
冷空间 WAL 节点平台近似值 = Σ（该空间本地副本数 × 该空间容量 × 本机构建的空日志系数 3.5）
容器预算还需加入热空间实际 WAL、RocksDB、查询/写入峰值、文件缓存等
```

即使高频空间的参数不变，低频空间偶发大批次写入与副本追赶也可能更多读取文件，与热业务共享 CPU/磁盘。因此“热参数不变”是风险缩小的依据，不是性能完全不变的证明。

## 五、“WAL 等参数”应该分几批改造

| 参数/资源 | 可行性与工作量来源 | 本方案优先级 |
|---|---|---|
| `wal_buffer_size` | 本来每副本独立缓存，构造时已有 spaceId，只需增加可靠选值与传参 | **第一版唯一可覆盖参数** |
| RocksDB `write_buffer_size` | 每空间/数据路径有独立引擎，适合合成空间级选项；仍需处理已有全局更新覆盖问题 | 第二版，MemTable 证据充分后再引入 |
| `max_write_buffer_number`、`arena_block_size` | 对象范围合适；要校验组合、默认派生、刷盘/写停顿与已存在 MemTable 行为 | 随第二版独立验证，不一次全部改小 |
| `max_log_buffer_size` | 当前是 GC 触发条件且读取全局值；空间化需要给 buffer 传独立阈值，而不是把它误当容量 | 当前收益不足，第一版保持原值 |
| `wal_file_size`、`wal_ttl` | 文件对象虽分片独立，但 TTL 在清理时仍读全局；需修改所有取值点并验证追赶/清理 | 不纳入本次降内存首期 |
| `raft_heartbeat_interval_secs` | 多处分片/Host/租约/超时依赖，含静态缓存值；不能只改一个入口 | 独立协议时序专项，不作为本次节内存参数 |
| `wal_sync`、`rocksdb_disable_wal` 等 | 涉及持久化与恢复策略，不只是内存大小 | 保持现有策略，不随空间容量一起修改 |
| RocksDB block cache / row cache | 目前跨空间共享 static Cache；单纯 per-space map 不会产生真实独立配额 | 需单独设计缓存预算/共享方式，暂不改 |
| MemoryTracker、水位线、purge、进程线程池 | 对象范围主要是进程；当前没有按空间完整归属与隔离计账 | 保留进程级，不能伪装成空间级限制 |

**RocksDB 空间化不是只改初始化的一行。** `initRocksdbOptions` 已有 `spaceId`，可以在解析全局选项后覆盖允许的键，见 [RocksEngineConfig.cpp:222](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/RocksEngineConfig.cpp#L222)。但后续全局配置更新还会通过 [MetaClient.cpp:3214](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/clients/meta/MetaClient.cpp#L3214) 广播所有空间，因此第二版必须让初始化与更新都经过同一套“全局基础值 + 空间覆盖”合成逻辑。未覆盖键保留已有配置，删除覆盖应明确恢复到当前默认，不能把空 map 当作已经重置引擎。

[NebulaStore.cpp:1014](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/NebulaStore.cpp#L1014) 已可按空间更新该空间全部引擎，最终调用 [RocksEngine.cpp:489](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/RocksEngine.cpp#L489) 的 `SetOptions`。这些只证明存在应用入口，不提供持久化、权限、集群一致下发或跨引擎原子成功。一个引擎成功、另一个失败时，需要明确部分失败状态、重试/回退；不能仅凭配置服务成功就宣布完成。

读取缓存的特殊性见 [RocksEngineConfig.cpp:307](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/kvstore/RocksEngineConfig.cpp#L307)：

```cpp
static std::shared_ptr<rocksdb::Cache> blockCache =
    rocksdb::NewLRUCache(FLAGS_rocksdb_block_cache * 1024 * 1024, FLAGS_cache_bucket_exp);
bbtOpts.block_cache = blockCache;
```

只将这里的 flag 替换成按空间查值，static 对象仍只有一份，可能变成“第一个空间决定所有空间容量”。直接每空间新建大 cache 又会放大总预算，损失共享收益。这需要独立方案，不能和 WAL 的局部选值当作同一种改造。

## 六、第一版与长期配置中心的取舍

第一版使用 gflag JSON，配合现有配置发布系统向**所有可能承载这些空间的 storage** 下发同一份策略；新增节点、迁移目标也必须纳入。优点是不需要修改 Meta schema、nGQL 或复制协议，开发和版本兼容面相对小。代价是配置同步由运维负责、增删配置需要重启，无法自然提供中心化在线管理。

同一分片的各副本只采用不同缓存容量，不会因为容量不同而直接改变日志内容/多数派规则，因此可设计滚动过渡；但可能出现不同内存与恢复性能，不能长期不检查配置漂移。发布版本号加规范化内容 hash 用于核对配置，不能只比较原始 JSON 字符串排版。过渡中允许旧/新版本并存，验收后要求所有目标实例完成；原有日志回退、磁盘读取和恢复流程仍需混合版本测试。

拟新增只读诊断至少包含：`spaceId`、空间名、角色、发布版本/hash、取值来源、实际容量、本地分片数、容量分布。已有 `/flags` 只能看到进程变量，新诊断必须从已构造对象或其实际生效记录读取，不能再次计算期望值冒充事实。空间容量配置通过不意味着内存已回收，实际缓存节点/字节和 RSS 仍应分别观察。

如果空间数量和变更频率很高，后续更适合“gflag 控制开关/默认策略，Meta 保存空间覆盖”。这需增加带版本的专用空间策略记录、管理接口、持久化与删除处理、权限审计、能力协商、下发通知、实例应用结果以及重试语义。Meta 记录更新与存储全部生效不能用同一个无条件成功状态表示。

当前 [SpaceDesc](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/interface/meta.thrift#L108) 没有这类参数。未来可以评审增加可选类型化字段或独立的 `(cluster, spaceId)` 策略表；首选先设计独立策略的版本/兼容语义，不把任意 gflag Map 塞进去。增加可选字段也不能直接宣称混合版本安全：旧代码存在读取 SpaceDesc 后重新序列化写回的路径，必须验证未知字段保留；独立策略表则必须补齐空间删除、备份恢复和旧版本管理接口的兼容。当前 [MetaClient::diff](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/clients/meta/MetaClient.cpp#L1020) 比较分片与宿主映射，不会自动触发新空间参数变化的应用，Meta 版还需要独立的“策略已准备好”状态，不能借现有 waitForMetadReady 自动宣称策略加载成功。

也不能把新 JSON 直接注册到现有 `NESTED` 就认为完成下发：[GflagsManager.cpp:28](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/common/meta/GflagsManager.cpp#L28) 的 map 转换按字符串值处理，[MetaClient.cpp:3204](https://github.com/cccxgit/nebula/blob/de9b3ed800a6627d9845e9289b6bbc5b6faf460a/src/clients/meta/MetaClient.cpp#L3204) 将 map 当 RocksDB 配置广播。空间策略需要自己的解析/回调，且不应通过改变 `local_config` 顺带改变现有配置来源。

## 七、验证、发布与回退

下面是开发后的验收计划，**本轮未执行新机制测试**。已有容量 CSV 只作为算法选值收益的依据。

| 验证 | 预期观察 | 什么结果否定设计/候选值 |
|---|---|---|
| 开关关闭、空映射、未覆盖空间 | 与旧版本同取值、同功能；无多余热路径解析/锁 | 无关空间或 Meta/Listener 行为改变 |
| 同进程三个空间并发创建和写入 | 每个实际缓存容量与各自策略一致；角色切换后不串值 | 读取全局临时值、首个空间决定所有缓存 |
| 无效输入、旧 ID、同名重建、错误集群、HTTP 修改 | 明确拒绝无效配置/运行时修改；不静默套到其他空间 | 配置显示成功却未应用，或错误映射到热空间 |
| 重启、新增节点、分片新增/迁移、单实例配置遗漏 | 覆盖全生命周期入口；有效版本/hash 可核对 | 重启后恢复全局大值、新节点遗漏冷策略却被验收为成功 |
| 空日志填充超过新容量、多读者、大 payload/批次 | 正常读者释放后符合较低平台；峰值有记录 | 持续异常积压或数据读取/恢复失败；不能只看 RSS 初降 |
| 冷空间偶发资产导入 + 热空间峰值同时运行 | 冷读写成功，热点 P99、吞吐、错误率满足既有 SLA | 参数虽隔离，文件 I/O 仍让热点不达标 |
| Follower 重启追赶、Leader 切换、冷副本升主 | 日志/数据一致、恢复时间达标，异常超时/快照没有不可接受增长 | 平稳压测通过但恢复失败或恢复 SLA 越界 |
| 配置回退、新旧二进制滚动共存 | 旧配置生效、服务可恢复、内存有回退余量 | 旧程序不认识新 flag 无法启动，或缓存回到大值后再次逼近限额 |

整机对照至少三组：旧版本原配置、新版本开关关闭、新版本仅冷空间覆盖。这样才能区分代码引入的开销与冷缓存调小的影响。先做单变量 WAL 验证；开发/构建如需执行完整 make，最大并行度仍为用户要求的 `-j5`。

发布建议分两步：先部署支持该功能但默认关闭的新版本，验证兼容；再给少量冷空间设置覆盖并逐台滚动启用。新 flag 不能提前发给不支持它的旧二进制；同样，二进制回退前需要移除新增 flag，不能仅把开关置 false 留给旧程序解析。

每次仅操作一个健康副本实例，恢复/追平后再操作下一个；依据实际副本数、Leader 与容量判断是否能滚动。空间级设置不提供免重启应用能力，不承诺发布完全无中断。所有服务启动仍遵守 `ulimit -n 65536`。

回退分两类：某些冷空间性能不达标，只把它们调回上一档并按重启流程生效；功能整体异常，则关闭新机制、恢复兼容配置并滚动回退。**回退到旧的 8 MiB 默认，会让冷缓存未来再次向较高平台增长。** 必须事先保留资源余量或准备扩容/隔离方案，不能把“代码能回退”当作“内存一定撑得住”。如果最初只能靠新机制才能勉强不超限，应先补足回退期间的容量安排。

验收时既看业务 SLA，也看实际生效分布、内存分项和长期填充。只看到重启后内存下降、HTTP flag 更新或配置文件一致，均不足以宣布完成。

## 八、对之前生产方案的具体修改

| 之前，在不改内核的前提下 | 现在，允许开发空间级配置后 |
|---|---|
| 在同一进程统一验证 8→4→2 MiB | 全局和热空间保持原值；只对选中的低频空间验证 8→4→2→1 MiB |
| 冷热必须先分部署，才能使用不同 WAL 容量 | 首期改造后可同进程不同容量；部署隔离保留为共享资源干扰仍不达标时的备选 |
| 中频空间跟随全局调整 | 第一轮保持原值，待冷空间收益与性能明确后单独评估 |
| RocksDB 写缓冲作为第二个全局候选 | 先不改；后续按空间合成并处理全局更新路径，再独立验证 |
| 降低水位线风险靠总体内存归因与预算 | 继续保留，空间容量也不是容器硬上限 |

未开发完成前，仍使用旧版本方案的约束；不能把这个设计格式直接加入现网。更新后的 [生产方案](PRODUCTION-PARAMETER-TUNING.md) 将两阶段分开，原始实验与参数语义不因设计变化而改写。

## 最后结论

推荐实施“gflag 输入 + 按空间解析的不可变策略 + 构造时传入 WAL 容量”，首期仅支持业务 storage 的 `wal_buffer_size`。它能解除冷热空间必须共用同一容量的限制；是否解决现网 80% 水位问题、是否满足热点性能，仍需完成空间归因、混合负载与恢复验收，不能由设计可行性直接替代。
