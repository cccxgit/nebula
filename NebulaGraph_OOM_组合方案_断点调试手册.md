# NebulaGraph 组合方案（方案1+方案2）断点调试手册

## 1. 目标
本手册用于你在本地对“内存保护组合方案”做**可重复断点调试**，重点验证：
1. 内存高水位标志位何时被置位。
2. 方案1（Storage inflight 限流 + 早停）是否按预期执行。
3. 方案2（执行器循环中分段检测）是否按预期中断查询。

---

## 2. 前置环境

- 代码目录：`/home/sch/nebula/nebula-release-3.6`
- 安装目录：`/usr/local/nebula`
- console：`/home/sch/nebula/nebula-console`
- 服务脚本：`/usr/local/nebula/scripts/nebula.service`

建议使用 Debug 构建（你当前流程已符合）：
```bash
cmake -DCMAKE_INSTALL_PREFIX=/usr/local/nebula -DENABLE_TESTING=OFF -DCMAKE_BUILD_TYPE=Debug ..
make -j20
sudo make install
```

---

## 3. 调试前准备

### 3.1 启动服务
```bash
sudo /usr/local/nebula/scripts/nebula.service start all
sudo /usr/local/nebula/scripts/nebula.service status all
```

### 3.2 准备可触发大查询的空间与数据
可复用压测脚本：
- `scripts/run_oom_pressure_test_installed.sh`

如果你只想手动最小化复现：
1. 建 space/tag/edge
2. 批量写入顶点与边
3. 执行高 fan-out GO 查询

---

## 4. 关键断点清单（按调用链）

## 4.1 全局内存信号
- `nebula::graph::QueryEngine::setupMemoryMonitorThread`
  - 文件：`src/graph/service/QueryEngine.cpp`
- `nebula::memory::MemoryUtils::hitsHighWatermark`
  - 文件：`src/common/memory/MemoryUtils.cpp`

目的：确认 `MemoryUtils::kHitMemoryHighWatermark` 的来源与更新周期。

## 4.2 查询入口兜底
- `nebula::graph::Executor::open`
- `nebula::graph::Executor::checkMemoryWatermark`
  - 文件：`src/graph/executor/Executor.cpp`

目的：确认“查询一开始就被拒绝”的路径。

## 4.3 方案1（StorageClient 限流）
- `nebula::storage::StorageClientBase<...>::collectResponse`
  - 文件：`src/clients/storage/StorageClientBase-inl.h`

重点观察：
- `inflightLimit`
- `state->nextToLaunch`
- `state->inFlight`
- `state->aborted`
- `markAbortAndSkipUnlaunched()` 是否触发

## 4.4 方案2（执行器内分段检测）
- `nebula::graph::StorageAccessExecutor::checkMemoryAndAbortQuery`
  - 文件：`src/graph/executor/StorageAccessExecutor.h`

再选一条具体执行链下断点：
- `TraverseExecutor::buildRequestVids`
- `TraverseExecutor::buildAdjList`
- `TraverseExecutor::expand`
- `GetNeighborsExecutor::handleResponse`
- `AppendVerticesExecutor::handleResp`

---

## 5. gdb 实操（推荐 attach graphd）

由于 graphd 通常由 root 运行，建议：

### 5.1 获取 PID
```bash
cat /usr/local/nebula/pids/nebula-graphd.pid
```

### 5.2 attach
```bash
sudo gdb -p $(cat /usr/local/nebula/pids/nebula-graphd.pid)
```

### 5.3 建议 gdb 设置
在 gdb 中执行：
```gdb
set pagination off
set print pretty on
set breakpoint pending on
set detach-on-fork off
handle SIGPIPE nostop noprint pass
```

### 5.4 下断点（示例）
```gdb
b nebula::memory::MemoryUtils::hitsHighWatermark
b nebula::graph::Executor::checkMemoryWatermark
b nebula::graph::StorageAccessExecutor::checkMemoryAndAbortQuery
b nebula::graph::TraverseExecutor::buildRequestVids
b nebula::graph::TraverseExecutor::buildAdjList
```

方案1模板函数断点（可能需要 `rbreak`）：
```gdb
rbreak collectResponse
```

### 5.5 观察关键变量
```gdb
p nebula::memory::MemoryUtils::kHitMemoryHighWatermark
p FLAGS_num_rows_to_check_memory
p FLAGS_max_storage_inflight_per_query
p FLAGS_system_memory_high_watermark_ratio
```

命中 `collectResponse` 时可进一步看：
```gdb
p inflightLimit
p state->nextToLaunch
p state->inFlight
p state->aborted
```

---

## 6. 建议的两组验证场景

## 6.1 验证方案1（限流）

### 配置
- `max_storage_inflight_per_query=0` 与 `8` 对比。

### 观察点
- 在 `collectResponse` 内 `inflightLimit` 是否符合配置。
- 并发发包节奏是否从“全发”变为“有限并发补位”。

### 期望
- `inflight=8` 时，`state->inFlight` 不应长期超过 8。

## 6.2 验证方案2（主动中断）

### 配置
- `num_rows_to_check_memory=256`
- `system_memory_high_watermark_ratio` 调低（如 0.75）更易触发。

### 观察点
- `checkMemoryAndAbortQuery` 被命中。
- `qctx()->markKilled()` 路径执行。
- 查询返回 `GraphMemoryExceeded` / `E_GRAPH_MEMORY_EXCEEDED`。

### 期望
- 查询失败但 graphd 进程存活，不出现系统 OOM kill。

---

## 7. 快速排障指引

### 7.1 断点打不上
- 确认使用 Debug 构建并重新 install。
- 用 `rbreak` 匹配模板函数。

### 7.2 命中次数太多
- 对断点加条件，例如：
```gdb
condition <bpnum> FLAGS_num_rows_to_check_memory==256
```

### 7.3 无法 attach
- graphd 若为 root 进程，请使用 `sudo gdb -p <pid>`。

### 7.4 查询无触发
- 提高查询规模（起点数、step、边密度）。
- 适当下调高水位阈值以放大触发概率。

---

## 8. 建议配套日志观察

可同时观察：
- graphd 日志：`/usr/local/nebula/logs/nebula-graphd.INFO`
- 查询错误返回（console 侧）
- 压测结果汇总：`oom_pressure_runtime_installed/results/summary.csv`

结合断点与日志可以确认：
1. 触发位置（入口/执行中/Storage 回包阶段）
2. 触发后是否走了预期错误码路径
3. 进程是否稳定存活

---

## 9. 常用命令速查

```bash
# 状态
sudo /usr/local/nebula/scripts/nebula.service status all

# 重启 graphd
sudo /usr/local/nebula/scripts/nebula.service restart graphd

# 查看 graphd pid
cat /usr/local/nebula/pids/nebula-graphd.pid

# attach 调试
sudo gdb -p $(cat /usr/local/nebula/pids/nebula-graphd.pid)

# 执行压测
./scripts/run_oom_pressure_test_installed.sh
```

