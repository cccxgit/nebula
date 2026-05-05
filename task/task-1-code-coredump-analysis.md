# Nebula Graph 死锁问题：代码位置 × coredump × 日志逐条对照分析

> 目标：把“为什么会 offline / 看起来像线程死锁”的根因，用**可定位到源码行号**的方式解释清楚，帮助你后续写 PR/回复 reviewer。

---

## 1. 先给结论（TL;DR）

这次问题的核心不是单点 crash，而是并发场景下的**阻塞等待 + 锁竞争导致的线程推进停滞**：

1. `CREATE SPACE` 路径会触发 `NebulaStore::newPart()`。
2. `newPart()` 在持有 `spaceLock_` 写锁时，调用 `newEngine()`。
3. 旧实现里 `newEngine()` 走 `newEngineAsync(...).get()`，这会在当前线程做阻塞等待。
4. 你的压测同时在做 `BALANCE LEADER`，大量 raft/admin 任务并发，执行资源紧张，形成锁等待环，最终表现为：
   - storaged 线程池/任务队列大量等待；
   - `SHOW HOSTS` 出现某个 storage offline；
   - 该节点 CPU 高位（任务忙等/调度抖动）。

对应修复提交 `60df94c...` 去掉了 `newEngineAsync(...).get()` 这一步“锁内阻塞”，改为直接同步构造 `RocksEngine`，因此能打断这条最关键的等待环。

---

## 2. 代码路径定位（精确到文件行）

## 2.1 `newPart()` 在写锁内调用 `newEngine()`

在 `src/kvstore/NebulaStore.cpp` 中，`newPart()` 对 `spaceLock_` 加写锁后创建 engine：

- 获取写锁：`folly::RWSpinLock::WriteHolder wh(&spaceLock_);`
- 调用 `newEngine(...)`：`auto engine = newEngine(spaceId, partPath, walPath);`

这两步是根因链路的起点：**写锁仍持有时，后续任何慢操作/阻塞都放大竞争风险**。

## 2.2 旧实现：`newEngine()` 内部阻塞 `.get()`

旧代码（提交 `60df94c` 之前）是：

```cpp
auto pair = this->newEngineAsync(spaceId, dataPath, walPath).get();
return std::move(pair.second);
```

这行 `.get()` 的语义是：当前线程阻塞直到异步 future 完成。

在“锁敏感路径 + 高并发 admin/raft 任务”场景下，`.get()` 很容易成为卡点。

## 2.3 修复后：去掉 `.get()`，同步直接构造

提交 `60df94c...` 后，`newEngine()` 变为直接 `std::make_unique<RocksEngine>(...)`，不再等待 `newEngineAsync` future。

关键收益：减少锁内等待外部调度结果的耦合，避免形成循环等待链。

---

## 3. coredump 对照说明（你给的 `bug-fix/core_dump.txt`）

> 注意：你的 coredump 已经给出非常强的“现场证据”。下面按线程栈逐条解释。

## 3.1 关键线程：明确卡在 `newEngineAsync(...).get()`

在 `core_dump.txt` 可以看到栈：

- `folly::futures::detail::waitImpl(...)`
- `folly::SemiFuture::wait()`
- `folly::Future::get()`
- `nebula::kvstore::NebulaStore::newEngine(...) at .../src/kvstore/NebulaStore.cpp:376`

这条栈非常关键，它直接说明：**storaged 线程在 `newEngine()` 里等待 future 完成**。

如果此时调用线程仍处在 `newPart()` 的写锁保护流程内，就会拖住其它需要访问/更新空间结构的并发流程。

## 3.2 其它线程：大量停在队列/信号量等待

你的 dump 里还有很多线程停在：

- `folly::UnboundedBlockingQueue::try_take_for`
- `folly::SaturatingSemaphore::tryWaitSlow`
- `apache::thrift::concurrency::ThreadManager::Impl::waitOnTask`

这不是“完全空闲”的意思，而是系统中存在大量等待调度/等待任务/等待条件满足的线程态，常见于**某个关键路径被卡住后，整个任务图推进变慢**。

## 3.3 raft/admin 线程仍有活动栈

dump 中也能看到 `RaftPart::replicateLogs` / `processAppendLogResponses` 等栈，说明集群并非立即崩溃退出，而是处于**部分线程继续推进、部分关键路径阻塞**的状态，这与“CPU 高但服务退化/offline”的观测一致。

---

## 4. 日志对照说明（你在 task-1.md 里贴的现场日志）

## 4.1 时序特征

你给的日志顺序是：

1. `open rocksdb on .../space 42/data`
2. 连续 `Space 42, part x has been added`
3. 随后大量 `Receive transfer leader ...`
4. 紧接着大量 `Can't find leader for space ... on storaged-2`

这条时序说明：

- 新空间/分片创建与 leader 迁移确实是并发发生的；
- 某节点在并发切换阶段无法及时响应“我是不是该 part 的 leader”相关操作，最终被观测为异常节点。

## 4.2 为什么会看到 `Can't find leader`

并发 balance 时，admin processor 会向目标节点发起 transfer leader。若目标节点在该时刻 part 状态未完成切换/本地执行线程推进受阻，就会出现 `Can't find leader`。

这本身不必然是根因，但在你这个场景中它和 coredump 证据共同指向：**底层执行推进不畅**，不是单个请求偶发超时。

---

## 5. 把三类证据拼成闭环

可以在 PR 里用下面这段“闭环式”表达：

1. **代码证据**：`newPart()` 写锁内调用 `newEngine()`；旧 `newEngine()` 里有 `newEngineAsync(...).get()` 阻塞点。
2. **coredump 证据**：线程栈明确落在 `NebulaStore::newEngine -> Future::get/waitImpl`。
3. **日志证据**：create space 与 leader transfer 并发，随后 leader 查询异常集中爆发，节点最终 offline。
4. **修复对应性**：去掉 `.get()` 后，锁内不再等待异步 future，等待环关键边被切断。

这四点结合，reviewer 一般就很难反驳“根因定位是否充分”。

---

## 6. 你可以直接复用到 PR 的“根因描述模板”

```markdown
Root cause analysis:
- `NebulaStore::newPart()` invokes `newEngine()` while holding `spaceLock_` (write lock).
- Before this fix, `newEngine()` called `newEngineAsync(...).get()`, introducing a blocking wait in a lock-sensitive path.
- Under concurrent `CREATE SPACE` and `BALANCE LEADER`, this blocking point can participate in a lock-wait cycle and significantly delay storage task progress.
- Evidence:
  1) coredump stack shows thread blocked at `NebulaStore::newEngine -> Future::get/waitImpl`;
  2) runtime logs show intensive part creation + transfer leader + repeated `Can't find leader` on the affected host;
  3) host eventually turns offline in `SHOW HOSTS`.
```

---

## 7. 建议你再补两组数据（会让 PR 更稳）

1. **修复前后对比表**（同压测 30 分钟）
   - offline 次数
   - `Can't find leader` 次数
   - storaged 平均/峰值 CPU

2. **线程栈抽样对比**
   - 修复前：可见 `Future::get()` 卡点
   - 修复后：该卡点不再出现（或显著减少）

这两组数据通常比长篇解释更能说服维护者。
