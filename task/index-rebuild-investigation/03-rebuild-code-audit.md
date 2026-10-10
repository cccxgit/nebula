# 索引重建回放路径审查、源码级复现与耗时下界

分析基线：`de9b3ed800a6627d9845e9289b6bbc5b6faf460a`，`nebula-index` 分支。本文件讨论这份代码中的确定行为；生产环境没有提供存储日志、运行参数、堆栈、每分区数据及并发写入量，因此不能把一个已证实的代码缺陷直接等同于已证实的生产事故归因。

本文件中的 `RebuildIndexTask.cpp` 行号均来自基线提交，原文件保存在 [audit/baseline/RebuildIndexTask.cpp](audit/baseline/RebuildIndexTask.cpp)。当前工作区该文件增加了诊断日志，行号已经偏移。其他引用可用 `git show de9b3ed800a6627d9845e9289b6bbc5b6faf460a:文件路径 | nl -ba` 精确核对。没有应用修复补丁。

## 1. 已确认的结论与证据边界

| 结论 | 证据强度 | 与两天未结束的关系 |
|---|---|---|
| operation log 回放在第一次超过字节阈值后，每处理一条记录都重新编码并提交本轮已累计的全部记录 | 原源码逐行证明；自动抽取原方法执行，验证提交次数与累计字节 | 单轮回放提交量从线性增长为平方增长，是最强根因候选 |
| 累计 BatchHolder 字节数超过 `rebuild_index_part_rate_limit` 后，每次调用限速器固定睡 1 秒 | 原始 `RateLimiter` 类直接运行，实测约 1000 ms/次 | 一个分区单轮积压 20 万条点索引 modify op，在编译缺省参数下，仅这段睡眠就超过 2 天 |
| `LOCKED` 后等待的是进程级全局 `onFlyingRequest_`，与当前 space/part 无关 | 原始 `IndexCountWrapper` 和完整回放函数执行；空 operation 集合仍可被一个无关计数挡住 | 持续其他分区写入或一个长期在途请求可导致另一种长时间不结束 |
| 回放内层及全局计数等待循环不检查 `canceled_` | 原方法中途设 cancel，仍把当前快照全部回放完；等待计数的线程也不立即退出 | STOP 请求可能已接受，但执行线程继续很久；这会放大“任务卡住”的感受 |
| 停止外部写入后有限 operation 集合仍能被回放完 | 无后续写入的源码级实验最终均 `SUCCEEDED`，索引数量正确 | 缺陷不是对任意有限数据都必现的逻辑死循环，而是极慢回放；持续新增日志另有收敛问题 |

需先从生产日志识别阶段。若最后日志为 `Start building index`，还不能套用 operation 回放结论；若已出现 `Building index successful` 和 `Processing operation logs`，才进入本文的主要问题路径。`RebuildIndexTask Finished` 只表示日志中那一个 partition 结束；应先查同 host 其他分区、其他 host task 是否尚未完成。只有所有目标 host task 都出现对应的 `task(J,T) finished`，而 meta 仍 RUNNING 时，才优先排查完成结果上报。

## 2. 重建状态机与点、边共用路径

`src/storage/admin/RebuildIndexTask.cpp:44–90` 从 meta 缓存取 index items，为每个分区生成子任务。互斥检查按 space 进行，状态记录使用 `(space, part)`，不是按 index ID。多个索引一起重建会一起参与同一分区扫描，点、边均继承这个类。

`invoke()` 的执行顺序（基线 93–131 行）：

1. 创建该分区的限速器。
2. 删除之前遗留的 operation logs。
3. 把分区设为 `BUILDING`，注册退出作用域时切为 `FINISHED` 的动作。
4. 调用点或边实现的 `buildIndexGlobal()` 扫描原始数据并写正式索引。
5. 调用共用的 `buildIndexOnOperations()`，回放扫描期间生成的 operation logs。
6. 回放一轮的累计字节不超过阈值时，把分区转为 `LOCKED`，等待在途请求归零。
7. 再扫描一次 operation logs，补齐上锁前进入的写入，满足条件后退出。

状态 `FINISHED` 表示写入路径恢复正常，并不单独表达作业成功。作用域退出时即使返回错误也会设 `FINISHED`，实际成功与否仍看任务返回值和 meta Job 状态。

点索引全量阶段见 `src/storage/admin/RebuildTagIndexTask.cpp:26–151`：按分区 tag prefix 扫描；只处理目标 tag IDs；解析行并按最新 schema 取索引字段；过滤 TTL 已过期数据；为每个匹配 index item 生成正式索引 KV。边索引全量阶段见 `src/storage/admin/RebuildEdgeIndexTask.cpp:26–165`：逻辑相同，额外在 85–89 行跳过 `edgeType < 0` 的反向边，只为出边建立索引。

这两个全量阶段在 72–80 行提交后都 `data.clear()` 且 `batchSize = 0`。因此本文确认的累计重发缺陷具体发生在共用 operation 回放，不能泛称“所有索引批写都没清空”。

### 2.1 在线写入如何产生 operation logs

点插入/覆盖见 `src/storage/mutate/AddVerticesProcessor.cpp:227–331`：按旧属性算旧索引、按新属性算新索引，再写数据行。在 `BUILDING` 状态不直接变更正式索引，而是写 operation KV；在 `LOCKED` 状态拒绝对应索引写入；其他状态正常维护索引。边插入对应 `src/storage/mutate/AddEdgesProcessor.cpp:226–341`，且 243–244 行限制只维护正向边索引。删除和 update 路径有同类状态检查。

`src/common/utils/OperationKeyUtils.cpp:13–37` 给出实际格式：

- modify operation key：分区及类型 4 B + 大端微秒时间戳 8 B + 操作类型 4 B + 正式索引 key；value 为正式索引 value，通常为空，TTL 时另有内容。
- delete operation key：前述 16 B 头；value 为待删除的正式索引 key。
- 操作 prefix 只有分区及类型 4 B（63–67 行），因此回放不是按当前 index ID 过滤，而是按分区收集 operation logs。

不能将 operation 条数等同于业务请求数或点数：一次更新可能产生旧索引删除和新索引写入；一个 schema 的多个索引及 geography 多 cell 索引也会增加操作数。本文的数量计算明确以回放迭代器里的 operation KV 条数为单位。

## 3. 逐行证明：为什么超过阈值后退化

核心代码位置见 [基线回放函数](audit/baseline/RebuildIndexTask.cpp#L134) 和 [基线 writeOperation](audit/baseline/RebuildIndexTask.cpp#L258)。

| 基线代码位置 | 实际动作 | 推论 |
|---|---|---|
| 147–149 | 每轮 outer loop 创建 operation prefix 迭代器 | 本轮读到一个已有视图，下一轮才读后续积压 |
| 155–156 | 只在 outer loop 开头新建并 reserve 一个 BatchHolder | 同一迭代器遍历期间持有同一个累积对象 |
| 161–173 | 每条 operation 至少追加正式索引 put/remove，再追加删除 operation key | 一个逻辑 operation 通常对应 2 个底层 batch op |
| 174–179 | `size() > batch_size` 就调用 writeOperation | 判定是字节数，比较是严格 `>` |
| 258–276 | 编码 getBatch，按 size 限速，异步 append，等待回调 | 完成后没有清空、没有换新 holder、没有移动 batch |
| 181 | 前移到下一条 operation | holder 继续增长，后续每条都再次越阈值 |
| 184 | 遍历结束无条件再提交同一 holder | 最后整个累计 batch 又重复一次 |
| 193 | 用本轮仍累积的 size 决定是否上锁 | 这里记录的是本轮总工作量，不是数据库中剩余 operation 字节 |

`src/kvstore/LogEncoder.h:159–174` 明确 `put/remove` 累加 `size_`；203–205 行 `getBatch()` 返回 **const 引用**；210–216 行 `size()` 返回未减过的累加值。`reserve(4096)` 只是 vector 容量预留，不是上限，也不会在达到 4096 时冲刷。`src/kvstore/LogEncoder.cpp:163–193` 的编码函数读取 const batch，把其中所有 entry 序列化到一个新的字符串，既不清空原 batch，也不去重。

因此 `std::move(encoded)` 仅移动本次新生成的编码字符串，不能使 BatchHolder 清空。异步存储删除了已处理的 operation key，也不会删掉 C++ vector 中早已保存的历史 entries。

`src/kvstore/RocksEngine.cpp:219–234` / `237–255` 创建标准 RocksDB iterator，没有配置 tailing；默认读视图不因后续删除而缩短。独立源码实验用快照 vector 表示该语义；真实存储实验由总报告另列。回放当前快照期间，即使最早的 operation key 已从 live RocksDB 删除，仍会反复把它对应的历史 put/remove 编进后续 batch。

### 3.1 实際阈值有两组，不能混用

| 参数来源 | `rebuild_index_batch_size` | `rebuild_index_part_rate_limit` |
|---|---:|---:|
| `src/storage/StorageFlags.cpp:25–29` 编译缺省 | 131,072 B（128 KiB） | 524,288 B/s（512 KiB/s） |
| `conf/nebula-storaged.conf.default:125–127` | 1,048,576 B（1 MiB） | 4,194,304 B/s（4 MiB/s） |
| `conf/nebula-storaged.conf.production:126–128` | 1 MiB | 4 MiB/s |

生产可能改过配置或动态 gflags，因此只看到源码缺省不能推断实例当前生效值。下文同时给两组计算。

### 3.2 限速器使“每条再处理一次”变成“每条至少一秒”

`src/kvstore/RateLimiter.h:40–49`：当 `toConsume > burstSize` 时，直接 `sleep_for(seconds(1))`；否则走 Folly token bucket 的借额等待。`writeOperation()` 传入的 `toConsume` 是累计 holder payload，`rate` 和 `burstSize` 都是 `rebuild_index_part_rate_limit`。

这不是在这一分支里按照 `大 batch 字节 / 速率` 精确睡眠；只要超过 burst，就固定睡 1 秒。超过阈值后的 holder 越来越大，故每个新 operation 都会重新产生一次至少一秒睡眠。单个分区串行执行、每次还 `baton.wait()` 等回调；增加其他分区并行数不能将该分区的这段循环并行化。

## 4. 数学推导：怎样从几十万条积压到两天

设一次 operation 迭代器快照有 N 条 modify op，正式索引 key 长度 K，value 为空，所有记录尺寸相同：

```text
d = 2K + 16                       每个 operation 对 BatchHolder.size() 的增加
B = rebuild_index_batch_size
R = rebuild_index_part_rate_limit = burst
q = floor(B / d) + 1              第一个在内层触发提交的记录序号
u = max(q, floor(R / d) + 1)      第一个内层提交且触发固定 1 秒睡眠的序号

当 N >= q 时：
  本轮 append 次数 = N - q + 2
  被提交的逻辑记录副本总数 C = sum(j, j=q..N) + N
                           = [N(N+1)-(q-1)q]/2 + N
  底层 batch 操作数 = 2C
  payload 总字节 = d*C
  encodeBatchValue 总字节 = (d+18)*C + 13*(N-q+2)
  固定睡眠秒数下界 = max(0,N-u+1) + [N*d>R]
```

编码的 `18` 来自每个逻辑 operation 有 2 个底层 entry，每个 entry 的 type 1 B、key size 4 B、value size 4 B；每次 batch 头另有时间戳 8 B、type 1 B、entry count 4 B，共 13 B。公式描述这一轮有数据的快照；进入 LOCKED 的空扫描最多在这个静态场景多提交两份 13 B 空日志。

对于 VID 长度 8 B、非 nullable 单 int 索引、无 TTL：

- 点正式索引 key = 分区/类型 4 + index ID 4 + int 8 + VID 8 = **24 B**，modify operation 的回放 payload 为 **64 B**。
- 边正式索引 key = 分区/类型 4 + index ID 4 + int 8 + src 8 + rank 8 + dst 8 = **40 B**，payload 为 **96 B**。

长度依据 `src/common/utils/IndexKeyUtils.cpp:87–132` 与 `src/common/utils/IndexKeyUtils.h:165–171`。nullable 字段会追加 2 B 位图，TTL 会增加 value；更宽的字段、多个索引会更早达到阈值。四列 `string(256)`、VID 8、非 nullable 点索引 K = 4+4+4×256+8 = 1040 B，d = 2096 B。

| 生效参数 | 单分区本轮 operation 数 | 索引类型 | 仅固定 1 秒分支的累计下界 | 本轮累计编码量 |
|---|---:|---|---:|---:|
| 128 KiB / 512 KiB | 200,000 | 点，K=24 | 191,809 秒 = **2.2200 天** | 1,639,855,122,957 B，约 1.64 TB |
| 128 KiB / 512 KiB | 200,000 | 边，K=40 | 194,540 秒 = **2.2516 天** | 2,279,930,500,638 B，约 2.28 TB |
| 1 MiB / 4 MiB | 250,000 | 点，K=24 | 184,465 秒 = **2.1350 天** | 2,551,527,261,581 B，约 2.55 TB |
| 1 MiB / 4 MiB | 250,000 | 边，K=40 | 206,311 秒 = **2.3879 天** | 3,555,745,700,685 B，约 3.56 TB |

这些是**解析外推，不是已经运行两天的基准测试**；不包括全量扫描、burst 以下的 token 等待、编码 CPU、Raft/WAL、RocksDB、后台 compaction、多轮新增日志和元数据上报。表格不代表生产必然有该规模的积压，也不能把所有分区的 operation 数相加代入单分区串行公式。计算脚本 [audit/calculate_amplification.py](audit/calculate_amplification.py) 已生成 [CSV](audit/amplification.csv) 与 [计算日志](audit/amplification.log)，并用小规模源码执行结果校验了编码量公式。

### 4.1 长期写入为何可能使它更难收敛

回放期间状态仍为 `BUILDING`，新的业务写入继续生成日志。本轮越阈值后，处理速度会被压低到约每秒推进一条 operation（另加存储耗时）。下一轮会读到期间的新日志，必须等某一轮处理总量 ≤ B 才会转 `LOCKED`。如果进入 operation 日志的速率长期大于回放净推进速度，积压很难降到这个条件；这与批次重复提交形成反馈。

若停止全部相关写入、数据有限、回调都成功返回且没有错误或取消退出，并且全局 onFlyingRequest_ 最终能被采样为 0，原算法会完成当前长轮，下一轮为空，继而上锁并结束。因此“停写后仍过很久才结束”不否定此缺陷：已经拿到的大快照仍必须继续逐条走完。另需注意内层没有 cancel 检查。

## 5. 独立源码级复现实验

### 5.1 保留的原实现和替身范围

[audit/run_source_harness.py](audit/run_source_harness.py) 自动抽取下列代码片段并**原样**编译，片段哈希见 [source_manifest.json](audit/source_manifest.json)。原始实验通过 `git show HEAD:...` 抽取，当时 HEAD 即分析基线；发布脚本已将 BASE 固定为 `de9b3ed800a6627d9845e9289b6bbc5b6faf460a`，并验证所得片段哈希完全相同，避免分支后续变动影响复现：

- 完整 `RebuildIndexTask::buildIndexOnOperations()`；
- 完整 `RebuildIndexTask::writeOperation()`；
- `BatchHolder` 类、batch 枚举、真实 `encodeBatchValue()`；
- 完整 `IndexCountWrapper` 类；
- 单独限速程序中的完整 `RateLimiter` 类。

回放程序使用可审阅的内存 KVStore：prefix 给快照副本、真实编码器生成字节、替身解码并执行 puts/removes、异步回调立即成功；限速替身只记录消耗字节，不等待，以便短时间观测数 GB 累计编码。Baton 是已回调即通过的替身；操作 key 使用 16 B 合成头及同长度索引 key，不用于证明真实 key 编码正确性。时间戳用系统时钟替代 TSC，仅影响编码 timestamp。该实验不启动 Nebula、不涉及真实网络/Raft/磁盘，不代表端到端吞吐量。

另一个限速程序使用本机 Folly 头和库，原封不动执行 `RateLimiter`；只为 `WallClock` 提供系统时间实现，两个被测调用均走不依赖时间值的 `> burst` 分支。因此“每调用一次睡 1 秒”有独立原代码实测，不是由 mock 推断。

环境：本机 `g++`，C++17，`-O2 -pthread`；依赖目录 `/opt/vesoft/third-party/3.3`。不是重编数据库，不使用 `make`。如需重编数据库，遵循用户最新约束 `make -j5`，启动前 `ulimit -n 65536`。

### 5.2 复现命令及完整输出位置

在工程根目录运行：

```bash
python3 task/index-rebuild-investigation/audit/run_source_harness.py > task/index-rebuild-investigation/audit/driver.log 2>&1
python3 task/index-rebuild-investigation/audit/calculate_amplification.py > task/index-rebuild-investigation/audit/calculate_driver.log 2>&1
```

编译和每组运行的全部输出在 [audit/source_harness.log](audit/source_harness.log)。生成的可审查 C++ 为 [source_harness.cpp](audit/source_harness.cpp) 和 [rate_harness.cpp](audit/rate_harness.cpp)，`original_*.inc` 是抽取的基线片段。日志中的 ErrorCode 是替身枚举映射：0 表示 SUCCEEDED，1 表示 E_USER_CANCEL，不是 Nebula RPC 的实际数值。

### 5.3 已运行结果

下面均无并发新增 operation，输入为 modify records，正式索引 value 为空。累计编码量包含结束时空扫描产生的日志。

| N | K | B / R | append 次数 | 实际序列化底层 entry 数 | 累计编码字节 | `size > R` 调用次数 |
|---:|---:|---|---:|---:|---:|---:|
| 2,000 | 24 | 128 KiB / 512 KiB | 2 | 4,000 | 164,026 | 0 |
| 2,200 | 24 | 128 KiB / 512 KiB | 155 | 650,248 | 26,662,183 | 0 |
| 8,250 | 24 | 128 KiB / 512 KiB | 6,205 | 63,890,898 | 2,619,607,483 | 59 |
| 5,500 | 40 | 128 KiB / 512 KiB | 4,138 | 28,401,910 | 1,618,962,664 | 40 |
| 500 | 1,040 | 1 MiB / 4 MiB | 2 | 1,000 | 1,057,026 | 0 |
| 510 | 1,040 | 1 MiB / 4 MiB | 13 | 11,130 | 11,764,579 | 0 |
| 2,030 | 1,040 | 1 MiB / 4 MiB | 1,533 | 3,876,490 | 4,097,469,859 | 30 |

每组断言确认：operation records 全部删除、最终正式索引条数恰好等于 N、返回成功。累计编码量是**每次调用新生成的编码字符串长度之和**，不是最终 DB 文件大小；旧索引 key 覆盖、重复删除并不会让最终索引条数跟着这个数字增长。

2,200 条点索引组，最初几个提交 `(holder payload, entry count, encoded bytes)` 为：

```text
(131136, 4098, 168031)
(131200, 4100, 168113)
(131264, 4102, 168195)
(131328, 4104, 168277)
(131392, 4106, 168359)
```

这直接展示每次 holder 只新增 64 B / 2 个 entry，却把 4000 多个历史 entry 全量重发。最后一轮完整 batch 是 `(140800, 4400, 180413)`，且在内层最后一条提交后又被尾部提交一次。8,250 条组仅最终索引 payload 198,000 B，却累计生成约 2.62 GB batch 编码；这不是估算，是实际执行原编码器计数。

原始 RateLimiter 两次调用实测：

```text
toConsume=524289,  rate=burst=524288   -> 1000.09 ms
toConsume=4194305, rate=burst=4194304  -> 1000.06 ms
```

## 6. 另一个等待点：全局 onFlyingRequest_

`src/storage/CommonUtils.h:70–76` 把 `onFlyingRequest_` 放在整个 `StorageEnv`，并无 space/part 分组。`IndexCountWrapper` 在 105–107 行构造加一，125–127 行析构减一；传入参数只有 env，不含当前分区。

调用方包括：

- `AddVerticesProcessor.cpp:233`、`AddEdgesProcessor.cpp:230`：进入带索引原子操作回调时计数；其生命周期主要覆盖构建 atomic batch，并不覆盖这些 add 请求的全部 RPC 时间。
- `DeleteEdgesProcessor.cpp:100,147–155`：wrapper move 到异步写入回调，生命周期包含回调完成前的阶段；删除 tag/vertex 同类。
- `src/storage/exec/UpdateNode.h:229,276–284`：UpdateTag 包括等待 `asyncAppendBatch` 的 Baton；UpdateEdge 551 行及对应写入逻辑同类。

因此不能笼统声称这个数等于“所有在途 RPC 总数”，也不能把任何读请求都算进去。它只统计使用该 wrapper 的作用域，但作用域可以来自该进程内任何 space/part。

当前分区转为 LOCKED 之后，只拒绝当前分区相应写入，不能阻止别的分区继续贡献全局计数。原等待代码在基线 204–209 行不断读计数并 `usleep(100)`，没有超时、没有 cancel 检查，也没有按分区过滤。若计数持续非零，可能一直等；即使两次采样之间极短暂出现 0，也可能未被读到。这个路径不需要大 operation backlog。

已运行实验直接调用完整原回放方法：输入 operation 数为 0；另创建一个使用同一个 env 的原始 `IndexCountWrapper` 表示无关在途工作。结果在日志中：

```text
wait cancel=0 locked=1 pending_operation_records=0 count_unrelated_request=1 completed_after_350ms=0
wait_after_release count=0 completed=1 code=0

wait cancel=1 locked=1 pending_operation_records=0 count_unrelated_request=1 completed_after_350ms=0
wait_after_release count=0 completed=1 code=1
```

350 ms 只是人为设定观测窗口，没有宣称真实工作负载已经维持两天。实验证明“无关全局计数足以阻止空队列重建完成”和“cancel 不会打断该等待”的逻辑事实；生产是否在此处等待，需要堆栈与计数日志确认。

## 7. STOP 延迟与容易误诊的现象

基线回放只在函数入口 136 行、outer loop 142 行检查 cancel，内层 157–182 行及等待 204–209 行没有检查。源码实验在第一个 append 回调后设 cancel：2,200 条组仍发生 153 次带数据 append，累计 26,662,157 B，所有 operation 都处理完后才返回 E_USER_CANCEL。与未取消组的有数据写入量完全相同，差别只是省去后续空扫描。

因此取消响应不代表这条执行线程立即释放；若它已进入包含几十万条的慢回放轮，取消也可能等本轮结束。此事实亦影响服务 shutdown 等待线程池退出的体验，但没有生产 shutdown 日志时不应把它直接归因成另一个事故。

以下现象与本缺陷相容，但不能单独作证：

- CPU 并不持续满载、每秒缓慢增加写流量：可能在固定睡眠。
- 当前 live operation 数看似已经很小，仍不结束：可能仍在遍历旧视图、重写已经放进 holder 的历史 entry。
- 停写后仍长时间 RUNNING：本轮大快照尚未完成。
- 其他分区已完成，只剩少数分区 RUNNING：热点分区积压决定单任务尾延迟。
- 最终索引数量没有指数增长：相同 key 覆盖和重复删除可保持数量稳定，但写入工作量仍很大。

## 8. 如何用生产证据确认，而不改业务逻辑

总报告中的 `INDEX_DIAG` 观测补丁和真实实验用于形成端到端证据。本子报告建议关联同一 host/space/part/task 的下列内容：

1. 已进入 `Processing operation logs`，而不是仍在全量扫描或已经结束等待 meta 上报。
2. 生效 B/R、schema 索引宽度、VID 长度、TTL/nullable、目标索引个数。
3. 本次补丁的回放轮结束字段是 `operations`、`retained_entries`、`retained_bytes`，提交字段是 `seq`、`entries`、`batch_bytes`、`encoded_bytes`；连续 seq 的提交若每次 entries 只增 2、但 batch 一直累积增长，直接匹配累计重发。append 日志对快提交做采样，不能把日志行数当作全阶段提交次数；seq 是该进程跨 task 的全局计数，需结合隔离实验阶段或 host/space/part 关联。
4. 写入耗时拆为限速等待与 async append 等待；若 holder > R 时稳定增加约 1 秒限速，命中第二环节。
5. 外部写入停止后观察当前轮是否继续推进、下一轮 backlog 是否下降。只以“停写后几秒没好”否定积压机制不充分。
6. 若 holder 已小且 LOCKED，记录 `onFlyingRequest_`，并采集相关写入线程栈与 task 线程栈。task 若停在 `buildIndexOnOperations` 的 `usleep`，且全局计数长期非零，则定位到另一路径。
7. 若 task 停在 `baton.wait()`，应区分写操作回调/Raft延迟和上面的两条根因；本审查不能仅凭一个 Baton 栈就推断是网络或死锁。

附带审查发现：`removeLegacyLogs()` 基线 221–230 行收集异步错误，但 234 行无条件返回 SUCCEEDED。这是另一个错误处理问题，本文没有复现其导致两天未结束，不纳入主要归因。`BUILDING` 状态切换前缺分区锁的 TODO（106–108 行）属于一致性风险，也不是上述持续耗时证据。

## 9. 本文件产物清单

- [完整基线 RebuildIndexTask](audit/baseline/RebuildIndexTask.cpp)：避免观测日志造成行号歧义。
- [自动抽取与执行脚本](audit/run_source_harness.py)：保留完整原方法并定义替身边界。
- [源码片段 SHA-256 清单](audit/source_manifest.json)：对应分析提交。
- [完整编译/执行日志](audit/source_harness.log)：包括成功断言、批次统计、限速实测及全局等待实验。
- [耗时与编码量计算脚本](audit/calculate_amplification.py)、[CSV](audit/amplification.csv)、[计算日志](audit/amplification.log)：区分数学外推与已运行实验。

本文件已证明当前代码能以可解释的规模产生超过两天的执行耗时，并给出两个可区分的长时间等待机制；确认生产现场具体命中哪一条，仍需以上阶段、计数及参数证据。
