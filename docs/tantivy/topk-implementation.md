# TEXT_SEARCH 的受限 TopK 下推

本次优化只对 FIXED_STRING 顶点的 TEXT_SEARCH 生效，并要求执行计划给出有限的结束位置 `K = offset + count`。Storage 再检查只有一个全文查询上下文、`mode=0`、没有剩余过滤条件、自定义排序或统计聚合。INT64 顶点、边、普通 CONTAINS / STARTS WITH / ENDS WITH、无界查询继续使用原完整候选路径。显式 ORDER BY 暂未新增优化规则，即使排序条件恰好为分数降序，也继续取完整候选。

每个分区最多返回自己的前 K 个有效且唯一的 VID；Graph 按分数降序、解码后的 VID 升序合并，再执行全局偏移和条数。不得把每分区上限误设为 count。当前三副本均含整个图空间的 20 个分区，但相同的有效文档集合不保证副本的 BM25 分数逐位相同：真实验收中同样文本出现两组分数，可能与删除文档的段统计有关。本实现不提供一般分布式部署的全局 IDF；TopK 验收以各副本当前返回分数组成的完整查询排序为对照。

同一分区、同一 Tag 的物理 key 前后缀固定。对规范的 FIXED_STRING VID，零填充后的 key 字节序等于解码后的字符串字节序，包含前缀关系和大于 0x7f 的字节。INT64 的小端编码以及边 key 字段顺序不具备这一性质，因此不进入优化路径。

Storage 在输出任何结果前读取全部有界候选的主记录，并缓存读到的数据，避免截断后再次读取遇到删除而无法补足。若候选主记录不存在、出现 NUL 后仍有非零字节的非规范 VID、出现重复解码 VID，或分数为正零/负零，则重新检索完整候选。完整回退按照 Graph 的数值分数和解码 VID 排序，跳过缺失主记录、保留每个 VID 最高有效分数，再取 K。零分回退是因为 Rust 的 total_cmp 区分正零与负零，而 Graph 的数值比较视为同分。其它读取和检索错误仍向上传播。

上述检查没有增加快照或事务一致性保证。主记录读取、全文检索以及 Graph 后续取属性仍是分离的步骤，并发更新可能跨越这些步骤；不能据此声称能检测候选文本的变化。范围内的性能收益主要来自减少跨语言结果、主记录读取、RPC 返回量和 Graph 排序量，Rust 当前收集器仍遍历匹配文档，本次未引入 WAND。

## 回归验证

独立 C++ 测试直接调用生产节点复用的 `TantivyTopK.h::prepareTopK`，通过注入检索与主记录读取构造稳定的失效候选，不修改运行中的数据库文件。覆盖前缀 VID、高位字节、正负零、缺失候选、NUL 别名、别名最高有效分数、错误传播，以及 20 分区 1600 条同分数据的 LIMIT/OFFSET 全量对照。

```bash
g++ -std=c++17 -O2 -Wall -Wextra -Werror -Isrc src/storage/fulltext/tests/TantivyTopKTest.cpp -o /tmp/tantivy_topk_test
/tmp/tantivy_topk_test
```

真实集群验收使用现有三 Storage 集群，创建独立的 20 分区、3 副本空间，默认插入 650 条数据。前 640 条精确覆盖每分区 32 条，再加入前缀及 UTF-8 高位字节 VID。使用 `TEXT_SEARCH(index, "*")` 的恒定 1.0 分数严格验证大量同分；另用 BM25 查询单独对照完整排序和分页。等待三个重建任务 FINISHED 后，检查六组同分分页并以 PROFILE 证明 Storage 实际使用对应 K；显式排序和普通 CONTAINS 则确认没有走有界路径。

```bash
python3 scripts/tantivy-demo/topk.py --output task-logs/tantivy-topk/verified
```

脚本不启动、停止或重启数据库，日志、完整排序、执行计划及结果写入指定目录。2026-11-05 本机验收：独立生产 helper 回归通过，真实集群 10/10 检查通过。证据分别为 `task-logs/tantivy-build/topk-helper-test.log` 和 `task-logs/tantivy-topk/verified-v2/report.json`；后者同时保存完整排序和各查询执行计划。
