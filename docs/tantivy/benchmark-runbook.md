# 首轮 1 万 / 100 万条验证步骤

这是一份待执行流程。2026-11-04 已完成脚本静态检查、离线真实语料 oracle、失败批次日志自检；本文件中的数据库步骤尚未由数据/基准任务执行。原型必须先通过独立功能回归并停止编译，才开始计时。构建并行度保持 `-j5`；不要与压测同时构建。

## 1. 前置条件和监控范围

已有运行中的隔离集群：graph 9669、meta 9559、storage 9779/9789/9799；启动服务前已设置 `ulimit -n 65536`。这里的流程不启动或停止任何服务。MS MARCO 下载需由持有下载任务的操作者暂停，待计时结束再恢复；不能根据旧 PID 盲目发送信号。

```bash
cd /home/sch/nebula/nebula-3.6-tantivy
python3 scripts/tantivy-demo/cluster.py status
python3 scripts/tantivy-benchmark/preflight.py \
  --output task-logs/tantivy-benchmark/first-run-preflight.json
```

preflight 失败就不运行计时命令：要求没有编译进程或活跃语料下载、可用内存至少 8 GiB、磁盘余量至少 40 GiB。它是瞬时检查，仍需观察后续内存、swap、CPU 和磁盘。已有下载被显式暂停时会单独记录，不视为活跃下载。

下面从专属集群 PID 文件读出 PID，并核对实际可执行文件与配置路径；不会操作进程。必须包含 graph、meta 和三个 storage，不能只监控 graph。

```bash
export TANTIVY_BENCH_PIDS="$(python3 - <<'PY'
import importlib.util
from pathlib import Path
p=Path('scripts/tantivy-demo/cluster.py')
s=importlib.util.spec_from_file_location('demo_cluster',p)
m=importlib.util.module_from_spec(s)
s.loader.exec_module(m)
runtime=Path('/home/sch/nebula-run/3.6-tantivy')
pids=[m.owned_pid(runtime,service) for service in m.SERVICES]
if any(pid is None for pid in pids):
    raise SystemExit('All five isolated services must be running')
print(' '.join(map(str,pids)))
PY
)"
```

另记导入前五服务 RSS/PSS、三个 storage 各自的图数据和 Tantivy 目录实占大小。磁盘使用实占块数（`du -B1`），不能用稀疏下载文件的表观大小作为已下载字节。分别记录图数据目录与索引目录，避免重复计入总目录和子目录。

## 2. 首轮 1 万条

保留完整原始 pid、每条最多前 128 个 Unicode 字符，10,000 个真实且唯一的记录。该语料是官方归档的连续前缀，不是随机样本；99.44% 文本被裁剪。采用新的空间名，已有同名空间将报错，不自动清空。

```bash
python3 scripts/tantivy-benchmark/readiness.py prepare \
  --space ft_bench_10k --output task-logs/tantivy-benchmark/schema-10k

bash scripts/tantivy-benchmark/run-measured-stage.sh import-10k import \
  --space ft_bench_10k \
  --input /home/sch/nebula-data/tantivy/sample-10000-128.jsonl.gz \
  --concurrency 1 --batch-size 200 --batch-bytes 131072

bash scripts/tantivy-benchmark/run-measured-stage.sh index-10k index \
  --space ft_bench_10k --index ft_passage_text_10k --wait-seconds 600

bash scripts/tantivy-benchmark/run-measured-stage.sh visible-10k verify \
  --space ft_bench_10k \
  --oracle task-logs/tantivy-benchmark/oracle-10000-128-safe.json \
  --expect-pushdown on --wait-seconds 600

bash scripts/tantivy-benchmark/run-measured-stage.sh query-10k query \
  --space ft_bench_10k \
  --queries scripts/tantivy-benchmark/queries-transparent-match.jsonl \
  --label tantivy-10k-pushdown-on --warmup 3 --repeats 40 --concurrency 1
```

`prepare` 创建 20 分片、3 副本、FIXED_STRING(64) 空间，等待 `USE`、Tag、普通空属性 Tag 索引元数据传播；等待 20 个分片均有 leader、3 个 peer、无失联副本；通过一次只读 FETCH 检查 storage 的 schema 可用。普通 Tag 索引用于关闭下推后的同语义 MATCH 扫描。

`index` 在数据写入后重建普通 Tag 索引，创建 `ANALYZER="standard"` 的全文索引，再提交 `REBUILD FULLTEXT INDEX`。每个 REBUILD 都轮询 `SHOW JOB`，要求 job 与所有 task 的状态均为 `FINISHED`，失败即停止。此方案包含 CREATE 自动回填与显式 REBUILD，计时不能直接宣传成“一次理想批量建索引速度”。

`verify` 不加 LIMIT，逐一比较完整返回 VID 集合与本地真实文本的精确子串 oracle，拒绝缺失、额外命中和重复 VID；保存 PROFILE 并检查确实使用 Tantivy。10k 四个查询的完整期望命中数分别为 **264 / 36 / 15 / 0**。这里的可见性覆盖这四个谓词；索引 job 完成是另外一份完成证据，不能由一次 LIMIT 10 查询代替。

任何步骤失败都先保留日志并解决原因，不继续百万测试。对可重试 INSERT 使用同一 VID 是幂等覆盖，但必须将重试作为单独运行记录，不能把重复确认写入数当独立新增记录数。

## 3. 百万条低并发试验

10k 全部通过且无资源异常后再执行。百万文件已准备完成，1,000,000 个唯一原始 pid、99.308% 记录被裁剪；压缩文件 54,984,169 bytes，原文投影平均 128.396899 UTF-8 bytes。使用新的空间，避免将 10k 更新写入混入百万新增数据指标。

```bash
python3 scripts/tantivy-benchmark/preflight.py \
  --output task-logs/tantivy-benchmark/one-million-preflight.json

python3 scripts/tantivy-benchmark/readiness.py prepare \
  --space ft_bench_1m --output task-logs/tantivy-benchmark/schema-1m

bash scripts/tantivy-benchmark/run-measured-stage.sh import-1m import \
  --space ft_bench_1m \
  --input /home/sch/nebula-data/tantivy/msmarco-1m-128.jsonl.gz \
  --concurrency 1 --batch-size 200 --batch-bytes 131072

bash scripts/tantivy-benchmark/run-measured-stage.sh index-1m index \
  --space ft_bench_1m --index ft_passage_text_1m --wait-seconds 3600

bash scripts/tantivy-benchmark/run-measured-stage.sh visible-1m verify \
  --space ft_bench_1m \
  --oracle task-logs/tantivy-benchmark/oracle-1m-128-safe.json \
  --expect-pushdown on --wait-seconds 600

bash scripts/tantivy-benchmark/run-measured-stage.sh query-1m-c1 query \
  --space ft_bench_1m \
  --queries scripts/tantivy-benchmark/queries-transparent-match.jsonl \
  --label tantivy-1m-pushdown-on-c1 --warmup 3 --repeats 200 --concurrency 1
```

百万四个完整期望命中数为 **18,267 / 3,166 / 147 / 28**。先用并发 1 观察负载，稳定且同机无额外负载才另开并发 2、4 的独立运行。每条查询 200 次只有约两个样本位于最末 1%，P99 仅为初步测量，尾延迟置信度有限；时间允许才增加每条重复次数。

所有 measured wrapper 日志在 `task-logs/tantivy-benchmark/UTC时间-标签/`：`preflight.json`、`command.json`、`resources.csv` 及其汇总、`stdout.log`、`stderr.log`、`results/`。导入含每批日志与 `import-summary.json`；查询含 CSV/JSONL 与 `query-summary.json`；建索引含 `events.jsonl`；可见性含 `visibility-summary.json` 与四份 PROFILE。wrapper 退出只结束自己的监控进程，不结束服务。

## 4. 两类对照，分别执行

**透明下推对照**：同一个新集群、相同语料、相同 MATCH 查询，分别关/开 `enable_tantivy_pushdown`，使用 `verify --expect-pushdown off/on` 记录实际 PROFILE 后计时。已按本仓库 `SetFlagsHandler.cpp` / `GetFlagsHandler.cpp` 核实本机 graph 的 `PUT /flags` 与读回接口；`toggle_pushdown.py` 固定 127.0.0.1:19669，仅修改这一 flag，不重启集群。切换时确保没有其他查询；helper 拒绝已发现的本任务查询客户端，但无法保证外部 console 无活动。每次变化后仍须完整 VID 与 PROFILE 校验。关闭下推后的全量扫描可能慢，先 10k，再百万；不能把超时排除后只报告成功请求。切换后重新预热，记录顺序与缓存条件。

下面在一个子 shell 中做 10k 的关闭下推试验，正常或失败退出都尝试恢复 true。百万对照在 10k 通过后将空间和 oracle 路径换为对应百万版本，使用新的日志标签。

```bash
(
  set -e
  task_toggle_stamp=$(date -u +%Y%m%dT%H%M%SZ)
  task_toggle_dir="task-logs/tantivy-benchmark/$task_toggle_stamp-toggle-10k"
  mkdir -p "$task_toggle_dir"
  trap 'python3 scripts/tantivy-benchmark/toggle_pushdown.py on --output "$task_toggle_dir/restore-on.json"' EXIT
  python3 scripts/tantivy-benchmark/toggle_pushdown.py off \
    --output "$task_toggle_dir/off.json"
  bash scripts/tantivy-benchmark/run-measured-stage.sh visible-10k-off verify \
    --space ft_bench_10k \
    --oracle task-logs/tantivy-benchmark/oracle-10000-128-safe.json \
    --expect-pushdown off
  bash scripts/tantivy-benchmark/run-measured-stage.sh query-10k-off query \
    --space ft_bench_10k \
    --queries scripts/tantivy-benchmark/queries-transparent-match.jsonl \
    --label tantivy-10k-pushdown-off --warmup 3 --repeats 40 --concurrency 1
)
```

退出后检查 `restore-on.json` 的 success/after 都为 true，再执行一次 `verify --expect-pushdown on`。不能仅根据 PUT 返回成功判断优化器实际选择。

**原生全文对照**：新集群 `TEXT_SEARCH` 对旧 Debug 集群 `ES_QUERY`，两边 standard analyzer、同一文本投影、相同完整候选 ID 集和同层级 LIMIT，分数值可不同。先跑 8 条合成语法 fixture 的 18 个回归 case，再跑真实语料词项、短语、AND 三项。按当前规模替换样例索引名，不改查询文本：

```bash
python3 - <<'PY'
import json
from pathlib import Path
src=Path('scripts/tantivy-benchmark/queries-text-search.jsonl')
dst=Path('task-logs/tantivy-benchmark/queries-text-search-1m.jsonl')
if dst.exists():
    raise SystemExit('Use a fresh query output path')
rows=[json.loads(line) for line in src.read_text().splitlines()]
for row in rows:
    row['query']=row['query'].replace('TEXT_SEARCH(ft_passage_text,','TEXT_SEARCH(ft_passage_text_1m,')
dst.write_text(''.join(json.dumps(row)+'\n' for row in rows))
PY
```

旧基线须另行启动并在前一方案停止后顺序测试；监控 PID 列表加上 Listener 与 ES。仅有新方案数据不能计算相对 ES 提升百分比。ES 的默认 1 primary / 0 replica 与图的 3 副本不同；Tantivy 随三个 storage 的索引副本也应全计入资源。

## 5. 扩到一亿之前的门槛

将百万**实测**填写到 `scripts/tantivy-benchmark/pilot-measurements.template.json` 的副本。所有三 storage 图数据/索引实占增量加总一次，不能再乘 3；PSS 使用同一时刻全服务之和，不能把各服务不同时刻峰值相加。

```bash
python3 scripts/tantivy-benchmark/capacity_gate.py \
  --pilot task-logs/tantivy-benchmark/pilot-1m-measured.json \
  --target-rows 100000000 --time-budget-hours 8 \
  --output task-logs/tantivy-benchmark/capacity-gate-100m.json
```

8 小时是示例预算，执行时应改成实际剩余比赛时间。工具拒绝空值和估算冒充实测。磁盘门槛计入 1.5 倍增长不确定性、50% 索引合并余量、剩余下载空间、规范化文件增长与 40 GiB 保留；可用内存须 ≥8 GiB，百万服务峰值 PSS ≤物理内存一半。查询完整候选集预计不能超过 100k；当前 `water` 在一亿外推约 182.7 万命中，**因此当前查询集不满足一亿扩容门槛**，需要换成更窄且完整验证的新查询集，不能靠 LIMIT 10 掩盖候选集物化成本。

通过门槛只批准下一个有界阶段：1m→5m→10m→最多每次增加 10m，每阶段重新评估，不直接从百万跳一亿。超过预算时应如实交付百万或已完成规模的实测与外推，不声称亿级已验证。
