# NebulaGraph OOM 组合方案完整测试方案（2026-02-21）

## 1. 测试目标

1. 验证方案1（`max_storage_inflight_per_query`）能有效限制查询期间的 storage 并发洪峰。
2. 验证方案2（执行器循环内 `checkMemoryAndAbortQuery`）能在执行期主动中断查询并返回内存超限错误。
3. 验证“启动期拒绝”与“执行期中断”两条路径都可被区分识别。
4. 验证恢复场景：从触发保护恢复到常规阈值后，查询可恢复正常。
5. 验证修改后系统稳定性：服务存活、不出现 graphd 被 OOM killer 杀进程。

## 2. 覆盖范围

1. Storage 客户端限流与早停：`src/clients/storage/StorageClientBase-inl.h`
2. 执行器统一中断入口：`src/graph/executor/StorageAccessExecutor.h`
3. 执行器热点循环检查点：
   - `src/graph/executor/query/TraverseExecutor.cpp`
   - `src/graph/executor/query/GetNeighborsExecutor.cpp`
   - `src/graph/executor/query/IndexScanExecutor.cpp`
   - `src/graph/executor/query/AppendVerticesExecutor.cpp`
4. 内存高水位信号来源：
   - `src/graph/service/QueryEngine.cpp`
   - `src/common/memory/MemoryUtils.cpp`

## 3. 关键观测信号

1. 查询输出错误码/错误文本：`E_GRAPH_MEMORY_EXCEEDED`、`GraphMemoryExceeded`。
2. graphd 关键日志 token：`[OOM_GUARD_TRIGGER]`（执行期中断应命中）。
3. 统计项：
   - `Success / Fail / OOMProtected / Timeout / OtherFail`
   - `PeakRSS(KB) / AvgRSS(KB)`
4. 服务存活性：case 执行过程中 `graphd` 不崩溃。

## 4. 测试前置条件

1. 使用 `/usr/local/nebula` 安装模式服务。
2. 启动前执行：`ulimit -n 65536`。
3. 优先使用已预置 LDBC 数据集图空间：
   - `stress_test_0221`
   - `stress_test_0220`
4. 避免测试过程对预置空间执行 `DROP/CREATE`、批量重写数据等破坏性操作。
4. 工具脚本：
   - `scripts/run_oom_protection_targeted_test.sh`
   - `scripts/run_oom_pressure_test_installed.sh`

## 4.1 本次修订（基于新增信息）

1. 数据准备流程改为“只读使用现有大空间”，不再执行自建 `oom_pt` 压测数据导入流程。
2. 查询样例改为通用 MATCH 语句（无固定 tag/edge 依赖），适配 LDBC 标准数据集。
3. 新增“跨空间一致性检查”：关键用例分别在 `stress_test_0221` 与 `stress_test_0220` 执行。

## 5. 用例矩阵

### 5.1 定向功能验证（方案2与错误路径）

1. `case_baseline_control`
   - 目标：常规参数下无误触发。
   - 预期：`Fail=0`，`OOMProtected=0`，`GuardLogs=0`。
2. `case_runtime_guard_match`
   - 目标：执行期触发主动中断（强制触发开关 + MATCH 长查询）。
   - 预期：`OOMProtected>0` 且 `GuardLogs>0`。
3. `case_start_phase_reject_control`
   - 目标：启动期拒绝路径验证（低水位阈值）。
   - 预期：`OOMProtected>0` 且 `GuardLogs=0`。
4. `case_recovery_normal`
   - 目标：恢复后正常查询能力。
   - 预期：`Fail=0`，`OOMProtected=0`，`GuardLogs=0`。

### 5.2 压力与参数敏感性验证（方案1主验证）

1. `case0_unlimited`：inflight 不限，作为基线。
2. `case1_inflight16`：中等限流。
3. `case2_inflight8_rows256`：更强限流 + 高频检查。
4. `case3_inflight4_rows256`：强限流。
5. `case4_inflight8_rows256_wm075`：更保守高水位阈值。

预期判定：
1. 各 case 下服务保持存活。
2. 限流配置相较 `case0_unlimited` 对 `PeakRSS` 有抑制趋势（允许存在波动）。
3. 不应出现不可恢复的连续失败（除预期保护触发 case）。

## 6. 执行顺序

1. 执行“定向功能验证”脚本，确认功能正确性与日志可观测性。
2. 执行“压力与参数敏感性”脚本，确认可靠性与参数效果。
3. 汇总报告与产物，给出通过/失败结论及原因。

## 7. 产物与断点续跑要求

1. 执行过程主日志：`Development_tasks_execution.log`
2. 定向验证报告：`NebulaGraph_OOM_Protection_Targeted_Test_Report.md`
3. 压测报告：`NebulaGraph_OOM_Pressure_Execution_Report.md`
4. 运行时明细目录：
   - `oom_protection_targeted_runtime/`
   - `oom_pressure_runtime_installed/`

## 8. 风险与回退

1. 若 `GuardLogs` 未命中但 `OOMProtected>0`，需检查是否落入“启动期拒绝”而非“执行期中断”。
2. 若服务启停异常，先恢复 `/usr/local/nebula/etc/nebula-graphd.conf` 后重启 graphd。
3. 若系统内存背景噪声较高，允许重跑定向 case 并对比两次结果。
