# NebulaGraph space1/part19 问题复现失败原因与可行复现方案（2026-02-25）

## 1. 先给结论
你按之前文档 3.2 注入故障未复现，**很可能是合理结果**。结合当前源码，至少有 5 个高概率原因：

1. 你当前代码已经包含修复，原始 bug 路径被切断。
2. 你的故障注入未满足“必须走 snapshot”的触发条件。
3. 你可能按“分片目录”删数据，但 Nebula 的数据按 space 级 RocksDB 存储，分片不是独立目录。
4. 3 副本下只要有 1 个 follower 能成功复制，leader 就可能很快 commit 本任期日志，lease 变 valid，死循环不成立。
5. 选主/心跳空日志存在竞态，未卡在 `commitInThisTerm_ = false` 窗口。

---

## 2. 源码证据：为什么你可能复现不出来

## 2.1 当前代码已经修复了关键点
旧 bug 的核心是 snapshot 扫描调用 `prefix(..., canReadFromFollower=false)`，会受 lease 约束。

当前源码已是：
- `src/kvstore/NebulaSnapshotManager.cpp:114`
- `store_->prefix(spaceId, partId, prefix, &iter, true, snapshot);`

这会绕过 `checkLeader(part, false)` 里的 lease 校验，旧版 `access prefix failed, error code:-4` 闭环不再成立。

同时 snapshot 发送又加了领导权复检：
- `src/kvstore/raftex/SnapshotManager.cpp:56-61`

因此，在当前源码上你即便制造“需要 snapshot”的场景，也更可能恢复成功而不是卡死。

## 2.2 必须走 snapshot 才能进入你想复现的路径
snapshot 触发条件在：
- `src/kvstore/raftex/Host.cpp:306-309`
- 条件：`lastLogIdSent_ + 1 < part_->wal()->firstLogId()`

也就是说：
- follower 落后点必须小于 leader 当前 WAL 最小 logId。
- 如果 leader WAL 还保留足够历史日志，系统会走 appendLog 补日志，不会走 snapshot。

## 2.3 默认参数下，leader WAL 不容易快速“抬高 firstLogId”
- `wal_ttl` 默认 14400 秒（4 小时）：`src/kvstore/wal/FileBasedWal.cpp:15`
- `clean_wal_interval_secs` 默认 600 秒：`src/kvstore/NebulaStore.cpp:24`

如果没有主动调小参数并等待清理，leader 的 `firstLogId` 往往不会很快变大。

## 2.4 你删“part19目录”可能并未删到关键数据
Nebula 存储布局：
- RocksDB 数据目录按 space 维度：`.../nebula/<spaceId>/data`
- WAL 根目录按 space 维度，再按 part 分子目录：`.../nebula/<spaceId>/wal/<partId>`
  - 见 `src/kvstore/NebulaStore.cpp:489`
  - `walPath = "%s/wal/%d"`

因此：
- “只删 part19 目录”通常只能影响 WAL 子目录；
- 提交位点 `systemCommitKey(partId)` 在 space 级 RocksDB 里，不在该 WAL 子目录。

## 2.5 3 副本下 quorum 行为会迅速解除 lease 失效
- 当选 leader 后：`commitInThisTerm_ = false`（`src/kvstore/raftex/RaftPart.cpp:1388`）
- 但 leader 会立即 `sendHeartbeat()`，其中会 append 空日志推动本任期提交（`src/kvstore/raftex/RaftPart.cpp:2041-2048`）
- 一旦本任期提交成功：`commitInThisTerm_ = true`（`src/kvstore/raftex/RaftPart.cpp:1091-1092`）
- `leaseValid()` 就不会再被 `commitInThisTerm_` 卡住（`src/kvstore/raftex/RaftPart.cpp:2259`）

所以如果 2 个 follower 中任意 1 个还能同步成功，复现窗口会很快关闭。

---

## 3. 复现前置检查（先做）

## 3.1 检查你当前版本是否包含修复
在源码树执行：

```bash
rg -n "prefix\(spaceId, partId, prefix, &iter, true, snapshot\)" src/kvstore/NebulaSnapshotManager.cpp
```

- 如果命中：你当前是“修复后版本”，旧 bug 不应稳定复现。
- 若要复现旧 bug，需在“修复前版本”复现（或临时回退该行）。

## 3.2 检查是否真的触发 snapshot
看 leader 日志必须出现：
- `Can't find log ... in wal, send the snapshot`（`Host.cpp:351`）

若没有该日志，说明你根本没进入 snapshot 路径。

## 3.3 检查 leader WAL 是否被清理到足够高
查看 leader 的目标 part WAL 文件名（文件名前缀是起始 logId）：

```bash
ls -1 /path/to/nebula/<spaceId>/wal/<partId>/*.wal | sort | head
```

若最小文件名仍很小（例如接近 1），通常不会触发 snapshot。

---

## 4. 可行复现方案（从最稳到最接近生产）

## 4.1 方案 A（最稳）：在修复前代码上复现
适用：你要复现“原始死循环 bug”。

步骤：
1. 切到修复前代码（或临时把 `NebulaSnapshotManager.cpp:114` 改回 `false`）。
2. 使用 3 副本独立测试 space（建议 `partition_num=1`，避免误删影响其他分片）。
3. 调小参数并重启 storaged：
- `--wal_ttl=1`
- `--clean_wal_interval_secs=1`
- 可选：`--raft_heartbeat_interval_secs=1`
4. 对该 space 连续写入，等待 > 1~2 分钟，让 leader 旧 WAL 被清掉。
5. 同时下线两个 follower，删除该测试 space 的数据目录与该 part WAL（注意是 space 级 data）：
- `.../nebula/<spaceId>/data`
- `.../nebula/<spaceId>/wal/<partId>`
6. 启动两个 follower，再触发一次 leader 选举（重启 leader 或短暂隔离后恢复）。
7. 立即对该 part 发写请求，观察日志。

预期（修复前）：
- 出现 `Can't find log ... send the snapshot`
- 随后 `access prefix failed, error code:-4`
- 再出现重复重试（`Need to try again`）

## 4.2 方案 B（无回退代码，强制注入）：最可控
适用：你需要“快速、稳定、可重复”验证故障分析链条。

在修复前代码上加一个临时注入开关（仅测试环境）：
- 在 `NebulaSnapshotManager::accessTable` 中，当目标 space/part 命中时直接模拟返回 `E_LEADER_CHANGED`。

优点：
- 不依赖 WAL 清理时机、选主竞态；
- 一次即可命中故障链。

## 4.3 方案 C（最接近生产，不改代码）
适用：你希望尽量贴近线上形态复现。

关键点必须同时满足：
1. leader 进入新任期且 `commitInThisTerm_ = false`；
2. 绝大多数 follower（3 副本里是 2 个）都需要 snapshot；
3. leader 端 `firstLogId` 已抬高到 follower 落后点之上。

你之前没复现通常是缺了第 2 或第 3 条。

---

## 5. 为什么“按 3.2 多次尝试”仍失败（高概率路径）

1. 你跑的是当前源码（已修复）。
2. 只清了 follower 的 WAL，但没让 leader 丢掉旧日志（firstLogId 不够高）。
3. 只让 1 个 follower 失配，另一个 follower 仍可复制，leader 很快提交空日志，lease 恢复。
4. 删除了错误目录（按 part 删 data），实际上 key/value 还在 space 级 RocksDB。
5. 注入后等待太久才发请求，已经错过 `commitInThisTerm_ == false` 窗口。

---

## 6. 推荐你下一步怎么做（实操顺序）

1. 先确认版本：是否已修复（第 3.1 节）。
2. 若已修复但你要复现“旧 bug”：用方案 A（回退到修复前）或方案 B（强制注入）。
3. 若只想验证“当前版本不会卡死”：
- 用方案 A 的同等故障注入，预期应看到 snapshot 最终成功而非死循环。

---

## 7. 附：和复现直接相关的源码坐标

- snapshot 触发条件：`src/kvstore/raftex/Host.cpp:306-309`
- snapshot 发送日志：`src/kvstore/raftex/Host.cpp:351-354`
- 当选后标志重置：`src/kvstore/raftex/RaftPart.cpp:1388`
- 心跳空日志：`src/kvstore/raftex/RaftPart.cpp:2041-2048`
- lease 依赖 `commitInThisTerm_`：`src/kvstore/raftex/RaftPart.cpp:2259`
- 读路径 lease 校验：`src/kvstore/NebulaStore.cpp:1289-1290`
- part WAL 路径构造：`src/kvstore/NebulaStore.cpp:489`
- data/wal 根路径逻辑：`src/kvstore/RocksEngine.cpp:44-47`
- 当前修复点（绕过 lease）：`src/kvstore/NebulaSnapshotManager.cpp:114`
- snapshot 发送期 leader 二次校验：`src/kvstore/raftex/SnapshotManager.cpp:56-61`

