# storage 内存持续上涨根因分析记录

## 任务背景

生产环境长期运行时，storage 进程内存持续升高不下降；图空间数量或分片数越多，内存上涨越快。测试现象中，多数图空间没有数据或数据操作不频繁时，内存仍然每天上涨，几天后又出现一次回落。

本次分析聚焦 storage 的 Raft/WAL 路径，因为该路径与“分片数量多、无业务写入也增长”的特征最吻合。

## 关键源码路径

1. `NebulaStore::newPart()` 为每个 partition 创建一个 `Part`。
   - 文件：`src/kvstore/NebulaStore.cpp`
   - 关键逻辑：`newPart()` 内构造 `Part`，并执行 `part->start(...)`。

2. `Part` 继承 `RaftPart`，`RaftPart` 构造时为每个 partition 创建独立 `FileBasedWal`。
   - 文件：`src/kvstore/raftex/RaftPart.cpp`
   - 关键逻辑：`RaftPart::RaftPart()` 中 `wal_ = FileBasedWal::getWal(...)`。

3. `FileBasedWal` 构造时为每个 WAL 创建独立 `AtomicLogBuffer`。
   - 文件：`src/kvstore/wal/FileBasedWal.cpp`
   - 关键逻辑：`logBuffer_ = AtomicLogBuffer::instance(policy_.bufferSize)`。

4. Raft leader 定时执行 `statusPolling()`，leader 会调用 `sendHeartbeat()`。
   - 文件：`src/kvstore/raftex/RaftPart.cpp`
   - 关键逻辑：`statusPolling()` 的 delay 为 `raft_heartbeat_interval_secs * 1000 / 3 + random(500)`，默认约 1.6 到 2.1 秒。

5. 修复前 `sendHeartbeat()` 每次心跳只要没有正在复制日志，就会追加一个空 NORMAL log。
   - 文件：`src/kvstore/raftex/RaftPart.cpp`
   - 修复前逻辑：
     - `if (!replicatingLogs_) appendLogAsync(clusterId_, LogType::NORMAL, "")`

6. `appendLogAsync()` 最终进入 `appendLogsInternal()`，先写 WAL，再复制给 follower。
   - 文件：`src/kvstore/raftex/RaftPart.cpp`
   - 关键逻辑：`wal_->appendLogs(iter)`。

7. `FileBasedWal::appendLogInternal()` 会把日志写磁盘 WAL，也会写入 `AtomicLogBuffer`。
   - 文件：`src/kvstore/wal/FileBasedWal.cpp`
   - 关键逻辑：`logBuffer_->push(id, term, cluster, msg)`。

8. `Part::commitLogs()` 遇到空 log 会跳过业务数据写入，但此时空 log 已经进入 WAL 和内存 buffer。
   - 文件：`src/kvstore/Part.cpp`
   - 关键逻辑：`if (log.empty()) { Skip the heartbeat; ++iter; continue; }`

## 根因判断

根因是：Raft leader 的 heartbeat 路径在无业务写入时仍持续追加空 WAL log，且空 log 会被复制到 follower 并进入每个副本的 `AtomicLogBuffer`。因此即使图空间没有数据，只要 partition 存在并有 leader，storage 仍会持续产生 WAL 内存缓存增长。

这个根因能解释三个关键现象：

1. **与分片数强相关**
   - 每个 partition 都有独立 Raft 状态机和 WAL buffer。
   - leader partition 越多，空 heartbeat log 产生越多。
   - 这些空 log 会复制到 follower，所以 3 副本下不只是 leader 本地增长，follower 也会写入 WAL 和内存 buffer。

2. **无数据图空间也增长**
   - 业务写入不是触发条件。
   - Raft heartbeat 定时器即可触发空 log 追加。

3. **持续上涨后又阶段性回落**
   - `AtomicLogBuffer` 超过容量后先把旧 node 标记为 deleted。
   - 真正释放发生在 iterator `releaseRef()`，并且依赖 dirty node 数或 `max_log_buffer_size` 阈值。
   - 因此内存表现是阶梯式上涨，在 GC 条件满足后阶段性回落。

## 本次代码改动

### 1. 修复空 heartbeat log 持续写入

修改文件：`src/kvstore/raftex/RaftPart.cpp`

修复策略：只在 leader 当前任期尚未提交过任何 log 时追加空 log。该空 log 的作用是完成 Raft “leader 需要提交当前任期日志后才认为 ready”的语义。当前任期已经 ready 后，心跳只发送 heartbeat RPC，不再每轮写空 WAL。

核心条件从：

```cpp
if (!replicatingLogs_) {
  appendLogAsync(clusterId_, LogType::NORMAL, "");
}
```

调整为：

```cpp
needAppendEmptyLog = status_ == RUNNING && role_ == LEADER && !commitInThisTerm_;
if (needAppendEmptyLog && !replicatingLogs_) {
  appendLogAsync(clusterId_, LogType::NORMAL, "");
}
```

### 2. 增加监控指标

修改文件：

- `src/kvstore/stats/KVStats.h`
- `src/kvstore/stats/KVStats.cpp`
- `src/kvstore/raftex/RaftPart.cpp`

新增指标：

- `num_raft_heartbeat.rate/sum`
  - 统计 Raft leader heartbeat 调用量。

- `num_raft_heartbeat_empty_log.rate/sum`
  - 统计 heartbeat 触发追加空 WAL log 的次数。
  - 修复前，该指标应接近 `num_raft_heartbeat`。
  - 修复后，稳定 leader 任期内该指标应接近 0；只有新 leader 任期启动且尚未提交当前任期日志时才会增加。

- `num_raft_heartbeat_without_empty_log.rate/sum`
  - 统计未追加空 log 的 heartbeat 次数。
  - 修复后，该指标应成为主要 heartbeat 路径。

## 验证思路

### 运行前准备

启动数据库前执行：

```bash
ulimit -n 65536
```

### 编译

当前 build 目录配置：

- `CMAKE_BUILD_TYPE=Debug`
- `CMAKE_INSTALL_PREFIX=/usr/local/nebula`
- `ENABLE_TESTING=OFF`

建议编译命令：

```bash
cd build
make -j10
make install
```

### 启动

```bash
/usr/local/nebula/scripts/nebula.service start metad
/usr/local/nebula/scripts/nebula.service start storaged
/usr/local/nebula/scripts/nebula.service start graphd
```

### 观测指标

通过 storage stats/http stats 观察新增指标：

- `num_raft_heartbeat`
- `num_raft_heartbeat_empty_log`
- `num_raft_heartbeat_without_empty_log`

预期：

1. 修复前或未生效时：
   - `num_raft_heartbeat_empty_log.rate` 与 `num_raft_heartbeat.rate` 接近。
   - 空闲图空间仍持续写 WAL。

2. 修复后：
   - leader 稳定后，`num_raft_heartbeat_empty_log.rate` 应接近 0。
   - `num_raft_heartbeat_without_empty_log.rate` 接近 `num_raft_heartbeat.rate`。
   - storage RSS 增长速度应显著下降，尤其是大量空闲 partition 场景。

### 推荐压测场景

与问题描述保持一致：

- 3 个 storage 实例。
- 5 个图空间。
- 每个图空间 3 副本 20 分片。
- 多数图空间不写入或少量写入。

对比修复前后：

- storage RSS。
- WAL 文件增长速度。
- 新增 heartbeat 指标。
- `num_raft_heartbeat_empty_log` 与 partition 数量的关系。

## 影响评估

### 功能影响

保留了 leader 任期初始化所需的空 log 追加：当 `commitInThisTerm_ == false` 时仍会追加空 log。因此不破坏 Raft leader ready 语义。

leader 任期已经提交当前任期日志后，后续 heartbeat 不再写空 WAL，只发送 heartbeat RPC。心跳保活、lease 更新时间仍由 heartbeat response 路径维护。

### 性能影响

正向影响：

- 大量空闲 partition 下，显著减少 WAL 追加、磁盘写入、内存 `AtomicLogBuffer` 增长和 follower 复制空 log。
- 降低 storage 后台 WAL 清理和 log buffer GC 压力。

潜在风险：

- 如果某些隐含逻辑依赖“每次 heartbeat 都推进 lastLogId/commitLogId”，该修复会改变该行为。
- 目前源码中的 `leaseValid()` 依赖 `commitInThisTerm_` 和 `lastMsgAcceptedTime_`；`lastMsgAcceptedTime_` 在 heartbeat response 成功时也会更新，因此稳定 leader 的 lease 应不受影响。

## 当前结论

短期根因位置已经定位到 `RaftPart::sendHeartbeat()` 每轮 heartbeat 追加空 log，再经 `FileBasedWal::appendLogInternal()` 写入 `AtomicLogBuffer`。

中期监控指标已补充，可用于验证空 log 写入是否从“每轮 heartbeat”下降到“每个 leader 任期必要时一次”。

长期修复采用最小行为变更：保留当前任期 ready 所需空 log，去掉稳定任期内的重复空 WAL 写入。

## 本机验证记录

验证时间：2026-07-04 16:22 CST 左右。

### 编译验证

执行命令：

```bash
cd build
make -j10 2>&1 | tee ../task-wal/build_20260704_raft_wal_fix.log
```

结果：

- 编译通过。
- `nebula-storaged` 链接通过。
- `nebula-graphd` 链接通过。
- `nebula-metad` 链接通过。
- 日志文件：`task-wal/build_20260704_raft_wal_fix.log`

### 安装验证

执行命令：

```bash
cd build
make install 2>&1 | tee ../task-wal/install_20260704_raft_wal_fix.log
```

结果：

- 三个核心二进制已安装：
  - `/usr/local/nebula/bin/nebula-graphd`
  - `/usr/local/nebula/bin/nebula-storaged`
  - `/usr/local/nebula/bin/nebula-metad`
- 安装过程后段在设置 `/usr/local/nebula/etc/nebula-metad.conf.default` 权限时报错：
  - `Operation not permitted`
- 因此 `make install` 最终退出码非 0，但本次验证需要的新二进制已经安装完成。
- 日志文件：`task-wal/install_20260704_raft_wal_fix.log`

### 启动验证

启动前执行：

```bash
ulimit -n 65536
```

启动命令：

```bash
/usr/local/nebula/scripts/nebula.service start metad
/usr/local/nebula/scripts/nebula.service start storaged
/usr/local/nebula/scripts/nebula.service start graphd
```

结果：

- `nebula-metad` 运行中，监听 `9559`。
- `nebula-storaged` 运行中，监听 `9779`。
- `nebula-graphd` 运行中，监听 `9669`。
- storage 提示：`storaged after v3.0.0 will not start service until it is added to cluster`。这是 Nebula 3.x 未执行 `ADD HOSTS` 前的正常提示。
- 日志文件：`task-wal/runtime_start_20260704_raft_wal_fix.log`

### 指标验证

查询命令：

```bash
curl --noproxy '*' -s 'http://127.0.0.1:19779/stats?stats=num_raft_heartbeat.sum.60,num_raft_heartbeat_empty_log.sum.60,num_raft_heartbeat_without_empty_log.sum.60,num_raft_heartbeat.rate.60,num_raft_heartbeat_empty_log.rate.60,num_raft_heartbeat_without_empty_log.rate.60'
```

结果：

```text
num_raft_heartbeat.sum.60=487
num_raft_heartbeat_empty_log.sum.60=0
num_raft_heartbeat_without_empty_log.sum.60=487
num_raft_heartbeat.rate.60=8
num_raft_heartbeat_empty_log.rate.60=0
num_raft_heartbeat_without_empty_log.rate.60=9
```

8 秒后再次查询：

```text
num_raft_heartbeat.sum.60=441
num_raft_heartbeat_empty_log.sum.60=0
num_raft_heartbeat_without_empty_log.sum.60=441
```

说明：

- 新增指标已成功注册并可通过 storage HTTP stats 读取。
- 当前观测窗口内 heartbeat 未再追加空 WAL log。
- `num_raft_heartbeat_empty_log=0` 且 `num_raft_heartbeat_without_empty_log` 与 heartbeat 总量一致，符合修复预期。
- stats 是滑动时间窗口，第二次查询 sum 下降属于窗口滑动造成，不代表计数回退。
