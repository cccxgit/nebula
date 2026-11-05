# 原生全文跨引擎正确性与分阶段 ES 基线

这些步骤是准备好的可执行流程，不能当作已经完成的 ES 实测。原型与 ES 基线应顺序运行，测量前停止编译、暂停语料下载并通过资源预检。ES 的 `start` 仍只操作专属运行目录，设置 `ulimit` 至至少 65536；`initialize` 原便利流程保持原样。新增的 `prepare-space` / `create-index` 用于与原型一致的“先无全文索引导入、后建索引”实验。

## 1. 分阶段建立基线

```bash
python3 scripts/tantivy-benchmark/es_baseline.py start \
  > task-logs/tantivy-benchmark/es-start.log 2>&1

python3 scripts/tantivy-benchmark/es_baseline.py prepare-space --space ft_bench_1m \
  > task-logs/tantivy-benchmark/es-prepare-space-1m.log 2>&1
```

`prepare-space` 要求新的空间名，等待三个 storage ONLINE、20 分片各 3 副本健康，再创建 `passage(text string)` 及普通空属性 Tag 索引 `passage_tag_lookup`，等待 schema 可用。此时没有全文索引，也不向该空间分配 Listener，避免把初次加载混成带全文索引的在线写入。ES 和 Listener 进程虽已启动但尚未为该空间建全文索引，其驻留资源仍属于旧方案成本。

```bash
python3 scripts/tantivy-benchmark/benchmark.py import \
  --host 127.0.0.1 --port 29669 --space ft_bench_1m \
  --input /home/sch/nebula-data/tantivy/msmarco-1m-128.jsonl.gz \
  --concurrency 1 --batch-size 200 --batch-bytes 131072 \
  --output task-logs/tantivy-benchmark/es-import-1m

python3 scripts/tantivy-benchmark/es_baseline.py create-index \
  --space ft_bench_1m --index nebula_index_ft_passage_1m \
  --expected-rows 1000000 --analyzer standard --result-window 100001 \
  --wait-seconds 3600 \
  > task-logs/tantivy-benchmark/es-create-index-1m.log 2>&1
```

需要测全服务资源时，用 `monitor.py` 覆盖 graph、meta、三个 storage、Listener、ES 七个 PID，围绕以上阶段单独记录。直接执行 `benchmark.py` 不会自动启动资源监控；已有 `run-measured-stage.sh` 的 `index` 子命令针对 Tantivy，不能拿它调用 ES 建索引。

`create-index` 注册专属 ES 服务、设置模板（1 primary / 0 ES replicas / max_result_window=100001），分配 Listener，等待 20 个 Listener 分片 ONLINE，创建标准分析器全文索引，验证 ES 实际索引设置，再提交 REBUILD 并等待 job 与所有 task 完成。最后轮询正常 refresh 可见的 ES `_count`，必须恰为输入 manifest 中的 1,000,000 条；不主动 refresh 干扰常规可见延迟。该阶段时间包含 Listener 分配、DDL、重建和正常 refresh，不能直接当纯 Lucene 构建时间。

Listener ONLINE 是元数据心跳状态，不单独证明每个分片回填完成。REBUILD 任务失败、HTTP 错误、部分 shard 或最终计数不符都不能继续计时；保留日志处理实际原因。操作日志还会追加到 `/home/sch/nebula-run/3.6-tantivy-es-baseline/operation-log.jsonl`。

## 2. 完整候选集检查

旧 Nebula 的 ESAdapter 在 FulltextIndexScan limit 为 -1 时不传 Elasticsearch size，ES 可以返回默认 10 条。因此“不写 LIMIT”并不证明完整结果。`native_compare.py` 请求 `max_hits + 1` 条作为溢出哨兵，默认 100001，并核对 ES 的 EXPLAIN 中该 limit 确实到达 FulltextIndexScan；缺少优化下推会直接拒绝执行。ES `max_result_window` 也必须足够，否则保留查询错误，不把失败当空集合。

新集群在数据和索引就绪后运行，随后再对旧 ES 集群运行；不必同时启动两个方案：

```bash
python3 scripts/tantivy-benchmark/native_compare.py collect \
  --backend tantivy --space ft_bench_1m --index ft_passage_text_1m \
  --manifest task-logs/tantivy-benchmark/msmarco-1m-128.manifest.json \
  --output task-logs/tantivy-benchmark/native-1m-tantivy

python3 scripts/tantivy-benchmark/native_compare.py collect \
  --backend es --space ft_bench_1m --index nebula_index_ft_passage_1m \
  --manifest task-logs/tantivy-benchmark/msmarco-1m-128.manifest.json \
  --output task-logs/tantivy-benchmark/native-1m-es

python3 scripts/tantivy-benchmark/native_compare.py compare \
  --left task-logs/tantivy-benchmark/native-1m-tantivy \
  --right task-logs/tantivy-benchmark/native-1m-es \
  --output task-logs/tantivy-benchmark/native-1m-comparison
```

固定三个 query-string 是 `water`、`"New York"` 和 `water AND river`；函数分别为 TEXT_SEARCH 和 ES_QUERY。每个采集目录保存原查询、EXPLAIN、PROFILE、完整排序后的 VID JSONL、SHA256、数量和失败详情。检查重复 VID、非有限 score、失败/部分成功、达到哨兵上限；ES 还要求查询前后 `_count` 完整 shard 成功且数量等于返回 VID 数，防止旧适配器忽略 ES 部分返回。

离线 compare 校验输入 manifest 的投影、规模、SHA256 相同，核对集合文件哈希后比较全部 VID；每项差集另存 JSONL。score 数值与全局排名不要求相同。本流程验证这三个查询的候选集合，不等于全部原生 query-string 语法兼容。任一端采集失败、不完整、上限错误或输入不同，比较结果均不能是成功。

检查期间不能同时写入或改变索引。`max_hits` 是结果完整性与后处理门槛，**不能保证 Tantivy 存储/graph 在返回前只物化这些候选**；限定此处百万固定查询，亿级前仍需容量门槛。

离线模拟已覆盖正常两端、集合相同、重复 VID、哨兵溢出、查询失败、失败集合不能被比较为相等，证据：[native-checker-offline-selftest.json](../../task-logs/tantivy-benchmark/native-checker-offline-selftest.json)。模拟没有连接数据库或发送 HTTP。

## 3. 100k 同 VID 在线 upsert

两端初始百万数据、全文索引及完整候选集合均确认后，分别对相同前 100,000 个原始 VID 执行同一 INSERT：

```bash
python3 scripts/tantivy-benchmark/benchmark.py import \
  --port 29669 --space ft_bench_1m \
  --input /home/sch/nebula-data/tantivy/msmarco-1m-128.jsonl.gz --limit 100000 \
  --concurrency 1 --batch-size 200 --batch-bytes 131072 \
  --output task-logs/tantivy-benchmark/es-upsert-100k
```

原型使用相同参数，仅端口改为 9669，输出使用新目录。100k 是确认写入次数，图中的独立 VID 仍应为 1m，不能宣称多了 100k 文档。相同 VID、相同文本只能测有全文索引时的写入确认吞吐；由于查询结果在更新前后不变，单靠再次查询不能证明这一批异步索引变更已追平，也不能称为可见吞吐。若要测更新可见延迟，应另定义明确改变文本的独立工作负载及可见性 oracle，并披露这不再是原始投影数据。

## 4. 百万前后容量快照

`system_snapshot.py` 只在测量阶段之间执行，使用 st_blocks 统计实占块，不读取文件内容、不清理缓存。示例 PID 必须替换成当前集群的实际 PID；禁止复用已经退出的旧 PID。

```bash
python3 scripts/tantivy-benchmark/system_snapshot.py \
  --graph-dir /home/sch/nebula-run/3.6-tantivy/tantivy-demo/storage1/data/nebula \
  --graph-dir /home/sch/nebula-run/3.6-tantivy/tantivy-demo/storage2/data/nebula \
  --graph-dir /home/sch/nebula-run/3.6-tantivy/tantivy-demo/storage3/data/nebula \
  --index-dir /home/sch/nebula-run/3.6-tantivy/tantivy-demo/storage1/data/tantivy \
  --index-dir /home/sch/nebula-run/3.6-tantivy/tantivy-demo/storage2/data/tantivy \
  --index-dir /home/sch/nebula-run/3.6-tantivy/tantivy-demo/storage3/data/tantivy \
  --pid GRAPH_PID --pid META_PID --pid STORAGE1_PID --pid STORAGE2_PID --pid STORAGE3_PID \
  --before task-logs/tantivy-benchmark/disk-before-1m.json \
  --output task-logs/tantivy-benchmark/system-after-1m.json
```

输出图/全文索引分别加总三份实占，只加一次；前后目录集合必须完全一致才计算增量。包含全服务 PSS、MemAvailable、SwapUsed、pswpin/out 的累计页数及相对前快照的字节增量、oom_kill 增量。计数器是全机口径，不能把其他软件的 swap 直接归因于 Nebula；现存 SwapUsed 也不是本阶段新增 swap。PSS 为一次顺序快照，不替代阶段监控的采样峰值。文件目录在扫描时变化会导致快照不完整，应保留证据后在稳定阶段重试。
