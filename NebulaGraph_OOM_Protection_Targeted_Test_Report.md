# NebulaGraph OOM 保护功能定向测试报告

- 执行时间: 2026-02-20 18:49:31
- 环境: /usr/local/nebula service 管理模式
- 图空间: oom_pt
- 压测数据规模: vertex=5000, edge_degree=15, 近似边数=75000
- 并发与时长: workers=6, 每 case 25s

## 测试目标

1. 验证“内存不足主动终止查询”保护可以被稳定命中（OOMProtected > 0 且出现关键日志）。
2. 验证基线 case 不误触发保护（避免误杀查询）。
3. 验证恢复 case（从低水位触发恢复到常规水位）可正常返回成功查询。
4. 区分“执行期触发”与“启动期拒绝”两类错误路径。
5. 通过 graphd 增量日志确认触发点来自 checkMemoryAndAbortQuery。

## 用例设计

- `case_baseline_control`: 常规阈值 + GO 查询基线，不应触发。
- `case_runtime_guard_match`: MATCH 长查询 + 延迟内存干扰，目标是命中执行期保护并出现关键日志。
- `case_start_phase_reject_control`: 低水位 + 无内存干扰，验证启动期拒绝（应 OOM 但不应出现关键日志）。
- `case_recovery_normal`: 恢复常规阈值后，应恢复稳定成功。

## 结果汇总

| Case | ExpectTrigger | ExpectGuardLog | Verdict | Success | Fail | OOMProtected | Timeout | OtherFail | GuardLogs | PeakRSS(KB) | AvgRSS(KB) |
|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| case_baseline_control | N | N | PASS | 1810 | 0 | 0 | 0 | 0 | 0 | 176204 | 148430 |
| case_runtime_guard_match | Y | Y | FAIL | 0 | 3 | 3 | 0 | 0 | 0 | 47760 | 47760 |
| case_start_phase_reject_control | E | N | PASS | 0 | 2 | 2 | 0 | 0 | 0 | 48528 | 48496 |
| case_recovery_normal | N | N | PASS | 1757 | 0 | 0 | 0 | 0 | 0 | 171516 | 145527 |

## 判读标准

- 执行期触发用例（ExpectTrigger=Y）：`OOMProtected > 0` 且 `GuardLogs > 0` 判定通过。
- 启动期拒绝用例（ExpectTrigger=E）：`OOMProtected > 0` 且 `GuardLogs = 0` 判定通过。
- 非触发型用例（ExpectTrigger=N）：`Fail=0 且 OOMProtected=0 且 GuardLogs=0` 判定通过。

## 产物目录

- 结果目录: `oom_protection_targeted_runtime/results/`
- case 配置: `oom_protection_targeted_runtime/conf/`
- 重载数据日志: `oom_protection_targeted_runtime/results/load_data*.out`
- case 增量日志: `oom_protection_targeted_runtime/results/<case>/graph_log_delta.log`

