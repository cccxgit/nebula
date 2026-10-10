# 索引重建作业生命周期、状态解释与现场判别

分析基线：`de9b3ed800a6627d9845e9289b6bbc5b6faf460a`（`nebula-index`）。本文全部源码行号均指 **`git show de9b3ed80:路径` 的原始版本**，不包含本次在 `RebuildIndexTask.cpp` 新增的观测日志。分析时间：2026-10-10。本文负责调度/状态机制的源码审计；没有自行启动、停止服务或实施故障注入。运行复现及原始日志见本目录其他报告，不能把本文列出的待验证场景当作已执行实验。

## 1. 与“重建两天未结束”直接相关的结论

1. **`RUNNING` 不是实时线程状态，也不是进度百分比。** `SHOW JOB` 从 meta KV 读取持久化 job/task 记录，不向 storaged 查询当前执行位置。因此既可能是实际工作仍在进行，也可能是完成结果没有成功到达 meta。
2. **本分支没有针对运行中索引重建的总执行时限。** `job_expired_secs` 默认一周，但 `JobManager::isExpiredJob()` 明确保留 `QUEUE` 与 `RUNNING`。等待两天不会自动转成失败，七天也不会因为这个参数终止运行中任务。
3. **一个分区拖住就能使整个作业长期 `RUNNING`。** 每个目标 storaged 对应一个 task；task 内每个 partition 对应一个 subtask。只有全部本机 subtask 返回才上报 task；只有全部目标 task 不再 `RUNNING`，meta 才结束 job。回放阶段的累计批次反复提交缺陷，因而会透过这条链路表现为作业长期未结束。
4. **`STOP JOB` 返回成功不等于所有索引重建线程已经退出。** 正常路径仅设置取消标志；回放操作日志的内层扫描不检查该标志。若该轮因累计批次缺陷耗时很久，返回成功之后依然可能持续回放、限速等待和写入。
5. **现场不能仅凭 `SHOW TAG/EDGE INDEX STATUS` 归因。** 必须关联 jobId/taskId、具体 host、`space/part` 阶段日志与上报结果。当前没有生产日志，本文不能认定生产实例一定命中累计批次缺陷，也不能认定它只是状态上报故障。

主要源码证据：

| 结论 | 源码（基线行号） |
|---|---|
| SHOW JOB 只读持久化记录 | `src/meta/processors/job/JobManager.cpp:768-794` |
| RUNNING / QUEUE 不过期 | `src/meta/processors/job/JobManager.cpp:689-702` |
| 每个 host 一条 task | `src/meta/processors/job/StorageJobExecutor.cpp:163-176` |
| 每个 part 一条 subtask | `src/storage/admin/RebuildIndexTask.cpp:84-89` |
| 全部本机 subtask 返回后 finish | `src/storage/admin/AdminTaskManager.cpp:291-324` |
| 全部 task 结束后聚合 job 状态 | `src/meta/processors/job/JobManager.cpp:510-529` |
| 回放内层不检查取消标志 | `src/storage/admin/RebuildIndexTask.cpp:134-214` |

## 2. graph → meta → storage 的完整执行链

### 2.1 graph 提交的是异步作业

`AdminJobValidator::validateImpl()` 在提交 TAG / EDGE 索引重建时检查当前空间索引、用户指定索引名是否存在（`src/graph/validator/AdminJobValidator.cpp:23-56`）。空参数表示重建该类型的全部索引；若本空间该类型索引列表也是空，则直接报语义错误。

规划阶段生成 `SubmitJob`（同文件 `63-68`）。`SubmitJobExecutor::execute()` 调用 `MetaClient::submitJob(spaceId, op, type, params)`（`src/graph/executor/admin/SubmitJobExecutor.cpp:15-38`）。成功的 `REBUILD` 只输出 `New Job Id`（`44-51`），说明作业已受理，不能解释为重建完成。

### 2.2 meta 持久化、串行调度同空间作业

`AdminJobProcessor::addJobProcess()` 先检查同空间、同类型、同参数的在途作业；如果发现重复请求，直接返回已有 jobId（`src/meta/processors/job/AdminJobProcessor.cpp:121-153`）。因此重复执行相同 REBUILD 可能仍得到原来的 ID，并没有启动独立的新重建。

调度线程把作业改为 `RUNNING` 并持久化，然后置 `spaceRunningJobs_[spaceId] = true`（`src/meta/processors/job/JobManager.cpp:149-195`）。`tryDequeue()` 对已有运行作业的空间跳过队列（`594-612`）。一个未结束重建可以让同一空间后续作业长时间 `QUEUE`；不同空间可以并行，但共享所在 storaged 的任务线程池。

重建执行器将索引名解析为 indexId（`src/meta/processors/job/RebuildJobExecutor.cpp:19-42`），且明确设置目标为 **partition leader 所在 host**（`src/meta/processors/job/RebuildJobExecutor.h:18-25`）。`getLeaderHost()` 从 meta 的 leader 记录按 host 聚合 partitions（`src/meta/processors/job/StorageJobExecutor.cpp:56-90`）；若一个 host 都没有，返回 `E_JOB_HAS_NO_TARGET_STORAGE`，本分支已经避免“零目标 task 永久 RUNNING”的那条已知路径。

执行器先为每个目标 host 写一条 task 记录，再逐 host 发送 `addAdminTask`（同文件 `163-198`）。task 的初始状态就是 `RUNNING`，起始时间是生成记录的时间（`src/meta/processors/job/TaskDescription.cpp:23-31`），因此该时间并非本机某个 partition 真正开始扫描的时间。

### 2.3 storaged 接收成功与真正执行完成分离

`AdminClient::addTask()` 构造带有 `jobId/taskId/spaceId/parts/indexIds` 的请求，发送到 storage 的 admin 端口（`src/meta/processors/admin/AdminClient.cpp:761-792`）。

`AdminTaskProcessor::process()` 创建 `RebuildTagIndexTask` 或 `RebuildEdgeIndexTask`，持久化初始任务状态并入队，随即返回 `SUCCEEDED`（`src/storage/admin/AdminTaskProcessor.cpp:16-46`）。初始记录的组合是：

- `statsItem.status = RUNNING`；
- `rc = E_TASK_EXECUTION_FAILED`；
- key 含 storage 启动序号、spaceId、jobId、taskId。

初始 rc 是为进程中断后识别“未真正完成”保留的失败结果，不是说正常入队已经失败。

`AdminTaskManager` 默认最多 10 个并发 subtask（`src/storage/admin/AdminTaskManager.cpp:16-24`）。为 task 生成全部分区 subtask 后，将 `unFinishedSubTask_` 设为分区数并向共享线程池提交工作（`252-285`）。执行完成一个分区才减一，减到零才调用 `task->finish()`（`291-324`）。假设 50 个分区而线程池只有 10 个工作线程，`SHOW JOB` 的一条 host task 仍仅显示一个整体状态。

`RebuildIndexTask::genSubTasks()` 在当前主机的该空间中检查是否已有非 `FINISHED` 重建 guard（`src/storage/admin/RebuildIndexTask.cpp:75-81`）。尚未退出的旧 task 会妨碍新重建；在 `STOP` 后立即再次启动，可能看到 `This space ... is building index on part ...` 和 `E_REBUILD_INDEX_FAILED`。

### 2.4 为什么两个阶段之间不更新 SHOW JOB

每个分区先执行全量扫描 `buildIndexGlobal()`，再执行操作日志回放 `buildIndexOnOperations()`（`src/storage/admin/RebuildIndexTask.cpp:109-130`）。阶段日志为：

```text
Start building index
Building index successful, space=..., part=...
Processing operation logs, space=..., part=...
RebuildIndexTask Finished, space=..., part=...
```

这些是 storaged 本地日志；它们没有对应的 meta 进度更新 API。出现 `Building index successful` **只表示该分区全量阶段成功**，仍必须完成增量回放和最后的收敛。它不是 task/job 完成证明。

此外 `Start building index` 原始日志没有 space/part；同机多分区并发时必须使用线程 ID、后续带分区日志或此次增加的观测字段关联，不能把不相干的两行日志当作同一分区时间差。

## 3. task 完成如何变成 SHOW JOB 的 FINISHED

`AdminTask::finish()` 输出 `task(job, task) finished, rc=[...]`，设置 `running_ = false`，调用完成回调（`src/storage/admin/AdminTask.h:128-135`）。重建 override 会清理空间内的重建 guard，然后调用基类（`src/storage/admin/RebuildIndexTask.cpp:20-30`）。

回调调用 `saveAndNotify()`，先将结果写本地 `adminStore_`，再通知专门的上报线程（`src/storage/admin/AdminTaskProcessor.cpp:19-25`；`src/storage/admin/AdminTaskManager.cpp:336-345`）。上报线程扫描本地记录，对非当前启动周期的记录或当前周期已经结束的记录调用 `MetaClient::reportTaskFinish()`（`44-105`）。

正常成功路径应看到下面三类证据依次出现：

```text
# storaged：本机 task 真正返回
task(J, T) finished, rc=[SUCCEEDED]

# storaged：发起上报及 meta 成功确认
reportTaskFinish(), job=J, task=T, task_rc=SUCCEEDED
reportTaskFinish(), job=J, task=T, report_rc=SUCCEEDED

# metad：聚合后结束 job
Trying to end job, spaceId=S, jobId=J, target phase status=FINISHED
```

**只看到第一条 `reportTaskFinish(... task_rc=...)` 不能认为已经发出 RPC。** 该日志位于启动周期/状态筛选之前（`AdminTaskManager.cpp:92-104`），扫描到当前周期 `RUNNING` 的记录时也可能先打印，随后并不发送 RPC。应以 `report_rc=SUCCEEDED` 或对应失败日志判断结果。

上报 RPC 失败或返回非成功，记录保留且标记需重试（`AdminTaskManager.cpp:107-144`）。关键异常日志有：

```text
reportTaskFinish() got rpc error:
reportTaskFinish() has bad status:
reportTaskFinish(), job=..., task=..., report_rc=...
```

在正常保留 adminStore 的条件下，这一机制具有重试能力，不能把“一次上报 RPC 超时”直接推断为永久丢失结果。

meta 收到结果后，`saveTaskStatus()` 根据 error code 把 task 转为 `FINISHED` 或 `FAILED` 并持久化（`src/meta/processors/job/JobManager.cpp:429-463`）。随后检查所有 task，只要仍有一个 `RUNNING`，job 不结束；全部结束且全成功，job 为 `FINISHED`，否则为 `FAILED`（`474-530`）。所以某主机早已 `FAILED`，另一主机仍拖着，也可能整体继续显示 `RUNNING`。

## 4. SHOW 命令的三个诊断陷阱

### 4.1 SHOW INDEX STATUS 是历史 job 摘要

`SHOW TAG INDEX STATUS` 从 job 记录中筛选 `REBUILD_TAG_INDEX`，按 jobId 倒序排序，再对索引参数串去重（`src/meta/processors/job/ListTagIndexStatusProcessor.cpp:26-62`）。边索引对应路径相同（`src/meta/processors/job/ListEdgeIndexStatusProcessor.cpp:26-61`）。

空参数的名称是 `all_tag_indexes` / `all_edge_indexes`；多索引重建的名称是以逗号连接的参数串。它不是逐个 indexId 对底层索引数据做一致性校验，也不提供 backlog 或扫描行数。同一索引可能通过不同参数组合出现在多个历史 job 摘要中，应以此次提交返回的 jobId 为准。

### 4.2 SHOW JOB task 行的 Error Code 使用 job 的 code

基线 `src/graph/executor/admin/SubmitJobExecutor.cpp:199-207` 在构造每条 task 展示行时，取的是 `jd.get_code()`，而不是 `taskDesc.get_code()`。job 仍处于 `E_JOB_SUBMITTED` 时，task 行错误列可能空白；job 失败后，各 task 行也可能显示同一个 job 错误码。

因此 task 行的 `Status` 和 host 可以用于定位，但不能把该行 `Error Code` 当作该 host 的真实 task 错误。真实错误应从 storaged 的 `task(...) finished, rc=[...]` / `task_rc=...` 或 meta 保存的 task 数据判断。

### 4.3 保留时长不是运行超时

`job_expired_secs` 默认一周（`src/meta/processors/job/JobManager.cpp:30-32`）。`isExpiredJob()` 对 `RUNNING/QUEUE` 直接返回 false（`689-693`）。它主要用于清理已结束的历史记录；改变它不会加速回放，也不会中断两天未完成的索引重建。

## 5. STOP 行为与累计回放缺陷的相互作用

正常 meta 进程仍持有该作业 executor 时，`STOP JOB J` 调用 `RebuildJobExecutor::stop()`，向该空间各 storage host 发送 stopTask（`src/meta/processors/job/RebuildJobExecutor.cpp:45-75`）。storage handler 只调用 `cancelTask(jobId)` 就回复（`src/storage/admin/StopAdminTaskProcessor.cpp:14-25`）。

`cancelTask()` 遍历匹配 job 的本地任务、调用 `cancel()`、移除当前周期的持久化状态（`src/storage/admin/AdminTaskManager.cpp:158-172`）。`cancel()` 的作用是置 `canceled_ = true`，尝试将 task rc 从成功改成 `E_USER_CANCEL`（`src/storage/admin/AdminTask.h:188-193`）。它不会等正在执行的函数退出。

取消响应位置并不均匀：

| 执行位置 | 检查取消标志情况 | 现象含义 |
|---|---|---|
| TAG / EDGE 全量扫描 | 在扫描内层每轮检查；两文件均为 `66-69` | 通常到下一条数据处理时退出，但当前同步写/限速等待仍要返回 |
| 操作日志回放 | 入口和外层每轮检查；`RebuildIndexTask.cpp:136-145` | 当前整个 iterator 内层扫描 `157-182` 不检查 |
| 每次回放写入 | `writeOperation()` 的 consume 与 Baton wait 不检查；`258-275` | 当前回放提交不能由 stop 标志直接打断 |
| LOCKED 后等在途写入 | `203-209` 的 do-while 无取消检查 | `onFlyingRequest_` 长期不归零时，stop 标志也不能解除这个等待 |

累计 `BatchHolder` 越过阈值后每处理一条新 op 都提交越来越大的旧批次；当批次超过 rate limiter burst 时，每次 consume 明确 sleep 1 秒（`src/kvstore/RateLimiter.h:40-48`）。假设当前 iterator 尚有 172800 条记录处于此路径，即使后续没有任何新写入，单看每条一次 1 秒等待已经是两天量级，尚未计入编码、Raft 和 RocksDB 成本。这是条件化的数量级推演，不是生产 backlog 的实测值。

因此以下时间线在源码上成立：

```text
t0  进入某轮 operation iterator，batch 已超过阈值
 t1 STOP JOB 返回 Job stopped，meta 可显示 STOPPED
 t2 storage 已记录 cancelled，但仍处理同一轮 iterator
 t3 直到当前轮回放结束并到达下一次外层取消检查，subtask 才返回
 t4 本机全部 subtask 返回，上报 E_USER_CANCEL，meta 可转为 FAILED
```

最终从 `STOPPED` 转成 `FAILED / E_USER_CANCEL` 是本分支允许的状态转换；已有测试 `src/meta/test/JobManagerTest.cpp:545-616` 明确断言这个行为。本文只阅读了该测试，未执行它。不要仅凭 STOP 成功就立即断言重建 guard 已释放，或立即反复启动 RECOVER/REBUILD 来验证。

`onFlyingRequest_` 是 `StorageEnv` 级别单一计数（`src/storage/CommonUtils.h:70-76`），不是该分区独占计数；`IndexCountWrapper` 构造递增、析构递减（`103-126`）。若其他分区或空间的相关写入使计数一直不为零，也存在另一条长等待路径。应通过 `On Flying Request:` 日志或线程栈区分；不能把所有回放末尾等待都归入 BatchHolder 缺陷。

## 6. 重启、离线与恢复：哪些路径会留下 RUNNING

### 6.1 storage 进程重启并保留 adminStore

`StorageServer::getAdminStoreSeqId()` 每次启动把持久化序号加一（`src/storage/StorageServer.cpp:159-175`），adminStore 位于首个 dataPath 的内部 RocksEngine（`152-156`）。初始化 `AdminTaskManager` 时会扫描未上报记录（`src/storage/admin/AdminTaskManager.cpp:36-40`）。

旧启动周期的未完成任务初始 rc 为 `E_TASK_EXECUTION_FAILED`，会被当作失败结果报告给 meta；已完成但未报告的结果也会补报（`AdminTaskManager.cpp:76-105`）。这不是自动恢复全量扫描/回放现场。只要旧记录存在、meta 可达、全部 host task 最终上报，作业通常会结束为失败或完成，而不是必然永久 RUNNING。

若执行 task 的原 host 永久离线，或替换主机/清空数据目录时丢失本地 adminStore 中待上报结果，meta 又没有收到结束报告，持久化 task 就可能长期维持 RUNNING。当前调度代码不以两天或存活检测自动将这个 task 结束。是否发生过该情形，需要生产 host 变更和数据目录证据。

### 6.2 meta 重启或领导权转移后的控制面差异

`JobManager::handleRemainingJobs()` 只将 **RUNNING 且 isMetaJob()** 的作业加入失败处理；`QUEUE` 作业重新入队；RUNNING storage job 保留原状态但没有在这里重建 `runningJobs_` executor（`src/meta/processors/job/JobManager.cpp:83-134`）。storage 仍可继续执行，后续上报通过持久化 task 记录完成聚合，因此不能简单认定一次 meta 重启导致重建失败。

但如果当前 meta 的 `runningJobs_` 中没有该 executor，`jobFinished(... STOPPED)` 走 `392-397` 分支，直接保存状态并返回，不调用 storage 的 stop RPC。原注释也明确写有 leader changed 的 TODO。这意味着：在这样的控制面状态下，用户看到 STOPPED 甚至可能连 storage 的取消标志都没有设置。

这是可从源码确认的条件化行为，本文没有做 meta 重启故障注入来验证现场日志。判断它需要将 metad 启停/leader 时间线、STOP 发起时间和 storaged 的 `task(... ) cancelled` 记录进行对齐。

### 6.3 RECOVER 是重新执行，并非接续原 iterator

`recoverJob()` 只将未过期的 `FAILED` 或 `STOPPED` 作业纳入候选，再置回 `QUEUE` 并重新调度（`src/meta/processors/job/JobManager.cpp:862-910`）。仍为 `RUNNING` 的作业不会仅因调用 RECOVER 就恢复或替换。

索引重建继承的 `StorageJobExecutor::recovery()` 仅返回成功（`src/meta/processors/job/StorageJobExecutor.h:91-93`），之后重新 execute，重新获取目标 host、构建任务。没有持久化并恢复全量扫描 iterator 或 operation iterator 的位置。分析运行时长时必须区分“同一个 jobId 被 recover 重新开始”和“一次执行连续两天”。

## 7. 生产现场证据采集与判别顺序

以下是只读采集示例；替换空间名和 jobId 后，在对应环境运行。本文未连接生产执行。保存多次采样结果时必须附带时间，至少间隔 30～60 秒采样两轮，以判断是否前进。

```ngql
USE <space>;
SHOW JOBS;
SHOW JOB <job_id>;
SHOW TAG INDEX STATUS;
SHOW EDGE INDEX STATUS;
SHOW HOSTS;
SHOW PARTS;
SHOW CONFIGS STORAGE;
```

本机默认测试环境可按 AGENTS.md 使用：

```bash
/home/sch/nebula-run/nebula-console -addr 127.0.0.1 -port 9669 -u root -p nebula
```

对相关 job 的所有目标 storaged 收集完整时间段，首先过滤阶段、task 结束和上报日志（目录按实际配置调整）：

```bash
rg -n 'Building index successful|Processing operation logs|RebuildIndexTask Finished|Finish rebuild index task|subtask of task|task\([0-9]+, [0-9]+\) finished|reportTaskFinish|Rebuild index canceled|This space .* is building index|On Flying Request' /home/sch/nebula-run/nebula-index/logs/nebula-storaged.INFO*
rg -n 'Trying to end job|Try to stop job|report to an in-active job manager|Report an invalid or outdate task|JobManager initialized|LoadJobDesc failed|Rebuild Index Space' /home/sch/nebula-run/nebula-index/logs/nebula-metad.INFO*
```

默认 INFO 看不到 `On Flying Request`，因为原实现是 VLOG(1)。若生产允许临时观测，可通过本次纯观测日志或定点栈采样补齐证据；不要为了诊断而盲目全局打开高频逐 op 日志。只看到缺失日志也要先排除轮转、级别和采集窗口缺口。

| 现场组合 | 优先判断 | 下一条必要证据 |
|---|---|---|
| task RUNNING；全量成功后停在 Processing operation logs；每秒提交且 batch 单调增长 | 命中累计 BatchHolder 重复提交路径 | 回放提交计数、每批累计 op/bytes、相邻提交时间、同一轮中已处理 op 数 |
| task RUNNING；还没有该分区全量成功日志 | 全量扫描、限速、线程排队或底层写入耗时 | 分区实际启动时间、扫描量、线程栈、磁盘和 Raft 写入时间 |
| task RUNNING；某些 part 已 Finished、尚有 part 未结束 | host task 等最慢 subtask | 对照该 host 获派全部 parts，不能仅统计已有 Finished 行 |
| storaged task 已 finished；没有 report_rc=SUCCEEDED，反复上报失败 | 完成状态未成功抵达 meta | RPC 错误、meta leader/启动状态、对应时间段 meta 日志 |
| storaged report_rc=SUCCEEDED；job 仍 RUNNING | 其他 host task 未结束，或观察的是另一个 job/space | SHOW JOB 每个 host/task 状态与所有目标主机日志 |
| storaged 已无该任务、host 曾更换/数据路径改变、meta task RUNNING | 原执行者丢失待上报结果的候选路径 | 原 adminStore、主机更换时间线、首次接收/持久化记录 |
| STOP 后继续回放，同 host 有 cancelled | 协作取消尚未到检查点 | 所在线程仍处内层 iterator 或 onFlyingRequest 等待 |
| STOP 后继续回放，同 host 无 cancelled，meta 曾重启 | meta executor 缺失而仅改状态的候选路径 | STOP 对应 RPC、meta 生命周期、确认日志采集完整 |
| On Flying Request 长期非零，线程栈位于收敛等待 | 全局在途计数迟迟不归零 | 哪些写请求持有 IndexCountWrapper、是否跨空间/分区持续写入 |

最小归因闭环应有：**具体 jobId → RUNNING host/task → 未完成 partition → 未完成阶段 → 该阶段真实瓶颈观测 → 源码对应行为**。其中任何一环缺失，都应该保留“候选根因”措辞，不能仅凭“执行两天”给出生产唯一根因。

## 8. 本文验证范围

本次执行了源码静态阅读、基线行号核对和既有测试阅读，没有修改 job 生命周期相关实现。没有把未执行的故障注入方案写成复现结果。关键运行记录保存在 `logs/job-lifecycle-audit.log`；全工程实际编译、复现实验及累计回放问题的测量结果由主调查报告记录。
