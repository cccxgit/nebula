# NebulaGraph 组合方案（方案1+方案2）技术实现方案书

## 1. 文档目标
本文用于辅助阅读当前已落地的源码改动，帮助你快速理解：
1. 大查询场景下 graphd 内存 OOM 的根因路径。
2. 组合方案（方案1+方案2）在代码中的实现位置与执行流程。
3. 如何通过 gflags 调参、压测与日志判断保护是否生效。

适用范围：NebulaGraph `nebula-release-3.6` 当前工作区改动版本。

---

## 2. 问题定义与目标

### 2.1 问题定义
在大查询（如高步数 GO / 大规模邻接扩展）中，storage 返回数据量过大，graph 在“请求构建 + RPC回包反序列化 + 结果拼接”阶段内存快速上升，可能触发系统 OOM Killer。

### 2.2 目标
- 在内存紧张时，查询应“可控失败”，返回内存超限错误，而不是被操作系统直接杀进程。
- 同时降低 graph->storage 的瞬时 inflight 压力，减少回包洪峰。

---

## 3. 组合方案总览

### 3.1 方案1：Storage RPC 并发节流 + 高水位快速失败
核心思想：
- 对单个查询发往 storage 的并发 RPC 数做上限控制（`max_storage_inflight_per_query`）。
- 一旦检测到系统内存命中高水位，立即停止继续发起未发送请求，并将未发请求标记为 `E_GRAPH_MEMORY_EXCEEDED`。

### 3.2 方案2：执行器路径内“分段内存检测 + 主动中断查询”
核心思想：
- 在关键循环（请求构建、结果聚合、遍历扩展）中按行数周期检查内存高水位。
- 命中后调用统一中断逻辑，标记 query killed 并返回 `GraphMemoryExceeded`。

### 3.3 两个方案的关系
- 方案1解决“请求侧洪峰输入”问题（降低并发冲击）。
- 方案2解决“执行侧处理过程”问题（快速止血）。
- 二者叠加形成“入口限流 + 执行中断”的闭环。

---

## 4. 关键改动文件与职责

### 4.1 Storage 客户端层（方案1主战场）
- `src/clients/storage/StorageClientBase.cpp`
- `src/clients/storage/StorageClientBase.h`
- `src/clients/storage/StorageClientBase-inl.h`

关键点：
- 新增 gflag：`max_storage_inflight_per_query`（默认 8）。
- `collectResponse(...)` 从“一次性并发发完”改为“受 inflightLimit 控制的调度器”。
- 命中 `MemoryUtils::kHitMemoryHighWatermark` 时，未发请求直接标记失败。

### 4.2 执行器公共层（方案2统一入口）
- `src/graph/executor/StorageAccessExecutor.h`
- `src/graph/executor/StorageAccessExecutor.cpp`

关键点：
- 新增 `checkMemoryAndAbortQuery()`：
  - 检查 `kHitMemoryHighWatermark`。
  - 命中则 `qctx()->markKilled()` + 返回 `memoryExceededStatus()`。
- 在构建请求集合时增加周期检测（`FLAGS_num_rows_to_check_memory`）。

### 4.3 具体查询执行器（方案2落地点）
- `src/graph/executor/query/AppendVerticesExecutor.cpp/.h`
- `src/graph/executor/query/GetNeighborsExecutor.cpp`
- `src/graph/executor/query/IndexScanExecutor.cpp`
- `src/graph/executor/query/TraverseExecutor.cpp/.h`

关键点：
- 在大循环中加入 `checkMemoryAndAbortQuery()`。
- 部分函数签名由 `void/DataSet` 改为 `Status/StatusOr<DataSet>`，用于向上层传播“内存中断”状态。

---

## 5. 关键执行链路（从监控到中断）

### 5.1 内存高水位来源
- `src/graph/service/QueryEngine.cpp`
  - 后台线程周期调用 `MemoryUtils::hitsHighWatermark()`。
  - 更新全局原子变量：`MemoryUtils::kHitMemoryHighWatermark`。

- `src/common/memory/MemoryUtils.cpp`
  - `system_memory_high_watermark_ratio` 控制阈值（默认 0.8）。

### 5.2 查询入口兜底
- `src/graph/executor/Executor.cpp`
  - `Executor::open()` 调用 `checkMemoryWatermark()`。
  - 若已命中高水位，查询在开始前直接失败。

### 5.3 查询执行中止（方案2）
- 各执行器在循环中按 `num_rows_to_check_memory` 周期检查。
- 命中后返回 `GraphMemoryExceeded`，并标记 query killed。

### 5.4 Storage RPC 调度限流与早停（方案1）
- `StorageClientBase-inl.h::collectResponse(...)`：
  1. 读取 `max_storage_inflight_per_query` 计算 `inflightLimit`。
  2. 通过 `launchMore/launchRequest` 仅维持有限并发。
  3. 命中高水位：
     - 已在飞请求继续收尾。
     - 未发请求统一写入 `E_GRAPH_MEMORY_EXCEEDED`。
  4. 汇总后返回 `StorageRpcResponse`。

---

## 6. 典型时序（GO 查询示例）

示例语句：
```ngql
GO 1 TO 2 STEPS FROM "v000001","v000002" OVER follow YIELD dst(edge) AS v;
```

时序（简化）：
```text
Client -> Graph Executor(open)
Graph Executor -> checkMemoryWatermark()
Graph TraverseExecutor -> buildRequestVids() [周期检查内存]
Graph StorageClientBase -> collectResponse() [按 inflightLimit 发RPC]
Storage -> Graph 回包
Graph Executor -> handleResponse/expand/buildAdjList [周期检查内存]
if 命中高水位:
  - 未发RPC不再发，标记 E_GRAPH_MEMORY_EXCEEDED
  - 当前执行器返回 GraphMemoryExceeded
  - QueryContext 标记 killed
Graph -> Client 返回内存超限错误
```

---

## 7. 新增/关键参数说明

### 7.1 `max_storage_inflight_per_query`
- 定义：`src/clients/storage/StorageClientBase.cpp`
- 含义：单查询最大并发 storage RPC 数。
- 建议：
  - 内存紧张环境先用 `4~16`。
  - `0` 表示不限制（不建议生产默认使用）。

### 7.2 `num_rows_to_check_memory`
- 定义：`src/graph/service/GraphFlags.cpp`
- 含义：执行器每处理 N 行做一次内存检测。
- 建议：
  - 值越小，响应越快，额外开销越高。
  - 常见区间：`128/256/512/1024`。

### 7.3 `system_memory_high_watermark_ratio`
- 定义：`src/common/memory/MemoryUtils.cpp`
- 含义：系统内存高水位阈值。
- 建议：
  - 生产常用 `0.75~0.85`，结合机器和并发量调。

---

## 8. 阅读源码建议顺序
建议按“全局 -> 入口 -> 调度 -> 执行器”阅读：
1. `src/common/memory/MemoryUtils.cpp`
2. `src/graph/service/QueryEngine.cpp`
3. `src/graph/executor/Executor.cpp`
4. `src/clients/storage/StorageClientBase-inl.h`
5. `src/graph/executor/StorageAccessExecutor.h/.cpp`
6. `src/graph/executor/query/TraverseExecutor.cpp`
7. `src/graph/executor/query/AppendVerticesExecutor.cpp`
8. `src/graph/executor/query/GetNeighborsExecutor.cpp`
9. `src/graph/executor/query/IndexScanExecutor.cpp`

---

## 9. 设计收益与边界

### 9.1 收益
- 避免“瞬时 fan-out + 大回包”导致的内存陡增。
- 查询可中断、可观测，替代进程被 OOM Killer 杀死。
- 对多执行器路径统一注入内存检测逻辑。

### 9.2 当前边界
- 已发送的 in-flight RPC 无法强制撤销（只能等待回收）。
- 高水位标志是全局状态，可能导致“连带拒绝新查询”。
- 保护触发后表现为失败率上升，属于“可控降级”。

---

## 10. 调试与验证建议

### 10.1 观察点
- graphd 返回 `E_GRAPH_MEMORY_EXCEEDED` / `GraphMemoryExceeded`。
- 请求成功率、超时率、峰值 RSS 变化。
- 不再出现 graphd 被系统 OOM kill。

### 10.2 你当前仓库配套脚本
- `scripts/run_oom_pressure_test_installed.sh`
- 报告：`NebulaGraph_OOM_Pressure_Execution_Report.md`
- 明细：`oom_pressure_runtime_installed/results/summary.csv`

该脚本按多组参数自动重启 graphd 并压测，可用于回归比较。

---

## 11. 后续可演进方向
1. 增加“按会话/查询级”动态 inflight 限额（而非静态全局 gflag）。
2. 将 memory watermark 与 query queue/ admission control 联动。
3. 在 storage 协议层增加可取消语义，缩短已发请求尾部耗时。
4. 增加专门的单元测试与集成测试覆盖“高水位中断”路径。

---

## 12. 结论
当前组合方案已实现“限流+中断”两层防护：
- 方案1控制输入洪峰；
- 方案2控制执行过程内存扩张；
并通过统一错误码返回实现可控失败，达到“防止 graph 服务因大查询直接 OOM”的设计目标。
