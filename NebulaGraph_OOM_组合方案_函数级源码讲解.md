# NebulaGraph 组合方案（方案1+方案2）函数级源码讲解

## 1. 使用说明
这份文档是对 `NebulaGraph_OOM_组合方案_技术实现方案书.md` 的补充，重点回答：
- 每个关键函数具体做什么。
- 返回值与异常如何向上层传递。
- 为什么部分函数签名由 `void/DataSet` 改成 `Status/StatusOr<DataSet>`。

---

## 2. 总调用链（先看全局）

```text
QueryEngine::setupMemoryMonitorThread()
    -> MemoryUtils::hitsHighWatermark()
    -> MemoryUtils::kHitMemoryHighWatermark (原子标志)

Executor::open()
    -> checkMemoryWatermark()            // 查询入口兜底

具体执行器（Append/Traverse/GetNeighbors/IndexScan/...）
    -> checkMemoryAndAbortQuery()        // 循环内分段检查

StorageClientBase::collectResponse()
    -> inflight 限流调度 + 内存命中后早停
```

---

## 3. 全局内存信号相关函数

### 3.1 `QueryEngine::setupMemoryMonitorThread`
文件：`src/graph/service/QueryEngine.cpp`

职责：
- 启动后台线程周期执行内存检查。
- 将检查结果写入全局原子变量 `MemoryUtils::kHitMemoryHighWatermark`。

你可理解为：
- 这是“全系统内存告警开关”的生产者。
- 后续所有执行器/Storage 客户端只是读取该开关。

### 3.2 `MemoryUtils::hitsHighWatermark`
文件：`src/common/memory/MemoryUtils.cpp`

职责：
- 读取系统可用内存（物理机或容器路径）。
- 用 `(1 - available/total) > FLAGS_system_memory_high_watermark_ratio` 判断是否命中高水位。

关键点：
- `system_memory_high_watermark_ratio` 是总开关阈值。
- 返回 `StatusOr<bool>`，上层可区分“读取失败”与“是否命中”。

### 3.3 `Executor::open` / `checkMemoryWatermark`
文件：`src/graph/executor/Executor.cpp`

职责：
- 查询执行前做入口兜底。
- 若已命中高水位，直接拒绝进入执行阶段。

意义：
- 防止“已经内存紧张时还接新重查询”。

---

## 4. 方案1核心：Storage 并发限流与早停

## 4.1 `StorageClientBase::collectResponse`
文件：`src/clients/storage/StorageClientBase-inl.h`

这是本次改动最核心函数。

### 4.1.1 入参/出参
- 入参：`requests`（按 host 聚合后的 RPC 请求）+ `remoteFunc`（具体 RPC 调用器）。
- 出参：`folly::SemiFuture<StorageRpcResponse<Response>>`。

### 4.1.2 新行为（相对旧实现）
旧实现：
- 一次性把所有请求都发出去，再 `collectAll` 等待回包。

新实现：
- 根据 `FLAGS_max_storage_inflight_per_query` 计算 `inflightLimit`。
- 用状态机（`CollectState`）和 `launchMore/launchRequest` 保持有限并发。
- 动态补位：某请求结束后再发下一批。

### 4.1.3 关键状态结构 `CollectState`
包含：
- `nextToLaunch`：下一个待发请求下标。
- `inFlight`：当前飞行中的请求数。
- `aborted`：是否已触发内存中断。
- `fulfilled`：promise 是否已完成。
- `rpcResp`：累积返回结果。

### 4.1.4 内存命中时处理
场景 A：请求尚未发出
- `markAbortAndSkipUnlaunched()` 会将未发请求统一写入 `E_GRAPH_MEMORY_EXCEEDED`。

场景 B：请求已发出，回包处理中
- 在 `thenTry` 中再次检查高水位。
- 当前请求也按内存超限失败路径写入失败分区。

### 4.1.5 错误映射
- 远端异常：`E_RPC_FAILURE`
- status 为 graph memory exceeded：`E_GRAPH_MEMORY_EXCEEDED`
- 其余 status 错误：`E_RPC_FAILURE`

### 4.1.6 为什么要这样做
- 限制并发能显著降低瞬时反序列化与聚合压力。
- 命中高水位后不再继续“扩张工作集”。

---

## 5. 方案2公共入口：统一中断接口

### 5.1 `StorageAccessExecutor::checkMemoryAndAbortQuery`
文件：`src/graph/executor/StorageAccessExecutor.h`

职责：
- 读取 `kHitMemoryHighWatermark`。
- 命中则 `qctx()->markKilled()`，并返回 `Executor::memoryExceededStatus()`。

为什么是这个位置：
- `StorageAccessExecutor` 是多种查询执行器的公共父类。
- 在这里统一接口，子类只需插入 `NG_RETURN_IF_ERROR(checkMemoryAndAbortQuery())`。

### 5.2 `buildRequestDataSet` / `buildRequestList`
文件：`src/graph/executor/StorageAccessExecutor.cpp`

改动点：
- 在遍历输入构建 VID 请求时，按 `FLAGS_num_rows_to_check_memory` 周期检查。
- 命中直接返回 `Status::GraphMemoryExceeded`。

意义：
- 防止“请求发出前”就已经在 graph 端攒出巨大内存。

---

## 6. 执行器级改动详解

## 6.1 `AppendVerticesExecutor`
文件：
- `src/graph/executor/query/AppendVerticesExecutor.cpp`
- `src/graph/executor/query/AppendVerticesExecutor.h`

### 6.1.1 插入检查点
- `handleNullProp`
- `handleResp`
- `buildVerticesResult`
- `buildMap`
- `handleJob`

这些都是“行级扫描 + 结果拼接”热点区。

### 6.1.2 签名变化原因
由：
- `void buildMap(...)`
- `DataSet buildVerticesResult(...)`
- `DataSet handleJob(...)`
改为：
- `Status buildMap(...)`
- `StatusOr<DataSet> buildVerticesResult(...)`
- `StatusOr<DataSet> handleJob(...)`

原因：
- 需要把“内存中断错误”从子任务线程可靠返回到 gather 阶段。
- 以前 `void/DataSet` 无法表达中断状态，只能继续执行，达不到主动止损目标。

### 6.1.3 多任务聚合路径
`handleRespMultiJobs` 在 gather 中显式遍历 `prepareResult` 的 `Status`，发现错误即 `NG_RETURN_IF_ERROR`，中断整个流程。

---

## 6.2 `GetNeighborsExecutor`
文件：`src/graph/executor/query/GetNeighborsExecutor.cpp`

改动点：
- `execute()` 中统计 hostLatency/size 的循环加周期检查。
- `handleResponse()` 遍历 responses 时加周期检查。

意义：
- 该执行器常处理大邻接回包，聚合阶段是高风险点。

---

## 6.3 `IndexScanExecutor`
文件：`src/graph/executor/query/IndexScanExecutor.cpp`

改动点：
- `handleResp(...)` 遍历 `rpcResp.responses()` 时按周期检查。

意义：
- 大索引扫描时避免在结果拼接阶段持续扩张内存。

---

## 6.4 `TraverseExecutor`
文件：
- `src/graph/executor/query/TraverseExecutor.cpp`
- `src/graph/executor/query/TraverseExecutor.h`

这是本次方案2改动最多的执行器。

### 6.4.1 主要检查点
- `buildRequestVids()`
- `buildAdjList()`
- `asyncExpandOneStep()` gather 合并阶段
- `handleResponse()`
- `expand()`

### 6.4.2 关键签名变化
- `expand(GetNeighborsIter*)`：`void -> Status`
- `buildAdjList(...)`：`void -> Status`

原因：
- 在多层遍历中，任何一层触发内存超限都需要可靠向上短路返回。

### 6.4.3 `asyncExpandOneStep` 的变化重点
- 原先 `futures` 是 `Future<Unit>`，现在变成 `Future<Status>`。
- gather 阶段先检查每个 task status，再做全局合并。

这使得“并行子任务中某一个触发内存中断”能被主流程正确感知。

---

## 7. gflags 与行为映射（实践视角）

### 7.1 `max_storage_inflight_per_query`
- 定义：`src/clients/storage/StorageClientBase.cpp`
- 行为：控制每个查询对 storage 的最大并发 RPC。
- 调小：更稳、更慢。
- 调大：更快、但更可能出现内存峰值。

### 7.2 `num_rows_to_check_memory`
- 定义：`src/graph/service/GraphFlags.cpp`
- 行为：执行器每处理 N 行检查一次内存。
- 调小：中断更及时，CPU 分支开销更高。
- 调大：开销更小，但中断滞后。

### 7.3 `system_memory_high_watermark_ratio`
- 定义：`src/common/memory/MemoryUtils.cpp`
- 行为：高水位阈值。

---

## 8. 一个简化错误传播示例

以 `TraverseExecutor` 为例：
1. `buildAdjList()` 在循环中调用 `checkMemoryAndAbortQuery()`。
2. 命中后返回 `Status::GraphMemoryExceeded`。
3. `asyncExpandOneStep()` gather 收到某 task status 非 OK，立即 `NG_RETURN_IF_ERROR`。
4. 上层 future 链收到失败，查询结束并向客户端返回内存超限错误。

这就是“从子任务点到整条执行链”的可靠短路。

---

## 9. 你可以优先重点阅读的函数
按学习收益排序建议：
1. `StorageClientBase::collectResponse`（限流调度核心）
2. `StorageAccessExecutor::checkMemoryAndAbortQuery`（统一中断入口）
3. `TraverseExecutor::asyncExpandOneStep`（并行 + 错误传播）
4. `AppendVerticesExecutor::handleRespMultiJobs`（签名改造示例）
5. `QueryEngine::setupMemoryMonitorThread`（全局内存信号来源）

---

## 10. 小结
这次改造的本质不是“防止任何失败”，而是把“不可控 OOM 崩溃”改成“可控、可返回、可观测的查询失败”。

换句话说：
- 方案1负责“不要一下子吃太多”；
- 方案2负责“吃不下就立刻停”；
- 两者共同保障 graphd 进程生存性。
