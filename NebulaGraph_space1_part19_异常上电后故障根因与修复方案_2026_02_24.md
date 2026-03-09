# NebulaGraph 异常下电后 space=1 part=19 故障根因与修复方案（2026-02-24）

## 1. 问题现象与关键证据

### 1.1 现象
- 仅 `space 1` 的 `part 19` 出现增删改查失败，其他图空间和其他分片正常。
- 该分片 leader 存活，但 lease 无效（`leaseValid()==false`），日志中可见 `commitInThisTerm_` 仍为 `false`。

### 1.2 日志证据（与你提供日志一致）
- leader 复制日志到两个 follower 时，发现 WAL 缺口，进入“发快照”路径：
  - `Can't find log ... in wal, send the snapshot, firstLogId in wal = 143167`（`src/kvstore/raftex/Host.cpp:351`）
- 快照发送启动后，读取本地分片前缀数据失败：
  - `access prefix failed, error code:-4`（`src/kvstore/NebulaSnapshotManager.cpp:113`）
  - `-4` 在错误码中是 `E_LEADER_CHANGED`（`src/interface/common.thrift:309`）
- 快照失败后复制持续重试：
  - `Only X hosts succeeded, Need to try again` + `usleep(1000)`（`src/kvstore/raftex/RaftPart.cpp:1130`）

### 1.3 源码侧关键事实
- leader lease 校验：`commitInThisTerm_ == false` 时直接 lease 无效（`src/kvstore/raftex/RaftPart.cpp:2259`）。
- 该标志在当选 leader 时被重置为 `false`（`src/kvstore/raftex/RaftPart.cpp:1388`），只有“本任期成功 commit 日志”后才置 `true`（`src/kvstore/raftex/RaftPart.cpp:1091`）。
- 快照读取调用 `store_->prefix(..., canReadFromFollower=false, snapshot)`（`src/kvstore/NebulaSnapshotManager.cpp:111`），会走 `checkLeader(part,false)`。
- `checkLeader` 依赖 `(isLeader && leaseValid)`（`src/kvstore/NebulaStore.cpp:1290`），lease 无效即返回 `E_LEADER_CHANGED`（`src/kvstore/NebulaStore.cpp:837`）。

## 2. 根因分析（闭环）

### 2.1 直接根因
该故障是一个“自锁闭环”：
1. leader 刚当选（或重启后重新当选）时，`commitInThisTerm_ = false`，lease 默认无效。
2. 该分片两个 follower 都落后到需要快照（日志显示 `lastLogIdSent+1 < leader firstLogId`，触发 `startSendSnapshot`，见 `src/kvstore/raftex/Host.cpp:306,348`）。
3. 发送快照前，leader 需要本地扫描分片数据；但扫描 API 误用了 `checkLeader + lease` 约束（`canReadFromFollower=false`）。
4. 因 lease 无效，扫描直接返回 `E_LEADER_CHANGED(-4)`，快照失败。
5. follower 永远追不上，leader 就永远无法完成本任期首个 commit，`commitInThisTerm_` 永远无法转 true。
6. lease 永久无效，读写持续异常，形成稳定死循环。

### 2.2 为什么只影响 part 19
- 只有 `space1/part19` 同时满足“新任 leader 尚未本任期 commit + 至少多数副本需要快照”的条件。
- 其他分片至少有一个 follower 不需要快照，leader 可先完成一次 commit，lease 建立后系统恢复正常。

### 2.3 与异常下电的关系
异常下电后，分片副本间日志/数据进度容易出现不一致；若某分片在恢复时恰好进入“多数 follower 需快照”的状态，就会触发该 bug。日志中 `firstLogId in wal = 143167` 且需要发送更早日志（143162/143165）正是此类“需要快照追平”的典型特征。



## 3. 结论

本问题不是单纯“某副本坏了”，而是一个明确的代码级闭环 bug：
- `snapshot 读取路径` 错误依赖了 `leader lease`；
- `leader lease` 又依赖“本任期先 commit 成功”；
- 而“先 commit 成功”在“多数 follower 需快照”场景恰恰依赖 `snapshot`。
