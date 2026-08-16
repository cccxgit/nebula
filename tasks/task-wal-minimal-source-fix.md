# WAL 空闲内存持续增长：最小源码修复设计

> 适用源码：Nebula Graph 3.6 当前工作树。
> 本文只给出修复设计、正确性边界和验证门槛；本轮没有修改产品源码。
> “不引入问题”无法由代码行数保证。本文所说的最优，是在修复根因、保持 Raft 语义、缩小改动面三者之间取得最小闭环。

## 1. 结论

最优方案不是仅在 `sendHeartbeat()` 中增加一行：

```cpp
!commitInThisTerm_
```

而是完成下面四个彼此依赖的最小闭环：

1. **按当前状态限制 no-op 日志**：新 Leader 在当前 term 尚未提交任何日志时，仍允许追加必要的空 `NORMAL` 日志；一旦当前 term 已经有日志提交，稳定状态下不再周期创建新的空日志。选举和业务并发边界允许极少量有界额外 no-op，不承诺“每个 term 物理上恰好一条”。
2. **补齐 follower 的零 entry happy path**：当前 follower 在“前置日志完全匹配且请求中没有新日志”时，没有把 `lastMatchedLogId` 更新到本地日志尾部。补齐这一状态后，既有提交逻辑才能应用 Leader 携带的 commit 水位。
3. **让 Host 待发送目标按同一 term 单调合并**：零 entry 通知不能覆盖正常复制中更大的目标 LogID；所有共享 Future 必须等到合并后的最大目标完成后才兑现。
4. **用零 entry 的 AppendLog 继续传播 commit 水位和驱动追赶**：稳定 Leader 每轮继续发送真正的 Heartbeat RPC；当没有正常日志正在复制时，再复用现有 `Host::appendLogs()`，发送当前 `lastLogId/committedLogId`。已追平的 follower 收到零 entry AppendLog，不产生新 WAL；落后的 follower 由现有逻辑补发已有日志。Leader 新行为必须受默认关闭的兼容开关控制，待兼容代码全量部署后再启用。

产品逻辑不需要改 WAL 格式、Thrift 协议、RocksDB 格式、Meta 数据或 `AtomicLogBuffer` 数据结构。严谨实现预计涉及 `RaftPart.cpp`、`Host.cpp/Host.h` 和对应测试；这比一行 guard 多，但它是避免复制确认错误所必需的最小安全边界。

## 2. 为什么不能只加一行 guard

### 2.1 无锁读取会引入 C++ data race

`commitInThisTerm_` 是普通 `bool`，不是 atomic：

- 字段与语义：[`src/kvstore/raftex/RaftPart.h:855-857`](../src/kvstore/raftex/RaftPart.h#L855-L857)
- 新 Leader 当选后置 `false`：[`RaftPart.cpp:1377-1389`](../src/kvstore/raftex/RaftPart.cpp#L1377-L1389)
- 当前 term 首次成功提交后置 `true`：[`RaftPart.cpp:1078-1095`](../src/kvstore/raftex/RaftPart.cpp#L1078-L1095)
- Leader lease 读取：[`RaftPart.cpp:2254-2267`](../src/kvstore/raftex/RaftPart.cpp#L2254-L2267)

这些既有访问都受 `raftLock_` 保护。直接在当前 [`RaftPart::sendHeartbeat()`](../src/kvstore/raftex/RaftPart.cpp#L2041-L2122) 顶部裸读，会与选举线程或复制响应线程发生未同步读写。

因此 `needNoop = !commitInThisTerm_` 必须在已有 `raftLock_` 临界区内读取，并与 `term_`、role、commit ID、日志尾部一起形成同一份状态快照。

### 2.2 周期空 AppendLog 还承担了隐式 commit 传播

Leader 在写入一批新日志之前，先快照旧的 `committedLogId_`，并把旧值放进本轮 AppendLog 请求：

- [`RaftPart::appendLogsInternal()`，RaftPart.cpp:874-915](../src/kvstore/raftex/RaftPart.cpp#L874-L915)

所以 follower 收到第 N 条日志时，通常只知道第 N-1 条以前的 commit 水位；follower 在下一次 AppendLog 中才按新的 `req.committed_log_id` 提交：

- [`RaftPart::processAppendLogRequest()`，RaftPart.cpp:1785-1804](../src/kvstore/raftex/RaftPart.cpp#L1785-L1804)

独立 Heartbeat RPC 虽然携带 `committed_log_id`，但当前 handler 只校验 Leader、重置选举计时器并返回，不执行 `commitLogs()`：

- 请求字段：[`src/interface/raftex.thrift:97-106`](../src/interface/raftex.thrift#L97-L106)
- 请求构造：[`Host::sendHeartbeat()`，Host.cpp:408-418](../src/kvstore/raftex/Host.cpp#L408-L418)
- follower handler：[`RaftPart::processHeartbeatRequest()`，RaftPart.cpp:1895-1951](../src/kvstore/raftex/RaftPart.cpp#L1895-L1951)

当前未修复版本依靠“下一轮又产生一条空 AppendLog”顺带传播上一次 commit 水位。现有测试甚至显式等待一个 heartbeat 周期，让最后一批日志在 followers 上提交：

- [`src/kvstore/raftex/test/LogAppendTest.cpp:110-117`](../src/kvstore/raftex/test/LogAppendTest.cpp#L110-L117)

若只加 guard，最后一条业务日志或新 term no-op 可能长期停留在 follower WAL 中而未 apply，直到下一次真实写入或选举。

### 2.3 周期空 AppendLog 还驱动空闲期 follower catch-up

真正的 Heartbeat RPC 只把暂停的 Host 恢复为可用，不会补发日志：

- [`Host::sendHeartbeat()`，Host.cpp:408-464](../src/kvstore/raftex/Host.cpp#L408-L464)

追赶由 `Host::appendLogs()` 和其 WAL 游标驱动：

- [`Host::appendLogs()`，Host.cpp:92-164](../src/kvstore/raftex/Host.cpp#L92-L164)
- [`Host::prepareAppendLogRequest()`，Host.cpp:287-345](../src/kvstore/raftex/Host.cpp#L287-L345)

纯 guard 会使离线 follower 重连后，在集群没有新写入时只收到 Heartbeat，而没有任何 AppendLog 去触发追赶。

此外，Leader 侧的 `Host::followerCommittedLogId_` 只由 AppendLogResponse 或 snapshot 更新：

- [`Host.cpp:176-214`](../src/kvstore/raftex/Host.cpp#L176-L214)
- [`Host.cpp:346-365`](../src/kvstore/raftex/Host.cpp#L346-L365)

它又参与 [`RaftPart::isCaughtUp()`](../src/kvstore/raftex/RaftPart.cpp#L2186-L2207) 的 snapshot/catch-up 判断。因此 commit 状态长期不更新还可能影响管理操作。

## 3. 推荐的最小语义闭环

### 3.1 修复点 A：补齐 follower 零 entry happy path

目标位置：[`RaftPart::processAppendLogRequest()`，RaftPart.cpp:1673-1679](../src/kvstore/raftex/RaftPart.cpp#L1673-L1679)。

当前条件：

```cpp
req.last_log_id_sent == lastLogId_ &&
req.last_log_term_sent == lastLogTerm_
```

已经证明 Leader 声明的前置日志与 follower 本地尾部匹配。若请求没有新 entry，应显式把匹配水位设为本地尾部：

```cpp
if (numLogs == 0) {
  lastMatchedLogId = lastLogId_;
  resp.last_matched_log_id_ref() = lastLogId_;
  resp.last_matched_log_term_ref() = lastLogTerm_;
}
```

随后既有代码会提交到：

```cpp
min(lastMatchedLogId, req.committed_log_id)
```

位置：[`RaftPart.cpp:1785-1804`](../src/kvstore/raftex/RaftPart.cpp#L1785-L1804)。

这比直接在 `processHeartbeatRequest()` 中按 `committed_log_id` 调用 `commitLogs()` 更安全，因为 AppendLog 路径已经校验前置日志 ID/term，并有日志缺口、回滚、snapshot 和错误响应逻辑；当前 Heartbeat handler 没有这些一致性检查。

### 3.2 修复点 B：保证 Host 的待发送目标单调

这是零 entry 方案的 P0 前置条件。当前 [`Host::appendLogs()`，Host.cpp:92-164](../src/kvstore/raftex/Host.cpp#L92-L164) 在已有请求进行时执行：

```cpp
pendingReq_ = std::make_tuple(term, logId, committedLogId);
return cachingPromise_.getFuture();
```

`pendingReq_` 只有一个槽，后来请求会无条件覆盖先前请求，多个等待者共享同一个 `cachingPromise_`。现有正常业务的目标 LogID 通常单调增加，所以问题不明显；引入周期零 entry 后，陈旧目标可能覆盖更大的业务目标：

```text
零 entry N 正在发送
  -> 正常复制 N+1 进入 pending
  -> 另一条陈旧通知 N 覆盖 pending
  -> N 的响应兑现共享 Future
  -> Leader 可能把未到达该 follower 的 N+1 误计为成功
```

这属于一致性风险，不能靠“调用前看一眼 `replicatingLogs_`”规避，因为检查与 Host 入队之间不是同一个同步边界。

`Host` 在同一 term 内必须按如下规则合并：

```text
mergedLogId     = max(inFlight.logId, pending.logId, incoming.logId)
mergedCommitId  = max(inFlight.commitId, pending.commitId, incoming.commitId)
mergedCommitId <= mergedLogId
```

并满足：

- 较小的同-term目标绝不能覆盖较大的目标；
- 共享 `cachingPromise_` 只能在合并后的最大目标已经完成时兑现；
- 低于当前发送 term 的陈旧请求直接失败，不能混入新 term；
- 高 term 请求不得悄悄与旧 term 合并，应由 RaftPart 的换届/reset流程处理；
- 响应必须与其实际完成的目标关联，测试不能只检查 RPC 返回 `SUCCEEDED`。

RaftPart 的多数派计数还应增加独立安全带：正常复制响应除了 `error_code == SUCCEEDED`，还必须满足：

```cpp
resp.last_matched_log_id >= requestedLastLogId
```

对应现有计数位置：

- [`RaftPart.cpp:951-972`](../src/kvstore/raftex/RaftPart.cpp#L951-L972)
- [`RaftPart.cpp:1002-1019`](../src/kvstore/raftex/RaftPart.cpp#L1002-L1019)

这样即使以后 Host 的 Promise/target 逻辑再次出现缺陷，也不能只凭一个与较小目标关联的 `SUCCEEDED` 形成新日志的 quorum ACK。

实现位置主要是：

- pending 写入：[`Host.cpp:100-118`](../src/kvstore/raftex/Host.cpp#L100-L118)
- 当前目标更新和 Future 兑现：[`Host.cpp:176-230`](../src/kvstore/raftex/Host.cpp#L176-L230)
- pending 取出：[`Host.cpp:498-529`](../src/kvstore/raftex/Host.cpp#L498-L529)
- 状态字段：[`Host.h:250-285`](../src/kvstore/raftex/Host.h#L250-L285)

这里应做确定性单元测试，而不是只靠集成压测碰竞态。

### 3.3 修复点 C：安全地限制稳定 term 的 no-op

目标位置：[`RaftPart::sendHeartbeat()`，RaftPart.cpp:2041-2122](../src/kvstore/raftex/RaftPart.cpp#L2041-L2122)。

行为应改为：

```text
raftLock_ 下确认当前仍是 Leader并读取当前状态
    |
    +-- commitInThisTerm_ == false 且复制流水线空闲
    |      -> 允许调度必要 no-op
    |
    +-- commitInThisTerm_ == true 且复制流水线空闲
    |      -> 不创建日志，只发送 AppendLog commit/catch-up 通知
    |
    +-- 无论哪种情况
           -> 继续发送现有真正 Heartbeat RPC
```

异步 no-op 任务需要：

- 捕获 `shared_from_this()`，不捕获裸 `this`；
- executor 真正执行前再次在 `raftLock_` 下检查当前仍是 Leader且 `!commitInThisTerm_`；
- 释放 `raftLock_` 后才调用 `appendLogAsync()`，避免该函数再次取同一把锁造成自死锁；
- 再检查 atomic `replicatingLogs_`，减少重复预约。

`appendLogAsync()` 没有 expected-term 参数，它只按真正执行时的当前 role/term 处理。因此最小实现不应承诺“任务严格属于预约时的 term”或“每 term 恰好一条”。在锁释放到 append admission 之间发生换届或业务提交时，可能多出极少量合法 no-op；但任务每次都重新读取受锁保护的当前状态，稳定 term 在 `commitInThisTerm_ == true` 后不会继续周期生成。

若产品要求严格的 per-term exactly-once scheduling，则必须新增 term-tagged reservation，并把 expected-term 校验放进 append admission 的同一同步边界；这会扩大改动面，而且 Raft 正确性并不要求 no-op 恰好一条。本方案选择“稳定后停止、竞态边界允许有界额外”的较小实现。

Raft 要求的是新 Leader提交一条**当前 term 的 entry**，不要求它一定为空。若业务 entry 先提交，`commitInThisTerm_` 变为 `true`，无需再强制写 no-op。

### 3.4 修复点 D：用现有 AppendLog 传播 commit 并驱动追赶

当兼容开关已启用、`commitInThisTerm_ == true` 且没有正常日志正在复制时，对每个 peer 调用现有：

```cpp
host->appendLogs(eb,
                 currTerm,
                 lastId,
                 committed,
                 lastTerm,
                 lastId);
```

这里的含义是“把 Leader 当前日志尾部和 commit 水位同步给 peer”，不是新增日志。

`Host::prepareAppendLogRequest()` 已支持两类请求：

- peer 已追平：`lastLogIdSent_ == logIdToSend_`，生成 `log_str_list` 为空的 AppendLog；
- peer 落后：从 Leader 现有 WAL 读取缺失区间并补发；若 WAL 已不保留，则沿既有 snapshot 路径处理。

源码位置：[`Host.cpp:287-345`](../src/kvstore/raftex/Host.cpp#L287-L345)。

必须为**每一个**返回的 Future 安装完成处理，不能像多数派提交那样在达到 quorum 后忽略迟到响应：

- 每个响应一到达，就检查 `resp.current_term`；若高于本地当前 term，立即按既有规则退位；
- 写本地 Leader 状态前重新持有 `raftLock_`，并校验回调所对应的 term/role；
- RPC error 只影响该 peer 的通知/追赶，不能把通知当作一批新日志的多数派 ACK；
- 通知路径绝不能再次调用 Leader `commitLogs()`；Leader commit 已在正常复制路径完成；
- 所有异常都必须被消费，不能留下未观察的 Future。

尤其要测试“先收到足够成功响应，随后慢 peer 返回更高 term”；Leader 仍必须因后者退位。

真正 Heartbeat RPC 必须保留。它负责选举计时、Host pause/unpause 和 Leader lease；零 entry AppendLog 负责日志一致性、commit 水位和 catch-up。当前旧实现的健康空载轮次本来就同时发送“一条 entry 的 AppendLog + Heartbeat”，所以新行为不会增加稳态 RPC 次数，AppendLog payload 反而更小。

### 3.5 修复点 E：默认关闭的兼容开关

新增 Leader 行为应放在默认 `false` 的 gflag 后；follower happy-path和 Host 单调合并不依赖开关，先作为兼容层部署。

```text
flag=false：完全保留旧 Leader 的周期 dummy log 行为
flag=true ：启用 term guard + 零 entry commit/catch-up通知
```

该开关不建议在首版加入动态配置白名单。三台配置一致，并通过逐台重启切换，避免某些调用点读取新值、另一些对象仍保留旧初始化状态。

## 4. 修复后的状态序列

```text
新 term 当选
  -> commitInThisTerm_ = false
  -> 追加必要的当前-term entry（业务 entry 也可替代；竞态边界允许有界额外 no-op）
  -> 多数副本接受
  -> Leader commit
  -> commitInThisTerm_ = true
  -> 后续 polling：
       1. 继续真正 Heartbeat RPC
       2. 零 entry AppendLog 携带最新 committedLogId
       3. 已追平 follower 不写 WAL，只推进 commit/apply
       4. 落后 follower 由现有 WAL/snapshot 路径追赶
  -> 稳定 term 不再创建周期空 NORMAL 记录
```

预期直接结果：

- Leader/Follower 的 heartbeat 空日志不再周期写 FileBasedWal；
- `AtomicLogBuffer::push()` 不再由 idle heartbeat 驱动；
- `nodes/node_bytes/accounted_bytes/empty_pushes` 在稳定 term 进入平台；
- 空日志不再周期进入 `Part::commitLogs()`，RocksDB `systemCommitKey` entry 斜率消失；
- 磁盘 Raft WAL 不再以空日志频率增长；
- 新 term 仍保留 Raft 所需的当前-term提交语义；
- follower 最终提交与空闲重连追赶不依赖新的业务写入。

修复不会在运行中的旧进程里主动释放已经分配的 Atomic Node。部署新二进制的 storaged 重启会释放旧进程内存；真正判断修复是否生效，应看重启后的新增斜率，而不是要求 RSS 在原进程中立即下降。

## 5. 为什么这是改动面最小的完整方案

| 方案 | 是否切断根因 | 语义风险 | 结论 |
|---|---:|---:|---|
| 只调小 `wal_buffer_size` | 否，只提前进入 GC | 低，但会增加磁盘回读 | 运维止损，不是根治 |
| 修改 `Record::size()`/Atomic GC | 只限制主路径上界 | 仍持续写 WAL/RocksDB；改变缓存淘汰 | 不优先 |
| 空 payload 不进入 Atomic buffer | 只切 Atomic 主路径 | 增加磁盘回读；RocksDB 次路径仍在 | 不完整 |
| 只加 `commitInThisTerm_` guard | 切断增长 | data race、follower commit/catch-up退化 | 不可直接发布 |
| Heartbeat handler 直接 commit | 可切断增长 | 缺少日志匹配验证，改动协议语义 | 风险更高 |
| **兼容开关 + term guard + Host单调队列 + 零 entry AppendLog + happy-path补齐** | **是** | **复用既有一致性路径并封闭并发竞态** | **推荐** |

推荐方案不增加新的持久化状态，不改变磁盘/网络格式，不迁移数据。数据格式支持回滚，但行为回滚必须先关闭新 Leader 开关，再考虑回滚二进制，不能让启用新行为的 Leader 长时间面对未兼容的旧 followers。

## 6. 发布前必须通过的测试

### 6.1 Raft 正确性

1. **新 Leader 空载**：当前 term 至少提交一条 entry，`commitInThisTerm_` 变为 true，Leader Ready/lease 正常。
2. **稳定 term**：等待 5～10 个 polling 周期，三副本 `wal()->lastLogId()` 不再因 idle heartbeat 增长，term/Leader 不变。
3. **最后一批业务日志**：停止写入后，所有健康 followers 的 committed ID 和状态机最终追平 Leader。
4. **follower 离线重连**：离线期间 Leader 提交业务，follower 重启后不再制造新业务写；仍能自动追 WAL、commit 和状态机。
5. **上一 term 未提交日志**：按 Raft Figure 8 场景换届，新 Leader 的当前-term entry 能连带提交旧 term 日志。
6. **并发与换届**：heartbeat/no-op预约期间发生业务提交、step-down、re-election，不死锁、不重复 apply，额外 no-op 只允许有界出现。
7. **Host 单调目标竞态**：确定性构造“零 entry N 在途、正常 N+1 pending、陈旧 N 再到达”，N+1 Future 在该 follower 真正匹配 N+1 前绝不能成功。
8. **quorum 目标校验**：构造 `SUCCEEDED + last_matched_log_id < requestedLastLogId`，该响应绝不能计入多数派。
9. **迟到高 term**：先返回多个成功，再由慢 peer 返回更高 term，Leader 必须退位。
10. **TSAN**：`commitInThisTerm_` 没有未加锁访问。
11. **现有 raftex/WAL 测试**：全部通过。

### 6.2 当前 3+3+3 实验拓扑

建议沿用已建立的：

```text
3 metad + 3 storaged + 3 graphd
50 spaces × 20 parts × RF3
1000 local replica-parts / storaged
H=1（只用于加速缺陷验证）
无业务 DML
```

Leader Ready 并排空已预约任务后，连续采样 10～30 分钟。严格门槛：

```text
term/Leader 稳定
Δempty_pushes = 0
Δpushes = 0                 # 无其他 Raft 日志的控制实验
Δnodes = 0
Δnode_bytes = 0
Δaccounted_bytes = 0
RocksDB active-entry 不再按旧 empty-push 速率增长
Follower committed ID 最终追平
```

RSS 只作为次级指标：重启后稳态斜率应显著低于未修复对照，但 jemalloc、RocksDB、线程栈和后台任务会产生波动，不能用 RSS 单独判定。

### 6.3 两阶段部署、激活与回滚

旧 follower 对尾部匹配的零 entry 会返回默认的旧 committed 匹配点。patched Leader 随后可能重复补发已有日志；如果对应 Leader WAL 已清理，还可能转入 snapshot。大量 Part 同时发生时可能形成重传或 snapshot storm。因此不能在第一个 patched 节点上直接启用新 Leader 行为。

生产发布分两阶段：

**阶段一：只部署兼容能力，开关保持 false**

1. 构建中包含 follower 零 entry happy-path、Host 单调合并和受开关保护的新 Leader 代码。
2. 三台 storaged 的新开关全部保持默认 `false`，Leader 行为与旧版本相同。
3. RF3 一次只滚动一台；每次等待三台 ONLINE、所有 Part 恢复副本数、每个 Part 恰有一个 Leader。
4. 完成三台后二次核验有效二进制/flag，确认不存在旧 follower。这里必须覆盖所有 AppendLog 目标，包括 voter、learner 和 listener；不能只检查组成 quorum 的三台 voter。

**阶段二：激活新 Leader 行为**

1. 三台配置都写入 `flag=true`，再一次只滚动一台。
2. 混合 flag 期间，仍为 false 的 Leader 会继续产生 dummy log；这只影响修复完成速度，不改变协议兼容性，因为三台二进制均已有 follower/Host兼容代码。
3. 单节点 canary 若没有承载 Leader，或者其他旧行为 Leader仍给它复制 dummy log，不能用该节点 RSS/empty-push斜率单独证明修复完成。
4. 全部激活后再做第 6.2 节的集群级斜率和 commit/catch-up验收。

**回滚顺序**

1. 先在兼容新二进制上把三台 Leader 开关逐台恢复为 `false`，每次恢复 3/3 健康后再继续；这会恢复旧 dummy log 行为，但不改变数据格式。
2. 只有在新行为已全部关闭后，才按相反顺序一次一台回滚旧二进制。
3. 若启动失败，恢复该节点原二进制和配置后启动；若已 ONLINE 但性能/正确性退化，冻结后续动作，待 3/3 健康后再回滚该节点。
4. 全程保留 data/WAL，禁止通过删除目录回滚。

还必须覆盖 `new Leader + old follower + Leader目标WAL已清理` 的实验，证明未按两阶段流程操作时的失败边界，而不是假设混版等价。

若运行环境无法可靠证明所有 voter/learner/listener 已升级，最小运维约束就不够，应在 `HeartbeatResponse` 增加向后兼容的 optional capability，并只对当前 Leader epoch 内明确声明支持的 peers 启用零 entry；旧节点缺少字段时按不支持处理。这会扩大到 Thrift schema 和能力状态管理，所以本文把它列为“非受控混版环境的强制增强”，而不是当前三 storage 可控集群的最小改动。

不能承诺绝对零风险：RF3 滚动时只剩 2/3，任何第二节点、链路或磁盘故障都可能失去 quorum；停止 Leader 也会产生一次选举和短时可重试错误。

## 7. 外部设计依据及边界

- [Nebula PR #606](https://github.com/vesoft-inc/nebula/pull/606) 解释了历史上使用当前-term空日志提交旧 term 遗留日志的目的。
- [nebula-storage PR #438](https://github.com/vesoft-inc/nebula-storage/pull/438) 在 2021 heartbeat 重构中明确记录了空闲时同时发送 dummy log 和独立 Heartbeat 的现有设计，因此重复 no-op 不能简单视为最近的偶然回归。
- [Nebula Issue #6156](https://github.com/vesoft-inc/nebula/issues/6156) 提议增加 `commitInThisTerm_` 判断，但截至 2026-08-16 仍为 Open、无维护者评论和关联 PR；它提供了方向，不是已经评审的完整修复。
- [Raft 扩展论文 §5.4.2](https://web.stanford.edu/~ouster/cgi-bin/papers/raft-extended.pdf) 说明：Leader 不能仅按副本数直接提交旧 term entry；一旦当前 term entry 被提交，之前的 entry 可由 Log Matching 性质间接提交。

因此最终设计必须同时满足两点：

1. 每个新 term 仍能产生并提交一条当前-term entry；
2. 之后不再通过不断创建新 entry 来维持 heartbeat、commit 传播和 follower catch-up。

## 8. 最终建议

按“最小化源码修改且不牺牲现有 Raft 行为”的标准，建议实施：

```text
commitInThisTerm_ 的加锁 term guard
    +
Host 同一 term 发送目标的单调合并
    +
quorum ACK 的目标 LogID 校验
    +
零 entry AppendLog 的 commit/catch-up 驱动
    +
follower 零 entry happy-path 匹配水位修正
    +
默认关闭、分阶段启用的兼容开关
```

不要仅提交一行 guard，也不要优先修改 AtomicLogBuffer GC 来掩盖上游持续制造空日志的事实。

在完成第 6 节测试之前，该设计只能称为候选修复；通过后才具备生产 canary 条件。
