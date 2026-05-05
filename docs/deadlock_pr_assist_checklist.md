# NebulaGraph 并发死锁问题详解材料（Leader Balance × Create Space）

> 目标：帮助你系统理解“leader 均衡任务与 create space 并发下 storage 偶发 offline、工作线程卡死”的可能根因，并把根因与修复点精确映射到代码位置，便于你完善社区 PR。

---

## 1）问题现象与触发条件

结合你提供的环境（3 meta / 3 graph / 3 storage，K8s 部署，`partition_num=20`、`replica_factor=3`），典型现象是：

- 并发执行 leader balance 与 `CREATE SPACE`；
- 某些 storage 节点在 `SHOW HOSTS` 中出现 offline；
- storage 线程池工作线程大量阻塞，服务对外请求处理能力显著下降或失效。

该现象与“持锁路径上进行跨线程阻塞等待”高度一致，尤其是出现线程间锁顺序反转（lock inversion）时，容易演化为死锁/饥饿。

---

## 2）关键代码路径与锁关系（带源码位置）

下面是当前 master 代码中最关键的几个锚点：

### A. `addSpace()` 在持有 `NebulaStore::lock_` 写锁时创建引擎

`addSpace()` 入口即加写锁：

```cpp
folly::RWSpinLock::WriteHolder wh(&lock_);
```

并在锁内调用 `newEngine(...)`：

```cpp
spaces_[spaceId]->engines_.emplace_back(newEngine(spaceId, path, options_.walPath_));
```

对应位置：`src/kvstore/NebulaStore.cpp` 第 406-434 行。  
这说明 “创建 space -> 创建 engine” 的主流程在 store 全局写锁保护区内执行。  
【F:src/kvstore/NebulaStore.cpp†L406-L434】

### B. 旧问题中的危险点：`newEngine()` 同步等待异步 future

你给出的历史修复提交（`60df94c7...`）表明：旧实现里 `newEngine()` 通过

```cpp
auto pair = this->newEngineAsync(spaceId, dataPath, walPath).get();
```

等待 `newEngineAsync()` 完成，这是一种“持锁线程等待异步线程返回”的模式。  
而 `newEngineAsync()` 是丢到全局 IO executor 执行：

```cpp
return folly::via(folly::getGlobalIOExecutor().get(), ...)
```

对应当前文件里异步构建函数定义在第 354-371 行。  
【F:src/kvstore/NebulaStore.cpp†L354-L371】

> 注：当前 master 上 `newEngine()` 已是同步直接构建 `RocksEngine`，不再 `.get()` future（第 373-389 行）。  
【F:src/kvstore/NebulaStore.cpp†L373-L389】

### C. store 内部大量路径共享同一把 `lock_`

除了 `addSpace()`，例如 `addPart()` 也会加 `lock_` 写锁；`partLeader()` 使用读锁。  
这意味着一旦写锁长时间不释放，会放大连锁阻塞影响。  
【F:src/kvstore/NebulaStore.cpp†L391-L404】【F:src/kvstore/NebulaStore.cpp†L448-L467】

---

## 3）可能的死锁机理（为什么会卡死）

以下是与你描述最一致、并且和修复改动逻辑闭环的“高可信机理”：

1. 线程 T1（create space 路径）进入 `addSpace()`，拿到 `NebulaStore::lock_` 写锁。  
   【F:src/kvstore/NebulaStore.cpp†L406-L408】
2. T1 在锁内调用旧版 `newEngine()`，内部执行 `newEngineAsync(...).get()`，因此 T1 会“持有 `lock_` 同步等待 IO 线程返回”。
3. `newEngineAsync()` 通过 `folly::via(folly::getGlobalIOExecutor())` 投递到全局 IO 线程池执行（T2）。其执行体包含两步关键动作：
   - 调用 `options_.cffBuilder_->buildCfFactory(spaceId)`；
   - 调用 `getSpaceVidLen(spaceId)`，进一步访问 `options_.schemaMan_->getSpaceVidLen(spaceId)`。  
   这些步骤都属于“依赖外部组件/远端状态”的慢路径，执行时延不可控。  
4. 并发触发 leader balance 时，storage admin 线程会持续处理 `AddPart`/`MemberChange`/`TransLeader` 等请求；其中 `AddPartProcessor::process()` 会在 space 不存在时直接调用 `store->addSpace(spaceId)`，而 `addPart()` 本身也要申请同一把 `NebulaStore::lock_` 写锁。  
   代码锚点：`AddPartProcessor::process()` 第 168-174 行；`NebulaStore::addPart()` 第 452 行。
5. 于是形成明确的阻塞放大链：
   - T1（create-space 元事件线程）：持有 `lock_`，阻塞在 `.get()`；
   - T3/T4...（leader-balance admin 线程）：进入 `addPart()`/`addSpace()` 时等待 `lock_`；
   - admin worker 被大量占满后，后续心跳相关/成员变更请求无法及时处理，表现为 storage offline。  
6. 因为 `.get()` 放在全局写锁临界区内，任何 IO 慢路径都会把“单次建引擎慢”放大成“全局元数据操作串行停滞”，这就是该问题可演化为“看起来像死锁”的直接原因。

这类问题本质是：**在全局写锁临界区里引入跨线程同步等待**，把“局部慢路径”升级成“全局阻塞风险”。

---

## 4）你当前修复方式为何合理

你的修复（`60df94c7...`）把 `newEngine()` 改为“当前线程直接构建 `RocksEngine`”，不再通过 `newEngineAsync(...).get()`。

合理性在于：

- 消除了“持锁线程等待异步线程”的等待边；
- 去掉了跨线程 future 汇合点，避免形成等待环；
- `newEngine()` 返回对象语义未变（仍返回 `unique_ptr<KVEngine>`，构建参数一致），行为上更可预测。

对应当前 `newEngine()` 实现可见第 373-389 行。  
【F:src/kvstore/NebulaStore.cpp†L373-L389】

---

## 5）为何你会在 3.6 分支遇到，而 master 可能“看起来已好”

根据你提供信息和本地仓库状态：

- 该修复提交 ID 为 `60df94c7d3ed2500086a763487c91b79da9ae22a`；
- 当前工作分支已包含等价修复形态（`newEngine()` 无 future `.get()`）。

这通常意味着：

- 问题最初可能存在于 `release-3.6` 的某个区间；
- 后续在 master 或你的工作分支中已合入/携带修复；
- 是否“master 仍可复现”要以**同一套并发脚本 + 同样压测时长**实测为准。

---

## 6）建议你对外（PR）如何表述根因（可直接改写为英文）

建议使用三段式：

1. **Root Cause**：`addSpace()` holds `NebulaStore::lock_` write lock, while old `newEngine()` waited on `newEngineAsync(...).get()`, introducing a cross-thread blocking wait in a locked critical section.  
2. **Deadlock Pattern**：under concurrent leader balancing and space creation, this could form lock/resource wait cycles and stall storage worker threads.  
3. **Fix**：make `newEngine()` construct engine synchronously in-place, remove the blocking future wait edge, and keep engine creation semantics unchanged.

---

## 7）你还可以如何“证据化”这个根因（强烈建议）

为了让 reviewer 快速接受，建议补三类证据：

- **线程栈证据**：卡死时 `thread apply all bt`，标注谁持有 `lock_`、谁在等待；
- **时序证据**：`create space` 与 `leader balance` 并发触发时间点 + storage 日志时间点对齐；
- **对照证据**：
  - 旧实现（含 `.get()`）高并发下可复现；
  - 修复后同负载下不再出现相同阻塞模式。

---

## 8）一句话总结（给你自己记忆锚点）

这个 bug 的核心不是“某个锁没释放”，而是：**在持有 NebulaStore 全局写锁的路径上，执行了对异步任务的阻塞等待，导致并发场景下形成等待环并拖垮 storage worker。**
