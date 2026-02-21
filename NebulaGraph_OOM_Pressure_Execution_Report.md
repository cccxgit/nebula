# NebulaGraph OOM 防护压测执行报告

- 执行时间: 2026-02-20 14:53:06
- 环境: /usr/local/nebula service 管理模式
- 图空间: oom_pt
- 压测数据规模: vertex=5000, edge_degree=15, 近似边数=75000
- 并发与时长: workers=6, 每 case 25s

## 执行步骤

1. 使用 `/usr/local/nebula/scripts/nebula.service start all` 启动集群。
2. 执行 `SHOW HOSTS`，并执行 `ADD HOSTS "127.0.0.1":9779`（若已存在则忽略）。
3. 创建压测空间与 schema，导入测试数据。
4. 逐 case 重启 graphd（自定义 config），执行高并发重查询压测并采样 RSS。

## 结果汇总

| Case | Success | Fail | OOMProtected | Timeout | PeakRSS(KB) | AvgRSS(KB) |
|---|---:|---:|---:|---:|---:|---:|
| case0_unlimited | 2024 | 0 | 0 | 0 | 168012 | 140092 |
| case1_inflight16 | 2087 | 0 | 0 | 0 | 171092 | 139545 |
| case2_inflight8_rows256 | 2062 | 0 | 0 | 0 | 169396 | 147735 |
| case3_inflight4_rows256 | 2112 | 0 | 0 | 0 | 169716 | 142225 |
| case4_inflight8_rows256_wm075 | 2065 | 0 | 0 | 0 | 168620 | 142964 |

## 判读

- `OOMProtected` > 0：命中“内存不足主动终止查询”保护。
- `PeakRSS` 越低：峰值内存压力越小。
- `Timeout` 越高：说明查询时延风险上升，需要权衡保护强度。

## 产物

- 明细结果: `oom_pressure_runtime_installed/results/`
- 每 case graphd 配置: `oom_pressure_runtime_installed/conf/`
