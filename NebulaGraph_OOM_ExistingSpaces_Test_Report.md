# NebulaGraph OOM 保护测试报告（预置 LDBC 空间）

- 执行时间: 2026-02-21（含用户调整 `storage_client_timeout_ms=600000` 后补跑）
- 环境: /usr/local/nebula service 管理模式
- 图空间: stress_test_0221, stress_test_0220
- 说明: 先执行完整套件，再对失败用例做定向补跑验证。

## 结果汇总

| Case | Space | Mode | ExpectTrigger | ExpectGuardLog | Verdict | Success | Fail | OOMProtected | Timeout | OtherFail | GuardLogs | PeakRSS(KB) | AvgRSS(KB) |
|---|---|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| case_baseline_0221 | stress_test_0221 | workers | N | N | PASS | 1111 | 0 | 0 | 0 | 0 | 0 | 64172 | 58611 |
| case_runtime_guard_0221 | stress_test_0221 | single | Y | Y | FAIL | 0 | 1 | 0 | 1 | 0 | 0 | 38380 | 38315 |
| case_start_reject_0221 | stress_test_0221 | single | E | N | PASS | 0 | 2 | 2 | 0 | 0 | 0 | 37468 | 37357 |
| case_recovery_0221 | stress_test_0221 | workers | N | N | PASS | 1290 | 0 | 0 | 0 | 0 | 0 | 63236 | 59170 |
| case_inflight16_heavy_0221 | stress_test_0221 | workers | OBS | N | PASS | 0 | 1 | 0 | 1 | 0 | 0 | 38352 | 38258 |
| case_inflight8_heavy_0221 | stress_test_0221 | workers | OBS | N | PASS | 0 | 1 | 0 | 1 | 0 | 0 | 38412 | 38307 |
| case_inflight4_heavy_0221 | stress_test_0221 | workers | OBS | N | PASS | 0 | 1 | 0 | 1 | 0 | 0 | 38316 | 38025 |
| case_baseline_0220 | stress_test_0220 | workers | N | N | FAIL | 1126 | 4 | 4 | 0 | 0 | 0 | 63104 | 59131 |
| case_runtime_guard_rerun3_0221 | stress_test_0221 | single | Y | Y | FAIL | 0 | 1 | 0 | 1 | 0 | 0 | 0 | 0 |
| case_baseline_0220_rerun | stress_test_0220 | workers | N | N | PASS | 490 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |

## 关键结论

1. `case_start_reject_0221` 通过：低水位下出现 `OOMProtected`，且无 Guard 日志，符合启动期拒绝路径预期。
2. `case_baseline_0221` 与 `case_recovery_0221` 通过：常规阈值下查询稳定。
3. `case_baseline_0220` 初跑失败，但在用户将 `storage_client_timeout_ms` 提升到 `600000` 后补跑 `case_baseline_0220_rerun` 通过（`oom=0`）。
4. 执行期强制触发用例仍未命中（`case_runtime_guard_0221`、`case_runtime_guard_rerun3_0221`）：当前观测为超时而非 `GraphMemoryExceeded`，且无 `[OOM_GUARD_TRIGGER]` 日志，需继续针对查询路径与配置做专项排查。

## 产物目录

- 过程日志: `Development_tasks_execution.log`
- 汇总 CSV: `oom_existing_spaces_runtime/results/summary.csv`
- 定向补跑: `oom_existing_spaces_runtime/results/case_runtime_guard_rerun3_traverse/`, `oom_existing_spaces_runtime/results/case_baseline_0220_rerun/`
