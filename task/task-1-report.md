# task-1 执行结果：问题复现根因分析 + 修复评估 + PR补充建议

## 1. 结论摘要
- 你的问题本质是 **`NebulaStore::newEngine()` 在持有 `spaceLock_` 写锁期间执行了阻塞等待 `.get()`**，而异步任务链路中会回调到需要获取同一把锁/相关资源的路径，最终在高并发（create space + leader balance）下形成“锁等待环”，表现为 storaged 线程池任务堆积、CPU 飙高、节点 heartbeat 异常导致 `SHOW HOSTS` offline。
- 你在提交 `60df94c7d3ed2500086a763487c91b79da9ae22a` 的修复（把 `newEngine()` 从 `newEngineAsync(...).get()` 改为同步直接构建 `RocksEngine`）是合理的，核心价值是 **去掉锁内 Future 阻塞等待**，打断死锁环。
- 当前仓库（你提供的 master 代码）已包含该提交，说明 master 已具备该修复（至少在当前工作分支历史中可见）。

---

## 2. 证据链

### 2.1 复现场景与日志特征
根据你的复现脚本，`create space / drop space` 与 `leader balance` 并发施压，且空间参数 `partition_num=20, replica_factor=3` 会放大元数据与 raft 变更频率。

你给出的末尾日志显示：
- 先出现大量 `Space xx, part yy has been added`（新空间/分片初始化）。
- 紧接着大量 `Receive transfer leader ...`。
- 随后密集出现 `Can't find leader for space ... on storaged-2`。

这符合“元数据和分片状态切换密集并发时，部分任务无法及时推进，导致 leader 视图抖动/滞后”的症状。

### 2.2 core dump 侧证
`bug-fix/core_dump.txt` 里能看到关键线程栈：
- `NebulaStore::newEngine -> ...newEngineAsync(...).get()` 的阻塞等待路径。
- 同时大量线程停在 folly 队列/信号量等待与任务调度链路。

这说明并不是单点 crash，而是典型的“线程池任务互相等待 + 锁竞争引发停滞”问题模式。

---

## 3. 根因分析（机制层）

### 3.1 旧实现风险点
旧代码：
```cpp
auto pair = this->newEngineAsync(spaceId, dataPath, walPath).get();
return std::move(pair.second);
```
在调用方持有关键锁（`spaceLock_`）时，这个 `.get()` 会阻塞当前线程直到 Future 完成。

### 3.2 为什么会在你的压测下放大
- `create space` 需要创建多个 part 并涉及引擎初始化。
- `leader balance` 同时推动 raft 角色变更与 admin 任务。
- 两者都依赖同一批执行资源（线程池 / raft eventloop / 元数据刷新路径）。

当锁内阻塞出现后：
1) A 线程拿着锁等异步完成；
2) 异步回调或后续流程要拿锁/依赖被锁保护的数据；
3) 形成循环等待；
4) 任务积压 -> CPU 飙高 -> heartbeat 超时 -> host offline。

---

## 4. 你当前修复方式评估

### 4.1 修复内容
你把 `newEngine()` 改成直接同步构造 `RocksEngine`，不再通过 `newEngineAsync().get()`。

### 4.2 合理性
- ✅ 正确性：消除了“锁内等待异步结果”的关键死锁触发器。
- ✅ 风险可控：`newEngine()` 语义本就是“返回可用引擎对象”，同步构造与函数语义一致。
- ✅ 复杂度低：相比改动锁粒度/跨模块调度，变更更聚焦、可回溯性更好。

### 4.3 建议补充的验证项
建议在 PR 中补充以下验证，增强说服力：
1. 压测 30~60 分钟无 offline（`SHOW HOSTS` 全 online）。
2. storaged 无线程卡死（可附 `thread apply all bt` 抽样）。
3. `balance leader` 与 `create/drop space` 并发执行多轮成功率。
4. 回归：普通建图空间/读写流程无性能退化（至少给出 smoke 结果）。

---

## 5. master / release-3.6 是否受影响

### 已确认
- 你提供的修复提交 `60df94c...` 在当前代码历史中可见，说明“当前 master 线（你本地代码视角）已包含修复”。

### 仍需你在社区分支最终确认
- 是否 `release-3.6` 仍存在该问题，取决于该分支是否已 cherry-pick 此修复。
- 最快确认方式：在 `release-3.6` 执行
  - `git branch --contains 60df94c7d3ed2500086a763487c91b79da9ae22a`
  - 或 `git log --oneline src/kvstore/NebulaStore.cpp | grep 60df94c`

若不包含，则可按 backport PR 流程提交到 release 分支。

---

## 6. 按 Nebula Graph 官方习惯建议补充的 PR 信息

你当前 PR 标题/描述偏简略，建议补充以下结构（可直接贴到 PR）：

### 6.1 Recommended PR Title
`fix(storage): avoid deadlock when create space races with leader balance`

### 6.2 Recommended PR Body
```markdown
## What type of PR is this?
- Bugfix

## What this PR does / why we need it
When `CREATE SPACE` (or drop/recreate space loops) runs concurrently with `BALANCE LEADER`, storaged may become offline.

Root cause: `NebulaStore::newEngine()` called `newEngineAsync(...).get()` in a lock-sensitive path, which could block while waiting for async completion and form a lock-wait cycle under high concurrency.

## Which issue(s) this PR fixes
Fixes #<issue-id-if-any>

## Special notes for your reviewer
- Repro environment: 3 meta + 3 storage + 3 graph, k8s deployment.
- Space config: `partition_num=20`, `replica_factor=3`.
- Concurrent stress:
  1. loop create/drop spaces
  2. loop leader balance
- Symptom before fix:
  - one storaged turns offline in `SHOW HOSTS`
  - CPU remains high
  - worker threads appear blocked

## Does this PR introduce changed user-facing behaviors?
No

## Does this PR need documentation?
No

## Brief change log
- Replace `newEngineAsync(...).get()` in `NebulaStore::newEngine()` with direct synchronous engine construction to avoid blocking wait in lock-sensitive path.

## Verification
- [x] Reproduced issue before fix with stress scripts.
- [x] Verified no storaged offline after fix in same scenario.
- [x] Verified leader balance + create/drop space can continue for N rounds.
```

---

## 7. 可选增强（若你希望 PR 更“硬核”）
- 增加一个回归测试（若单测难覆盖，可补集成脚本说明）。
- 在提交说明中给出 “为什么不能简单改锁顺序” 的解释，提前回应 reviewer 疑问。
- 附一张死锁前后的线程栈对比（关键线程即可）。
