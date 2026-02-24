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

## 3. 快速且可行的复现方案（允许手动注入故障）

下面给出可在测试集群快速复现的方式（3 副本、RF=3）。

### 3.1 复现前准备
- 选择目标分片（例如 `space=1 part=19`），确认当前 leader 主机。
- 确认可控制 3 个 storaged 实例启停。

### 3.2 故障注入步骤（数据面注入，最快）
1. 停止两个 follower 的 storaged。
2. 在这两个 follower 上删除目标分片本地数据目录（至少该分片的数据与 WAL），制造“必须快照恢复”的状态。
3. 启动两个 follower。
4. 触发一次 leader 重选（如重启 leader 或短暂网络隔离后恢复），确保新任期 `commitInThisTerm_ = false`。
5. 对该分片发起写请求（可用落到该 part 的 VID），观察 leader 日志：
   - 出现 `Can't find log ... send the snapshot`
   - 随后 `access prefix failed, error code:-4`
   - `Only X hosts succeeded, Need to try again` 持续循环

### 3.3 预期结果
- 该分片写请求超时/失败；读请求出现 leader lease 相关失败。
- 其他不涉及该分片的请求可正常。

## 4. 解决方案（按落地速度分层）

### 4.1 手动规避措施（生产应急）
1. 先让“至少一个 follower”对该分片恢复为可追平状态（例如从 leader 离线拷贝该分片数据/WAL到一个 follower，再启动）。
2. 一旦 leader 能与一个 follower 成功复制并 commit 一条日志，`commitInThisTerm_` 会置 true，lease 恢复。
3. lease 恢复后，再让另一个 follower 通过快照追平。

说明：RF=3 时，leader 只需 1 个 follower 成功即可过半提交（代码中 remote quorum 为 `(peers+1)/2`，见 `src/kvstore/raftex/RaftPart.cpp:417`）。

### 4.2 新增“异常恢复能力”建议（增强可运维性）
1. 新增快照读取专用接口（例如 `prefixForSnapshot`），语义上明确“本地只读快照扫描，不做 leader lease 校验”。
2. 新增自愈保护：若同一 part 连续出现 `send snapshot failed + E_LEADER_CHANGED`，输出结构化告警并进入退避重试（避免 1ms 自旋）。
3. 新增启动期恢复模式：在“刚当选且未 commit”阶段允许内部恢复任务（仅快照扫描）绕过 lease，但外部读写仍保留 lease 保护。

### 4.3 修改源码彻底修复 bug（推荐）

#### 修复点 A（核心，最小改动）
- 文件：`src/kvstore/NebulaSnapshotManager.cpp`
- 位置：`NebulaSnapshotManager::accessTable`
- 当前：
  - `store_->prefix(spaceId, partId, prefix, &iter, false, snapshot)`
- 建议：
  - 将 `canReadFromFollower` 改为 `true`，使“本地快照数据扫描”不依赖 lease。

原因：
- 这里是 leader 本地存储快照读取，不是客户端一致性读；其正确性由 raft term/角色与 `sendSnapshot` RPC 校验兜底（`src/kvstore/raftex/SnapshotManager.cpp:40-43,63-88`）。

#### 修复点 B（建议一起做）
- 在快照发送循环中每批次重校验 `(term, role)`，若 leader 变化立即中止，进一步降低绕过 lease 的语义风险。

#### 修复点 C（可选）
- `NebulaStore::prefix/range/multiGet` 在“isLeader=true 但 lease 失败”时返回 `E_LEADER_LEASE_FAILED`，提升可观测性（当前很多路径统一返回 `E_LEADER_CHANGED`，易误判）。

## 5. 修复后验证清单

1. 复现用例回归：
- 修复前可稳定复现 `access prefix failed, error code:-4` 循环。
- 修复后快照成功发送，至少一个 follower 追平，leader 成功 commit。

2. 指标与日志：
- `kNumSendSnapshot` 增加后，随后出现 `Send snapshot succeeded`（`src/kvstore/raftex/Host.cpp:365`）。
- `commitInThisTerm_` 从 false 变 true（可加临时日志或断点确认）。

3. 业务验证：
- `space1/part19` 的 CRUD 恢复，跨分片请求无异常。

## 6. 结论

本问题不是单纯“某副本坏了”，而是一个明确的代码级闭环 bug：
- `snapshot 读取路径` 错误依赖了 `leader lease`；
- `leader lease` 又依赖“本任期先 commit 成功”；
- 而“先 commit 成功”在“多数 follower 需快照”场景恰恰依赖 `snapshot`。

因此该问题会在异常下电后以分片级方式长期卡死。建议优先落地 4.3 的 A 点修复，并补齐 B/C 作为工程化增强。
