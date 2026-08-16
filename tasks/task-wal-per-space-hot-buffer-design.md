# Nebula Graph 3.6 按图空间热更新 WAL 缓存容量：系统设计方案

> 文档状态：详细设计，尚未实施产品代码。
>
> 适用范围：Nebula Graph 3.6，3 个 storaged、RF=3 的生产拓扑；重点治理长期低写入 Space 的 `AtomicLogBuffer` 内存。
>
> 源码口径：产品源码基线来自分支 `3.6-w-1`；当前工作树的 `AtomicLogBuffer.*` 和 `StorageHttpStatsHandler.cpp` 含此前仅用于根因实验的观测插桩，因此这些文件的本地行号相对原始提交有所后移。本文链接均指向当前工作树，并同时标注关键符号名。
>
> 前置报告：[完整根因分析](./task-wal-root-cause-analysis.md)、[源码系统讲解](./task-wal-source-code-walkthrough.md)、[不改源码的缓解方案](./task-wal-no-source-mitigation.md)、[主动老化方案审查](./task-wal-idle-cache-aging-design.md)。

## 1. 执行摘要

### 1.1 推荐方案

在“允许少量源码扩展，但不能改变 Heartbeat、周期空日志、Raft 复制、提交、WAL 格式和状态机机制”的约束下，推荐方案是：

> 为每个 Space 持久化一个期望的 WAL 内存缓存容量；storaged 将该策略应用到此 Space 的每个普通 replica-part。每个 `AtomicLogBuffer` 只把原来构造期固定的 `capacity_` 改成可原子更新的目标容量，后续缓存收缩仍完全复用现有 `push() -> 标记旧 tail -> releaseRef() GC` 路径，不增加后台主动 trim，不直接修改链表，也不删除磁盘 WAL。

第一版采用 `OBSERVE_ONLY` 冷热观察、人工审核和人工下发 Space 策略。冷热观察采用 **Graph-first、Storage-guarded、Meta-coordinated** 三层架构：graphd 是业务请求冷热的主观测面，storaged 提供非空 WAL、Raft、副本和后台作业的权威安全否决信号，Meta Leader 负责按完整成员集合聚合；观察器不得自动修改容量。自动执行只作为完成 shadow 验证后的可选阶段。

这里不能简化为 Graph-only。graphd 最清楚“用户是否还在查询或写入某个 Space”，但看不到 Follower 复制、commit lag、Snapshot、ingest、实际 WAL append 和本地 Atomic cache；因此 graphd 可以产生 `GRAPH_COLD_CANDIDATE`，只有 Graph 与 Storage 两侧证据都完整时，Meta 才能产生 `SPACE_COLD_ELIGIBLE`。

第一版的准入条件是：目标 Space 的**全部普通 Part** 都已被证明为低非空 WAL 写入，或业务负责人明确接受热点 Part 也同步降容的性能代价。只要存在“一部分 Part 热、一部分 Part 冷”的写入倾斜且不能接受误伤，就应跳过该 Space；per-part policy 必须另立项目，Phase 3 的 Space 级自动执行也不会自动避开热点 Part。

### 1.2 为什么它是在线精细化治理中的最佳平衡点

它同时满足：

1. **局部影响**：只缩小指定冷 Space，热 Space 继续保留原有缓存窗口；
2. **无需重启**：目标容量可热更新，避免为了缓存参数反复触发 Leader 迁移；
3. **不改 Raft 协议**：容量不进入 term、quorum、RPC、WAL、Snapshot 或 RocksDB 数据；
4. **不新增第二条链表 Writer**：控制线程只原子写一个容量值，tail 的移动和 Node 标脏仍由现有 `push()` 单 Writer 完成；
5. **可以灰度**：同一 Raft Group 的三个副本可以暂时使用不同容量；一致性不受影响；
6. **容易回滚目标**：可以把目标容量重新调大，虽然已经淘汰的缓存不会立即预热；
7. **收益可计算**：冷 Space 的有效 Node 平台近似与目标容量线性相关。

这里的“最佳”有明确语境：当目标是**以后无需重启即可频繁、选择性地治理冷 Space**时，它是风险与收益最均衡的方案。如果只追求绝对最低代码风险，并且能够接受逐台重启，静态 per-space override 更保守；如果完全不能改源码，全局调小 `wal_buffer_size` 后滚动重启仍是最低代码风险的止损手段。

首次部署支持热容量的新二进制仍然需要滚动重启，并会因为进程重建而立即释放旧 Node。热生效的主要价值是：完成首次部署后，后续 Space 策略调整不再要求重启；不能把首次重启造成的 RSS 下降当成 setter 已完成稳态收缩的证据。

### 1.3 不能忽略的发布阻断项

这个方案不能被简化成“把 `capacity_` 改成 atomic”。启用热降容以前，必须完成以下 P0 闭环：

1. 加固现有 `AtomicLogBuffer::push()` 淘汰与 `releaseRef()` GC 的并发窗口；热降容会把原来低频的淘汰分支放大为连续执行；
2. 对 `WalFileIterator` 与 `rollbackToLog()/ftruncate` 的并发边界完成确定性测试，并在无法证明安全时先完成同步修复；小缓存会增加磁盘 iterator 的使用频率；
3. 策略必须持久化，并具有 generation、单调 snapshot epoch、规范化 hash、幂等应用和 applied 状态；不能只依靠进程内 HTTP 改值；
4. 必须区分“目标容量已设置”“逻辑容量已收敛”“dirty Node 已物理删除”和“RSS 是否归还”。
5. 若启用 AUTO，必须实现11.9.1节的 owner/decision/lease/fail-hot版本格、Graph成员准入和新Part hot启动；只做周期判断再调用setter会被迟到任务重新降冷。

如果不准备解决这些前置项，最低风险选择仍然是“静态 per-space override + 单节点滚动重启”，而不是热更新。

## 2. 问题、目标与非目标

### 2.1 已验证的问题链

根因报告已经证明以下路径：

```text
RaftPart::statusPolling()
  -> 稳定 Leader 周期 sendHeartbeat()
  -> appendLogAsync(NORMAL, "")
  -> Leader/Follower FileBasedWal
  -> 每个 replica-part 的 AtomicLogBuffer::push()
  -> 空记录逻辑只计16B
  -> 每64条却实际分配一个固定 Node
  -> 达到原8MiB逻辑容量前，live Node 长期线性增加
```

关键源码：

- 周期调度：[`RaftPart::statusPolling()`](../src/kvstore/raftex/RaftPart.cpp#L1401-L1434)；
- 空 NORMAL 日志：[`RaftPart::sendHeartbeat()`](../src/kvstore/raftex/RaftPart.cpp#L2041-L2049)；
- WAL 先写文件、再 push cache：[`FileBasedWal::appendLogInternal()`](../src/kvstore/wal/FileBasedWal.cpp#L442-L500)；
- 空记录逻辑计费：[`Record::size()`](../src/kvstore/wal/AtomicLogBuffer.h#L40-L56)；
- Node 固定64条：[`Node`](../src/kvstore/wal/AtomicLogBuffer.h#L61-L128)；
- 超容量后原生标脏：[`AtomicLogBuffer::push()`](../src/kvstore/wal/AtomicLogBuffer.cpp#L119-L172)；
- reader 释放时原生 GC：[`AtomicLogBuffer::releaseRef()`](../src/kvstore/wal/AtomicLogBuffer.cpp#L220-L268)。

### 2.2 设计目标

本方案必须达到：

1. 支持按 Space 配置 WAL 内存缓存目标容量；
2. 目标容量可以在 storaged 不重启的情况下生效；
3. 未配置 Space 的行为与当前开源版本保持一致；
4. 不改变空日志生成、复制、commit 和 apply；
5. 不改变磁盘 WAL、Snapshot、RocksDB 格式；
6. 不从控制线程直接移动 `tail_`、标脏或 delete Node；
7. 新增 Part、重建 Part、删除 Space 和滚动升级期间策略语义明确；
8. 允许单节点、单 Space、单批 Part 灰度；
9. 可以观测 desired、applied、shrinking、steady 和 warming；
10. 异常配置、乱序更新和进程崩溃不能造成静默回到错误容量。
11. 能够给出可解释、可审计的 Space 冷热证据，缺报、重启、拓扑变化和后台作业一律 fail-closed；
12. 第一阶段的冷热观察只读，不允许未经人工批准自动修改 target。
13. 业务请求热度以 graphd 的逻辑 StorageClient dispatch 为主，storaged ingress 只作直连/内部流量兜底；任何一侧缺报都不能解释为冷。

### 2.3 非目标

第一版明确不做：

- 不修改 `sendHeartbeat()` 或禁止周期空日志；
- 不新增 committed-aware 主动 trim；
- 不主动 reset/swap 整个 `AtomicLogBuffer`；
- 不主动预热已经淘汰的日志；
- 不对冷 Space 主动 flush RocksDB；
- 不自动降低 `wal_ttl`；
- 不承诺 RSS 立即下降；
- 第一阶段不自动执行冷热容量切换；观察器只给出 `GRAPH_BUSINESS_HEAT`、`STORAGE_WAL_WRITE_HEAT`、`STORAGE_RPC_HEAT`、`SYSTEM_BUSY` 和候选建议；
- 不把 Listener 默认纳入第一版；
- 不承诺整个 Space 的所有 Part 在同一时刻原子切换容量。

## 3. 不可破坏的正确性边界

### 3.1 AtomicLogBuffer 只是本地读缓存

`FileBasedWal::appendLogInternal()` 的顺序是先编码并写本地 WAL 文件，之后才调用 `logBuffer_->push()`：[`FileBasedWal.cpp:442-500`](../src/kvstore/wal/FileBasedWal.cpp#L442-L500)。

读取时，如果起始 LogID 不在 Atomic cache，会使用磁盘 iterator：[`FileBasedWal::iterator()`](../src/kvstore/wal/FileBasedWal.cpp#L530-L536)。Leader 为落后 peer 准备 AppendLog 也走统一 WAL iterator：[`Host::prepareAppendLogRequest()`](../src/kvstore/raftex/Host.cpp#L287-L345)。

因此缓存容量不参与 Raft 数据路径中的：

- Raft term 和 role；
- 多数派确认；
- `committedLogId`；
- 状态机 apply；
- 查询数据；
- WAL 文件内容；
- Snapshot 内容；
- RocksDB sequence 和 key/value。

三个副本使用不同缓存容量，在协议正确性上是安全的：一个副本从内存读，另一个从磁盘读，权威日志内容相同。

### 3.2 本方案改变的是策略，不是日志机制

当前每个 `RaftPart` 构造时无条件使用全局 gflag：

```cpp
FileBasedWalPolicy policy;
policy.bufferSize = FLAGS_wal_buffer_size;
```

源码：[`RaftPart::RaftPart()`](../src/kvstore/raftex/RaftPart.cpp#L330-L365)。

本方案只让这一容量在运行期可变。原生的下列逻辑保持不变：

```text
记录进入buffer
  -> size + record charge 超过capacity
  -> 标记最旧有效Node
  -> 推进tail和firstLogId
  -> dirtyNodes增加
  -> 最后一个旧reader释放时执行GC
```

准确术语应是“复用原生淘汰机制的最小机制扩展”，不能写成“源码和行为完全不变”。

## 4. 总体架构

```mermaid
flowchart LR
    A["管理命令或配置系统"] --> B["持久化的Space WAL策略"]
    B --> C["desired generation/epoch/hash"]
    C --> D["storaged策略管理器"]
    D --> E["不可变spaceId到target map"]
    E --> F["普通Part shared_ptr快照"]
    F --> G["AtomicLogBuffer::setCapacity"]
    G --> H["atomic target capacity"]
    H --> I["后续原生push容量判断"]
    I --> J["原生tail标脏"]
    J --> K["原生releaseRef GC"]
    D --> L["applied generation/epoch/hash和状态指标"]
```

架构分成两条互不混淆的路径：

### 4.1 控制路径

负责：

- 校验 Space 和容量；
- 持久化 desired policy；
- 分配同一 generation 内严格单调的 snapshot epoch；
- 将策略下发到各 storaged；
- 收集 applied generation/epoch/hash；
- 支持查询、灰度、重试和回滚。

控制路径不操作 Node 链表。

### 4.2 数据路径

负责：

- 每个 buffer 原子保存目标容量；
- 每次 `push()` 读取一次容量快照；
- 按现有算法逐步标记旧 Node；
- 按现有 reader-ref 协议物理回收；
- Atomic miss 时按现有路径读取磁盘 WAL。

数据路径不读取 Meta，不等待集群共识，也不要求三个副本容量相同。

## 5. 策略模型

### 5.1 策略快照与版本术语

逐 Space CAS 与整张策略快照不能共用一个含义模糊的 `revision`。本文统一采用：

```text
PolicySnapshot {
  policy_generation: uint64/UUID     # restore/rebase后的新世代
  snapshot_epoch: uint64             # 该世代内任一Space变化都+1
  canonical_hash: bytes              # 完整map的规范化hash
  policies: map<space_id, SpaceWalBufferPolicy>
}

SpaceWalBufferPolicy {
  space_id: int32
  target_capacity_bytes: int64
  last_modified_epoch: uint64         # 本Space的CAS token，可跳号
  enabled: bool                        # false表示RESET tombstone
  control_owner: OBSERVE_ONLY | MANUAL | AUTO
  include_listener: bool = false
}
```

含义：

- 管理命令的 `EXPECT` 比较完整 `(policy_generation,last_modified_epoch)`，防止 restore/rebase 后 generation 变化但 epoch 数值复用形成 ABA；
- storaged desired/applied 比较完整快照的 `(policy_generation, snapshot_epoch, canonical_hash)`；
- 每个 Part 使用6.3节完整 `EffectiveCapacityVersion`（含 gate 与 `generation_install_epoch`）和 target 比较；
- RESET 保留 `enabled=false` tombstone 和新的 `last_modified_epoch`，不能直接物理删除 CAS token；
- RESET 必须原子规范化为 `enabled=false + control_owner=OBSERVE_ONLY`；Meta 拒绝为 disabled policy 创建 MANUAL/AUTO 执行或 AUTO decision；
- Drop Space 可以在删除 Space 元数据时同批删除 tombstone；
- Graph read/write epoch、`wal_activity_epoch/storage_ingress_epoch/safety_epoch` 是未来冷热观察的本地版本，与以上管理版本完全独立。

当前 generation 中从未配置过的 Space，其初始 CAS token 定义为 `(current_generation, 0)`。

约束：

- `space_id > 0`；
- `0 < target_capacity_bytes <= INT32_MAX`；
- 实际产品上限还应远低于 `INT32_MAX`，避免现有 `int32_t size_` 计算接近溢出；
- `enabled=false` 是独立的强制 hot 条件，等价于恢复该 Part 构造时保存的原始 hot capacity；storaged 应在同一 Part policy 临界区清除/否决旧 AUTO overlay、调用 reducer 升热后才确认 RESET applied；
- `control_owner=OBSERVE_ONLY` 时 target 只展示、不执行；`MANUAL` 时 target 是持续生效的人工值；`AUTO` 时 target 只是经过审批的 cold 候选值，只有11.9节的有效 AUTO overlay 才能临时覆盖 original hot capacity；
- 同一 `policy_generation` 内 `snapshot_epoch` 只能增加；回滚也创建更大 epoch；
- 第一版 `include_listener=false`，普通 Storage Part 与 Listener 分开灰度。

### 5.2 运行状态

每个 Part 至少暴露：

```text
DEFAULT    未配置override，使用原始容量
APPLYING   新target已收到，正在写入buffer
SHRINKING  accounted size明显大于target
STEADY     accounted size位于target附近的Node粒度锯齿区间
WARMING    target已调大，但缓存尚未重新填充
ERROR      策略无法解析、对象缺失或应用失败
```

注意：`STEADY` 只表示逻辑有效窗口接近目标，不代表 jemalloc 已把页面归还给操作系统。

### 5.3 Space策略、Part执行

管理对象是 Space，实际执行对象必须是本机的每个 replica-part：

```text
space 10 target=2MiB
  -> part 1 buffer target=2MiB
  -> part 2 buffer target=2MiB
  ...
  -> part 20 buffer target=2MiB
```

这样管理体验保持 Space 级别，同时不引入一个共享 Space buffer。每个 Part 仍拥有独立 `FileBasedWal` 和 `AtomicLogBuffer`：[`RaftPart.cpp:330-372`](../src/kvstore/raftex/RaftPart.cpp#L330-L372)、[`FileBasedWal.cpp:40-60`](../src/kvstore/wal/FileBasedWal.cpp#L40-L60)。

这里的“冷”特指低非空 Raft WAL 写入活动，不等于低查询量。第一版会无差别作用于该 Space 的全部普通 Part；存在写热点分片时，必须放弃 Space 级降容或取得业务风险接受，不能用其余19个冷 Part 掩盖1个热 Part。

## 6. AtomicLogBuffer 热容量实现

### 6.1 字段与接口

当前字段是普通 `int32_t`：[`AtomicLogBuffer.h:385`](../src/kvstore/wal/AtomicLogBuffer.h#L385)。跨线程直接写会形成 C++ data race。

建议接口：

```cpp
struct CapacityChange {
  int32_t oldCapacity;
  int32_t newCapacity;
  bool changed;
};

class AtomicLogBuffer {
 public:
  StatusOr<CapacityChange> setCapacity(int64_t capacity);
  int32_t capacity() const;

 private:
  std::atomic<int32_t> capacity_{8 * 1024 * 1024};
};
```

`setCapacity()` 必须：

1. 拒绝 `capacity <= 0`；
2. 拒绝 `capacity > kProductMaxWalBufferSize`；
3. checked narrowing 到 `int32_t`；
4. 只做一次 `atomic::exchange(memory_order_relaxed)`，可靠取得审计所需的旧值；
5. 不移动 tail；
6. 不修改 `size_`；
7. 不标脏或 delete Node；
8. 返回旧值、新值和是否实际改变，供审计日志使用。

容量是独立策略标量，不发布其他对象，`memory_order_relaxed` 足够；但代码中应明确说明这一点。

当前实验指标聚合直接读取 `capacity_`：[`AtomicLogBuffer::metrics()`](../src/kvstore/wal/AtomicLogBuffer.cpp#L98-L115)。字段原子化后，所有指标、日志和测试读取也必须改用 `capacity()` 或显式 `.load()`；registry mutex 只保护 buffer 生命周期，不能替代 atomic 读写。

### 6.2 push只读取一次容量快照

`push()` 开头应做：

```cpp
const auto capacity = capacity_.load(std::memory_order_relaxed);
const int64_t projected =
    static_cast<int64_t>(size_.load(std::memory_order_relaxed)) + recSize;

if (projected > capacity) {
  // 继续执行原生淘汰路径
}
```

一次 push 内不能多次读取可能变化的容量，否则同一条记录可能在判断过程中使用两个阈值。热更新恰好并发时，允许这一条 push 使用旧 target；下一条自然收敛。

将加法提升到 `int64_t` 是为了避免现有 `int32_t size + recSize` 靠近上界时发生有符号溢出。它不改变正常范围内的淘汰语义。

### 6.3 分层转发

基础 Space 策略的调用链建议明确命名为 `applyBase...`，避免与后续 AUTO overlay 混成一个状态：

```text
NebulaStore::applyBaseWalBufferPolicy(spaceId, effectiveVersion, baseTarget)
  -> Part::applyBaseWalBufferCapacity(effectiveVersion, baseTarget)
  -> RaftPart::applyBaseWalBufferCapacity(effectiveVersion, baseTarget)
  -> FileBasedWal::setBufferCapacity(target)
  -> AtomicLogBuffer::setCapacity(target)
```

策略版本和本机 activation gate 不能在转发链中丢失。第一版的复合 key 为：

```text
EffectiveCapacityVersion {
  gate_epoch
  desired_gate_enabled
  generation_install_epoch
  policy_generation
  space_last_modified_epoch
  policy_enabled
  space_control_owner
}
```

每个普通 Part/WAL 必须在一把仅保护策略状态的小锁内保存两个**独立版本域**：

```text
BasePolicyState {
  effective_version
  policy_enabled
  base_target
}

ActiveAutoOverlay {                 # 详细字段见11.9.1
  decision_ordinal
  commit_hash
  cold_target
  lease/fence/guard identity
}
```

`applyBaseWalBufferCapacity(effectiveVersion,baseTarget)` 只更新 `BasePolicyState`，在同一临界区内完成：

1. 先比较 `gate_epoch`：更小的任务一律拒绝；
2. `gate_epoch` 相同但 `desired_gate_enabled` 不同：一致性冲突，拒绝；
3. `desired_gate_enabled=false` 时，target 必须是 original capacity，即使 policy 版本未变化也允许升容；
4. 再比较 host-local 单调 `generation_install_epoch`：更小的任务拒绝；
5. install epoch 相同但 `policy_generation` 不同：一致性冲突，拒绝；
6. gate启用、generation相同时，再比较 Space 的 last-modified epoch、`policy_enabled` 和 `space_control_owner`；同一 version 下 enabled/owner 不同属于一致性冲突；disabled 必须同时是 `OBSERVE_ONLY`，否则拒绝；
7. 完整复合 key 和 base target 全相同：幂等成功，可用于崩溃后的补应用；
8. 完整复合 key 相同但 base target 不同：一致性错误，拒绝并告警。

每次基础策略或 AUTO 状态变化后，都必须在**同一把 Part policy lock** 内调用唯一 reducer：

```text
recomputeEffectiveCapacityLocked()
  gate关闭                                -> originalHotCapacity
  gate开启但policy_enabled=false           -> originalHotCapacity
  gate开启且owner=OBSERVE_ONLY            -> originalHotCapacity
  gate开启且owner=MANUAL                  -> BasePolicyState.base_target
  gate开启且owner=AUTO且overlay本地有效    -> ActiveAutoOverlay.cold_target
  gate开启且owner=AUTO但无有效overlay      -> originalHotCapacity
  最后才调用AtomicLogBuffer::setCapacity(effectiveTarget)
```

因此基础 reconcile 只改 base state，`applyCold()/fail-hot/lease` 只改 AUTO overlay、guard 和 fence；它们都不得直接把“最终 target”写回另一个版本域。owner=AUTO 时基础状态即使已经以版本 `E` 记录了 `originalHotCapacity`，同一个 `E` 下仍可由决策 `D` 安装 cold overlay；周期性重放 `E` 也只会幂等更新 base，不会覆盖或与 `D` 冲突。相反，RESET/disabled、gate、MANUAL/owner 更新会在同一锁内先失效 overlay/fence，再由 reducer 立即压过 overlay。不能再使用 `(EffectiveCapacityVersion, 最终target)` 作为唯一幂等键，否则基础 hot target 与同版本 AUTO cold target 必然互相冲突。

这里的基础策略只决定控制权和允许使用的容量：policy disabled、gate关闭或 owner 为 `OBSERVE_ONLY` 时 base target 必须是 `originalHotCapacity`；owner 为 `MANUAL` 时 base target 是人工 target；owner 为 `AUTO` 时要求 policy enabled=true，base target 仍是 `originalHotCapacity`，只有11.9节同时满足 decision、scope、prepare token 和 lease 的 AUTO overlay 才允许 reducer 选择 cold target。不能把“Meta 已持久化一个 AUTO 候选容量”直接解释成 Part 已获准降容。

`policy_generation` 可以是 UUID，本身不可排序。策略管理器每次通过当前 Meta leader 的 rebase 握手接受新 generation 时，必须先持久化递增的 `generation_install_epoch` 及其 generation 绑定，再发布/应用快照；所有 Part 调用都携带该本地 epoch。迟到的旧 generation 调用因此携带更小 install epoch，并在 Part 策略锁内被拒绝。进程重启后继续使用持久值，不能重新从0开始。

这些规则可同时关闭旧 target、旧 gate 和旧 generation 的 check-then-store 窗口。该策略锁不进入 Raft/WAL 链表临界区，也不能在持有时获取 NebulaStore 或 Meta 锁。

Phase 3 若启用自动执行，还要把 Graph read/write epoch、`wal_activity_epoch/storage_ingress_epoch/safety_epoch` 和 HOT/COLD mode 纳入 effective-capacity 状态机；第一版不预留一个含义模糊的单一 revision 来同时处理这些版本域。

`FileBasedWalPolicy::bufferSize` 当前是构造期策略并保存在 const `policy_`：[`FileBasedWal.h:265`](../src/kvstore/wal/FileBasedWal.h#L265)。热更新后它只能表示 initial capacity；HTTP/metrics 必须从 `AtomicLogBuffer::capacity()` 获取当前有效 target，不能继续展示 `policy_.bufferSize`。

构造路径也必须使用与热 setter 相同的范围校验和 checked narrowing。`FileBasedWalPolicy::bufferSize` 的类型是 `size_t`：[`FileBasedWal.h:20-31`](../src/kvstore/wal/FileBasedWal.h#L20-L31)，而创建 buffer 时会传入 `int32_t`：[`FileBasedWal.cpp:40-60`](../src/kvstore/wal/FileBasedWal.cpp#L40-L60)。只校验热更新、继续允许启动期隐式窄化是不完整的。

每个 Part 还应保存 `originalHotCapacity`，用于 reset policy。这个值来自 Part/WAL 构造时的有效全局或静态配置，不能在回滚时临时读取一个可能已经变化的全局 flag。

## 7. P0：push淘汰与reader GC并发硬化

### 7.1 现有潜在窗口

当前 `push()` 在容量超限后大致执行：

```text
old tail markDeleted=true
  -> firstLogId更新
  -> tail_.store(prev) 发布新tail
  -> 继续读取 oldTail->size_
  -> dirtyNodes++
```

源码：[`AtomicLogBuffer::push()`](../src/kvstore/wal/AtomicLogBuffer.cpp#L144-L167)。

与此同时，最后一个 Reader 可能在 [`releaseRef()`](../src/kvstore/wal/AtomicLogBuffer.cpp#L220-L268) 中：

1. 读到已经发布的新 tail；
2. 发现此前 dirty 数已超过阈值；
3. 从 `newTail->next_` 开始删除 dirty 链；
4. 删除 Writer 尚未完成读取的 old tail。

结果可能是 UAF、dirty 计数下溢或 CHECK 失败。本文将其定义为“源码审计发现的潜在竞态”，不是已经由本次生产实验复现的根因。

正常稳态大约每创建一个新 Node 才需要淘汰一个旧 Node；热降容时 `size_ >> target`，几乎每个非 new-head push 都会进入该分支，因此必须先加固。

### 7.2 推荐线性化方式

更小且可证明的修复是把 `tail_.store(prev, release)` 作为淘汰操作的**最后线性化点**：

```text
CAS标记old tail
  -> 缓存prev和oldSize
  -> 更新firstLogId、size、dirty计数
  -> 最后发布tail=prev
  -> 发布后绝不再解引用old tail
```

两种 GC 交错都可闭环：

1. `releaseRef()` 在新 tail 发布前捕获 old tail：即使 GC 已启动，它只会从 `capturedOldTail->next_` 删除更老的 dirty 链，不会删除 old tail；Writer 仍可安全完成计费。
2. `releaseRef()` 在新 tail 发布后捕获 prev：此时 Writer 已完成 old tail 的全部读取和计数，发布后不再解引用它，因此 GC 可以安全删除 old tail。

这不改变哪个 Node 被标脏、容量判断、firstLogId、dirty 阈值和 GC 删除范围，只修正“发布可回收性”和“最后一次对象访问”的先后关系。现有注释要求若干操作不能重排；实现时必须更新注释并用确定性交错测试证明新的线性化顺序，不能随意交换原子操作。

备选方案是为实际淘汰路径增加 RAII synthetic ref：只在 `tail != head` 且准备标脏时、在发布新 tail 前取得，覆盖最后一次 old tail 解引用和全部 size/dirty 计数，并保证所有出口释放。它也能闭环，但 guard 释放可能在 WAL Writer 线程同步触发长链 GC，改变 GC 时机和 append P99；若采用该方案，必须额外观测 GC duration、单次 deleted nodes 和最大批次。因此第一版优先采用“发布 tail 为最后线性化点”。

### 7.3 必须通过的确定性交错测试

至少覆盖：

1. 初始已有6个 dirty Node；
2. 一个真实 Reader 持有旧区间；
3. Writer 开始标记新 tail；
4. Reader 分别在 tail 发布前、发布后释放；
5. 验证无 UAF、无重复 delete、无 dirty 下溢；
6. 覆盖 Reader 已在 Writer 开始前完成 `refs:1->0` 并启动 GC 的分支；
7. 若采用 synthetic ref，验证 guard 释放能正常触发 GC，并量化 Writer 同步 GC 的最长耗时；
8. ASan、TSan 和重复压力测试均通过。

## 8. 热更新的精确语义

### 8.1 降低容量

`8MiB -> 2MiB` 的 setter 成功只表示 target 已更新。它不会立即遍历或删除旧 Node。

后续每次原生 `push()`：

- 发现 `size + recSize > target`；
- 每次最多标脏一个完整旧 Node；
- new-head 分支仍会在容量检查前返回；
- `tail == head` 时仍不能淘汰唯一 Node；
- 物理 delete 仍等待 reader-ref GC。

因此降低容量是异步、渐进、最终收敛，而不是同步 shrink。

### 8.2 增大容量

`2MiB -> 8MiB` 会让后续 push 使用更大的阈值，但：

- 已经 delete 的 Node 不会复活；
- 已经 markDeleted 的 Node 不会取消标记；
- 不会从磁盘主动预热；
- 已触发的磁盘读取和 Snapshot 无法撤销；
- 只有未来日志会逐渐把窗口重新填大。

因此升容后的状态应显示为 `WARMING`，不能把它描述为即时回滚缓存内容。

### 8.3 没有后续push

没有 push 就不会收缩。当前周期空 NORMAL 日志会持续驱动冷 Part 收敛；如果未来上游修复停止空日志，本方案的自动收缩也会停止。

这是一项明确依赖：

> 本方案适合当前“不改变空日志机制”的约束；它不是独立于未来上游行为的通用主动回收器。

### 8.4 快速重复更新

同一 generation 内，每次策略快照都携带单调 `snapshot_epoch`，目标 Space 同时更新自己的 `last_modified_epoch`：

```text
snapshot 10 / space last_modified=10: 8MiB -> 2MiB
snapshot 11 / space last_modified=11: 2MiB -> 8MiB
```

Meta/管理面用目标 Space 的 `(expected_policy_generation, expected_last_modified_epoch)` 做 CAS；storaged 的每个 Part 使用上一节唯一的 `applyBaseWalBufferCapacity(effectiveVersion,baseTarget)`。迟到的 epoch 10 即使已经排队，也会在同一策略临界区被 epoch 11 拒绝。

同一 host 还应只有一个串行 single-flight apply worker：回调只发布最新不可变 `{generation,snapshot_epoch,map,hash}` 并唤醒 worker；旧 job 在每批 Part 前发现 desired 已变化就停止，由唯一 worker重放最新版。host applied 只有在 desired 快照没有再次变化、当前所有目标 Part 都匹配该快照后才能推进。相同 epoch/hash 必须允许幂等 reconcile，不能因为“epoch 没增大”而跳过半应用恢复；相同 epoch 但 hash 不同必须报一致性错误。

回滚也必须提交新的更大 epoch，不能依赖普通 last-write-wins。无论 target 如何更新，已经标脏或删除的缓存都不可逆。

### 8.5 大批次边界

目标容量至少应覆盖现网 p99 单个 Raft 批次的逻辑 charge，并留出余量：

```text
target >= p99 Σ(sizeof(ClusterID) + sizeof(TermID) + payload.size)
```

当前 Leader 批次上限为256条。若一个批次的累计 charge 已超过 target，批首可能在本批复制或提交完成前离开内存，并立即转磁盘 iterator。它不破坏正确性，但可能显著增加 commit P99 和磁盘 IO。

## 9. Part生命周期与并发应用

### 9.1 NebulaStore对象保护

`spaces_` 和 `spaceListeners_` 由 [`NebulaStore::lock_`](../src/kvstore/NebulaStore.h#L868-L883) 保护。新增 Part 也在这把写锁下进行：[`NebulaStore::addPart()`](../src/kvstore/NebulaStore.cpp#L437-L479)。

安全应用顺序：

1. single-flight worker 接收并验证不可变 `{generation,snapshot_epoch,map,hash}`；
2. 获取 `NebulaStore` 写锁，拒绝陈旧 generation/epoch/hash；
3. 在同一锁域替换不可变 policy map，并复制目标 Part 的 `shared_ptr`；
4. 释放 store 锁后发布本地 desired snapshot identity；
5. 逐个调用只含 atomic store 的 setter；
6. 记录成功、缺失和过期任务数量；
7. desired 未变化且全部目标 Part 对齐后，更新 host applied snapshot identity。

禁止：

- 保存 raw `Part*` 或 `AtomicLogBuffer*`；
- 持有 `NebulaStore::lock_` 再等待每个 `raftLock_`；
- 在 MetaClient heartbeat 线程中同步遍历和更新上千 Part；
- 将部分成功误报为整个 Space 已收敛。

### 9.2 新建Part

策略 map 必须是进程内长期状态。新 Part 在构造时主动读取当前 map，但 MANUAL 与 AUTO 的继承规则不同：

```text
MANUAL且gate启用 -> 当前人工target
OBSERVE_ONLY/gate关闭 -> original hot capacity
AUTO -> 默认original hot capacity；仅精确命中当前未过期decision scope/token时才允许cold
```

不能只依赖“配置变化回调遍历现有 Part”，否则配置值没有再次变化时，后来新增的 Part 会漏掉策略。

安全顺序应保证：

- 策略先发布、后创建的 Part 直接读取新 generation/epoch；
- 策略发布前创建的 Part 会被本轮 shared_ptr 快照覆盖；
- 即使 Part 随后被删除，局部 shared_ptr 也能保证 setter 调用期间对象有效。

启动恢复还有一条独立路径：[`NebulaStore::loadPartFromDataPath()`](../src/kvstore/NebulaStore.cpp#L234-L274) 会在后台、store 锁外构造并启动 Part，之后才插入 map；而 [`newPart()`](../src/kvstore/NebulaStore.cpp#L484-L515) 在插入前已经注册并启动 Raft。若只扫描 map，可能出现“按旧 policy 构造、更新扫描时尚未插入、最后带旧 target 插入”的漏配窗口。

第一版必须同时满足：

1. 本地 last-known-good policy 在 NebulaStore 开始加载 Part 前完成读取；
2. 热 apply worker 在启动 Part 扫描完成后才对外报告 applied；
3. Part 构造前读取一次 policy，在插入 map 的线性化点再读取一次最新 effective version，并通过幂等 `applyBaseWalBufferCapacity(effectiveVersion,baseTarget)` 补齐；AUTO overlay 只能按11.9节的 exact Part instance/scope/token另行安装；
4. 启动期间收到的新 desired snapshot 在加载屏障结束后做一次全量重放；
5. host applied snapshot 只有在加载屏障完成、当前 map 内所有目标 Part 都达到对应 policy version 后才能发布。

新建、balance迁入、重建或重启恢复的 Part 会得到新的 `part_instance_id`。旧 AUTO decision 的 application scope 绑定旧实例，绝不能让新 Part 仅凭 Space policy 直接继承 cold target；它必须 hot 启动、使旧 decision 失效，并由新 topology 下的完整 Graph+Storage collection round重新授权。只有显式 MANUAL policy 可以按上述线性化规则直接继承。

### 9.3 Listener

Listener 同样拥有独立 Raft/WAL，并可能继承 `RaftPart`。若在 `RaftPart` 基类中按 `spaceId` 无差别应用，Listener 会同步缩小缓存，可能增加外部索引 apply 延迟和磁盘回退。

第一版建议：

- 普通 Storage Part 应用 per-space target；
- Listener 保持原始容量；
- 指标明确区分 ordinary part 与 listener；
- Listener 经过独立追赶、Snapshot 和外部索引验证后再增加 `include_listener=true` 能力。

## 10. 控制面与持久化

### 10.1 不能只改现有gflag

现有 `/flags` PUT 只调用 `gflags::SetCommandLineOption()`：[`SetFlagsHandler.cpp:83`](../src/webservice/SetFlagsHandler.cpp#L83)。它不会自动更新已经构造的 `AtomicLogBuffer`，也没有 per-space applied ack。

现有通用配置键只包含 `(module, name)`，没有 `spaceId`：[`MetaKeyUtils::configKey()`](../src/common/utils/MetaKeyUtils.cpp#L934-L943)。虽然 `ListConfigsReq` 带有 `space` 字段，但 [`ListConfigsProcessor::process()`](../src/meta/processors/config/ListConfigsProcessor.cpp#L11-L35) 只按 module 前缀列举，没有使用该字段。因此不能把现有 `UPDATE CONFIGS` 直接描述成通用的 per-space 控制面。

storaged 默认 `local_config=true`：[`StorageServer.cpp:40`](../src/storage/StorageServer.cpp#L40)，并把它传成 `skipConfig_`：[`StorageServer.cpp:194`](../src/storage/StorageServer.cpp#L194)。此时 [`MetaClient::loadCfg()`](../src/clients/meta/MetaClient.cpp#L3147-L3183) 会跳过 Meta 配置加载。

不能仅为了本功能把生产集群改成 `local_config=false`，因为 Meta 中其他历史配置也可能同时覆盖本地值。

### 10.2 第一阶段：本机持久化策略＋热应用

仅用于实验、单节点 canary，或生产已经具备可靠配置管理系统时，第一阶段可以采用：

```text
每台storaged本机持久化的override文件
  -> 原子rename更新
  -> 策略worker解析和校验
  -> host-local generation/epoch/hash
  -> live Part版本保护的capacity apply
  -> applied状态
```

要求：

- 在目标文件同一文件系统创建临时文件，完整写入并校验后执行 `fsync(temp fd) -> rename -> fsync(parent directory)`；
- 解析失败保留 last-known-good，不自动 reset；
- 文件中保存 generation、epoch、canonical hash 和完整 map，不使用增量补丁；
- 同时持久化 host-local `generation_install_epoch` 及其 generation 绑定，重启后保持单调；
- 三台 storaged 可以不同步灰度，但必须显示每台 desired/applied hash；
- 管理接口只允许 localhost 或受控管理网访问；
- 重启后先读取策略，再开始加载/创建 Part。

这一阶段无需修改 Meta/Graph wire，适合验证 Atomic 热容量和性能，但运维需要负责三台配置一致性。

本地文件不是集群真源。如果没有统一分发、原子落盘、三节点一致性校验和审计能力，不应把三台机器分别维护的文件用于正式生产热更新；这种情况下，第一版应退回静态 per-space override 加滚动重启，或直接完成下一节的 Meta 控制面。

本地 epoch 属于 host-local 命名空间，三台机器上的数字不能直接比较。若现有生产配置管理系统承担权威源，它必须提供单一 artifact 的 generation/CAS、相同 full-map hash、幂等重投和审计；否则该阶段严格限于实验或单机 canary。

### 10.3 产品化阶段：独立的Meta Space策略

产品化后建议新增独立 Meta key，而不是把字段塞进 `SpaceDesc` 或复用普通 `(module, name)` ConfigItem：

```text
__space_wal_buffer__<spaceId>
  -> {target_capacity_bytes, enabled, control_owner, include_listener, last_modified_epoch}

__space_wal_buffer_snapshot_meta__
  -> {policy_generation, snapshot_epoch, canonical_hash}
```

原因：

- 现有 Config key 不包含 `spaceId`；
- 整张 map 更新容易发生并发覆盖；
- 需要逐 Space CAS 和全局一致快照；
- 需要独立 backup、restore 和 drop-space 生命周期；
- 旧版本对 `SpaceDesc` 未知字段的重新序列化可能丢失扩展字段。

建议管理语义：

```text
SET SPACE WAL BUFFER <space> <bytes> OWNER MANUAL EXPECT GENERATION <g> LAST_MODIFIED_EPOCH <n>
SET SPACE WAL BUFFER <space> <approved-cold-bytes> OWNER AUTO EXPECT GENERATION <g> LAST_MODIFIED_EPOCH <n>
SET SPACE WAL BUFFER <space> OWNER OBSERVE_ONLY EXPECT GENERATION <g> LAST_MODIFIED_EPOCH <n>
RESET SPACE WAL BUFFER <space> EXPECT GENERATION <g> LAST_MODIFIED_EPOCH <n>
SHOW SPACE WAL BUFFER <space>
SHOW SPACE WAL BUFFER STATUS <space>
```

Meta 返回必须区分：

```text
desired persisted
部分storaged applied
全部目标storaged applied
各host正在shrinking或warming
```

Meta heartbeat 可以携带 optional desired generation/epoch/hash；storaged 通过**独立于普通 gflags 配置**的专用接口拉取同一读快照下生成的 `{generation,epoch,full map,hash}`，应用后在 heartbeat 回报状态。该路径不得调用或依赖 `MetaClient::loadCfg()`、`skipConfig_`、`localCfgLastUpdateTime_`，生产可以继续保持 `local_config=true`。字段缺失或旧 metad 响应不能解释为 reset，必须保留 last-known-good。

Meta processor 必须在 `LockUtils::lock()` 写锁内完成“读取目标 Space token、同时比较 expected generation 与 epoch、分配新 snapshot epoch”，再把 Space record/tombstone 与 snapshot meta 用同一个 Meta Raft KV batch 提交。快照读取在同一读锁下返回 full map 和 canonical hash。Drop Space 时同批清理对应 policy；RESET 则保留 tombstone。新 key 前缀还必须纳入 Meta backup/restore。`applied` 只表示本机 map 已发布、当时存在的目标 Part 已执行 setter、以后新增 Part 会读取该 map；它不表示 Node 或 RSS 已经收敛。

### 10.4 快照版本、崩溃与恢复一致性

一次策略更新应先持久化 desired，再异步 apply：

```text
Meta或本地文件持久化 generation 7 / snapshot epoch 21 / hash H21
  -> storaged A应用一半后崩溃
  -> 重启读取同一快照
  -> 所有新建Part直接使用目标
  -> 幂等重放到现有Part
  -> applied更新为(7,21,H21)
```

不能先修改 live buffer、后持久化目标，否则进程崩溃后会静默恢复旧值。相同 generation/epoch/hash 的重放必须允许补齐半应用，不能因版本“没有变大”而整体跳过。

Meta backup 的时间点回退还会让 `snapshot_epoch` 倒退。恢复流程必须生成新的 `policy_generation`，在当前 Meta leader 身份下发布强制 full snapshot；storaged 经当前集群握手确认新 generation 后，先原子持久化递增的 host-local `generation_install_epoch` 与 generation 绑定，再进行完整 reconcile。所有旧 generation 的在途任务保留旧 install epoch，因而可在 Part 锁内确定拒绝。

rebase 不能只遍历新 map：较早 backup 可能根本不含旧 generation 中后来创建的 override。安全顺序必须与9.1节使用同一个线性化点：

1. 在 `NebulaStore` 写锁内保留 old map，计算受影响 Space 集合 `old enabled overrides ∪ new policies`；
2. 在同一锁域原子发布 new map/新 gate-generation，并抓取这些 Space 当前全部普通 Part 的 `shared_ptr`；
3. 释放 store 锁后，对抓到的 Part 应用新 policy；对新 map 中消失的旧 override，使用新 gate/generation 的 synthetic reset 恢复 `originalHotCapacity`；
4. 发布后新增的 Part 直接按 new map 构造，发布前已插入的 Part 一定在 shared_ptr 快照中；
5. 最后重新核验 desired identity 未变化、受影响 Part 全部对齐，才推进 host applied。

gate enable/disable 也沿用这一顺序，不能先在锁外 reset、后发布新 gate/map。若产品不实现 generation rebase，则必须明确“不支持带该 policy 的时间点回退”，恢复后由运维清除各 host last-known-good 并重建策略；仅把 key 纳入 backup 不足以保证一致性。

### 10.5 混合版本与功能开关

容量是本地 cache 策略，新旧 storaged 或三个副本暂时使用不同 target，不改变 Raft wire、日志内容或 quorum；但磁盘 fallback 和 Snapshot 性能可能不对称。

产品控制面应按以下顺序演进：

1. 新 Meta/Storage Thrift 字段只追加 optional 字段；
2. 先升级全部 metad；
3. 若提供 Graph 管理语法，再升级全部 graphd；在三 graphd 全部升级前禁用新语句，或将管理流量明确路由到已升级 graphd/专用 Meta admin API；
4. 再升级 storaged，新二进制默认 feature gate 关闭；
5. gate 关闭的节点报告 `supported but disabled`，不能冒充 applied；
6. 全量部署关闭状态验证完成后，才启用一个 storaged canary；
7. 旧 metad 或缺失 optional 字段只表示“不支持/未知”，绝不能解释成 reset；
8. 回滚旧二进制前，先停用新管理入口、用更大 epoch 恢复 original target、等待 applied，再按 storaged -> graphd -> metad 逆序回滚并清理旧版本不认识的配置。

把控制字段加入 Meta heartbeat 是向后兼容的管理面扩展，不是 Raft AppendLog/Heartbeat 协议变化；验收时必须把两者区分开。

为了真正支持“只启用一台 storaged”，还需要独立、持久的 per-host activation gate，且不能复用 `local_config`：

```text
DISABLED   拉取并缓存desired，但所有Part保持original target；报告supported_disabled
ENABLING   重放最新完整快照
ENABLED    可报告applied
DISABLING  先把所有Part恢复original target并确认，再进入DISABLED
```

每次启用或停用请求先持久化一个更大的 `gate_epoch`。Part key 保存6.3节完整 `EffectiveCapacityVersion`：`(gate_epoch, desired_gate_enabled, generation_install_epoch, policy_generation, last_modified_epoch, policy_enabled, space_control_owner)`；`ENABLING/DISABLING` 是 host 聚合状态，不进入 Part key，也无需定义同 epoch 下的四态排序。全部 Part 对齐 `desired_gate_enabled=true/false` 后，host 才分别切换为 `ENABLED/DISABLED`。这样迟到的旧启用任务和旧 generation 都无法覆盖后来的禁用/升容。关闭 gate 只恢复 target，已经淘汰的 Node 仍不会自动预热。

gate 默认 `DISABLED`，由每台 storaged 的专用本地状态文件和受控管理接口维护，使用与10.2节相同的原子落盘协议并记录操作者/时间/旧值/新值；不能仅使用不持久的 `/flags` PUT。中央 Space policy 负责“希望哪些 Space 使用多大容量”，本地 gate 负责“本机是否参与”，两者职责不能混用。

Heartbeat 状态至少携带 `process_instance_id/boot_id`、capability、`gate_epoch/gate_state`、ready、desired generation/epoch/hash 和 `applied_gate_epoch`/generation/epoch/hash。storaged 首次 heartbeat 可能早于 kvstore 构造完成（初始化顺序见 [`StorageServer.cpp:236-257`](../src/storage/StorageServer.cpp#L236-L257)），因此新进程先报告 `INITIALIZING` 且没有 applied；Meta 对同一 boot_id 拒绝较小 gate epoch，只统计当前活跃、属于目标集合、capability/gate匹配且 boot_id 对应本进程的 ack，不能沿用相同 HostAddr 上一个进程的陈旧 applied。

## 11. 图空间冷热观察与可选自动决策

### 11.1 交付边界：观察是必选项，自动改容量是后续项

本方案必须包含冷热观察，否则“目标 Space 的全部普通 Part 已经冷”无法形成可审计证据。交付应拆为两个互相隔离的能力：

1. **只读观察器**：首个版本即提供，默认 `OBSERVE_ONLY`，只采集、聚合和解释冷热，不调用 capacity setter；人工审核 Space 时必须以该结果为主要依据。
2. **自动决策器**：只有观察器经过至少一个完整业务周期的 shadow 验证后，才作为后续可选能力逐 Space 启用。

第一批生产版本仍由运维人工下发 target，这样可以单独验证 atomic capacity、push/GC 并发硬化、磁盘 fallback、Snapshot、控制面版本和回滚语义。观察器即使判断为冷，也只输出建议；不得在第一阶段自动修改容量。

### 11.2 “冷热”必须拆成三种信号

OSS 3.6 当前没有一个现成指标能够证明“整个 Space 业务空闲且适合缩小 WAL cache”；进程级 operation counter、WAL mtime、RSS 和 Meta 的 Space 状态都不足以单独授权降容。不能用一个 `last_active` 或一个不透明分数替代证据链，至少拆成：

| 维度 | 精确定义 | 与 WAL buffer 的关系 | 决策用途 |
|---|---|---|---|
| `WAL_WRITE_HEAT` | storaged 本地 replica 成功接受的非空 Raft WAL record | 直接反映 Atomic cache 中是否持续进入有业务/管理 payload 的日志 | 冷判定的权威主信号 |
| `GRAPH_BUSINESS_HEAT` | graphd 已解析/验证的业务意图和实际 StorageClient 读写 dispatch | 单次 logical dispatch 不被 host/Part fan-out 放大；上层/客户端重试仍是新 attempt | 业务冷热的主观测面和自动降容的性能 gate |
| `STORAGE_RPC_HEAT` | storaged 收到的全部 Graph Storage 或商用内部/直连 RPC | 可发现绕过 graphd 的入口，但包含正常Graph fan-out、重试和失败 | 全量兜底与交叉验证；出现即保守判热 |
| `SYSTEM_BUSY` | Snapshot、ingest、restore、rebuild、balance、追赶、拓扑或角色不稳定 | 可能绕过普通 WAL，或使小缓存更容易走磁盘/Snapshot | 直接否决自动转冷 |

```mermaid
flowchart LR
    Q["客户端逻辑查询"] --> G["graphd业务热度观察器"]
    G --> GD["per-Space逻辑读写窗口"]
    A["成功的本地WAL append"] --> S["storaged per-replica观察器"]
    B["Storage直连/内部RPC"] --> S
    C["Snapshot/Admin/Balance/Raft状态"] --> S
    S --> SD["per-Part WAL与安全窗口"]
    GD --> M["Meta Leader完整collection round"]
    SD --> M
    M --> F["Graph业务证据 + Storage安全证据"]
    F --> H["OBSERVE_ONLY查询/告警"]
    F --> I["人工审批MANUAL策略"]
    F -.->|shadow通过后才允许| J["显式启用的AUTO决策"]
    J --> K["per-Space target policy"]
```

因此本文中的“冷 Space”不是简单的“用户没有查询”，而是：

```text
storaged WAL写观察证据完整且满足冷窗口
  + 全部预期graphd的业务请求观察完整且满足冷窗口
  + storaged未发现绕过graphd的请求热度
  + 无后台管理/恢复/拓扑忙碌状态
  + 所有预期普通replica-part均有新鲜报告
```

读热点不属于 Raft correctness 风险，但为了最小化性能回归，首个可执行的 AUTO 版本应默认让 `READ_HOT` 或 `READ_UNKNOWN` 阻止降容。人工模式可以在经过专项压测后显式接受“只读热点、WAL 写冷”的 Space。

### 11.3 正式状态模型：Part观察与Space控制分域

不能把证据有效性、WAL温度、请求热度、安全状态和控制权塞进一个枚举。Storage 与 Graph 的观测粒度不同，必须分别建模。

每个 storaged 普通 replica-part 维护：

```text
ObservationValidity = UNKNOWN(reason_mask) | OBSERVING | VALID
WalTemperature      = HOT | COOLING | COLD
StorageIngressGuard = STORAGE_HOT | STORAGE_COLD | STORAGE_UNKNOWN
SafetyGuard         = SAFE | BUSY(reason_mask) | SAFETY_UNKNOWN(reason_mask)
```

每个预期 graphd 实例对每个 Space 维护：

```text
GraphObservationValidity = GRAPH_UNKNOWN(reason_mask) | GRAPH_OBSERVING | GRAPH_VALID
GraphDemand               = GRAPH_HOT | GRAPH_COOLING | GRAPH_COLD
```

graphd 是 Space 级逻辑业务观察，不伪造 per-replica 状态；storaged 是 replica-part 级物理事实，不把 fan-out RPC 冒充逻辑查询 QPS。`SpaceControlOwner = OBSERVE_ONLY | MANUAL | AUTO` 是 **Space policy 域**，不属于任何 graphd 或 replica 的温度。集群级还维护独立的 Space decision state：

```text
NO_DECISION | COLD_PREPARING | COLD_APPLYING
            | CANARY_COLD_APPLIED | COLD_APPLIED
            | RECOVERY_REQUIRED | HOT_RECOVERING | ABORTING
```

只有以下条件同时成立，某个 Storage Part 才能为 Space 自动判冷提供一份有效资格：

```text
ObservationValidity == VALID
&& WalTemperature == COLD
&& StorageIngressGuard == STORAGE_COLD
&& SafetyGuard == SAFE
&& health/topology/lag检查通过
```

Graph 侧还必须满足：全部**显式注册的预期 graphd 实例**在同一 collection round 提供新鲜、同配置版本的报告，该 Space 的 `GraphObservationValidity=GRAPH_VALID`、`GraphDemand=GRAPH_COLD`、活动查询数为零。最终只有 Graph 条件、所有预期 Storage Part/replica 资格、Meta 作业/拓扑条件、`policy.enabled=true` 和 `SpaceControlOwner=AUTO` 同时成立，协调者才可发起 `COLD_PREPARING`。任一证据缺失或 policy disabled 都必须得到 `eligible_for_cold=false`，不能把“没有收到指标”解释成“没有流量”。

如果 AUTO 已经应用 cold target，任一 Storage Part 出现非空 WAL、进入 UNKNOWN/BUSY 或发现 Storage ingress 热度时，本 Part 必须先恢复 original/hot target，并使当前 Space auto decision 失效；协调者随后将整个 Space 异步恢复 hot。Graph 新请求只能立即使**本机 Graph observation token**失效并异步通知 Meta，不能对集群 decision 提供跨进程线性化保证；通知与并发 COLD commit 存在短窗口。因此第一版还要求 storaged ingress 在处理当前目标 Part 前，以与 `applyCold()` 相同的 per-part policy lock 先增加epoch并恢复hot，非空 WAL hook再在 `push()` 前兜底。已经淘汰的缓存不会立即预热，但不能继续按冷容量淘汰。`MANUAL` 模式下观察器只报告和告警，不能覆盖人工 target；`OBSERVE_ONLY` 永远不能调用 setter。

### 11.4 WAL写热度的权威采集点

最强的 WAL 活动信号位于 [`FileBasedWal::appendLogInternal()`](../src/kvstore/wal/FileBasedWal.cpp#L442-L500)：

```text
write本地WAL文件
  -> 可选fsync
  -> 更新WAL文件元数据和first/last LogID
  -> 记录冷热活动
  -> AtomicLogBuffer::push()
```

具体应在 [`FileBasedWal.cpp:489-499`](../src/kvstore/wal/FileBasedWal.cpp#L489-L499) 完成元数据更新之后、调用 `logBuffer_->push()` 之前执行 O(1) 回调。`FileBasedWal` 已保存 `spaceId_` 和 `partId_`：[`FileBasedWal.h:260-265`](../src/kvstore/wal/FileBasedWal.h#L260-L265)。回调由 `RaftPart` 构造时注入到 per-part heat controller，热路径不得查询全局 policy map 或 Meta。

采集语义必须写准确：

- `!msg.empty()`：本地 replica 成功接受一条非空 Raft WAL record，增加 `wal_activity_epoch`、record 总数和 payload bytes；
- `msg.empty()`：只增加 empty-record 计数，用于解释空日志增长斜率，不重置非空活动冷却时间；
- append 失败不计成功活动；
- 该成功不等于 quorum commit，也不保证已经 apply；默认 `wal_sync=false` 时也不等于介质已经完成 fsync；
- 日志后来 rollback 时不撤销此次热活动，保守地多判热不会造成误降容。

成功 hook 之外还必须有 WAL health hook：[`FileBasedWal.cpp:446-455`](../src/kvstore/wal/FileBasedWal.cpp#L446-L455) 的 gap/preprocessor拒绝在返回前调用 `onWalAppendRejected(reason)`；[`FileBasedWal.cpp:480-488`](../src/kvstore/wal/FileBasedWal.cpp#L480-L488) 的短写会终止进程并通过新 boot进入UNKNOWN，fsync告警则必须同步设置 `WAL_APPEND_ERROR`/`safety_epoch`，不能一边记录错误一边继续把观察证据标成 VALID。失败不增加成功热度，但会否决自动判冷，直到明确恢复并重新完成观察窗口。

Leader 写 WAL 的路径见 [`RaftPart::appendLogsInternal()`](../src/kvstore/raftex/RaftPart.cpp#L874-L915)，Follower 写 WAL 的路径见 [`RaftPart::processAppendLogRequest()`](../src/kvstore/raftex/RaftPart.cpp#L1757-L1781)。健康并完成复制时，一条逻辑写通常被多个、最终可能被全部副本观察；部分复制、换主和rollback时不保证如此，因此集群汇总不能把 RF=3 的 record 数直接相加当成业务写次数。

空 payload 也不能无条件命名为“heartbeat no-op”。周期空 NORMAL 的来源确实是 [`RaftPart::sendHeartbeat()`](../src/kvstore/raftex/RaftPart.cpp#L2041-L2049)，但 `LogType` 只存在于 [`RaftPart::appendLogAsync()`](../src/kvstore/raftex/RaftPart.cpp#L786-L825) 的内存队列，WAL 层只保留 payload；而 [`Part::sync()`](../src/kvstore/Part.cpp#L115-L117) 也会写空 COMMAND。冷热观察可以把所有空 record 排除在“非空 WAL 热度”之外，但展示字段必须叫 `empty_wal_records`，不能叫 `heartbeat_count`。

非空 record 也可能来自 membership、command、Follower catch-up或历史重放，而不全是用户 DML；它会造成保守的 false-hot，但不会造成危险的 false-cold。

### 11.5 业务请求热度：以 graphd 为主观测面

纯读查询不会产生 WAL，因此只观察非空 WAL 无法说明“业务是否正在频繁读取”。业务语义观察的最优位置是 graphd，而不是 storaged：graphd 看到一次逻辑查询、读写类型、最终状态和用户延迟；storaged 看到的已经是按 host/Part 扇出的 RPC、失败尝试和内部访问，不能直接代表用户 QPS。

这不意味着 Graph-only。graphd 的结果必须命名为 `GRAPH_BUSINESS_HEAT`/`GRAPH_COLD_CANDIDATE`；它看不到实际 WAL append、Follower 复制、commit lag、Snapshot 和后台作业，不能独自生成 `SPACE_COLD_ELIGIBLE`。

#### 11.5.1 不能直接复用当前 session Space 指标

Graph 请求统一进入 [`GraphService::future_executeWithParameter()`](../src/graph/service/GraphService.cpp#L146-L198)。现有 `--enable_space_level_metrics` 在 [`GraphService.cpp:179-195`](../src/graph/service/GraphService.cpp#L179-L195) 按**请求进入时 Session 当前 Space name**计数；parse/validate/optimize、成功和失败生命周期见 [`QueryInstance.cpp:39-113`](../src/graph/service/QueryInstance.cpp#L39-L113) 与 [`QueryInstance.cpp:126-229`](../src/graph/service/QueryInstance.cpp#L126-L229)。这些已有指标适合运维参考，但不能直接授权冷热决策：

- `USE A; ...; USE B; ...` 可以在一个请求中访问多个 Space；
- 请求进入时、验证时和完成时读取的 Session Space 可能不同；
- `USE`、常量表达式、DDL 或验证失败不等于访问数据；
- 当前动态 label 使用 Space name，没有 boot/report/config epoch 和完整长窗口；
- 现有 active query 在 [`GraphService.cpp:190-195`](../src/graph/service/GraphService.cpp#L190-L195) 的 `QueryEngine::execute()` 返回后立即递减，但查询调度 Future 仍可在 [`QueryInstance.cpp:55-66`](../src/graph/service/QueryInstance.cpp#L55-L66) 继续运行，不能用它证明真实在途为零；
- [`StatsManager`](../src/common/stats/StatsManager.cpp#L165-L220) 的动态 label 查表/注册及其 [counter更新路径](../src/common/stats/StatsManager.cpp#L270-L299) 不适合成为高频、长窗口的自动决策状态库。

顺序语句由 [`SequentialValidator::validateImpl()`](../src/graph/validator/SequentialValidator.cpp#L18-L44) 逐句验证；`USE` 在 [`UseValidator.cpp:18-43`](../src/graph/validator/UseValidator.cpp#L18-L43) 把目标写入验证上下文，在 [`SwitchSpaceExecutor.cpp:16-47`](../src/graph/executor/admin/SwitchSpaceExecutor.cpp#L16-L47) 才更新 Session。[`ValidateContext`](../src/graph/context/ValidateContext.h#L28-L95) 虽保留 Space 切换轨迹，但当前只公开最后一个 `whichSpace()`。因此不得把整条多语句请求归给入口或结束时的 Session Space；`USE` 只计 `SESSION_ONLY`，不重置数据冷热窗口。

#### 11.5.2 graphd 的权威业务归属点

第一版应由 [`QueryEngine`](../src/graph/service/QueryEngine.cpp#L27-L33) 向graphd专属 `StorageClient` 注入 `GraphBusinessHeatObserver`，并在 [`StorageClient::CommonRequestParam`](../src/clients/storage/StorageClient.h#L42-L58) 增加一个引用计数/弱引用安全的 `query_heat_token + op_seq`，禁止保存裸 `QueryContext*`。`CommonRequestParam` 已携带准确的 `space`、`session` 和 `plan`；实际请求构造也把它写入 `space_id`，例如：

- 读：[`StorageClient::getNeighbors()`](../src/clients/storage/StorageClient.cpp#L40-L111)、getProps、lookup 和 scan；
- 写：[`StorageClient::addVertices()`](../src/clients/storage/StorageClient.cpp#L149-L186)、[`addEdges()`](../src/clients/storage/StorageClient.cpp#L188-L224)、delete 和 update；
- fan-out、聚合、partial/error：[`StorageClientBase::collectResponse()`](../src/clients/storage/StorageClientBase-inl.h#L72-L153)；
- 每个真实 host RPC、Leader Changed 和异常：[`StorageClientBase::getResponse()`](../src/clients/storage/StorageClientBase-inl.h#L157-L237)。

采集分成四个阶段：

```text
QUERY_INTENT    收到并开始解析一次查询，仅用于入口/失败解释
LOGICAL_DISPATCH 某个公开StorageClient方法按准确space_id发起一次逻辑操作
RPC_ATTEMPT     单次logical dispatch实际发送到某个host
RESULT          success / partial / failed / timeout / ambiguous
```

`LOGICAL_DISPATCH` 是业务冷热主计数：每次带有效 heat token 的 StorageClient 方法调用记一个逻辑 attempt，单次调用不会因 host/Part fan-out重复；执行器重发或外部客户端重试仍可能形成新的 attempt，只有携带稳定 identity 时才能关联，不能声称它是全局去重后的业务事务数。identity 可使用 `(graph_boot_id, session_id, plan_id, op_seq)`；`RPC_ATTEMPT` 单独展示每host发送和失败压力。读写类型采用显式编译期 traits 或集中枚举映射，禁止解析方法名字符串/RTTI；AST 的现有读写分组可参考 [`PermissionCheck.cpp:30-242`](../src/graph/service/PermissionCheck.cpp#L30-L242)，但权限判断与冷热分类必须是两个独立模块。

分类至少包括：

```text
READ | MUTATE | ADMIN | SESSION_ONLY | UNKNOWN
```

Mutation 统一从已知准确 `space_id` 的 public method `LOGICAL_DISPATCH` 阶段保守判热，不能等成功返回。若 leader/host 路由在任何 RPC 发出前就失败，该次请求确实没有提交，只是首版为了 fail-closed 仍保留一次 intent 型 false-hot；一旦已发出 RPC，timeout、partial 和 Leader Changed 都不能证明请求没有提交。`QUERY_INTENT` 只作全局/未归属解释；验证失败若未产生 Storage dispatch，不伪造某个 Space 的数据热度。

每个 `QueryContext` 应持有 `QueryHeatTracker`，内部是 `map<space_id, READ|MUTATE bitmask> + OPEN/CLOSED`。在小型tracker锁内，某Space某类别首次logical dispatch才增加对应active token，后续只累计logical op；`QueryInstance::onFinish()/onError()` 以compare-and-close保证恰好一次释放所有bit token，绝不能再读取完成时Session Space。若dispatch无法关联有效tracker，仍要把该Space活动epoch置热，同时将Graph evidence标为UNKNOWN直到无界在途风险消除，不能把active计数缺失当作0。

当前 `StorageClient::getUUID/get/put/remove` 只接收 `space/evb`，没有 `CommonRequestParam`：[`StorageClient.h:139-177`](../src/clients/storage/StorageClient.h#L139-L177)、[`getUUID()`](../src/clients/storage/StorageClient.cpp#L482-L510)、[`get/put/remove`](../src/clients/storage/StorageClient.cpp#L652-L725)。第一版若graphd未调用它们，应在build capability中明确 `PROVEN_UNUSED_BY_GRAPH`；若商用graphd启用，必须先扩展为携带heat token/op_seq，否则Graph evidence为UNKNOWN。storaged ingress仍无条件兜底这些请求。

建议每个 graphd/Space 的有界状态为：

```text
GraphSpaceHeatObservation {
  space_id
  graph_host, graph_boot_id, report_seq
  observer_config_epoch, graph_membership_generation
  admission_mode/state/epoch
  observation_start_steady
  read_epoch, write_epoch
  last_read_intent/dispatch_steady
  last_write_intent/dispatch_steady
  logical_read/write_ops
  read/write_rpc_attempts
  success/partial/error/timeout/ambiguous
  active_read/write_queries
  bounded_short/long_windows
  graph_observation_validity
  graph_demand
  reason_mask
}
```

热路径只按 `space_id` 做 O(1) 原子更新；窗口滚动和不可变报告由后台完成。普通 `/stats` 只输出状态数量和 top-K，按 Space 明细走过滤、分页接口；不能为 session/plan/Part 建立永久 Prometheus label。

“本机没有这个 Space 的 map entry”绝不能解释为零请求。Graph observer 必须从同一 Meta Space membership snapshot 预注册/对账全部当前 Space ID，或在不可变报告中提供一个可验证的“observer自某 generation 起完整覆盖全部入口”的零值证明；新建 Space、observer启用、配置变化和graphd重启都从 `GRAPH_OBSERVING/BOOT_WARMUP` 开始。Drop 后的旧 Space ID使用tombstone清理，Space name复用不能继承旧ID窗口。

每份 Graph 报告还必须携带逐方法 `GraphMethodCoverage` 与规范化hash：

```text
INSTRUMENTED              已接入准确space/类别/query token和生命周期
PROVEN_UNUSED_BY_GRAPH    由不可变build feature + 受版本控制配置证明graphd不可调用
UNSUPPORTED_UNKNOWN       可能被调用但没有完整观测
```

AUTO 只接受前两种，任一 data method 为 `UNSUPPORTED_UNKNOWN` 时该graphd对所有Space报 `GRAPH_UNKNOWN/UNSUPPORTED_PROVENANCE`。该矩阵覆盖当前 graphd 构建内全部 data `StorageClient` 及由graphd发起的TOSS/商用扩展入口；storaged直连、InternalStorageClient和后台入口不属于Graph capability声明范围，必须由Storage ingress/safety capability兜底。不能把代码漏埋点解释为冷。

#### 11.5.3 storaged ingress 作为旁路探测器，而非业务主计数

[`GraphStorageServiceHandler.cpp:89-219`](../src/storage/GraphStorageServiceHandler.cpp#L89-L219) 仍应保留轻量 `StorageIngressObserver`，但字段改名为 `STORAGE_RPC_HEAT`，只承担两项职责：

1. 发现绕过受控 graphd 的直连 Storage 请求或商用 Internal/TOSS/事务路径；
2. 交叉验证 graphd dispatch 与 storaged ingress 是否长期严重不一致。

它会包含 fan-out、重试、参数错误、not-leader、空 batch 和失败，天然是保守 false-hot。请求处理器取得 Space ID 的位置见 [`GetNeighborsProcessor.cpp:32-39`](../src/storage/query/GetNeighborsProcessor.cpp#L32-L39)、[`AddVerticesProcessor.cpp:24-33`](../src/storage/mutate/AddVerticesProcessor.cpp#L24-L33) 和 [`AddEdgesProcessor.cpp:23-33`](../src/storage/mutate/AddEdgesProcessor.cpp#L23-L33)；现有进程级 counter 在 [`GraphStorageServiceHandler.cpp:69-85`](../src/storage/GraphStorageServiceHandler.cpp#L69-L85) 与 [`BaseProcessor::onFinished()`](../src/storage/BaseProcessor.h#L41-L60)，没有 Space/origin/boot 语义，不能直接复用。

第一版无需修改 Storage RPC wire 来证明来源：`StorageIngressGuard` 对全部 data RPC 保守计数，冷窗口内只要有任何 ingress 就是 `STORAGE_HOT`；当 Graph 已经满足零业务窗口时，仍出现的 Storage ingress 自然暴露旁路或内部流量。若将来每个请求携带可信的 `origin_graph_boot_id/request_trace_id`，可额外分成 `GRAPH_FORWARDED` 与 `NON_GRAPH_OR_UNKNOWN` 供解释和一致性审计，但 origin 不能成为安全授权的必需依赖。OSS 3.6 的 Storage service 端口仍直接对外提供 Graph Storage RPC，架构上不能仅凭“通常没有人直连”跳过这个兜底。

如需进一步解释实际存储 I/O，可在 [`NebulaStore` 五个读入口](../src/kvstore/NebulaStore.cpp#L746-L857) 和 [`NebulaStore` 六个写入口](../src/kvstore/NebulaStore.cpp#L878-L955) 记录 `PHYSICAL_KV_ACTIVITY`。它会把索引维护、事务内部读写和后台任务放大，只能作为诊断维度。OSS 3.6 的 [`StorageServer`](../src/storage/StorageServer.cpp#L272-L422) 未注册 Internal/TOSS server；商用分支若启用，必须为 prime、remote、commit/abort、recovery retry 标注 origin/stage 并加入 capability 矩阵。

#### 11.5.4 为什么最终必须是融合判定

| 能力 | Graph-only | Storage-only | 推荐融合方案 |
|---|---|---|---|
| 逻辑查询、读写语义、用户延迟 | 最准确 | 被host/Part fan-out放大，且缺少上层query语义 | Graph主观测 |
| 多语句实际Space归属 | StorageClient dispatch可准确归属 | 只知收到的RPC | Graph主观测、Storage交叉验证 |
| 实际非空WAL/Follower/commit lag | 不可见 | 权威 | Storage硬门槛 |
| Snapshot/ingest/rebuild/restore | 只能看到部分发起意图 | 本地执行可见 | Storage+Meta硬门槛 |
| 直连Storage/内部事务 | 可能漏报 | 可见 | Storage旁路否决 |
| 热事件前本地同步升容 | 跨服务无法保证 | 可在WAL push前完成 | Storage最后防线 |

所以 graphd 是“业务冷热观察功能”的最佳起点，却不是“WAL 缓存自动缩容”的完整终点。Graph-only 只能输出候选和解释；融合方案才允许输出资格。

### 11.6 SYSTEM_BUSY与安全否决信号

只依据“非空 WAL 为零”自动降容仍不充分。下列状态必须进入 `busy_mask` 或 `UNKNOWN`：

| 状态 | 推荐信号点 | 决策 |
|---|---|---|
| Part STARTING/WAITING_SNAPSHOT | [`RaftPart::needToCleanWal()`](../src/kvstore/raftex/RaftPart.cpp#L1452-L1462)、Raft state | UNKNOWN，禁止转冷 |
| 接收 Raft Snapshot | [`RaftPart::processSendSnapshotRequest()`](../src/kvstore/raftex/RaftPart.cpp#L1954-L2038)、[`needToCleanupSnapshot()/cleanupSnapshot()`](../src/kvstore/raftex/RaftPart.cpp#L1438-L1450) | 从进入 WAITING_SNAPSHOT 到完成/超时清理均 busy |
| 向 Follower 发送 Snapshot | [`Host::startSendSnapshot()`](../src/kvstore/raftex/Host.cpp#L348-L379) | 对该 Part busy；通过锁内 accessor/RAII 观察 |
| checkpoint/CREATE SNAPSHOT | [`CreateCheckpointProcessor.cpp:11-41`](../src/storage/admin/CreateCheckpointProcessor.cpp#L11-L41)、[`NebulaStore::createCheckpoint()`](../src/kvstore/NebulaStore.cpp#L1090-L1171) | Space 级 busy |
| SST ingest | [`IngestTask.cpp:17-47`](../src/storage/admin/IngestTask.cpp#L17-L47) | 围绕每个 Part 的 `engine->ingest()` 设置 RAII busy |
| tag/edge index rebuild | [`RebuildIndexTask.cpp:75-112`](../src/storage/admin/RebuildIndexTask.cpp#L75-L112)、[`IndexGuard`](../src/storage/CommonUtils.h#L46-L100) | STARTING/BUILDING/LOCKED 全部 busy |
| fulltext rebuild/其他 AdminTask | [`AdminTask::getSpaceId/jobType/running`](../src/storage/admin/AdminTask.h#L156-L227)、[`AdminTaskManager`](../src/storage/admin/AdminTaskManager.cpp#L252-L323) | 按 Space 和 job type 观察，不使用混合全局计数 |
| restore/direct ingest | [`NebulaStore::restoreFromFiles()`](../src/kvstore/NebulaStore.cpp#L1368-L1384) | 以 Space RAII busy 覆盖整个 engine ingest；商用扩展入口也必须注册 |
| engine backup | [`NebulaStore::backup()`](../src/kvstore/NebulaStore.cpp#L1323-L1332) | 当前接口遍历全部Space，调用期间使用全局backup busy；若产品未启用则明确capability状态 |
| data balance | [`BalanceTask` 状态机](../src/meta/processors/job/BalanceTask.cpp#L46-L259) | 以 Meta 持久任务状态为权威；缺失视为 UNKNOWN |
| 角色、term、拓扑变化 | [`RaftPart::getState()`](../src/kvstore/raftex/RaftPart.cpp#L1197-L1216) | 进入稳定观察期，旧候选失效 |
| commit/apply lag | `max(0,last_log_id-committed_log_id)`，字段语义见 [`RaftPart.h:829-837`](../src/kvstore/raftex/RaftPart.h#L829-L837) | 超过经 SLO 批准的门槛则 busy/UNKNOWN |
| WAL append异常 | [`FileBasedWal::appendLogInternal()`](../src/kvstore/wal/FileBasedWal.cpp#L442-L500) 的拒绝/fsync错误hook | 增加safety epoch并置 WAL_APPEND_ERROR；重新完成窗口前不可判冷 |

Snapshot、ingest、restore、backup 和 rebuild 可能绕过普通业务 WAL 信号。每个 storaged heat report 必须带 `observer_capabilities` 矩阵，逐项把 WAL error、RPC、snapshot、checkpoint、ingest、restore、backup、rebuild、balance和Raft state标成 `OBSERVED`、`PROVEN_DISABLED` 或 `UNSUPPORTED_UNKNOWN`。AUTO只接受前两种；任一所需来源为 `UNSUPPORTED_UNKNOWN` 即 `SAFETY_UNKNOWN/UNSUPPORTED_PROVENANCE`。不能因为某个产品分支“通常不用该路径”就在运行时默认放行；`PROVEN_DISABLED` 必须来自不可变build capability或受版本控制的启动配置及其hash。

`RaftPart::getState()` 在同一 `raftLock_` 下返回 term、role、status、committed/last LogID 和 peers，优于分别读取多个无锁字段。跨副本采样不是原子快照；只有同一轮中 term 一致、唯一 Leader、状态 RUNNING 且报告新鲜时，才能计算集群 lag。角色切换本身不一定是业务热，但必须让当前 cold candidate 失效并重新进入稳定期。

### 11.7 每Part观察数据模型

建议的数据结构如下，字段名仅表示设计语义：

```text
PartHeatObservation {
  space_id, part_id
  host, boot_id, part_instance_id
  observation_config_epoch
  wal_activity_epoch
  storage_ingress_epoch
  safety_epoch

  wal_nonempty_records_total
  wal_nonempty_payload_bytes_total
  wal_empty_records_total
  last_nonempty_monotonic_time

  storage_read_rpc_attempts_total
  storage_write_rpc_attempts_total
  storage_rpc_attempts_total
  non_graph_or_unknown_rpc_attempts_total（仅在可信origin可用时）
  storage_request_items_total
  storage_request_bytes_estimate_total
  last_storage_read_monotonic_time
  last_storage_write_monotonic_time

  role, term, raft_status
  last_log_id, committed_log_id, commit_lag
  membership_revision
  busy_mask
  observer_capabilities

  observation_validity
  wal_temperature
  storage_ingress_guard
  safety_guard
  reason_mask
}
```

Graph 侧不复制 per-Part 模型，而使用11.5.2节的 `GraphSpaceHeatObservation`。Meta 发布的 Space 汇总必须同时保留 `graph_demand`、`storage_ingress_guard`、`wal_temperature` 和 `safety_guard`，不能把它们提前折叠成一个无法解释的分数。

storaged 还要维护轻量 `SpaceStorageIngressObservation{space_id, ingress_epoch, last_ingress, malformed_or_empty_attempts}`。请求没有有效Part、Part map为空或参数错误时仍先增加Space级epoch并令该Space的 `StorageIngressGuard=STORAGE_HOT`，但不伪造某个Part活动；有有效目标Part时再增加各Part的 `storage_ingress_epoch`。因此“空/非法请求也保守覆盖”不依赖虚构Part ID。

原则：

- 决策时间全部使用 `steady_clock`；wall clock 只用于人类展示和审计；
- 计数器在一个 `boot_id/part_instance_id` 内单调，counter reset、溢出或倒退立即进入 UNKNOWN；
- WAL append 热路径只做原子计数、时间和 epoch 更新，不分配字符串、不写日志、不获取全局锁；
- graphd 每次逻辑 StorageClient dispatch 增加对应 Space 的 graph read/write epoch；storaged ingress 只增加独立的 `storage_ingress_epoch`，两者都与 WAL `wal_activity_epoch` 分离；
- 周期 worker 从累计计数生成窗口快照，避免每条 heartbeat/no-op 都写高基数时序指标；
- Graph read/write epoch、`wal_activity_epoch`、`storage_ingress_epoch`、`safety_epoch`、观察配置版本和管理面的 `EffectiveCapacityVersion` 是不同版本域，不得复用一个 revision。

### 11.8 滚动窗口、EWMA和判冷算法

第一版判冷应采用“完整窗口内非空 WAL 精确为零”，而不是任意加权分数。低速但非零写入只展示为 `COOLING`，在 shadow 数据证明以前不能自动判冷。

实现可采用有界的两级时间桶，例如短窗口使用细粒度桶、长窗口使用粗粒度桶；具体桶数必须按最大 Part 数做内存预算和启动校验。不能为每 Part 保留无限时序。累计计数按实际采样间隔计算：

```text
rate = counter_delta / delta_time
alpha = 1 - exp(-delta_time / tau)
ewma = alpha * rate + (1 - alpha) * previous_ewma
```

record/s 和 payload-byte/s 两条通道都要保留。短/长 EWMA适合展示和防抖，但不能单独授权转冷，因为启动零值、采样暂停和缺报都会把它错误衰减到零。

首个可执行的 `COLD` 条件应全部满足：

```text
observation_age >= W_long
&& 长窗口覆盖完整、无sample gap
&& W_long内wal_nonempty_records_delta == 0
&& W_long内wal_nonempty_payload_bytes_delta == 0
&& last_nonempty_age >= T_idle
&& 全部预期graphd报告完整，Graph业务热度未超过经SLO批准的保护门槛
&& Graph admission_mode==ENFORCED且全部预期实例为当前boot的ADMITTED
&& graph active_queries == 0
&& storaged全部data RPC ingress未触发保护门槛
&& wal_activity_epoch在candidate和confirm期不变
&& 所有graph read/write epoch在candidate和confirm期不变（首个AUTO零读门槛）
&& storage_ingress_epoch在candidate和confirm期不变
&& safety_epoch在candidate和confirm期不变
&& observation_config_epoch不变
&& busy_mask == 0
&& Raft/拓扑/lag/副本覆盖全部有效
&& 连续满足T_confirm
```

若未来开放“低速非零也可冷”，必须满足 `cold_enter_threshold < hot_exit_threshold`，同时配置最短 HOT 保持期和确认期，避免阈值附近抖动。窗口长度必须覆盖实际业务周期；存在日批或周批任务时，24小时未必足够。

首个 AUTO 版本如果没有经过业务 SLO 审批的 read threshold，应保守要求确认期内 graph logical read dispatch 为零；不能把缺省阈值解释成“读热度不参与”。只读热点 Space 需要保留 MANUAL 审批路径，由专项磁盘 fallback、Leader transfer 和查询 P99 测试决定是否可降容。

建议只作为 shadow 起点、等待生产 SLO 批准的候选参数：

```text
sample_interval       = 60s
short_window          = 5m
idle/long_window      = 24h或一个完整业务周期中的更长者
confirm_window        = 1h
report_interval       = 60s
max_sample_gap        = 2 × sample_interval
replica_coverage      = 100%
```

这些不是通用默认真理。容量 target 仍由第12节的内存预算、batch charge 和追赶窗口决定，不能从“冷分数”直接推导出 2MiB/1MiB。

### 11.9 状态机与防竞态线性化

`ObservationValidity` 生命周期：

```mermaid
stateDiagram-v2
    [*] --> UNKNOWN
    UNKNOWN --> OBSERVING: "boot/Part构造/证据源恢复"
    OBSERVING --> VALID: "完整窗口且无sample gap"
    VALID --> OBSERVING: "safety epoch变化后重建窗口"
    VALID --> UNKNOWN: "缺报/配置/boot/拓扑/计数异常"
    OBSERVING --> UNKNOWN: "缺报/配置/boot/拓扑/计数异常"
```

`WalTemperature` 生命周期：

```mermaid
stateDiagram-v2
    [*] --> HOT
    HOT --> COOLING: "完整短窗口低活动"
    COOLING --> COLD: "完整长窗口零非空WAL且确认期通过"
    COLD --> HOT: "第一条成功非空WAL"
    COOLING --> HOT: "出现非空WAL"
```

`GraphDemand`、`StorageIngressGuard` 和 `SafetyGuard` 是独立状态，不改写 `WalTemperature`；例如一个只读热点 Space 可以同时显示 `WalTemperature=COLD`、`GraphDemand=GRAPH_HOT`、`eligible_for_cold=false`。`COLD_PREPARING/COLD_APPLIED/HOT_RECOVERING` 属于 Space decision state，不得出现在 Part 的温度枚举中。

自动迁移不能实现成“后台检查 epoch，然后在另一个临界区 store capacity”。每个 Part 必须提供明确的 prepare/apply/失效原语：

```text
prepareCold(auto_decision_key,
            application_scope_hash,
            expected_part_instance,
            expected_wal_activity_epoch,
            expected_storage_ingress_epoch,
            expected_safety_epoch,
            expected_observation_config_epoch,
            expected_current_effective_capacity_version)

applyCold(auto_decision_key,
          prepared_token_hash,
          committed_cold_effective_capacity_version)
failHotCurrentForActivity(reason)
failHotIfMatches(expected_auto_decision_key, authenticated_recovery_token, reason)
failHotOnDeadlineIfMatches(expected_auto_decision_key,
                           deadline_identity,
                           reason)

onNonEmptyActivity()
onStorageIngressActivity()
onSafetyStateChange()
```

三种fail-hot不能混用：真实 Storage ingress、非空 WAL 和当前 Safety活动调用 `failHotCurrentForActivity()`，在锁内无条件失效当时的当前decision；Meta 的 ABORT/HOT_RECOVERING 控制RPC调用 `failHotIfMatches(expected_key, authenticated_recovery_token)`，精确匹配当前key并验证该恢复命令已由当前Meta authority持久化后即可立即升热，不要求lease到期；qualification/prepare/watch/lease定时器调用 `failHotOnDeadlineIfMatches(expected_key,deadline_identity)`。

`deadline_identity` 不能只有 deadline 数值，必须完整绑定：

```text
DeadlineIdentity {
  kind: QUALIFICATION | PREPARE | WATCH | ACTIVE_LEASE
  qualified_identity_hash
  object_generation             # 对应watch/token/active对象的本机单调实例号
  expected_phase
  exact_deadline_steady
}
```

回调在 Part policy lock 内必须同时验证 decision key、kind、identity hash、对象 generation、当前 phase 和当前保存的 deadline 全部精确相同，并确认 `now >= exact_deadline_steady`，才能 fail-hot。qualification timer 只允许命中 `QualificationWatchState=QUALIFIED`，prepare timer 只允许命中 `PreparedColdToken=ISSUED`，watch timer 只允许命中 `QualificationWatchState=WATCHING`，lease timer 只允许命中同一个 `ActiveAutoOverlay`。同一 decision 从 QUALIFIED 进入 WATCHING/ACTIVE 后，旧 qualification 或 prepare timer 即使时间已到也必须 no-op；陈旧D1任务对已经建立的D2同样必须 no-op。迟到clear也只比较完整key。

`prepareCold()` 在同一 per-part policy/temperature 临界区重验 Storage 观察 token、当前hot effective version、时间窗、busy 和管理策略，只返回包含这些字段规范化hash的 PREPARED ack，**不修改 target**。Meta 的 Space prepare token还必须绑定当前完整 Graph collection snapshot hash、全部 graphd read/write epoch和成员 generation。只有集群协调者以CAS持久化同一 `space_decision_epoch` 的 COMMIT 和新cold effective version后，`applyCold()` 才验证 prepared hash、确认观察epoch未变化，并执行“旧hot version或幂等new cold version -> committed cold version”的受控转换。任一失败都会使该 Space decision 进入 ABORTING，并补偿性恢复已经应用的 Part；并发MANUAL/gate更新必须使COMMIT CAS失败。

`onNonEmptyActivity()` 必须在当前非空 record 进入 `logBuffer_->push()` **之前**先做无锁观测，再只对活跃 AUTO decision 进入慢路径：

```text
原子增加wal_activity_epoch并把wal_temperature置HOT
若没有当前qualification watch/capacity guard -> 直接返回
若有qualification watch -> 原子置INVALID并可靠排队Space decision invalidation
若本Part有匹配当前decision/version的capacity guard
  -> 获取policy lock，重验guard key/epoch
  -> fail-hot fence并恢复original/hot target
然后允许当前record push
```

Graph 侧 `onGraphRequestActivity(space)` 在 logical dispatch 前增加 graph read/write epoch、立即使本机 candidate 失效并异步通知 Meta。它不能在 graphd 内直接修改任一 storaged buffer，也不能阻塞用户请求等待整个 Space 升容；Meta 收到后将 Space decision 转为 `HOT_RECOVERING`，但通知到达前仍可能有旧冷决策在途。

Storage ingress不能让所有正常热Space的RPC无条件获取per-Part mutex。正式快慢路径同样先无锁增加Space级epoch，并对每个有效目标Part原子增加 `storage_ingress_epoch`；只在存在当前 decision 的 qualification watch/capacity guard 时执行上述失效/加锁慢路径。owner 虽为AUTO、但尚无决策或已经fail-hot完成清理时，仍只做O(1)原子观测。scope内 `capacity guard` 必须在prepare前按decision/version置为 `ARMING -> GUARDING` 并ack，随后重新核验epoch；退出时只有完全匹配同一guard key的clear任务才可把它改为OFF，迟到的D1 clear不能清掉D2 guard。`prepareCold()/applyCold()` 在锁内最后一次store target前再次比较epoch。真正写入再由 `onNonEmptyActivity()` 在当前非空 WAL `push()` 前兜底。

`onSafetyStateChange()` 也先原子增加 `safety_epoch`、更新 `SafetyGuard`；只有发现当前 qualification watch/capacity guard 才进入policy慢路径，同步fail-hot并发布失效。OBSERVE_ONLY、MANUAL或AUTO无decision时不能为每次安全采样获取policy锁。busy结束后置 `SafetyGuard=SAFE`，同时把观察窗口重置为 `ObservationValidity=OBSERVING`，重新积累完整窗口。

`prepareCold()/applyCold()` 不能只相信周期采样的 Raft字段。入口应先获取 `raftLock_`，在当前状态下复核 role、term、status、commit lag和membership，再获取 per-part temperature/policy lock 检查全部 token。这样迟到任务不能把已经重新活跃或进入恢复状态的 Part 降回小容量。Raft Leader/Follower WAL append 可能已经持有 `raftLock_`，锁序必须固定为：

```text
raftLock -> per-part temperature/policy lock
```

后台观察器禁止持有 temperature lock 后获取 Raft、NebulaStore、Meta 或指标 registry 锁。回调必须是 O(1)，不能在 WAL Writer 内做 Space 聚合。

控制权优先级固定为：

```text
本机activation gate
  > RESET/disabled policy
  > 显式MANUAL Space policy
  > AUTO观察决策
```

第6.3节的 `EffectiveCapacityVersion` 是**每台host自己的版本**，因为其中包含本机 `gate_epoch/generation_install_epoch`；它不能被当成一个集群标量写进 Meta CAS。AUTO 必须同时携带下节定义的 Meta owner token、每副本 host effective version、持久化 decision、scope、Part实例和本机期限。任何 gate、人工策略、Part实例或观察配置变化都使旧 AUTO 任务失效。

#### 11.9.1 AUTO overlay、fail-hot fence与租约

AUTO 不能直接覆盖5.1节的基础策略 target。它必须是一个有期限、可被本机活动永久否决的临时 overlay。每个 storaged 把 Meta 的 `policy_generation` 安装为本机单调 `generation_install_epoch` 后，使用以下本机可比较序号和分层令牌：

```text
MetaOwnerVersion {                 # 集群共同、可由Meta CAS
  policy_generation
  space_last_modified_epoch
  policy_enabled=true
  space_control_owner=AUTO
  canonical_space_policy_hash
}

AutoDecisionOrdinal {
  generation_install_epoch         # host-local，用于跨generation排序
  space_decision_epoch
}

QualifiedColdIdentity {             # 不可变，全部预期replica生成
  ordinal
  meta_owner_version
  host_effective_version            # 本机EffectiveCapacityVersion
  application_scope_hash
  part_instance_id, storage_boot_id
  topology/membership/observation epochs
}

QualificationWatchState {           # 可变，但不改变identity hash
  qualified_identity_hash
  watch_object_generation
  local_qualification_deadline_steady
  local_decision_watch_deadline_steady
  watch_commit_hash
  state: QUALIFIED | WATCHING | INVALID
}

PreparedColdToken {                 # 仅application scope内生成
  qualified_identity_hash
  token_object_generation
  target_capacity_bytes
  effective_lease_duration_ms
  local_prepare_deadline_steady
  state: ISSUED | CONSUMED | INVALID
}

ActiveAutoOverlay {                 # apply恰好一次后建立
  ordinal
  active_object_generation
  qualified_identity_hash
  meta_owner_version, host_effective_version
  application_scope_hash
  target_capacity_bytes
  commit_hash
  part_instance_id, storage_boot_id
  local_lease_deadline_steady
}

CapacityGuard {                     # 仅scope内、decision-scoped
  guard_key=(ordinal, host_effective_version, application_scope_hash)
  state: OFF | ARMING | GUARDING | FAIL_HOT_IN_PROGRESS
}

LocalAutoFence {
  reject_auto_through_ordinal
  local_fail_hot_seq
  last_fail_hot_reason
}
```

`policy_generation` 可以是 UUID，不能直接排序；跨 generation 的先后由已经持久化的 `generation_install_epoch` 判定，同 generation 内再比较 `space_decision_epoch`。Meta 必须在同一 generation 内为每个 Space 持久化单调递增的 decision epoch；restore/rebase 后的新 generation 会先安装更大的本机 install epoch。这样旧 generation 和旧 decision 都有确定的拒绝顺序。

每个 Part 的唯一有效容量按以下优先级计算：

```text
gate关闭                                      -> original hot
gate开启 + policy_enabled=false               -> original hot
gate开启 + owner=OBSERVE_ONLY                 -> original hot
gate开启 + owner=MANUAL                       -> MANUAL target
gate开启 + owner=AUTO + overlay全部校验通过   -> AUTO cold target
gate开启 + owner=AUTO + 无有效overlay          -> original hot
```

Meta 的 AUTO COMMIT 只 CAS 集群共同的 `MetaOwnerVersion + space_decision_epoch + membership/scope/hash`，并持久化全部 replica `QualifiedColdIdentity/QualificationWatchState` 与scope内 `PreparedColdToken` 的规范化 vector/hash。每个 host 在 apply 时校验自己的 `EffectiveCapacityVersion`；不能把三台不同的 gate/install epoch 压成一个所谓“owner effective version”。

Part 本地的 `LocalOverlayValid` 必须同时满足：

- 当前 `BasePolicyState.policy_enabled=true`；`meta_owner_version` 与本机最后安装的 Meta owner token 完全一致，且 `host_effective_version` 与当前本机 gate、generation、Space policy epoch、enabled 和 owner 完全一致；
- exact `(host, storage_boot_id, space, part, part_instance_id)` 位于当前 application scope；
- decision ordinal 严格大于 `reject_auto_through_ordinal`；
- 对应 Prepared token 已由本次 apply **恰好消费一次**；同一 `qualified_identity_hash` 的 watch 已为 `WATCHING` 且 `watch_commit_hash` 与active commit一致；
- 本地 WAL/ingress/safety/config epoch 与 qualification/prepare 时一致；
- scope内匹配同一key的 `CapacityGuard=GUARDING`；
- cold lease 未过期。

`ClusterDecisionValid` 由 Meta Leader维护：Graph collection、Graph membership/admission、Storage topology、全部 qualification/watch ack、Meta owner token和decision state仍有效。storaged 数据路径不能查询Meta或同步知道其他graphd变化；它只校验上面的本地谓词和Meta签发的commit hash。Graph变化到Meta通知/下一次Storage ingress之间允许短暂的cluster-state stale，真正保证当前本地访问先升热的是CapacityGuard/ingress/WAL slow path，不能把Graph collection有效性误写成Part可同步验证的不变量。

所有预期 replica 先在 Part policy 临界区生成不可变 `QualifiedColdIdentity`，以其规范化hash创建 `QualificationWatchState=QUALIFIED`；scope外 replica也保留这个可原子失效的 watch，活动时将其置INVALID并可靠排队通知，但不修改容量。scope内还必须以完整 `guard_key` 安装 `CapacityGuard=GUARDING`，之后重新读取活动epoch，才可签发只绑定 `qualified_identity_hash` 的 `PreparedColdToken`。guard 的 enable/clear 都比较完整 key；D1 的迟到 clear 遇到D2 key必须 no-op。

Meta持久化COMMIT后不能立刻只向scope apply：它先要求**全部预期 replica**在identity hash不变的前提下，把本地 watch 从QUALIFIED转为 `WATCHING(watch_commit_hash, local_decision_watch_deadline)` 并ack，全部完成后才允许scope apply。watch duration由受限的 `apply_timeout + max_lease_duration + propagation_margin` 计算；到期不能静默清除，而要转INVALID、推进fence并通知HOT_RECOVERING。WATCHING持续到该decision完成hot/abort清理，保证scope外在整个cold lease期间仍参与活动失效。

Meta在prepare请求中提出 lease duration；每个scope host必须在prepare阶段校验产品最小/最大值，超界直接拒绝，或把经过批准的最终 `effective_lease_duration_ms` 明确返回。Meta只在所有scope token承诺同一个最终值后才把它写入 token hash和持久COMMIT hash。apply 不再二次clamp，只使用token中已经承诺的exact duration。Meta 不能比较不同主机的 monotonic timestamp；host 在 `applyCold()` 的 Part policy 临界区重验本地 deadline、token仍为ISSUED、同identity的watch已为WATCHING且watch commit hash匹配、part instance、guard key和全部本地epoch。首次 apply 原子把token改成CONSUMED并建立固定 `local_lease_deadline=first_apply_steady+effective_lease_duration_ms`。

相同 decision 的幂等 apply 只在已经存在完全相同的 `ActiveAutoOverlay` 时返回成功，必须返回原来的 lease deadline，**不得重算或延长 deadline，也不得再次消费token**。任何延长都必须使用更大decision。

以下事件在scope内必须进入同一 Part policy 临界区执行 fail-hot：Storage ingress、非空 WAL、Safety 变为 BUSY/UNKNOWN、gate关闭、owner离开AUTO、MANUAL更新、Part实例变化或 AUTO lease 到期。真实活动使用 `failHotCurrentForActivity()`；认证的恢复控制RPC使用 `failHotIfMatches()`；定时回调使用 `failHotOnDeadlineIfMatches()`，并按上述 kind/object generation/phase/deadline 全量重验。操作顺序固定为：

```text
reject_auto_through_ordinal = max(current, qualified/prepared/active ordinal)
local_fail_hot_seq++
把qualification/prepared/active标成INVALID，但保持匹配guard=FAIL_HOT_IN_PROGRESS
setCapacity(original hot或当前MANUAL target)
清除active/prepared状态
最后以release store发布guard=OFF并移除watch
然后才允许当前业务/安全路径继续
```

活动快路径对guard使用acquire load：看到任何非OFF状态都必须进入policy lock；因此 `FAIL_HOT_IN_PROGRESS` 期间到达的第二个请求会等待，不能在hot target store之前旁路。只有最后的release-store OFF之后，后续请求才可依赖“target已hot”的先行关系。scope外没有cold target，只需原子把WATCHING置INVALID、推进watch fence并可靠通知。

因此迟到的同一或更旧 decision 永远不能把已经升热的 Part 再降冷。一旦发生 fail-hot，即使 Meta 重发相同 decision 也必须拒绝。再次降冷必须完成新的完整 collection/qualification/prepare/commit，并使用严格更大的 decision ordinal。

RESET/disabled、MANUAL/gate变化通过新的 `MetaOwnerVersion/EffectiveCapacityVersion` 使所有旧 AUTO token失效；RESET 必须在 Part policy lock 内先推进fail-hot fence、恢复 original hot，再确认 disabled base state applied。Meta 禁止 disabled policy创建新 AUTO decision；AUTO COMMIT 的 CAS 必须绑定当前 enabled Meta owner token，而每个 apply 还必须绑定对应host token。这样控制优先级不是文档约定，而是 Part 临界区可执行、可测试的版本格。

第一版续租采用更保守、可复用现有 `prepareCold()` 前提的 **hot-before-renew**：协调者在旧 lease 到期前把旧 decision 转为 `HOT_RECOVERING`，所有scope replica写fail-hot fence、恢复hot并确认旧guard已按key清除；随后才用新鲜完整round和更大decision重新qualification/prepare/cold。它不会预热已淘汰Node，但避免设计另一条“cold D原子切到cold D+1”的状态迁移。普通heartbeat、同D重试和仅更新Meta deadline都不能续租。

### 11.10 从Part聚合为Space：必须fail-closed

策略是 Space 级，实际观察对象是所有普通 replica-part。聚合分母必须来自同一 topology/membership revision 的**预期集合**，不能只统计当前返回了指标的对象。

预期集合由 Meta 中的 Space partition/replica 分配生成。若现有元数据没有可直接使用的全局 topology revision，聚合器必须从一次一致读取的规范化 membership snapshot 计算 hash/epoch；不能把多次读取中跨越 balance 的旧、新成员拼成一个分母。

每个逻辑 Part 的保守合并规则：

```text
任一当前副本HOT                         -> LOGICAL_PART_HOT
全部预期副本fresh、VALID且COLD          -> LOGICAL_PART_COLD
副本缺失/陈旧/boot变化/term或拓扑不一致 -> LOGICAL_PART_UNKNOWN
其他完整且有效的混合状态                -> LOGICAL_PART_COOLING
```

健康且完成复制的逻辑写通常会被多个、最终可能被全部副本观察，但部分复制、换主、失败和rollback时不保证三副本记录完全一致。因此 Space write rate 应先对同一个逻辑 Part 取各副本的 `max`，再跨逻辑 Part 求和；同时输出 `max_part_rate`。这个值是保守的 replica-normalized WAL 活动率，不是精确业务写 QPS。不得对副本直接求和，也不得只看 Space 平均值掩盖一个热点分片。

Space 级规则：

```text
全部预期普通logical Parts均COLD
  + 所有当前replica报告fresh/VALID
  + admission_mode=ENFORCED且全部预期graphd为ADMITTED、报告fresh/GRAPH_VALID且GRAPH_COLD
  + storage ingress和system busy保护通过
  -> SPACE_COLD_ELIGIBLE

任一Part WAL HOT、任一graphd GRAPH_HOT或Storage ingress HOT
  -> SPACE_HOT或SPACE_PARTIALLY_HOT，不自动降容

任一Part或graphd UNKNOWN/COOLING/busy/缺报
  -> SPACE_NOT_SAFE_TO_CLASSIFY，不自动降容
```

Graph 聚合不能只使用“当前 active graphd”作为分母。[`ActiveHostsMan::getActiveHosts()`](../src/meta/ActiveHostsMan.cpp#L119-L172) 会在 heartbeat TTL 后排除沉默实例，但 heartbeat 失败时 [`MetaClient::heartBeatThreadFunc()`](../src/clients/meta/MetaClient.cpp#L182-L200) 只记录错误并返回，本身不会停止 graphd 对外服务；若直接缩小分母会制造 false-cold。必须新增持久化 `GraphObserverMembership(generation, expected_instances, ingress_policy_hash)`；每个 instance record 另有独立单调的 `admission_epoch/state/boot_id`。全局 membership generation 用于 collection snapshot，不作为所有graphd的服务开关；新增/删除别的实例不会使现有实例的 admission token失效。任一预期 graphd 缺报、重启、boot变化、mixed version或观察窗口不足，所有相关 Space 都必须显示 `GraphObservationValidity=GRAPH_UNKNOWN(reason=GRAPH_OBSERVER_MISSING_OR_STALE)`。

Graph observer membership 必须同时定义新增和移除协议，不能让一个已经接流量的新 graphd 暂时游离在分母之外：

```text
新实例：REGISTERING
  -> 先加入expected set并提升membership generation；旧collection/冷decision全部失效并恢复hot
  -> observer完成Space零值对账、capability校验和进程readiness，状态OBSERVER_BOOTSTRAPPING
  -> Meta签发仅与该instance/boot/admission_epoch绑定的ADMITTED token
  -> graphd服务端与LB/网络入口确认token后才允许业务流量
  -> 从准入时刻开始积累真实路由下的GraphObservation长窗口；完成前仍是GRAPH_OBSERVING

移除实例：ADMITTED
  -> admission gate原子切到DRAINING，拒绝新的process request lease
  -> LB/网络入口摘流，并等待全部process request lease归零
  -> 显式DECOMMISSIONED并提升membership generation
  -> 才从expected set移除

注册失败：REGISTERING/OBSERVER_BOOTSTRAPPING
  -> 证明该boot从未ADMITTED、LB/网络未路由且process request lease为0
  -> 持久化REGISTRATION_ABORTED并提升membership generation
  -> 才从expected set移除；旧collection/decision继续失效
```

在 **ENFORCED** 模式下，实例在 `REGISTERING/OBSERVER_BOOTSTRAPPING` 期间已经属于 expected 分母，因此 Space 是 Graph UNKNOWN；未获得当前 boot/admission epoch 的 ADMITTED token 时，graphd 数据查询入口必须拒绝或不被路由。`ADMITTED` 只代表入口可服务，不代表冷热证据已经有效；新增实例从首次准入时刻重新进入 `GraphObservationValidity=GRAPH_OBSERVING`，在真实路由下积累完整长窗口后才成为 `GRAPH_VALID`，不能借用接流量前的零值时间立即判冷。

这里的 process request lease 与 per-Space `QueryHeatTracker` 是两种对象：前者由 [`GraphService::future_executeWithParameter()`](../src/graph/service/GraphService.cpp#L146-L198) 的 admission gate 在接受每个请求前取得，并覆盖无效Session、解析失败和多Space查询的整个 response Future 生命周期；后者只跟踪已知Space的读写在途。DRAINING 必须原子关闭新 process lease，再等待旧 lease 在所有成功/失败/提前返回路径归零，不能使用当前过早递减的 `kNumActiveQueries` 代替。

为保证 G0 真正只读，Graph admission 另有集群级 `SHADOW | ENFORCED` 模式。G0/F0 的 SHADOW 阶段把当前expected实例标成 `SHADOW_SERVING`：允许它们继续接流量、不要求ADMITTED token，只计算“若启用会否放行”。SHADOW报告仍可在完整窗口后达到 `GraphObservationValidity=GRAPH_VALID`，但聚合结果最多是 `GRAPH_COLD_CANDIDATE`，`admission_mode!=ENFORCED` 本身阻止 `SPACE_COLD_ELIGIBLE/AUTO`。

现网已有3个graphd的无中断 bootstrap 顺序是：先把所有实际入口一次性纳入expected set并确认没有旁路实例；在现有流量下以SHADOW_SERVING完成observer/capability和长窗口；预装每实例 admission token；全部实例ack后再切ENFORCED，并因配置epoch变化把正式证据重置为GRAPH_OBSERVING，重新积累完整Graph窗口。以后新增实例在ENFORCED模式严格走上述REGISTERING流程。若部署环境无法证明这种流量 fencing/admission，Graph 报告只能生成 `GRAPH_COLD_CANDIDATE`，必须禁用 AUTO。

当前方案是 **Space 级 all-or-nothing policy**：只有全部普通 Part 都满足冷准入，desired policy 才能从 hot 切到 cold；正式全量模式的 target 覆盖该 Space 的所有普通 replica-part。这里的 all-or-nothing 指“资格和同一 application scope 内的期望策略”，不承诺跨三台机器瞬时原子更新。每个 buffer 独立执行 setter，应用期间会短暂出现 mixed capacity，但必须有同一 decision epoch、完整 applied 计数和失败补偿；不能把这种过渡状态偷换成永久 per-part policy。若业务存在稳定的一热十九冷倾斜，应跳过该 Space，或另立 per-part policy 项目。

单host灰度是唯一允许的显式例外：decision 持久化 `application_scope={canary host/replicas}` 及其hash，仍用全Space、全副本观察证据判资格。全部预期 replica 都执行只读 qualification/epoch确认并参与 decision 失效；只有scope内生成可改target的prepare token并执行apply。此时状态必须叫 `CANARY_COLD_APPLIED`，不能冒充全Space容量已一致；正式扩面时提交新的更大 decision epoch，将scope扩到全部当前普通replica。scope外Part仍参与热事件失效判断。

Listener 第一版不在普通 Part 分母中，必须单独报告 scope。单台 storaged 的本地结果只能叫 `LOCAL_CANDIDATE`，不能宣称集群 Space 已冷。自动模式不以多数派覆盖掩盖缺失副本：RF=3 中缺一份新鲜报告仍是 UNKNOWN。

#### 11.10.1 一致采样协议与集群聚合主体

Phase F0 的集群聚合器运行在当前 Meta Leader；Meta 已持有权威 Space membership，Graph 的 `SHOW SPACE WAL HEAT` 只读取聚合器最近一次完整 collection round。Heartbeat 不承载全量 Part 数据。

一次 collection round 必须这样完成：

1. Meta Leader 从一次一致的 Meta 读取生成规范化 membership snapshot，得到 Storage `topology_revision/hash`、预期 host/Part/replica 集合，以及持久化的 Graph observer membership generation/预期 graphd 集合；
2. 分配在当前 Leader任期内单调的 `collection_round_id=(meta_term, local_round_seq)`，并分别向每台目标 storaged 和每台预期 graphd 请求本轮不可变 heat snapshot；
3. storaged 与 graphd 各自在短临界区复制本地观察摘要，生成不可变报告：

```text
HeatReportIdentity {
  role, host, boot_id
  admission_mode/state/epoch        # role=GRAPH时必需
  report_seq
  collection_round_id
  topology_or_membership_revision
  observation_config_epoch
  created_wall_time_display_only
  snapshot_ttl_ms
  page_count
  canonical_report_hash
}
```

4. 分页 token 必须绑定同一不可变 `HeatReportIdentity`；报告端用自己的 `steady_clock` 拒绝已过 `snapshot_ttl_ms` 的分页，Meta用接收端 `steady_clock` 记录本轮freshness，不能跨主机比较monotonic值或用wall clock授权；任一页 boot/report_seq/hash 不同、快照过期或缺页，整台实例的报告作废并重拉，不能把两轮页面拼起来；
5. Meta 校验全部 Storage 报告的 topology/Part集合与全部 Graph 报告的 membership/boot/capability/config/freshness都与第1步完全一致，再按 `space_id` 连接 `GRAPH_BUSINESS_HEAT + STORAGE_WAL_WRITE_HEAT + STORAGE_SAFETY_STATE`，才原子发布一个完成的 cluster heat snapshot；
6. collection期间发生 balance、任一 Storage/Graph boot变化、Graph membership变化、Meta切主或超时，本轮不发布，Space维持 UNKNOWN。

Graph 现有 heartbeat schema [`HBReq/HBResp`](../src/interface/meta.thrift#L543-L582) 没有本设计所需的 boot/report identity；[`MetaClient::heartbeat()`](../src/clients/meta/MetaClient.cpp#L2605-L2668) 及 [`HBProcessor`](../src/meta/processors/admin/HBProcessor.cpp#L29-L79) 只适合实例存活与基础元数据。全量 Space 热度不能塞进高频 heartbeat，应使用独立低频 pull/report API；heartbeat最多携带 capability、boot id、membership generation和摘要hash。

`fresh_ttl` 是配置项，候选下限可取 `max(3 × report_interval, 2 × collection_timeout)`，但最终按现网抖动/SLO批准。`SHOW` 响应必须带 collection round、完成时间和过期状态；过期 snapshot只能展示，不能授权 AUTO。

Phase F0 即使只读也只允许一个 Meta Leader 发布 authoritative round；新 Leader 不复用前任内存中的半轮数据，从新的 membership snapshot 和 round 开始。外部运维采集器可作为诊断客户端，但不能成为第二个 AUTO 写入者。

#### 11.10.2 Space AUTO决策、失败补偿与热恢复

Phase 3 唯一 AUTO 协调者是当前 Meta Raft Leader。它把每个 `space_decision_epoch`、集群共同的 `MetaOwnerVersion`、membership hash、collection round、全部replica qualification token vector/hash、scope内prepare token vector/hash、`application_scope/hash`、target、lease参数和 decision state 持久化到 Meta Raft；每个 storaged再按自己的 `EffectiveCapacityVersion` 与11.9.1节规则拒绝陈旧 decision。没有 Meta quorum/Leader 时禁止创建新冷决策。

冷进入采用 prepare/commit：

```text
SPACE_COLD_ELIGIBLE
  -> 持久化COLD_PREPARING(decision_epoch, tokens, membership_hash)
  -> 所有预期replica执行只读qualification prepare并确认观察epoch
  -> application scope内按完整key完成CapacityGuard ARMING->GUARDING并ack，再重验epoch
  -> scope内额外生成绑定本机deadline的PreparedColdToken；仍不改target
  -> 全部资格ack与scope内PREPARED均完整、未失效且仍在各host TTL内
  -> Meta在持久化前做一次全token confirm，缩小异步活动窗口
  -> Meta持久化COLD_APPLYING/desired cold
  -> 全部预期replica把qualification转为WATCHING(commit_hash, watch_deadline)并ack
  -> scope内各replica在Part锁内重验token/deadline/lease后执行applyCold并回报applied decision epoch
  -> scope为全量且全部applied才显示COLD_APPLIED；canary scope显示CANARY_COLD_APPLIED
```

`application_scope` 只决定哪些 replica 修改 target，不能缩小冷资格分母。scope外的全部预期 replica也必须参加本轮 qualification，并在COMMIT后进入覆盖整个lease的WATCHING。活动会在本机立即把token置INVALID并最终促使Meta进入HOT_RECOVERING；Meta在COMMIT前执行全token confirm以缩小窗口。但scope外通知仍是异步的，不能宣称它与Meta COMMIT全局线性化；真正保证“当前访问不会继续使用cold target”的线性化点仅在scope内CapacityGuard/Storage ingress/WAL slow path。若业务要求scope外事件绝对先于COMMIT否决，就必须阻塞入口并引入分布式屏障，不属于本设计。

任一 qualification/prepare/apply 失败、token变化、报告过期或 membership变化，协调者持久化 `ABORTING/HOT_RECOVERING`，把 scope内已经降容的 replica 全部补偿性恢复 hot，并让全部预期replica按完整key清理本decision的watch/guard；只有scope内全部 replica 报告 hot target、其他replica报告watch清理完成后才回到无冷决策状态。迟到clear遇到更新decision key必须 no-op。

冷状态出现 Graph 新业务请求、Storage 新非空 WAL/旁路请求，或 SafetyGuard失效时：

1. Graph 请求先增加本机 Space graph activity epoch并通知 Meta，不等待跨服务升容；Storage ingress 在处理当前目标 Part 前、非空 WAL在push前分别用本地线性化点同步恢复hot target；
2. 增加对应 graph/storage invalidation sequence，使该 `space_decision_epoch` 失效；
3. Meta持久化更大的 decision epoch 和 `HOT_RECOVERING`，向当前 application scope 的全部 replica下发 hot target；scope外本来就保持hot；
4. 传播期间允许协议安全的 `MIXED_CAPACITY/HOT_RECOVERING`，但必须展示 hot/applied/remaining Part 数和最长持续时间；
5. 若 Meta暂时不可用，其他 replica最迟在 AUTO decision lease 到期时用本地 `steady_clock` 恢复 hot、写入拒绝该 decision 的 local fail-hot fence；每个自身出现非空 WAL/安全活动的 Part无需等待Meta。Graph-only读活动在Meta不可用期间只能通过decision lease触发其他replica恢复，因此AUTO lease上限必须纳入纯读业务P99风险评估。

因此 all-or-nothing 不是分布式瞬时原子性承诺，而是“一个 Space decision、声明清楚的application scope、scope内同一desired、失败即全量补偿”。本 Part先升容保证当前活动路径，Space scope异步升容保证不会长期保留一热十九冷的旧AUTO决策；正式全量scope覆盖全部普通replica。

Meta 切主、失去 quorum、任一目标 host boot变化或任一 AUTO lease 到期后，持久化的旧 cold decision **不得直接重放为 COLD**。新 Leader只能把它恢复为 `RECOVERY_REQUIRED/HOT_RECOVERING`，先要求当前仍存活的scope实例hot ack；续租也严格执行11.9.1节的hot-before-renew。随后重新完成一个完整 Graph+Storage collection round、所有 replica qualification和scope prepare，并分配严格更大的 decision ordinal，才可再次降冷。普通 heartbeat、同D幂等apply都不能续租；这样本机 lease fail-hot 不会被 Meta 的旧记录重新覆盖。

旧scope精确绑定旧 `storage_boot_id/part_instance_id`，崩溃或Part删除后不可能再返回hot ack。协调器只有取得该旧对象的终结证明，才可用 `INSTANCE_GONE` 代替hot ack：旧Part已从权威topology移除，或基础设施/进程准入层已经fence旧boot并证明其服务端口不再提供请求；单纯heartbeat超时不够，因为失联进程仍可能服务。替代的新boot/new Part必须以新identity明确报告original hot且完成BOOT_WARMUP。没有终结证明时保持RECOVERY_REQUIRED，不得为了恢复自动化而缩小分母。

### 11.11 UNKNOWN原因码与重启/时钟语义

至少定义以下稳定 reason bitset：

```text
BOOT_WARMUP
SAMPLE_GAP
COUNTER_RESET_OR_OVERFLOW
OBSERVATION_CONFIG_CHANGED
PART_STARTING_OR_STOPPED
WAITING_SNAPSHOT
WAL_APPEND_ERROR
ROLE_TERM_OR_TOPOLOGY_CHANGE
REPLICA_MISSING_OR_STALE
LEARNER_OR_CATCHUP
SNAPSHOT_SEND_OR_RECEIVE
CHECKPOINT
INGEST_OR_RESTORE
BACKUP
REBUILD
BALANCE
COMMIT_LAG
GRAPH_REQUEST_CONTEXT_UNKNOWN
GRAPH_OBSERVER_MISSING_OR_STALE
GRAPH_MEMBERSHIP_OR_INGRESS_FENCE_UNKNOWN
GRAPH_ADMISSION_SHADOW_OR_NOT_READY
STORAGE_INGRESS_UNKNOWN
UNSUPPORTED_PROVENANCE
COLLECTION_ROUND_STALE_OR_INCOMPLETE
AUTO_DECISION_LEASE_EXPIRED
AUTO_QUALIFICATION_OR_WATCH_EXPIRED
UNSUPPORTED_MIXED_VERSION
```

重启后不能沿用持久化 wall timestamp 直接判冷：

- 新 storaged `boot_id/part_instance_id` 从 `UNKNOWN/BOOT_WARMUP` 开始，重新积累完整长窗口；新 graphd `boot_id` 的全部 Space 从 `GRAPH_UNKNOWN/BOOT_WARMUP` 开始；
- NTP前后跳不影响 `steady_clock` 决策；进程 suspend 或采样间隔超限触发 `SAMPLE_GAP`；
- WAL 文件 mtime 会被周期空记录持续更新，不能反推出最后业务写时间；
- 新建、balance迁入、重建 Part 和 mixed-version 不支持观察能力的副本均为 UNKNOWN；
- 中央历史时序可供人查看，但不能替新 graphd/storaged 进程授权跳过 warmup；
- MANUAL policy 可按控制面规则在重启后继续生效，AUTO cold decision 必须重新验证。

### 11.12 指标、查询接口与高基数控制

默认接口只输出每 Space 的有限聚合：

```text
space_id / space_name
topology_revision
collection_round_id / report_fresh_deadline
expected/observed logical_parts
expected/observed replicas
expected/reported graphd instances
hot/cooling/cold/unknown/busy counts
unknown reason counts
short/long nonempty-record rate
short/long nonempty-byte rate
graph logical read/write rate and active_queries
graph RPC attempt/result rate
storage total ingress rate / optional origin breakdown
max_part_rate
minimum_last_nonempty_age
coverage
observation_config_epoch
graph_demand / graph_observation_validity
wal_temperature / storage_ingress_guard / safety_guard / observation_validity
eligible_for_cold
current/original/approved_target（仅展示已配置或已审批值）
space_control_owner
space_decision_epoch/state/application_scope_hash/lease_expiry
hot/cold/mixed applied replica counts
```

每 Part 明细必须通过带 `space_id/part_id` 过滤、分页和数量上限的只读接口查询。建议命令形态：

```text
SHOW SPACE WAL HEAT <space>
SHOW SPACE WAL HEAT DETAIL <space> [PART <id>] [LIMIT <n>]
SHOW WAL BUFFER CANDIDATES
```

Graph 明细至少展示 graph host/boot、report_seq、Space、逻辑读写、RPC attempts/results、active queries、最后活动年龄、窗口完整度和Graph状态；Storage Part明细至少展示 host、boot/Part实例、role/term/status、最后非空活动年龄、WAL窗口、旁路ingress、commit lag、capability、busy/unknown reason和当前/目标容量。

禁止为所有 `(space,part,host,boot_id)` 或 `(space,graph_host,boot_id)` 创建永久 Prometheus/StatsManager label。普通 `/stats` 只提供聚合状态数和 top-K hot/unknown Space/Part；boot ID、版本、错误文本放在按需响应字段，不作为 label。Heartbeat 只携带紧凑的 capability、boot、配置/成员版本和摘要hash；高基数明细由 Meta 独立低频从 graphd/storaged 分页拉取。

### 11.13 Shadow验证、人工准入和自动化门槛

发布顺序必须是：

```text
OBSERVE_ONLY
  -> 至少覆盖一个完整业务周期
  -> 与业务审计/定时任务/请求日志对照
  -> 人工使用观察结果选择Space
  -> MANUAL capacity canary
  -> 证明无false-cold后，才允许单Space AUTO canary
```

shadow 阶段至少验证：

1. 每个判定为 `SPACE_COLD_ELIGIBLE` 的窗口都同时具备完整 Graph 业务证据与 Storage WAL/安全证据，且确实没有已知业务读写、批处理、ingest、rebuild、balance 或 Snapshot；
2. 一个热点 Part、一个缺失副本、一个缺失graphd或任一侧旧 boot 报告都能阻止 Space 判冷；
3. `cold -> hot` 唤醒次数、唤醒后的 WAL disk fallback、Snapshot、commit P99 和业务 P99 可解释；
4. 观察器 CPU、锁等待和内存开销在预算内；
5. 长窗口至少覆盖业务的日/周峰谷周期，而不是只覆盖测试空闲时段；
6. 上线 AUTO 前 shadow 记录中的 false-cold 必须为零；false-hot 可以接受并继续优化。

自动化只允许对明确设置 `SpaceControlOwner=AUTO` 的 Space 生效。首个 AUTO canary 仍遵循第22节单 host、单 Space、Leader路径、跨 target、多轮GC和24小时稳态门槛。观察器的 `COLD` 只说明有资格使用已批准的 target，不负责选择 target 大小。

### 11.14 一个完整判定示例

假设 `archive_graph` 有20个逻辑 Part、RF=3，Meta 的一致 membership snapshot 期望60个普通 replica和3个显式注册graphd报告：

```text
SHOW SPACE WAL HEAT archive_graph

topology_revision: 8f31...
collection_round_id: 9182
expected_logical_parts: 20
expected_replicas: 60
observed_replicas: 60
graph_membership_generation: 17
expected_graphd: 3
reported_graphd: 3
coverage: 100%
wal_temperature: COLD
observation_validity: VALID
graph_demand: GRAPH_COLD
graph_observation_validity: GRAPH_VALID
storage_ingress_guard: STORAGE_COLD
safety_guard: SAFE
cold/cooling/hot/unknown: 20/0/0/0 logical parts
minimum_last_nonempty_age: 8d 3h
long_window_nonempty_records: 0
max_part_nonempty_rate: 0/s
graph_logical_read/write_rate: 0/s / 0/s
graph_active_queries: 0
storage_total_rpc_rate: 0/s
busy_parts: 0
space_control_owner: OBSERVE_ONLY
space_decision_state: NO_DECISION
eligible_for_cold: true
approved_target: not_set
```

这里的 `true` 只表示“可以进入人工容量评审”，不会自动选择2MiB或修改 target。以下任一变化都会得到不同结果：

- 只收到59/60个 replica：`coverage<100%`、`REPLICA_MISSING_OR_STALE`、不可判冷；
- 只收到2/3个预期 graphd，或一个 graphd 重启尚未观察完整窗口：`GraphObservationValidity=GRAPH_UNKNOWN(reason=GRAPH_OBSERVER_MISSING_OR_STALE)`、不可判冷；
- 第17个逻辑 Part 有一个副本出现非空 WAL：该逻辑 Part 为 HOT，整个 Space 不可判冷；
- WAL 全冷但 Graph 仍有逻辑读请求：显示 `GRAPH_HOT`，AUTO不可判冷；MANUAL必须有专项审批；
- Graph 显示冷但 storaged 仍收到任意data RPC（包括未知来源直连RPC）：显示 `STORAGE_HOT`，不可判冷；
- 正在 rebuild 第5个 Part：显示 `SYSTEM_BUSY/REBUILD`，busy结束后重新积累完整窗口；
- storaged 重启：该 host 上的 replica进入 `BOOT_WARMUP`，中央历史不能替新 boot 直接授权。

只有证据持续满足、target经过第12节容量预算审批并由 MANUAL/AUTO控制面提交后，才会进入 `COLD_APPLIED`；`COLD_APPLIED` 仍须继续观察磁盘 fallback、Snapshot 和P99。

### 11.15 建议从 graphd 侧先开展，但按三阶段交付

用户业务冷热能力可以、也建议先从 graphd 开始，因为它能最早产出有价值的 Space 使用画像，且第一阶段完全只读：

```text
G0  graphd GraphBusinessHeatObserver
    -> 输出GRAPH_HOT/GRAPH_COLD_CANDIDATE
    -> 只展示，不改wal_buffer_size

S0  storaged WAL/ingress/safety observer
    -> 输出Storage Part的WAL/Safety资格或否决
    -> 仍只展示

F0  Meta Leader融合完整Graph+Storage collection round
    -> 才允许输出SPACE_COLD_ELIGIBLE
    -> 先供MANUAL审批；shadow通过后才进入AUTO
```

G0 的最小交付范围是：StorageClient准确Space归属、READ/MUTATE分类、logical/RPC分栏、active query、boot/config epoch、有界窗口、预期graphd成员与缺报UNKNOWN、分页查询接口。它不依赖 Atomic setter，可以独立上线验证一个完整业务周期。

但 G0 不能直接驱动缓存：Graph 查询服务本身没有集群 Leader/共识，单个 graphd 只看到部分流量；而 Graph 请求活动也不能替代 Storage 的本地 WAL 和副本事实。自动修改 `wal_buffer_size` 的控制权始终留在 Meta+Storage 闭环内。

## 12. 容量、时间窗与收敛模型

### 12.1 当前实验二进制的计费关系

本次 Debug 构建实测：

```text
空Record逻辑charge = 16B
每Node固定Record数 = 64
sizeof(Node) = 3200B
jemalloc usable/Node = 3584B
```

因此一个填满空日志的 Node：

```text
逻辑charge = 64 × 16 = 1024B
allocator usable = 3584B
放大系数 = 3584 / 1024 = 3.5
```

在当前 ABI/allocator、正常短 Reader、已经进入稳态的条件下：

```text
每Part有效Node usable平台 ≈ target_capacity × 3.5
```

它不是 RSS hard limit，还不包含：

- partial head；
- 0～约6个正常锯齿 dirty Node；
- 长 Reader 延迟回收的 dirty 链；
- allocator metadata、active/resident page；
- 非 SSO payload 的外部分配；
- RocksDB、RPC、线程栈和其他 storaged 内存。

商业生产二进制的编译器 ABI 和 allocator 未必相同，上线前必须重新测 `sizeof(Node)` 和 allocator usable。

`capacity` 还是软逻辑阈值，不是对象数或 RSS 的硬上限：new-head 分支不检查容量，唯一 Node 不能淘汰，大 payload 也可能造成明显 overshoot。验收区间必须允许至少一个 Node 和最大正常 record 的粒度偏差。

### 12.2 空日志内存时间窗

设：

```text
C = 目标逻辑容量（byte）
H = raft_heartbeat_interval_secs
T_delay ≈ H/3 + 0.2495秒
```

忽略 callback 执行、worker 排队和调度抖动，空日志名义保留时间为：

```text
window(C,H) ≈ (C / 16) × T_delay
```

发布配置 H=30 的条件估算：

| target | 有效空日志Node/Part | 空日志名义时间窗 | 当前构建usable/Part | 1000 cold parts |
|---:|---:|---:|---:|---:|
| 8MiB | 8192 | 约62.2天 | 约28MiB | 约27.34GiB |
| 4MiB | 4096 | 约31.1天 | 约14MiB | 约13.67GiB |
| 2MiB | 2048 | 约15.5天 | 约7MiB | 约6.84GiB |
| 1MiB | 1024 | 约7.8天 | 约3.5MiB | 约3.42GiB |
| 256KiB | 256 | 约46.6小时 | 约896KiB | 约875MiB |

`256KiB` 只是在空日志模型下看起来仍有约两天窗口；真实业务 payload 会更快消耗逻辑容量，第一版不能据此直接使用极小值。

### 12.3 在线降容收敛时间

原生 `push()` 每次最多标脏一个旧 Node，并且每64条日志会有一次 new-head push 跳过容量判断。满缓存、空日志条件下，从 `C_old` 降到 `C_new` 的粗略估算为：

若初始 head 已满，前 `p` 次后续空 push 的逻辑有效 size 近似为：

```text
size(p) = C_old + 16p - 1024 × (p - ceil(p/64))
```

它同时计算了“新 push 自身增加16B”和“非 new-head push 每次淘汰一个1024B满 Node”。忽略取整后：

```text
pushes_to_remove ≈ (C_old - C_new) / (1024 × 63/64 - 16)
                  = (C_old - C_new) / 992
shrink_time ≈ pushes_to_remove × T_delay
```

逐条模拟当前分支后，H=30 的名义值为：

| 热更新 | 净移除有效Node/Part | 约需push/Part | 名义逻辑收敛时间 |
|---|---:|---:|---:|
| 8MiB -> 4MiB | 4096 | 4230 | 约12.04小时 |
| 8MiB -> 2MiB | 6144 | 6344 | 约18.06小时 |
| 8MiB -> 1MiB | 7168 | 7400 | 约21.07小时 |
| 8MiB -> 256KiB | 7936 | 8192 | 约23.32小时 |

1000个 Part 会并行收缩，所以空日志模型下墙钟时间仍是约一天，而不是1000倍；但总 allocator、GC 和磁盘压力也会并行出现。push 更密只代表获得更多淘汰机会；真实收敛取决于“新 Record charge”与“被淘汰 tail Node charge”的差值。若新业务 Record 远大于旧空日志 Node，新增 charge 可能抵消甚至超过每次约1024B的淘汰，逻辑 size 未必更快收敛，soft-target overshoot 也会更大。

这些只是名义逻辑窗口估算，实际墙钟必须使用每个 Part 的实测 push 差值。长 Reader 会让物理 Node 收敛更晚；没有后续 push 时永远不会收缩。

首次部署新二进制时，滚动重启已经把旧进程的8MiB缓存清空，不能在该 canary 上观察“8MiB满缓存降到2MiB的18.06小时收缩”。该模型应在实验室先预填满8MiB后验证，或在以后真实高水位热降容时验证。首次生产启用2MiB target，要在 H=30、纯空日志假设下跨过约15.55天的从空填充时间，才有资格判断新平台是否成立。

### 12.4 内存预算选值

设：

```text
N_hot  = 本机保持默认容量的local replica-parts
N_cold = 本机应用cold target的local replica-parts
C_hot  = hot容量
C_cold = cold容量
alpha   = 本机实测物理放大系数
```

空日志主缓存预算近似：

```text
B_atomic ≈ alpha × (N_hot × C_hot + N_cold × C_cold)
```

应选择“满足 Atomic 内存预算的最大 `C_cold`”，以尽量保留内存追赶窗口：

```text
C_cold <= (B_budget / alpha - N_hot × C_hot) / N_cold
```

同时还必须满足：

```text
C_cold >= max(2 × 现网p99.9单批Raft逻辑charge, 最大正常批次逻辑charge)
```

还要验证磁盘 WAL 的保留范围覆盖现网故障恢复 RTO，并为新增磁盘回读预留资源。生产 canary 前建议数据盘空闲至少30%，最终仍以现网更严格的容量门槛为准。

如果两个条件无法同时满足，说明仅调缓存不能兼顾内存与追赶性能，需要减少不必要 partition、迁移冷 Space 或增加资源，而不是继续把容量压到危险值。

## 13. 完整场景示例

### 13.1 拓扑

假设每台 storaged：

```text
50个Space × 每Space 20个local parts = 1000 local replica-parts
5个热Space = 100 hot parts
45个冷Space = 900 cold parts
```

全部使用8MiB时，本次 ABI 条件估算：

```text
1000 × 8MiB × 3.5 ≈ 27.34GiB
```

热 Space 保持8MiB、冷 Space 设置2MiB：

```text
hot: 100 × 8MiB × 3.5  ≈ 2.73GiB
cold: 900 × 2MiB × 3.5 ≈ 6.15GiB
total ≈ 8.88GiB
```

相对全局8MiB，条件估算减少约18.5GiB；相对全局2MiB，多保留约2GiB用于保护热 Space 的 WAL 命中窗口。

### 13.2 一次热更新

运维设置：

```text
spaceId=10
target=2MiB
expected policy_generation=7
expected last_modified_epoch=41
```

控制面：

```text
CAS成功；同一Raft batch写space last_modified=42和snapshot epoch=42/hash=H42
  -> storaged-1收到desired (generation=7, epoch=42, hash=H42)
  -> 发布本地policy map
  -> 对space 10的20个普通Part执行版本保护的capacity apply
  -> 报告applied (7,42,H42)，20/20成功
  -> storaged-2/3随后分别应用
```

数据面：

```text
setter完成时：若此前已填满，buffer可能仍接近8MiB
后续空push：逐个标脏最旧Node
reader release：按原生GC删除dirty链
约6344次push、名义18.06小时后：逻辑有效窗口接近2MiB
物理Node和RSS：可能更晚或不同比例回落
```

### 13.3 Space恢复业务

人工模式下，运维以新的更大 snapshot epoch 将 target 恢复8MiB。自动模式下，graphd 的首个逻辑请求 dispatch 会增加本机 Space graph activity epoch、使本机旧 candidate 失效并异步请求整个 Space进入 `HOT_RECOVERING`；它不阻塞查询，也不能单独线性化集群状态。请求抵达storaged后，目标 Part先在ingress线性化点升容；第一条非空 Raft WAL再增加 `wal_activity_epoch`、确认本 Part hot target并发布 Space decision invalidation，然后执行当前 `push()`。

恢复后的第一条业务批次不会被继续按2MiB target主动淘汰，但此前已经删除的缓存不会预热。落后 peer 如需更旧日志，仍可能走磁盘 WAL 或 Snapshot。

## 14. 数据、查询与Raft影响分析

### 14.1 查询正确性

图查询读取的是 RocksDB 状态，不读取 Atomic WAL cache：[`NebulaStore::get()`](../src/kvstore/NebulaStore.cpp#L746-L761)、[`NebulaStore::range()`](../src/kvstore/NebulaStore.cpp#L808-L823)。

因此容量热更新不会直接改变查询返回的数据。它可能通过以下间接路径影响延迟或可用性：

- Follower 追赶变慢；
- Leader transfer 后磁盘读取增加；
- Snapshot 占用 IO；
- storaged P99 上升；
- 极端磁盘 iterator 异常导致分片暂时不可用。

### 14.2 Raft safety

容量不改变日志内容或 committed 水位。即使内存缓存已经淘汰尚未 commit 的日志，磁盘 WAL 仍是权威副本，这是现有原生容量淘汰就允许的行为。

本方案不需要使用 `committedLogId` 作为 setter 条件，因为 setter 不主动删除任何日志，只改变未来 `push()` 使用的原生容量阈值。

### 14.3 Follower追赶与Snapshot

内存 miss 时会尝试磁盘 WAL；如果目标起点早于磁盘 `firstLogId()`，Leader 可能转 Snapshot：[`Host.cpp:306-345`](../src/kvstore/raftex/Host.cpp#L306-L345)。

因此小缓存主要改变追赶性能路径，而不是复制协议：

```text
内存命中减少
  -> 磁盘WAL读取增加
  -> 磁盘范围不足时Snapshot增加
```

第一批变更必须保持现网有效 `wal_ttl` 不变。降低 TTL 不会释放 Atomic live Node，却会进一步缩短磁盘增量追赶窗口。

### 14.4 磁盘iterator/rollback边界

`rollbackToLog()` 会修改或截断磁盘 WAL：[`FileBasedWal.cpp:569-619`](../src/kvstore/wal/FileBasedWal.cpp#L569-L619)，而 `WalFileIterator` 生命周期当前没有持有对应的长期读锁：[`WalFileIterator.cpp:15-105`](../src/kvstore/wal/WalFileIterator.cpp#L15-L105)。

热降容会增加这一既有边界的暴露概率。启用前必须：

- 做磁盘 iterator 与 rollback/ftruncate 的确定性交错测试；
- 覆盖 Leader/Follower 换届、冲突回退和 Snapshot；
- 监控短读、EOF、`E_RAFT_NO_WAL_FOUND` 和 CHECK；
- 若不能证明当前实现安全，先完成统一锁序或不可变文件/version 方案。

不能简单给 iterator 生命周期加读锁而不审计 Listener 等路径的反向锁序。

## 15. 不能消除的残余路径

### 15.1 空日志和磁盘WAL

本方案不停止：

- 周期空 NORMAL 日志；
- Leader/Follower 本地 WAL 文件写入；
- AppendLog 网络复制；
- Node 的持续分配与释放 churn。

达到 cold target 后，预期变化是：

```text
Atomic live valid Node净增长接近0
```

不是：

```text
累计分配、CPU、网络和磁盘活动全部归零
```

健康稳态仍会持续分配和释放 Node：当前 H=30、1000 local parts、本次 jemalloc size class 的条件估算，allocator usable churn 仍约450MiB/日，只是 live valid Node 不再继续净累计。长 Reader 还会延迟 dirty Node 的物理回收，并可能在最后一次释放时形成批量 free。

### 15.2 RocksDB commit-key路径

空日志在 `Part::commitLogs()` 中跳过业务 payload，但仍更新每个 Part 的 `systemCommitKey(partId)` 并执行 RocksDB Write：[`Part.cpp:215-358`](../src/kvstore/Part.cpp#L215-L358)、[`Part.cpp:403-410`](../src/kvstore/Part.cpp#L403-L410)、[`RocksEngine.cpp:120-140`](../src/kvstore/RocksEngine.cpp#L120-L140)。

因此以下现象仍然存在：

- active memtable entries/bytes 增长；
- memtable flush；
- SST 和 compaction；
- allocator和page cache导致RSS不完全回落。

第一版不应同时做 idle-specific RocksDB flush，否则无法区分 Atomic 收益和 IO/compaction 副作用。

### 15.3 RSS边界

Node delete 只表示 live object 释放。jemalloc 可能继续保留 extent/page，RSS 不一定同步下降。验收必须优先看 live/valid/dirty Node 和 allocator allocated，再把 RSS 作为次级指标。

## 16. 风险清单与处理

| 等级 | 风险 | 可能后果 | 必须措施 |
|---|---|---|---|
| P0 | push发布新tail后与reader GC并发 | UAF、计数下溢、storaged崩溃 | 以tail发布为最后线性化点；或经证明的synthetic ref/互斥；确定性交错测试 |
| P0 | 磁盘iterator与rollback/ftruncate | 短读、EOF、CHECK、分片不可用 | 并发测试；必要时先修同步/文件版本机制 |
| P1 | target过小 | 磁盘回读、commit P99、Snapshot增加 | 按p99 batch和内存预算选最大可接受值 |
| P1 | 长Reader延迟GC | dirty链累积、最后释放时CPU长尾 | refs/dirty指标、压力测试、分阶段降容 |
| P1 | 控制面乱序或部分应用 | 节点容量漂移、性能不对称 | per-Space CAS、完整快照generation/epoch/hash、applied ack |
| P1 | 把各host不同的gate/install epoch当成Meta单一CAS版本 | 某副本错误接受旧owner或无法提交 | Meta只CAS MetaOwnerVersion；持久每replica token vector/hash；host apply校验各自EffectiveCapacityVersion |
| P1 | 重启丢失内存态策略 | 静默恢复默认容量 | 先持久化desired，再应用live Part |
| P1 | 误把热Space设为冷 | 热业务磁盘IO和P99上升 | 第一版OBSERVE_ONLY；完整业务周期shadow；全部普通Part/副本100%覆盖；人工审批 |
| P1 | 缺报、旧boot或拓扑变化被误当成无流量 | false-cold、错误自动降容 | UNKNOWN fail-closed；以同一topology revision的预期副本集合为分母 |
| P1 | 以heartbeat active set替代显式Graph成员、让新graphd先接流量后入分母，或把未建Space entry当零 | 仍在接流量的graphd被移出/游离分母、false-cold | 持久GraphObserverMembership；SHADOW->ENFORCED；新增REGISTERING/OBSERVER_BOOTSTRAPPING/ADMITTED入口fencing；移除走DRAINING request lease；全Space零值覆盖证明；缺报/重启/缺traits均UNKNOWN |
| P1 | 分页/多host数据跨collection round拼接 | 虚假100% coverage和false-cold | immutable report identity；缺页/过期/换boot整轮作废；SHOW只读完整round |
| P1 | 多个AUTO协调者、Meta切主/lease过期后重放旧冷决策 | 已fail-hot的Part被迟到任务再次降冷 | 仅Meta Raft Leader单写；完整AUTO overlay与reject-through fence；切主/quorum/boot/lease事件进入RECOVERY_REQUIRED并用新round、更大decision重授权 |
| P1 | Space冷应用部分成功 | 长期一热十九冷、行为难审计 | prepare/commit；任一失败ABORTING并补偿恢复；mixed状态和超时指标 |
| P1 | AUTO冷任务与Graph请求、新WAL/Storage ingress/安全状态竞态，或prepared token过期仍apply | 活跃或busy Space被迟到任务重新设成cold target | Graph snapshot只作集群证据；Part锁内重验本机deadline/lease/instance/epochs；Storage ingress与wal/safety/config/version使旧prepare失效并先恢复hot；非空WAL在push前再次兜底 |
| P1 | 新建/balance迁入Part直接继承旧AUTO cold | 新实例未参与原decision却以小缓存运行 | AUTO scope精确绑定boot/part instance；新Part hot启动并使旧decision失效；只有MANUAL可直接继承 |
| P1 | Snapshot/ingest/rebuild/balance绕过普通WAL | 后台忙碌Space被误判冷 | 独立SYSTEM_BUSY provenance；未接入即UNKNOWN |
| P1 | Space平均值掩盖单个热点Part | all-or-nothing策略误伤热点分片 | 任一Part HOT即阻止；输出max_part_rate和热点Part列表 |
| P2 | false-hot过多 | 可治理Space长期不进入候选 | 区分Graph logical/RPC、Storage旁路、WAL和admin origin；false-hot先保守接受，shadow后再调阈值 |
| P2 | per-Part指标高基数或热路径锁竞争 | 额外内存、CPU和WAL写长尾 | O(1)原子采集；有界时间桶；默认仅Space聚合和top-K；明细按需分页 |
| P1 | Listener被意外纳入 | 外部索引apply lag | 第一版显式 DATA_PART_ONLY |
| P1 | 多Space同时降容 | allocator、GC、IO峰值 | 分host、分Space、8->4->2分级应用 |
| P2 | 升容后缓存不预热 | 短期磁盘回读仍高 | WARMING状态和预期说明，不做危险reset |
| P2 | jemalloc不还页 | RSS下降不明显 | 同时观察allocated/active/resident和live Node |
| P2 | RocksDB次路径继续 | 仍有残余内存周期 | 独立观测，后续单独立项 |

## 17. 与其他方案对比

### 17.1 总体对比矩阵

| 方案 | 是否选择性影响冷Space | 是否需重启 | 是否改变Raft/空日志 | 主路径收益 | 主要风险/缺点 | 结论 |
|---|---|---|---|---|---|---|
| 全局调小现有 `wal_buffer_size` | 否 | 是 | 否 | 压低所有Part平台 | 热Space也更早磁盘回退；重启选举 | 无源码时首选，不是本次最优目标 |
| 静态per-space override | 是 | 是 | 否 | 冷Space平台降低 | 每次冷热变化需滚动重启 | 允许重启时的最低源码风险首选 |
| **per-space atomic热容量** | **是** | **首次部署需重启，后续更新不需要** | **否** | **冷Space渐进降低，热Space保持原窗口** | **需并发硬化；收敛非即时；控制面复杂** | **在线精细化治理首选** |
| committed-aware主动trim | 是 | 首次部署需重启，后续触发不需要 | 否 | 可主动、限量收缩 | 新增第二条tail Writer、valid计数、GC协调，回归面大 | 仅在需要确定快速回收时考虑 |
| 修改 `Record::size()` 物理计费 | 否 | 是 | 否 | 全局更早淘汰 | ABI/SSO/allocator难精确；改变全部Space容量语义 | 后续独立产品化课题 |
| 只为空日志增加accounting charge | 否 | 是 | 不改复制，但针对空日志 | 压低空日志平台 | 魔法常量、空/1字节跳变、全局影响 | 不推荐第一版 |
| 修改Heartbeat/no-op机制 | 全局 | 滚动升级 | 是 | 从上游消除主次路径 | commit传播、Follower追赶、混版和Raft liveness风险 | 根治潜力最高，但不符合低风险约束 |
| 增大Heartbeat间隔 | 全局 | 通常需滚动 | 改变时序 | 近似按比例降低斜率 | 选举、lease、故障发现和RTO改变 | 不作为首波 |
| 降低 `wal_ttl` | 否 | 视配置而定 | 否 | 对Atomic无效 | 缩短追赶窗口、增加Snapshot | 不应使用 |
| allocator purge/drop caches | 否 | 否 | 否 | 对live Node无效 | 只能处理已free/page cache，可能增加IO | 无法解决主因 |
| 定期滚动重启 | 全局 | 是 | 不改源码 | 立即清空进程缓存 | 反复选举，原斜率重新开始 | 只作为应急止血 |
| 减少partition | 按Space | 现有Space需迁移 | 否 | 同时减少主路径和Rocks次路径 | 无法原地缩分片；迁移、校验和切流成本高 | 长期架构治理 |
| 删除确认废弃Space | 是 | 不一定 | 否 | 完全消除对应开销 | 数据恢复、业务审计和误删风险 | 仅适用于真正废弃数据 |

### 17.2 相比全局wal_buffer_size

优势：

- 热 Space 保留8MiB或现有原始容量；
- 只对明确冷 Space 付出磁盘 fallback 代价；
- 不需要为每次目标调整重启 storaged；
- 可以逐 Space、逐 host 灰度。

劣势：

- 需要少量源码和控制面；
- 热更新收敛需要后续 push；
- 必须解决并发和持久化边界。

### 17.3 相比静态per-space override

优势：

- 避免缓存策略变更引起的 Leader 迁移和 RF3 单节点下线窗口；
- 可以快速把误配置的 target 调回较大值；
- 更适合大量 Space 生命周期管理。

劣势：

- `capacity_` 从 immutable 变成 atomic，增加并发模型；
- 需要 desired/applied/converged 状态；
- 回滚 target 不会预热已经淘汰的内容。

### 17.4 相比主动trim

优势：

- 控制线程不修改链表；
- 不需要 committed 水位判断；
- 不需要 valid-node 计数和批量 tail CAS；
- 不新增第二条 Writer；
- 不会在一次清理任务中主动标脏数万 Node；
- 代码和一致性证明明显更小。

劣势：

- 不能分钟级确定释放；
- 无 push 时不收缩；
- 依赖当前周期空日志继续驱动；
- 收缩速率只能通过容量分级和 Part 批次间接控制。

主动 trim 可以做到与未来 push 无关，但速度与资源峰值必须交换：例如1000个满缓存 Part、每10分钟全进程最多标脏4096个 Node 时，扣除空日志继续新增的 Node 后，8MiB->2MiB 的名义理想下界仍约13.4天；若无全局限制地允许每 Part 每轮标脏64个 Node，名义可缩到约16.2小时，但一轮最多触及64,000个 Node，约218.75MiB allocator usable，容易形成同步 GC/free 峰值。这些都是“候选全部 eligible、无长 Reader/Snapshot/balance 阻挡”的理想值，标脏字节也不等于同轮 RSS 下降。它能更确定地推进，却引入第二个链表 Writer 和更难的并发证明，所以不作为第一版。

### 17.5 相比修改空日志上游

优势：

- 不改变 Raft wire、commit 传播或 Follower catch-up；
- 新旧副本可以安全混跑；
- 不需要重新证明 Leader lease 和 election 行为；
- 回归主要集中在缓存和性能路径。

劣势：

- 不能消除磁盘 WAL、网络和 RocksDB commit-key；
- Node 分配/释放 churn 仍存在；
- 它是内存治理，不是上游根因消除。

## 18. 为什么本方案最佳

“最佳”不是指零风险，而是指在当前约束下位于更好的 Pareto 前沿：

```text
选择性：优于全局调参和Record计费
可用性：优于每次滚动重启
协议风险：优于修改Heartbeat/no-op
并发复杂度：优于后台主动trim
长期治理能力：优于周期重启
实施成本：低于现有Space迁移减分片
```

它保留了原生缓存淘汰的核心机制，只扩展“这个阈值从哪里来、能否原子更新”。与主动 trim 相比，这是最关键的风险收敛：控制线程永远不成为新的链表 Writer。

但推荐结论有明确条件：

1. push/GC 并发窗口已经修复并通过 sanitizer；
2. 磁盘 iterator/rollback 边界已验证；
3. 策略有持久化 generation/epoch/hash 和 applied 状态；
4. 第一版启用只读观察和人工操作，不把自动执行同时上线；
5. 冷热证据对持久Graph成员中的全部graphd和Storage拓扑中的全部预期普通Part/副本100%覆盖，UNKNOWN一律fail-closed；
6. 容量按真实 batch 和内存预算选取；
7. 先单 Space、单 host canary。

任一条件不满足时，应退回静态 per-space override，而不是带风险启用热更新。

## 19. 预计代码改动面

### 19.1 Phase 0：并发硬化

涉及：

- `src/kvstore/wal/AtomicLogBuffer.h/.cpp`
  - Writer/GC淘汰线性化修复；
  - 确定性测试 hook；
  - 精确valid-node计数、一致指标快照与GC指标。
- `src/kvstore/wal/WalFileIterator.*`、`FileBasedWal.*`
  - 磁盘 iterator/rollback 并发审计和必要修复。

这一阶段不增加热配置，行为和容量保持默认。

### 19.2 Phase G0/S0/F0：只读冷热观察与融合

这一阶段默认 `OBSERVE_ONLY`，不得依赖或调用 capacity setter。涉及：

- `FileBasedWal.h/.cpp`
  - 在成功更新 WAL 元数据之后、`logBuffer_->push()` 之前调用 O(1) 活动回调；
  - 分开累计 non-empty records/bytes 和 empty records；
  - gap/preprocessor拒绝、fsync错误调用 WAL health hook，设置safety epoch/UNKNOWN；短写进程退出后由新boot warmup兜底；
- graphd `StorageClient`、`StorageClientBase` 与 Query生命周期
  - 按准确 `space_id` 记录 logical dispatch、真实RPC attempt、result和active query；
  - 显式区分 READ/MUTATE/ADMIN/SESSION_ONLY，并单列重试、partial、timeout/ambiguous；
  - 注入 `QueryHeatTracker/query_heat_token`，touch/close恰好一次；逐方法输出 `GraphMethodCoverage` 与hash；
  - 新增有界 `GraphBusinessHeatObserver`，不用入口/结束时Session Space和动态高基数label作为决策真值；
- graphd 请求 admission gate
  - process request lease覆盖所有成功、失败和early-return Future；
  - 支持SHADOW/ENFORCED、每实例admission epoch/token、DRAINING与注册失败回滚；
- storaged `GraphStorageServiceHandler`、必要的 Processor/`StorageEnv`
  - 记录全部 per-Space/per-Part Storage ingress；可信origin只用于解释，不能成为首版安全依赖；
  - completion 只补成功率和延迟，不反推逻辑业务 QPS；
- `RaftPart`/`Host`/Snapshot manager
  - 通过一致 Raft state 快照和受锁 accessor 暴露 role、term、status、lag、incoming/outgoing Snapshot；
- storage admin task 与 Meta balance state
  - 以 Space/Part 和 job type 暴露 ingest、checkpoint、rebuild、restore、backup、balance busy provenance；
  - 输出每种provenance的 `OBSERVED/PROVEN_DISABLED/UNSUPPORTED_UNKNOWN` capability矩阵；
- 新增有界 `StorageSpaceHeatObserver/PartHeatObservation`
  - boot/Part实例、wal/storage-ingress/safety/config epoch；
  - 两级固定时间桶、短/长 EWMA、UNKNOWN reason bitset；
  - 同一 topology revision 下的全部预期副本 fail-closed 聚合；
- graphd 与 storaged immutable heat report
  - boot/report_seq/collection round/topology/config/hash identity；
  - 快照绑定分页、TTL和缺页整轮失败；
  - graphd提供独立只读heat-report RPC/HTTP管理端点；不把全量Space窗口塞进普通query响应或高频heartbeat；
- Meta Leader只读 `HeatAggregationCoordinator`
  - 新增持久 `GraphObserverMembership`、全局generation、每实例admission epoch、SHADOW/ENFORCED模式和显式decommission/fencing流程；
  - 新graphd执行REGISTERING/OBSERVER_BOOTSTRAPPING/ADMITTED准入；加入expected set并使旧decision失效后，才签发绑定instance/boot/admission epoch的入口token；注册失败走REGISTRATION_ABORTED；移除走DRAINING process lease；
  - 生成一致Storage topology和持久Graph observer membership snapshot；
  - 拉取全部预期graphd与storaged报告，只原子发布两侧都完整的round；
  - Meta切主丢弃半轮并重新采集；
- Graph admission在G0/F0仅运行SHADOW判定，不拒流；现有实例无中断bootstrap和ENFORCED切换属于Phase 3前置变更，切换后重建完整Graph长窗口；
- 只读管理接口
  - 默认输出 Space 汇总与 top-K；
  - Part 明细按 Space/Part 过滤和分页；
  - 不在默认 StatsManager/Prometheus 中创建全量高基数 label。

G0 可先独立运行一个完整业务周期，输出 Graph候选；S0/F0 接入后再完成端到端 shadow，并与业务审计对照。三个阶段都只为后续人工策略提供证据，不能自动改变任何 target。

### 19.3 Phase 1：本机per-space热容量

涉及：

- `AtomicLogBuffer.h/.cpp`
  - atomic capacity；
  - checked setter/getter；
  - push单次容量快照；
  - `int64_t` projected比较；
- `FileBasedWal.h/.cpp`
  - 转发setter和当前target指标；
- `RaftPart.h/.cpp`、普通 `Part`
  - original hot capacity；
  - DATA_PART_ONLY 转发；
- `NebulaStore.h/.cpp`
  - immutable policy map；
  - generation/epoch/hash；
  - Part shared_ptr快照和应用状态；
- storage policy manager与配置解析；
- `StorageHttpStatsHandler` 或独立只读状态接口；
- 配置模板和测试。

### 19.4 Phase 2：集群级产品控制面

涉及：

- 独立 Meta key 编解码；
- Meta processors 和 service handler；
- MetaClient policy snapshot 拉取；
- 独立于普通gflags `loadCfg()/skipConfig_` 的缓存与reconcile；
- heartbeat desired/applied optional 字段；
- Graph 管理语法和 executor；
- Drop Space、backup、restore；
- mixed-version 和 failover 测试。

### 19.5 Phase 3：可选自动执行

涉及：

- 在 Phase F0 融合只读证据之上增加 `SpaceControlOwner=AUTO`；
- per-part `prepareCold()/applyCold()/failHotCurrentForActivity()/failHotIfMatches()/failHotOnDeadlineIfMatches()` 与活动/安全回调原子迁移；
- 完整 `BasePolicyState/MetaOwnerVersion/QualifiedColdIdentity/QualificationWatchState/PreparedColdToken/ActiveAutoOverlay/CapacityGuard/LocalAutoFence`，基础策略与AUTO overlay分域，并由唯一reducer实现owner/gate/MANUAL/AUTO版本优先级；
- 持久化 `space_decision_epoch` 的Meta Leader单写协调器；
- 全replica qualification、scope内prepare/commit、部分应用ABORT补偿和AUTO decision lease；
- Graph请求使Space token失效并异步HOT_RECOVERING；Storage非空WAL/安全事件使本Part同步hot恢复；
- Graph read/write snapshot + Storage wal/ingress/safety/config/effective-capacity 完整版本令牌；
- Graph observer的SHADOW->ENFORCED、REGISTERING/OBSERVER_BOOTSTRAPPING/ADMITTED/REGISTRATION_ABORTED准入与DRAINING移除fencing；
- 新Part、Meta切主、quorum中断或lease过期后的fail-hot与重新授权；
- decision-scoped、完整key保护的guard enable/clear；活动回调统一采用atomic快路径、仅当前decision进入policy-lock慢路径；
- Space all-or-nothing 资格判断和显式 per-Space AUTO gate；
- hysteresis、冷却/确认窗口、自动升回hot target和竞态测试；
- 保留 MANUAL 和 activation gate 对 AUTO 的绝对优先级。

## 20. 测试方案

### 20.1 AtomicLogBuffer单元测试

- `capacity_` 并发 set/load 的 TSan 测试；
- lower、raise、reset policy；
- 本次 push 使用一次容量快照；
- `size + recSize` 的64位边界；
- 63/64/65条和唯一 Node 边界；
- 8MiB -> 2MiB 连续淘汰多轮；
- dirty=6、最后 Reader 在淘汰中间释放的确定性交错；
- 多 Reader、Writer、容量更新线程的 ASan/TSan 长跑；
- 长 Reader 导致 dirty 积累，最后释放后安全 GC；
- raise 后 dirty Node 仍按预期删除；
- 无 push 时 setter 不导致链表变化。

### 20.2 FileBasedWal测试

- initial、target、current capacity 指标一致；
- Atomic miss 后磁盘 iterator 返回完全相同的 LogID/term/source/msg；
- 大批次超过 cold target；
- iterator 与 rollback/ftruncate 并发；
- reset、rollback 后保留正确 original/target policy；
- WAL first/last LogID 和磁盘文件不因 setter 改变。

### 20.3 策略管理器测试

- 非法 Space、0、负数、溢出、重复 policy；
- expected last_modified_epoch CAS冲突；
- 旧 generation/epoch 拒绝、同版本同hash幂等补应用、同版本异hash报错；
- 新generation安装后旧generation Part任务晚到，按持久generation_install_epoch拒绝；同install epoch异generation报错；
- MetaOwnerVersion只含集群共同字段；三host不同gate/install epoch分别进入token vector并由各Part校验，不能压成单一CAS值；
- owner在OBSERVE_ONLY/MANUAL/AUTO间切换时，基础target与旧AUTO overlay按优先级正确失效；
- 同一 `EffectiveCapacityVersion=E` 已记录基础hot target后，安装/重放 AUTO decision D 的cold overlay不得发生同key异target冲突；周期性重放E只能更新 `BasePolicyState`，不得清除D；MANUAL/gate更新经唯一reducer压过D；
- single-flight worker并发收到多个快照时只完成最新版，旧任务不能覆盖；
- 完整快照解析失败保留 last-known-good；
- apply 中途新增/删除 Part；
- 进程在持久化后、应用一半时崩溃；
- 启动后台loadPart在构造、插入之间收到新快照，不得漏应用；
- 重启后新 Part 按 owner规则恢复：MANUAL继承当前target，AUTO默认hot且旧decision失效；
- Listener 默认不受影响；
- RESET 原子规范化为 `enabled=false + OBSERVE_ONLY`，恢复 original hot capacity并清除/否决 AUTO overlay；`AUTO cold D1 -> RESET -> delayed D1/new D2` 均不得再次降冷，直到一个更大epoch显式重新启用policy；
- activation gate的ENABLE/DISABLE/重启与boot_id语义；
- enable->disable后旧enable任务晚到、disable->enable重放最新版，均以gate_epoch拒绝陈旧任务；
- applied 不等于 converged；
- 旧backup恢复后新policy_generation强制全量reconcile。

### 20.4 三副本集成测试

- 三副本分别使用8MiB/4MiB/2MiB，LogID、term、commit、查询一致；
- 单节点、单 Space hot apply；
- Leader 使用较小容量时的落后 Follower 追赶；
- Leader transfer；
- Follower 短时和长期离线；
- 磁盘 WAL 存在时增量追赶；
- 磁盘 WAL 不足时 Snapshot；
- 写入、查询和管理命令并发；
- balance/rebuild/addPart/removePart；
- storaged 在 APPLYING 中崩溃并恢复；
- metad Leader 切换和 epoch CAS；
- 新旧 storaged 混跑但功能未启用。

### 20.5 性能与长跑

- H=1 的加速跨容量实验：先把 buffer 预填满8MiB，再热降到2MiB，至少覆盖6344次后续 push 和多轮 GC；
- H=30 的真实调度验证；
- 1000 local parts 同时降容；
- 8->4->2分级与直接8->2对比；
- p50/p95/p99 batch charge分布；
- Atomic hit/miss；
- WAL disk bytes、IOPS、await；
- Snapshot数量和耗时；
- allocator allocated/active/resident；
- valid/dirty/live Node；
- 查询、写入、Leader transfer和追赶P99。

### 20.6 冷热观察与自动决策测试

- 成功非空 WAL append 在 `push()` 前增加 wal activity epoch；失败 append 不计成功热度，但拒绝/fsync错误必须增加safety epoch并置WAL_APPEND_ERROR；空 record 只进入 empty 计数；
- Leader、Follower、Follower catch-up 和 rollback 下的信号语义符合11.4节，RF3汇总不重复相加；
- Graph `USE A; READ; USE B; MUTATE` 按实际 `StorageClient::CommonRequestParam.space` 分别归属A/B，不能按入口或结束Session Space混算；
- Graph logical dispatch与physical RPC attempt分栏；失败、partial、timeout、Leader Changed、多Part/多host fan-out和商用事务 origin 不混算，Mutation timeout仍判热；
- `QueryHeatTracker` 并发touch同一Space的READ/MUTATE只各增一次active，finish/error/cancel只close一次；过期或无法关联heat token必须置UNKNOWN；长查询期间不能判冷；
- process request lease覆盖无效Session、解析/验证失败、正常完成、异常和所有early-return；DRAINING关闭新lease并等旧lease归零，不能被当前kNumActiveQueries提前放行；
- Graph observer对全部当前Space建立零值覆盖证明；新Space、缺失traits方法或启用未观测商用入口必须进入GRAPH_OBSERVING/UNKNOWN，map中无entry不能当冷；
- `GraphMethodCoverage` 仅接受INSTRUMENTED/PROVEN_UNUSED_BY_GRAPH；构建或配置变化使hash变化并重建窗口，UNSUPPORTED_UNKNOWN阻止AUTO；
- storaged任意data RPC能独立触发Storage ingress保护；直连/内部/未知来源在Graph报告为冷时仍不能被漏掉；
- Snapshot send/receive、checkpoint、ingest、restore、backup、各类 rebuild、balance 和 commit lag 正确设置/清除 busy；
- capability矩阵任一AUTO必需来源为UNSUPPORTED_UNKNOWN时必须阻止资格；PROVEN_DISABLED只接受受版本控制的build/config hash；
- 一个 HOT Part、一个缺失/陈旧 replica、一个预期graphd缺报、任一侧boot变化或 mixed-version 均阻止 `SPACE_COLD_ELIGIBLE`；
- Graph expected membership只有完成DRAINING request lease、流量fencing和显式decommission才能缩小；模拟graphd失去Meta heartbeat但仍接收请求时必须保持`GraphObservationValidity=GRAPH_UNKNOWN(reason=GRAPH_OBSERVER_MISSING_OR_STALE)`；
- 现有3实例以SHADOW_SERVING无中断bootstrap：SHADOW不拒流且最多输出CANDIDATE；预装全部per-instance token并ack后切ENFORCED，配置epoch变化必须重置正式长窗口；
- ENFORCED下新graphd必须先加入expected set、完成readiness并取得当前boot/admission epoch的ADMITTED token，之后才允许LB/服务端放行业务；注册失败且从未ADMITTED/无lease时可REGISTRATION_ABORTED移除；
- add/remove一个graphd必须使旧heat round/AUTO decision失效，但不能撤销其他实例各自仍有效的admission token或造成全Graph拒流；
- 重启、新建/balance迁入 Part 必须重新走完整 `BOOT_WARMUP`，wall clock/NTP跳变不影响判断；
- sample gap、counter reset/overflow、配置版本变化都进入 UNKNOWN，而不是零速率；
- 时间桶边界、窗口覆盖、EWMA按实际采样间隔计算，并验证低速非零第一版只展示不执行；
- qualification/prepare/apply冷任务与第一条非空 WAL 的确定性交错均保证先恢复 hot target、旧 decision 无法覆盖；scope外qualification活动也必须把token置INVALID并通知；
- `Storage ingress升hot -> delayed applyCold(D)` 必须被local fail-hot fence拒绝；`lease expiry升hot -> delayed applyCold(D)/Meta重放D` 同样拒绝；
- `D1 delayed guard clear -> D2 guard remains GUARDING`；guard ARMING/ack/epoch重验与ingress/WAL/safety/applyCold全部确定性交错；无decision的OBSERVE/MANUAL/AUTO热路径不得获取policy mutex；
- D1迟到的ABORT/HOT_RECOVERING、prepare/watch/lease timer和clear必须因expected key不匹配而no-op，不能把已合法COLD_APPLIED的D2 fail-hot；真实业务活动仍无条件失效当前D2；
- 同一个D跨阶段的旧timer也必须no-op：QUALIFIED转WATCHING后旧qualification timer、token CONSUMED并建立ACTIVE后旧prepare timer、ACTIVE建立后旧watch timer均须因 kind/object generation/phase/deadline 不匹配被拒绝；只有同一ActiveOverlay的exact lease timer可以结束该lease；
- fail-hot持锁且尚未store hot target时到达第二个ingress，必须看到FAIL_HOT_IN_PROGRESS并等待，不能从已清watch/guard的无锁路径旁路；
- 同D幂等apply不能改变首次lease deadline；host在prepare阶段拒绝越界lease或回报最终effective duration，Meta只提交所有scope一致的exact值，apply不得二次clamp；续租必须先hot ack、再以更大D重新qualification/prepare；
- 全部预期replica在COMMIT后先转WATCHING再允许scope apply；scope外watch覆盖整个lease，watch超时转INVALID/HOT_RECOVERING而不是静默清除；
- MANUAL/activation gate 始终高于 AUTO，旧 AUTO token 在策略/Part/config变化后失效；
- 聚合使用预期 topology 集合和每逻辑Part副本max；单热点Part不会被Space平均值掩盖；
- Graph/Storage immutable report分页必须保持同一boot/report_seq/hash；缺页、跨页换boot、过期、跨membership/topology round全部作废；
- Meta Leader切换时丢弃半轮采集，只发布完整collection round；旧AUTO协调者/decision epoch无法写入；旧cold record只能进入RECOVERY_REQUIRED/HOT_RECOVERING，不能直接重放；
- 所有预期replica参加qualification，只有application scope修改target；COLD prepare不修改target，Part锁内必须重验本机deadline/lease/token/instance/epochs；任一prepare/apply失败进入ABORTING，并把已降Part补偿恢复hot；
- 任一Part Storage ingress/新非空WAL/busy在各自本地线性化点先同步升容，再使Space decision失效；Graph新请求只立即失效本机Graph token并异步通知Meta，不能被测试误认为集群同步屏障；验证与并发COLD commit交错时Storage prepare/apply被epoch拒绝或先cold后立即hot，MIXED_CAPACITY最终收敛HOT；
- Meta不可用时AUTO decision lease到期，各host按本地steady clock恢复hot并写reject-through fence；普通heartbeat不能续租，重新cold需要新完整round和更大decision；
- cold host崩溃后，只有权威topology移除或基础设施明确fence旧boot才接受INSTANCE_GONE；新boot/new Part必须hot ack，不能等待不可能返回的旧实例，也不能仅凭heartbeat超时缩小scope；
- 10,000级 Part 下验证观察器CPU、锁等待、时间桶内存和管理接口分页上限；
- 至少运行一个完整业务周期的 OBSERVE_ONLY shadow，与业务审计、批处理和管理任务日志逐项对照；
- AUTO canary 统计 false-cold、cold->hot唤醒、WAL disk fallback、Snapshot、commit/业务P99；false-cold非零即禁止扩面。

## 21. 指标与状态接口

### 21.1 每Part指标

```text
space_id
part_id
policy_generation
policy_snapshot_epoch
generation_install_epoch
space_last_modified_epoch
gate_epoch/desired_gate_enabled
space_control_owner
original_capacity_bytes
target_capacity_bytes
auto_decision_generation_install_epoch/space_decision_epoch
auto_application_scope_hash/auto_state
auto_qualified_identity_hash/qualification_deadline
auto_watch_state/watch_commit_hash/watch_deadline
auto_capacity_guard_key/state
auto_reject_through_epoch/local_fail_hot_seq
auto_prepare_state/deadline/commit_hash
auto_effective_lease_duration/fixed_deadline
accounted_bytes
valid_nodes
dirty_nodes
live_nodes
refs
state
last_policy_apply_time
atomic_hit/miss
wal_file_iterator_bytes/latency
gc_runs/deleted_nodes/duration_us/max_deleted_nodes
dirty_nodes_high_water
boot_id/part_instance_id
observation_config_epoch
wal_activity_epoch/storage_ingress_epoch/safety_epoch
observation_validity/wal_temperature/storage_ingress_guard/safety_guard/reason_mask
observer_capabilities
wal_nonempty_records/bytes_total
wal_empty_records_total
last_nonempty_age
short/long wal record/byte rate
storage total/read/write RPC attempt rate and optional origin breakdown
raft role/term/status/commit_lag
busy_mask/topology_revision
report_seq/collection_round_id/fresh_deadline
```

当前观测补丁的 `nodes_` 统计尚未 delete 的全部 Node，包含 dirty Node，并不存在精确 `valid_nodes`：[`AtomicLogBuffer.h:390-393`](../src/kvstore/wal/AtomicLogBuffer.h#L390-L393)。产品实现必须新增在结构线性化点维护的 valid-node 计数。由于 push Writer 与 `releaseRef()` GC 可以并发，分别读取多个 atomic 不是一致快照；用于硬验收的 `snapshotMetrics()` 必须使用轻量 per-buffer stats mutex 或另一种经过证明的 multi-writer 一致快照机制。普通独立 atomic 采样只能用于趋势，不能断言 `live=valid+dirty` 的瞬时不变量。

统计同步只保护计数，不保护或遍历 Node 链表；不得在持有它时获取 registry、NebulaStore、Raft 或 Meta 锁。若采用 synthetic ref 备选，还必须用 GC duration/max-deleted 指标量化 Writer 同步 GC 长尾。

### 21.2 每Space汇总

```text
target_parts
applied_parts
shrinking_parts
steady_parts
warming_parts
error_parts
expected/observed logical_parts
expected/observed replicas
expected/reported graphd instances
hot/cooling/cold/unknown/busy parts
unknown reason counts
short/long nonempty record/byte rate
graph demand/validity
graph logical read/write rate
graph RPC attempt/success/partial/timeout/ambiguous rate
graph active read/write queries
graph minimum observation age/last read/write age
graph membership generation/ingress policy hash
graph admission mode / per-instance admission state+epoch / process request leases
storage total ingress rate / optional origin breakdown
max_part_rate/minimum_last_nonempty_age
observation_coverage/eligible_for_cold
space_control_owner
space_decision_generation/epoch/state/application_scope_hash/lease_expiry
recovery_required/hot_recovering counts
hot/cold/mixed applied replica counts
sum_accounted_bytes
sum_live_node_bytes
desired_generation/epoch/hash
host_applied_gate/generation/epoch/hash
```

### 21.3 集群状态

```text
host capability/process_instance_id/boot_id/ready
role=GRAPH|STORAGE and observer membership generation
graph admission mode/instance admission epoch+state/process request leases
gate_epoch/state
desired generation/epoch/hash
applied gate/generation/epoch/hash
policy hash
heat observer capability/config epoch
collection_round/topology_revision/report hash/freshness
AUTO coordinator Meta term/space decision epoch/state
AUTO application scope/hash
last error
```

接口必须避免高基数默认全量输出。普通 `/stats` 可只给聚合值，按 Space/Part 的明细通过带过滤参数的只读管理接口查询。

### 21.4 冷热查询与告警

建议提供：

```text
SHOW SPACE WAL HEAT <space>
SHOW SPACE WAL HEAT DETAIL <space> [PART <id>] [LIMIT <n>]
SHOW WAL BUFFER CANDIDATES
```

告警至少包括：

- Space 曾为 COLD/COLD_APPLIED 后短时间内恢复 HOT；
- observation coverage 低于100%、报告陈旧或 boot/topology 不一致；
- SYSTEM_BUSY 长时间不清除；
- AUTO 与 MANUAL/gate 冲突，或旧 token 被拒绝；
- cold target 后 Atomic miss、磁盘 WAL read、Snapshot、commit P99 超出基线；
- 观察器 sample gap、时间桶内存预算或热路径耗时超限。

普通监控只保留 Space 聚合与状态计数。Part/replica明细必须按需分页拉取，不能把 `boot_id`、reason文本或所有 Part ID 作为长期时序 label。

## 22. 发布、灰度与回滚

### 22.1 发布前门槛

- Phase 0 的并发硬化已独立合入并通过 ASan/TSan；
- 磁盘 iterator/rollback 测试完成；
- capacity/AUTO功能默认关闭，冷热观察默认 `OBSERVE_ONLY`；
- 观察器已覆盖至少一个完整业务周期，所有候选均能与业务审计和管理作业日志对上；
- 预期普通Part/副本coverage为100%，false-cold为0，UNKNOWN严格fail-closed；
- 观察器CPU、锁等待、内存和管理接口高基数开销在预算内；
- 三台 storaged 均健康，RF完整；
- 无 balance、rebuild、backup、snapshot 管理作业；
- 无持续 `E_RAFT_*`、LOG_GAP、WAITING_SNAPSHOT；
- 已记录基线 Atomic、WAL IO、Snapshot、P99 和 allocator；
- 备份和恢复路径已验证。

### 22.2 推荐灰度顺序

1. 按10.5节顺序完成 metad、必要的 graphd、storaged 部署，所有 storaged capacity/AUTO gate 保持关闭，Graph admission保持SHADOW；
2. 一次性把当前所有实际Graph入口纳入持久expected membership，完成observer capability/零值对账/boot绑定；SHADOW只记录准入判定，不拒绝任何现有业务；
3. 仅开启 `OBSERVE_ONLY`，验证容量关闭且admission SHADOW时与原版本数据/协议行为一致；
4. 让观察器覆盖至少一个完整业务周期，核验100% Graph/Storage覆盖、UNKNOWN reason、业务日/周峰谷和false-cold；
5. 在实验环境完成“预填满8MiB，再热降4MiB/2MiB”的真实跨容量验证；
6. 仅一台 Leader 最少的 storaged 启用本地capacity能力，仍不启用AUTO；
7. 从 `SHOW WAL BUFFER CANDIDATES` 选择1个全部普通 Part/副本均有VALID COLD证据的 Space；存在热点Part、缺报或busy时不得选择，除非业务负责人显式接受并记录；
8. 以MANUAL方式设置经预算批准的 canary target；若当前 accounted 已接近8MiB，可按8->4->2分级，否则直接设置最终 target；
9. 确认 canary 节点实际承载该 Space 的 Leader；若没有，在健康RF3和无管理作业时做一次受控 Leader transfer；
10. 等待 buffer 真正达到/跨过 target，观察原生淘汰、磁盘 fallback 和多轮 GC，并在逻辑/物理收敛后完成至少24小时稳态验收；
11. 同时覆盖一个业务高峰，检查观察状态、查询/写入P99、Follower追赶、Snapshot、IO和异常日志；
12. 只有上述路径全部通过后，才依次启用第二、第三台；
13. MANUAL全量稳定且shadow false-cold持续为0后，为现有graphd预装各自boot/admission token；全部ack后无中断切到ENFORCED，并因admission配置变化重新观察一个完整长窗口；
14. ENFORCED窗口仍为false-cold=0后，另开变更对单一Space、单一host启用AUTO；该decision仍要求全部预期replica完成qualification，只有canary scope修改target；
15. 第一轮始终保持 heartbeat、`wal_ttl`、RocksDB 参数不变。

首次生产部署的重启已经清空旧缓存，不能借此证明“8MiB->2MiB在线收缩”。H=30、纯空日志、2MiB target 从空触顶名义约15.55天；若不使用经审批的受控负载，就必须等待这段实际 push 窗口后才能扩第二台。不能用15分钟或尚未跨容量的24小时 RSS下降证明方案有效；重启、allocator purge和 target setter 的效果必须分开。

### 22.3 中止门槛

出现以下任一情况，冻结后续更新并恢复较大 target：

- 持续 `E_RAFT_NO_WAL_FOUND` 或 LOG_GAP；
- Snapshot backlog 或频率显著上升；
- WAL read await/IO 超出现网容量；
- 业务错误率或P99突破既有SLO；
- term/Leader频繁抖动；
- dirty Node 长时间只增不减；
- applied generation/epoch不收敛或节点policy hash漂移；
- observation coverage不足、boot/topology报告不一致或出现任何false-cold；
- 观察器热路径CPU/锁等待、时间桶内存或管理接口负载超出预算；
- AUTO覆盖MANUAL/gate，或第一条非空WAL前未恢复hot target；
- ASan/TSan/日志出现链表、引用或计数异常。

### 22.4 回滚

回滚不是倒退 epoch，而是在同一 generation 中提交一个更大的新 snapshot epoch：

```text
snapshot epoch 42: target=2MiB
snapshot epoch 43: target=8MiB
```

顺序：

1. 停用新管理命令入口，冻结后续 Space 更新；
2. 持久化恢复 hot target 的新 snapshot epoch；
3. 等目标节点 applied；
4. 持续观察 WARMING、磁盘 IO 和 Snapshot；
5. 逐 host 进入 DISABLING，确认所有 live buffer target 已恢复 original 后再进入 DISABLED；
6. 清理旧二进制不认识的新配置，按 storaged -> graphd -> metad 逆序逐台降级；
7. 不删除 data、WAL 或 RocksDB 文件。

已经淘汰的 Node 不会回填，回滚 target 不能撤销此前发生的磁盘读取、Snapshot 或业务超时。

## 23. 验收标准

### 23.1 正确性

- 三副本 LogID、term、commit/apply一致；
- 业务数据校验一致；
- 无 UAF、double-free、dirty下溢和CHECK；
- 不改变 WAL、Raft Thrift、Snapshot 和 RocksDB 数据格式；Meta 管理面只允许向后兼容的 optional 字段扩展；
- mixed capacity 下 Leader transfer 和追赶正确；
- 同一 generation 内 policy snapshot epoch 在崩溃、重试和 Meta切主后严格单调；restore 使用新 generation rebase。

### 23.2 功能

- 指定 Space 的全部普通 Part 收到 target；
- 未指定 Space 保持 original capacity；
- 新建 Part 按owner应用：MANUAL继承当前policy，AUTO以hot启动并使旧scope失效；
- reset policy 恢复 original target；
- Listener 第一版不受影响；
- desired/applied/converged 三种状态可区分；
- 目标 Part 在经生产SLO批准的初始 guardrail（候选值60秒）内100%报告 desired generation/epoch/hash、target 和 applied 状态，不接受旧版本回写；
- 重启、新建 Part、balance 后继续继承MANUAL持久策略；AUTO必须以新实例、新topology重新授权。

### 23.3 内存

- cold Part 的 accounted bytes 最终接近 target 的 Node粒度区间；
- valid/live Node 从旧平台下降并形成锯齿平台；
- dirty Node 在健康短 Reader 下不长期线性增长；
- allocator allocated 与 live Node方向一致；
- 不把 RSS 立即下降设为唯一成功条件；
- 实验室预填满8MiB后热降2MiB：每 Part 累计约6344次空 push 后，`valid_nodes <= ceil(2MiB/1024)+1`；
- 逻辑收敛且 Writer/旧 Reader 静默后，确认 `gcOnGoing=false`，再由一次最后引用释放触发/完成 GC；在此条件的一致快照中验证 `dirty_nodes <= 5`、`live_nodes <= valid_nodes+5`；若 GC CAS 竞争失败则等待后重试，不能把一次普通 release 当成必然完成；
- 目标 Space 的有效 Node usable 相对8MiB平台下降理论值约75%，验收下限建议不低于70%；
- 逻辑/物理收敛后继续观察至少24小时，valid/live Node 不再出现持续正斜率。

### 23.4 性能

以下百分比是等待生产 SLO/容量团队批准的初始 guardrail，不是由当前测试环境推导出的通用不变量；现有 SLO 更严格时取更严格者。

- WAL disk fallback、Snapshot、IO和P99在现网SLO和资源预算内；
- 热 Space 的 Atomic命中和业务P99不因其他冷 Space策略发生明显回归；
- 1000 Part 批量应用没有形成不可接受的GC/allocator峰值；
- 大批次、Follower落后和Leader transfer专项通过；
- 若现有SLO没有更严格门槛，建议业务/commit P99回归不超过10%，CPU P95增幅不超过10%；
- 数据盘持续 util 低于70%、5分钟峰值低于85%，WAL读延迟回归不超过20%；
- Follower catch-up 不超过基线1.2倍且仍满足故障恢复RTO；
- 排除正常管理作业后，健康RF3下非预期 Snapshot 不高于基线，且无持续 `E_RAFT_NO_WAL_FOUND` 或不可用 Part。

### 23.5 冷热观察

- `SPACE_COLD_ELIGIBLE` 必然覆盖同一 topology revision 下100%的预期普通 logical Part/replica，以及同一持久Graph membership generation下100%的预期graphd；
- 每次判断只使用一个Graph+Storage两侧完整且未过期的 collection round；分页boot/report_seq/hash一致，Meta切主、拓扑或Graph membership变化不拼接旧页；
- `WalTemperature=COLD` 只有在完整长窗口、零 non-empty WAL delta且wal epoch稳定时成立；自动资格还要求Storage ObservationValidity=VALID、StorageIngressGuard=STORAGE_COLD、SafetyGuard=SAFE、GraphObservationValidity=GRAPH_VALID、GraphDemand=GRAPH_COLD且active query为零；
- 任一非空 WAL 在进入 Atomic buffer 前完成 HOT迁移和AUTO target升容；
- Graph logical dispatch能按实际Space ID区分读写和多语句切换，单次logical attempt不被host/Part fan-out重复计数；执行器/客户端重发另计attempt，Mutation timeout/partial仍判热；
- 一个热点Part、缺失/陈旧副本、缺失graphd、任一侧boot变化、任意Storage ingress、sample gap、Snapshot、ingest、rebuild或balance都能阻止自动判冷；
- WAL拒绝/fsync错误、backup busy或任一必需observer capability未知都能阻止AUTO；
- RF3重复日志不会被当作三倍业务写，Space平均值不会掩盖max_part_rate；
- 重启和新Part必须经过完整BOOT_WARMUP，wall clock跳变不能制造COLD；
- OBSERVE_ONLY覆盖至少一个完整业务周期，候选与业务审计/批处理日志一致，false-cold为0；
- G0/F0的admission SHADOW不得拒流；AUTO前必须无中断切到ENFORCED并重建完整长窗口。新graphd先进入expected membership、完成OBSERVER_BOOTSTRAPPING并取得per-instance ADMITTED token再接流量；移除走DRAINING lease，注册失败只能在从未准入/无lease时回滚；缺少可验证入口fencing时AUTO保持禁用；
- MANUAL和activation gate永远优先于AUTO；本机活动/lease expiry形成reject-through fence，陈旧或同一已失效decision全部被拒绝；
- AUTO冷进入要求全部预期replica完成qualification、application scope内完成prepare后才commit（正式全量scope即全部普通replica）；Part锁内重验token/deadline/instance/epochs，失败补偿恢复hot；本Part活动同步升容后整个scope最终收敛HOT_RECOVERING->hot；
- Meta切主、失去quorum、host boot变化或lease到期后旧cold只能进入RECOVERY_REQUIRED/HOT_RECOVERING；重新降冷必须使用新完整round和更大decision；
- 默认接口无无限高基数label，10,000级Part观察开销满足已批准CPU、内存和锁等待预算。

## 24. 最终建议

推荐按以下顺序实施：

```text
Phase 0  修复并证明现有淘汰/GC与磁盘iterator并发边界
Phase G0 graphd业务热度OBSERVE_ONLY，只输出GRAPH_COLD_CANDIDATE
Phase S0 storaged WAL/旁路/安全事实OBSERVE_ONLY
Phase F0 Meta融合完整Graph+Storage round，只输出/展示SPACE_COLD_ELIGIBLE
Phase 1  默认关闭的本机per-space atomic热容量，人工操作
Phase 2  独立Meta policy generation/epoch/hash/applied控制面
Phase 3  经完整业务周期shadow验证后的可选Space级自动执行
```

第一批生产版本不要同时启用主动 trim、AUTO容量切换和 RocksDB idle flush。冷热观察保持 `OBSERVE_ONLY`，先证明：

```text
指定冷Space的target可热更新
  + 原生push/GC安全渐进收缩
  + 热Space保持原缓存窗口
  + 磁盘fallback和Snapshot在预算内
```

第一版只对观察器证明“同一Graph成员版本下全部预期graphd均为GRAPH_VALID/COLD，同一Storage拓扑版本下全部普通 Part/副本均为VALID COLD，且旁路请求和SYSTEM_BUSY保护通过”的 Space 使用。存在不可接受的分片冷热倾斜、任一侧缺报或UNKNOWN时，跳过该 Space，等待证据恢复或另立per-part策略。这个准入条件与并发硬化、磁盘 fallback压测同等重要。

在这些条件成立后，按图空间热更新 `wal_buffer_size` 是当前约束下最合适的方案：它没有触碰 Raft 正确性核心，把影响限制在明确的冷 Space，并且避免了主动 trim 的第二 Writer 和全局调参对热业务的无差别伤害。冷热功能的推荐职责固定为：**graphd 主观察业务，storaged 提供权威否决并执行，Meta Leader 聚合与协调**；不能退化成任何单个 graphd 自主改容量。
