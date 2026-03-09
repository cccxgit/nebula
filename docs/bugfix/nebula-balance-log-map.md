# Nebula Graph Balance Bug 日志与代码映射表

用途：
- 把关键日志关键词与代码位置建立映射
- 帮助 Codex 和人工排查时快速从日志回到函数/模块
- 逐步形成“日志 → 状态 → 代码 → 根因”的统一索引

使用建议：
- 每次只补充高价值日志
- 优先记录“状态切换、失败判断、重试、超时、leader变化、任务汇总”相关日志
- 一条日志映射不到精确函数时，可以先映射到模块/目录级别

---

## 1. 记录模板

| 日志关键词/片段 | 时间点 | 模块/文件 | 函数/方法 | 状态含义 | 触发条件 | 关联假设 | 备注 |
|---|---|---|---|---|---|---|---|

---

## 2. 关键日志类型建议

### 2.1 任务生命周期日志
建议关注：
- balance started
- generate plan
- submit task
- task dispatched
- task finished
- task failed
- job completed
- job cancelled

意义：
- 用于恢复任务完整时间线
- 用于识别失败前最后一个正常状态
- 用于判断状态机是否出现跳变

---

### 2.2 状态切换日志
建议关注：
- update state
- transition to
- mark as failed
- mark as succeeded
- retry task
- recover task

意义：
- 用于定位状态机缺陷
- 用于识别状态被覆盖或重复更新

---

### 2.3 storage / host 视图日志
建议关注：
- active hosts
- host status changed
- heartbeat
- storage registered
- remove host
- get host info

意义：
- 用于排查扩容后视图不一致
- 用于判断 balance 使用的 host 视图是否稳定

---

### 2.4 partition 迁移日志
建议关注：
- move part
- transfer part
- add learner
- catch up data
- remove peer
- transfer leader

意义：
- 用于判断失败发生在迁移哪一阶段
- 用于排查 leader 或副本状态变化

---

### 2.5 异常与重试日志
建议关注：
- timeout
- rpc failed
- retry
- backoff
- aborted
- partial success
- duplicate task

意义：
- 用于判断是否属于可重试错误被放大
- 用于判断失败后重入是否异常

---

## 3. 初始映射表

> 下面先放模板化条目。你拿到实际日志后，把“待确认”替换成具体文件和函数。

| 日志关键词/片段 | 时间点 | 模块/文件 | 函数/方法 | 状态含义 | 触发条件 | 关联假设 | 备注 |
|---|---|---|---|---|---|---|---|
| balance started | 待补充 | 待确认 | 待确认 | balance 任务开始 | 用户触发或系统触发 balance | H2/H5 | 作为时间线起点 |
| generate balance plan | 待补充 | 待确认 | 待确认 | 开始生成迁移计划 | 进入 plan 阶段 | H5 | 看输入 host/part 视图 |
| task dispatched | 待补充 | 待确认 | 待确认 | 子任务下发 | 计划进入执行阶段 | H2/H3 | 看是否重复下发 |
| update task state | 待补充 | 待确认 | 待确认 | 状态变更 | success/fail/retry 前后 | H2/H3 | 核心状态日志 |
| retry task | 待补充 | 待确认 | 待确认 | 开始重试 | 超时或错误后 | H4/H6 | 看是否有清理逻辑 |
| timeout | 待补充 | 待确认 | 待确认 | 子任务超时 | 网络、执行过慢或判断阈值问题 | H4 | 看 timeout 分类 |
| rpc failed | 待补充 | 待确认 | 待确认 | 远程调用失败 | storage 或 meta 交互异常 | H4/H1 | 看错误码 |
| host status changed | 待补充 | 待确认 | 待确认 | host 视图变化 | 扩容、心跳、下线、抖动 | H1 | 看与失败时间点是否重叠 |
| leader changed | 待补充 | 待确认 | 待确认 | leader 切换 | raft/复制组变化 | H7 | 看是否导致任务中断 |
| mark job failed | 待补充 | 待确认 | 待确认 | 总任务被判失败 | 某子任务失败或汇总异常 | H2/H8 | 看失败判定来源 |
| aggregate result | 待补充 | 待确认 | 待确认 | 汇总子任务结果 | 收尾阶段 | H8 | 看是否误判 |
| cleanup before retry | 待补充 | 待确认 | 待确认 | 重试前清理 | 失败重入前 | H6 | 看是否真正执行 |

---

## 4. 每轮排查后的更新方式

每次排查建议只新增三类内容：

### A. 新确认的日志映射
- 日志：
- 文件：
- 函数：
- 状态含义：
- 关联假设：

### B. 新确认的高价值关键词
- 关键词：
- 为什么关键：
- 下一轮如何用它缩小范围：

### C. 发现的异常时间线
- 时间点：
- 事件：
- 对应代码：
- 结论：

---

## 5. 与 Codex 配合的推荐提示词

```text
请先不要改代码。

下面是一段日志，请帮我把它映射到代码路径：
[贴日志]

输出：
1. 关键事件时间线
2. 最可能对应的文件/函数
3. 当前所处状态
4. 前一个状态和后一个状态应该是什么
5. 最可疑的状态切换点
6. 建议我下一步补充哪些日志关键词
