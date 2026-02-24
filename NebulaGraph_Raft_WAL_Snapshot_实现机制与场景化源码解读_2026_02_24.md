# NebulaGraph Raft / WAL / Snapshot 实现机制与场景化源码解读（2026-02-24）

## 0. 阅读说明
本文基于当前工程源码（`nebula-release-3.6`）梳理 storaged 侧一致性链路，重点解释：
- Raft 在 Nebula 中如何实现“选主-复制-提交-读租约”；
- WAL 如何持久化、恢复、截断、清理；
- Snapshot 如何触发、发送、落盘、恢复；
- 在真实生产场景下，函数调用如何串联、为什么这么设计。

核心源码入口：
- Raft 主流程：`src/kvstore/raftex/RaftPart.cpp`
- Leader->Follower 复制通道：`src/kvstore/raftex/Host.cpp`
- RPC 分发：`src/kvstore/raftex/RaftexService.cpp`
- Snapshot 发送：`src/kvstore/raftex/SnapshotManager.cpp`
- Snapshot 数据遍历：`src/kvstore/NebulaSnapshotManager.cpp`
- WAL：`src/kvstore/wal/FileBasedWal.cpp`
- 状态机提交：`src/kvstore/Part.cpp`
- 读请求 leader/lease 校验：`src/kvstore/NebulaStore.cpp`

---

## 1. 整体数据流

### 1.1 写路径总览（从业务请求到提交）
1. 业务写（如 `put/multiPut/remove`）进入 `Part::async*`，被编码为 raft log（`src/kvstore/Part.cpp:69-92`）。
2. 调用 `RaftPart::appendLogAsync` 入复制队列（`src/kvstore/raftex/RaftPart.cpp:786`）。
3. Leader 本地先写 WAL（`appendLogsInternal -> wal_->appendLogs`，`src/kvstore/raftex/RaftPart.cpp:891-903`）。
4. 并行向 followers 发 `appendLog` RPC（`replicateLogs`，`src/kvstore/raftex/RaftPart.cpp:918-999`）。
5. 达到多数派后提交状态机（`commitLogs`，`src/kvstore/raftex/RaftPart.cpp:1078`）。
6. `Part::commitLogs` 把日志翻译成 RocksDB batch，并写入 `systemCommitKey`（`src/kvstore/Part.cpp:220-410`）。

关键原则：
- 先 WAL，后复制，再状态机提交。
- 一致性边界是 “多数派接受 + 提交点推进”。

### 1.2 读路径总览（租约读）
`NebulaStore::checkLeader` 要求：
- `isLeader()==true` 且 `leaseValid()==true`（`src/kvstore/NebulaStore.cpp:1289-1290`）。
- `leaseValid` 依赖本任期是否有提交（`commitInThisTerm_`，`src/kvstore/raftex/RaftPart.cpp:2259`）与最近心跳/复制确认时间窗口。

---

## 2. Raft 机制（函数级）

## 2.1 选主与任期

触发入口：`statusPolling`（`src/kvstore/raftex/RaftPart.cpp:1401`）
- 周期检查是否超时未收到心跳（`needToStartElection`，`src/kvstore/raftex/RaftPart.cpp:1143-1155`）。
- 走预投票 + 正式投票（`leaderElection(true/false)`，`src/kvstore/raftex/RaftPart.cpp:1412-1414`）。

投票收敛：
- `handleElectionResponses` 成功后切为 leader（`src/kvstore/raftex/RaftPart.cpp:1370-1399`）。
- 当选后 `commitInThisTerm_ = false`（`src/kvstore/raftex/RaftPart.cpp:1388`），并立即 `sendHeartbeat()`。

设计点：
- 代码里专门处理了 WAITING_SNAPSHOT 期间误入 candidate 的死循环风险，失败时强制回 follower（`src/kvstore/raftex/RaftPart.cpp:1306-1318`）。

## 2.2 日志复制与冲突处理

Leader 侧：
- `appendLogAsync` 缓冲并批量发送（`src/kvstore/raftex/RaftPart.cpp:786-872`）。
- `appendLogsInternal` 第一步写 WAL，第二步复制（`src/kvstore/raftex/RaftPart.cpp:874-915`）。

Follower 侧接收：`processAppendLogRequest`（`src/kvstore/raftex/RaftPart.cpp:1610`）
- 校验状态和 leader 合法性（`verifyLeader`，`src/kvstore/raftex/RaftPart.cpp:1830`）。
- 处理三类不一致：
1. 前置日志不存在/已被清理 -> `E_RAFT_LOG_GAP`（`src/kvstore/raftex/RaftPart.cpp:1688-1694`）
2. term 不匹配 -> `E_RAFT_LOG_GAP`（`src/kvstore/raftex/RaftPart.cpp:1720-1725`）
3. 找到分叉点 -> `rollbackToLog` 回退（`src/kvstore/raftex/RaftPart.cpp:1750-1753`）
- 之后追加剩余日志并按 leader 提交点推进本地提交（`src/kvstore/raftex/RaftPart.cpp:1765-1822`）。

## 2.3 心跳与 lease

`sendHeartbeat`（`src/kvstore/raftex/RaftPart.cpp:2041`）有两层职责：
1. 空日志心跳：若本任期未提交，发送空日志推动“本任期提交”建立合法 lease（`src/kvstore/raftex/RaftPart.cpp:2042-2048`）。
2. 心跳 RPC：刷新 follower 活性，统计多数派接受时间（`src/kvstore/raftex/RaftPart.cpp:2074-2121`）。

`leaseValid` 核心条件（`src/kvstore/raftex/RaftPart.cpp:2254-2268`）：
- `commitInThisTerm_ == true`
- 最近一次多数派确认在 heartbeat 窗口内（扣除网络耗时）

这解释了“刚当选 leader 但还未本任期提交时，读会失败”的现象。

---

## 3. WAL 机制（FileBasedWal）

## 3.1 WAL 记录格式与滚动
`appendLogInternal` 写入格式（`src/kvstore/wal/FileBasedWal.cpp:457-468`）：
- `LogID` + `TermID` + `len(head)` + `ClusterID` + `payload` + `len(foot)`

双 `len`（head/foot）用于快速校验尾部完整性，崩溃恢复时可截断损坏尾部。

文件滚动策略：
- 超过 `wal_file_size` 滚新文件（`src/kvstore/wal/FileBasedWal.cpp:472-478`）
- 可配置 `wal_sync` 强制 fsync（`src/kvstore/wal/FileBasedWal.cpp:486-488`）

## 3.2 启动恢复与自修复
初始化时 `scanAllWalFiles()`（`src/kvstore/wal/FileBasedWal.cpp:85`）会做：
1. 扫描 `*.wal` 文件并校验命名与首 log id（`src/kvstore/wal/FileBasedWal.cpp:86-148`）
2. 校验最后一条记录结构（`src/kvstore/wal/FileBasedWal.cpp:150-221`）
3. `scanLastWal` 逐条扫最后文件，发现坏尾则 `truncate`（`src/kvstore/wal/FileBasedWal.cpp:371-439`）
4. 检测全局 log gap，删除 gap 前旧段（`src/kvstore/wal/FileBasedWal.cpp:237-259`）

这使异常宕机后 WAL 仍可恢复到“最后完整位置”。

## 3.3 回退与清理
- 分叉冲突回退：`rollbackToLog` 删除后续段并在最后段内截断（`src/kvstore/wal/FileBasedWal.cpp:569-620`）。
- 常规清理：`cleanWAL()` 按 TTL 清理但至少保留 2 个 WAL 段（`src/kvstore/wal/FileBasedWal.cpp:640-674`）。
- 按提交点清理：`cleanWAL(id)` 仅清理 `< id` 的旧段，同样保留底线（`src/kvstore/wal/FileBasedWal.cpp:676-707`）。

工程取舍：
- 保留至少两段可显著降低“轻微落后即触发 snapshot”的概率。

---

## 4. Snapshot 机制（触发、发送、接收）

## 4.1 触发条件（Leader 视角）
在 `Host::prepareAppendLogRequest`：
- 若 `lastLogIdSent_ + 1 < leader.wal.firstLogId()`，说明 follower 需要的日志 leader WAL 已无 -> 触发快照（`src/kvstore/raftex/Host.cpp:306-309`）。
- `startSendSnapshot` 异步发快照并返回 `E_RAFT_WAITING_SNAPSHOT`（`src/kvstore/raftex/Host.cpp:348-379`）。

## 4.2 发送流程
`SnapshotManager::sendSnapshot`（`src/kvstore/raftex/SnapshotManager.cpp:29-105`）：
1. 确认自己是 leader。
2. 调 `accessAllRowsInSnapshot` 分批扫描分区数据。
3. 每批通过 `future_sendSnapshot` 发送给 follower。
4. done 批次带最终 `commitLogId/Term`。

数据扫描在 `NebulaSnapshotManager`：
- 先读取 `systemCommitKey` 作为快照提交位点（`src/kvstore/NebulaSnapshotManager.cpp:53-65`）。
- 再按 key 前缀遍历分区数据并编码发送（`src/kvstore/NebulaSnapshotManager.cpp:77-146`）。

## 4.3 接收与落盘流程（Follower）
`RaftexService::sendSnapshot -> RaftPart::processSendSnapshotRequest`（`src/kvstore/raftex/RaftexService.cpp:150-159`, `src/kvstore/raftex/RaftPart.cpp:1954`）：
1. 首包切状态到 `WAITING_SNAPSHOT`，并 `reset()` 清理旧状态。
2. 每批调用 `commitSnapshot` 持久化（`src/kvstore/raftex/RaftPart.cpp:2008`）。
3. done 包到达后推进 `committedLogId_/lastLogId_`，回到 `RUNNING`（`src/kvstore/raftex/RaftPart.cpp:2024-2033`）。

`Part::commitSnapshot` 会把每批 KV 写入 RocksDB，并在 done 时写 `systemCommitKey`（`src/kvstore/Part.cpp:366-401`）。

---

## 5. 场景化调用链（重点）

## 5.1 场景 A：正常写入（无故障）
示例：`INSERT VERTEX` 命中 `space=1 part=8`
1. `Part::asyncPut` 编码日志。
2. `appendLogAsync` 入队。
3. Leader WAL 追加成功。
4. 两个 follower 至少一个返回 `SUCCEEDED`。
5. Leader `commitLogs`，写状态机 + commitKey。
6. 返回成功给上层。

底层原理：
- Raft 的“多数派复制后提交”保证线性一致写。
- 状态机提交幂等由日志序列化顺序保障。

## 5.2 场景 B：leader 切换后首个心跳阶段
示例：节点重启后新 leader 上任
1. `handleElectionResponses` 当选，`commitInThisTerm_=false`。
2. `sendHeartbeat` 会先 append 空日志。
3. 空日志提交成功后 `commitInThisTerm_` 被置 true（`src/kvstore/raftex/RaftPart.cpp:1091-1094`）。
4. lease 建立，读请求恢复。

底层原理：
- Raft 论文语义：leader 在当前 term 至少提交一条日志后，才能安全宣称自己可对外提供强一致读。

## 5.3 场景 C：follower 轻度落后（WAL 仍覆盖）
1. follower 返回 `E_RAFT_LOG_GAP` + `last_matched_log_id`。
2. leader 从该 log id 后继续补发 appendLog。
3. 无需 snapshot，成本低。

## 5.4 场景 D：follower 严重落后（WAL 不覆盖）
1. leader 检测 `lastLogIdSent+1 < firstLogId`。
2. 触发 snapshot；follower 进入 `WAITING_SNAPSHOT`。
3. 快照完成后再转普通 appendLog 增量同步。

典型触发原因：
- follower 长时间离线；
- leader 已清理历史 WAL（TTL 到期）；
- 异常恢复后副本分叉严重。

## 5.5 场景 E：异常下电恢复
1. storaged 重启，WAL 扫描修复尾部、截断坏段。
2. 若发现 gap，旧段会被删除到最后无 gap 起点。
3. 分区可能变成“多数 follower 需要 snapshot”状态。
4. 后续由 raft append/snapshot 自动收敛。

底层原理：
- WAL 的职责是“可恢复到最后一致边界”，不是“保证永不丢段”。
- 复制收敛最终由 Raft 日志匹配 + Snapshot 补齐完成。

---

## 6. 当前版本的关键工程策略与注意点

1. 读租约依赖 `commitInThisTerm_`，避免旧 term leader 脑裂读。
2. heartbeat 不仅保活，也承担 lease 时间窗统计。
3. snapshot 和 appendLog 并行状态通过 `Host::sendingSnapshot_`、`WAITING_SNAPSHOT` 协调。
4. WAL TTL 清理和 snapshot 频率存在权衡：TTL 过短会增加 snapshot 压力。

---

## 7. 实战排障清单（建议）

1. 看 leader 复制日志：
- `Can't find log ... in wal, send the snapshot` -> follower 需快照。

2. 看快照链路：
- `start send snapshot of commitLogId ...`
- `send snapshot failed` / `E_RAFT_WAITING_SNAPSHOT`

3. 看 lease：
- `leaseValid` 是否因 `commitInThisTerm_==false` 失败。

4. 看 WAL 覆盖范围：
- `firstLogId/lastLogId` 是否覆盖 follower 需要区间。

5. 看提交位点：
- `systemCommitKey` 是否与日志/快照行为一致。

---

## 8. 结合本次修复的机制说明

本次修复后（见源码改动）：
- `src/kvstore/NebulaSnapshotManager.cpp`：快照内部扫描改为 `canReadFromFollower=true`，避免被 leader lease 阻断。
- `src/kvstore/raftex/SnapshotManager.cpp`：每批快照发送前增加 term/role 复检，leader 变化即停止发送。

这使系统在“新 leader 尚未本任期提交 + 多 follower 需快照”的极端恢复场景下，不再出现自锁。

---

## 9. 小结

Nebula 的实现是一个典型“Raft 一致性层 + RocksDB 状态机 + WAL 持久化 + Snapshot 兜底恢复”的工程化方案：
- Raft 负责正确性边界；
- WAL 负责崩溃恢复和增量复制来源；
- Snapshot 负责跨越 WAL 缺口的全量追平；
- Lease 负责强一致读性能优化。

理解这四者的时序关系，比只看单个函数更关键。
