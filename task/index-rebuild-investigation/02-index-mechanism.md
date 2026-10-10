# Nebula Graph 3.6 点、边原生索引实现解读

分析对象：当前 nebula-index 分支基线提交 de9b3ed800a6627d9845e9289b6bbc5b6faf460a。本文只解释机制，不修改业务逻辑。源码行号均以该提交为准，可用 git show de9b3ed80:相对路径查看；重建诊断新增日志会使工作区 RebuildIndexTask.cpp 的行号变化。本文中的字节示例与时序示例是源码推导，不冒充数据库实测；真实服务复现、命令及日志见本目录复现报告。

## 1. 与本次“重建长期不结束”最直接相关的机制

1. CREATE INDEX 保存索引定义；历史行由 REBUILD 扫描补建。新的写入在存储服务获知索引定义之后同步维护索引。
2. REBUILD 的全量扫描期间，业务数据仍可写入，但索引增删被转写成同一分区内的 operation KV，留到全量扫描结束再回放。
3. 重建状态的粒度是 (space, partition)，不是 index ID。某个分区在重建一个索引时，该分区内其他点、边索引的并发变更也可能进入同一 operation 前缀。因此“只重建一条索引”不等于“只产生这一条索引的增量日志”。
4. 点索引扫描遍历分区所有 tag 数据，再筛选 tagId；边索引扫描遍历分区所有 edge 数据，再跳过反向边和无关 edgeType。全量耗时取决于扫描前缀内的数据量，不只是最终索引条数。
5. 普通索引一行通常对应一个索引键；更新已存在行通常产生“删旧索引 + 建新索引”两个操作日志。复合索引更宽，索引数量更多，地理索引一行展开为多个键，都会扩大回放工作量。
6. 全量日志“Building index successful”仅说明全量阶段完成；其后还有 operation 回放、LOCKED 排空和任务状态上报。该日志不能作为整个重建结束的证据。

证据：src/storage/CommonUtils.h:46–100；src/storage/mutate/AddVerticesProcessor.cpp:269–327；src/storage/mutate/AddEdgesProcessor.cpp:243–332；src/storage/admin/RebuildTagIndexTask.cpp:53–143；src/storage/admin/RebuildEdgeIndexTask.cpp:53–157；src/storage/admin/RebuildIndexTask.cpp:109–130。

## 2. 数据层：一个点、一个 tag、一条边分别如何存

Nebula 在这个实现中用 KV 表达点、边和原生二级索引。逻辑 space 由存储引擎/调用参数隔离，下面的数据键没有再拼接 space ID。键头 H 是 4 字节整数：

    H = (partitionId << 8) | keyType

低 8 位是类型：tag=1、edge=2、index=3、operation=5、vertex=7。这里的 H 和 tag/index/type ID 直接追加本机整数内存；不能误认为整个键的每个字段都采用大端编码。属性整数和 rank 才使用后文所述的可排序编码。

设 V 为 space 的 VID 固定字节宽度：INT64 VID 为 8，FIXED_STRING(n) VID 为 n；短字符串 VID 尾部补零。

| 记录 | 键结构 | 值的作用 |
| --- | --- | --- |
| 点存在记录，可配置使用 | H(vertex) + VID[V] | 空值 |
| 点上的一个 tag | H(tag) + VID[V] + tagId[4] | 该 tag 的属性行，RowReader 按 schema 版本解析 |
| 有向边物理记录 | H(edge) + src[V] + edgeType[4] + rank[8] + dst[V] + placeholder[1] | 该边的属性行 |
| 点索引 | H(index) + indexId[4] + encodedProperties + optionalNullBitmap[2] + VID[V] | 一般空值；启用 TTL 时携带 TTL 字段值 |
| 边索引 | H(index) + indexId[4] + encodedProperties + optionalNullBitmap[2] + src[V] + rank[8] + dst[V] | 一般空值；启用 TTL 时携带 TTL 字段值 |

源码：src/common/utils/Types.h:14–66；src/common/utils/NebulaKeyUtils.cpp:37–88；src/common/utils/IndexKeyUtils.cpp:87–132；src/storage/mutate/AddVerticesProcessor.cpp:178–209。

“点索引”准确说是某个 tag 的属性索引：同一 VID 的不同 tag 有不同数据行；某个 tag 的索引不直接包含这个点上其他 tag 的属性。索引键不存 tagId/edgeType，是因为 indexId 已经通过元数据唯一关联一个 tag/edge schema。

索引值相同的多个实体不会互相覆盖，因为索引键后面还带 VID 或 (src, rank, dst)。这不是唯一约束索引。

### 2.1 分区归属

客户端把点按 VID 分区，把边物理记录按该记录的 src 分区。MetaClient::partId 在 ID 字节长度恰为 8 时把它当 uint64 参与取模，否则计算 MurmurHash2，再用 vid % numParts + 1。长度为 8 的字符串 VID 也会进入前一兼容分支，不能笼统描述为“所有字符串一定取哈希”。

源码：src/clients/meta/MetaClient.cpp:1218–1231；src/clients/storage/StorageClientBase-inl.h:246–274；src/clients/storage/StorageClient.cpp:802–835。

索引键与对应数据同在实体所属分区，属性值本身不决定新的全局分区。因此按属性查找通常要访问有关 space 的多个分区并汇总；这不是一个按属性全局重新分片的独立索引服务。

### 2.2 正向边与反向边

常规 INSERT EDGE 的图层构造两条物理记录：

    正向：(src=A, edgeType=+T, rank=r, dst=B)
    反向：(src=B, edgeType=-T, rank=r, dst=A)

它们分别按自己的 src 分区，可能落到不同分区。该路径在交换端点后复用属性，因此反向记录不是只有一个“空指针”。这些物理记录为出边和入边遍历服务。

原生边属性索引只为正向边构建：AddEdgesProcessor 明确用 edgeType > 0 包住索引逻辑；重建明确跳过 edgeType < 0。反向边仍写入数据，但不会再生成一套负 edgeType 的同名属性索引。边查询回表时从索引尾部提取 src/rank/dst，并以索引所属的正 edgeType 重建数据键。

源码：src/graph/validator/MutateValidator.cpp:289–305；src/storage/mutate/AddEdgesProcessor.cpp:233–244、318–332；src/storage/admin/RebuildEdgeIndexTask.cpp:85–95；src/storage/exec/IndexEdgeScanNode.cpp:82–91。

分区内的“数据 + 索引”原子批次与跨分区正反边一致性是两个层次。本文不把前者扩展解释成所有正反边天然属于一个 RocksDB 原子批次；事务边路径另有 transaction/ 下的协议。

## 3. 索引定义、创建与历史数据

以点索引为例，CreateTagIndexProcessor 执行以下步骤：

1. 校验 space、字段名重复、字段数上限 16，以及索引名称是否已存在（13–64 行）。
2. 找到 tagId，读取已有索引并做重复/前缀覆盖检查（66–113 行）。
3. 读取最新 schema，校验字段存在与类型合法，整理 IndexItem 中字段类型（115–189 行）。
4. 在 space 内分配 indexId，保存 indexName、schemaId/schemaName、字段列表和参数（191–214 行）。
5. 写入名字到 ID 的映射以及 ID 到 IndexItem 的定义，并更新元数据修改时间（216–225 行）。

边索引以 edgeType 取代 tagId，保存形态相同。客户端元数据缓存通过 listTagIndexes/listEdgeIndexes 拉取定义并建立 ID/名称映射。

源码：src/meta/processors/index/CreateTagIndexProcessor.cpp:13–225；src/meta/processors/index/CreateEdgeIndexProcessor.cpp:192–226；src/clients/meta/MetaClient.cpp:578–613。

这里 CREATE 的执行路径没有遍历存储数据。因此先有大量数据、后 CREATE INDEX 时，历史数据不会仅凭定义写入而自动全部出现在索引中。后续 REBUILD 的作用就是扫描历史数据并与期间新增变更合并。相反，在索引定义已传播后写入的新数据，会走同步维护路径。

示意 nGQL（用于解释，不代表本次实测脚本）：

    CREATE TAG person(age int NOT NULL, name string NOT NULL);
    CREATE TAG INDEX person_age_name ON person(age, name(5));
    REBUILD TAG INDEX person_age_name;

字符串 schema 字段 name 原本可以是变长 STRING，IndexItem 里的该字段却被转换为长度 5 的 FIXED_STRING。这只是索引保存前 5 字节，不改变原始属性行的完整字符串。

## 4. 属性编码、排序、NULL 和复合索引

### 4.1 为什么 RocksDB 的字节顺序能支持数值范围

IndexKeyUtils::encodeInt64 先翻转 int64 的最高位，再转大端；结果的无符号字节字典序与有符号整数顺序对应。

| 原值 | 编码的 8 个字节（十六进制） |
| --- | --- |
| INT64_MIN | 00 00 00 00 00 00 00 00 |
| -1 | 7f ff ff ff ff ff ff ff |
| 0 | 80 00 00 00 00 00 00 00 |
| 1 | 80 00 00 00 00 00 00 01 |
| INT64_MAX | ff ff ff ff ff ff ff ff |

边 rank 复用同一种编码。FLOAT/DOUBLE 在索引中作为 double 编码：非负数大端后设置符号位；负数先取反，再大端；NaN 编成全 ff，并由查询策略额外检查。BOOL 保存布尔字节；DATE/TIME/DATETIME 按固定宽度字段拼接。这些编码不是对所有字段使用同一个通用序列化器。

源码：src/common/utils/IndexKeyUtils.h:121–151、154–245、247–310；数值与属性类型的映射见同文件 29–59 行。

### 4.2 字符串长度是字节数，截断会产生候选碰撞

encodeValue(value, len) 对短串补零，对长串用 substr(0, len) 截断，所以长度按字节处理，不是按 Unicode 字符计数。例如索引长度 3 时，abcdef 与 abcXYZ 的索引属性部分都是 abc，但它们的原始属性不同。

索引字段长度固定，可以直接计算后一个复合字段的偏移。变长 STRING 建索引必须指定长度，FIXED_STRING 字段建索引使用 schema 既定长度而不允许再指定。DURATION 不允许建该类索引；GEOGRAPHY 当前只能单列索引。

查询不能把截断的 abc 当成完整属性值：IndexScanNode 会对可能截断的等值或范围边界标记 UNCERTAIN，回表后用完整属性判断；如果返回列要求字符串属性，当前实现也选择回表，即使原始 schema 就是 FIXED_STRING。是否命中索引与是否回表是两回事。

源码：src/common/utils/IndexKeyUtils.h:107–118；src/meta/processors/index/CreateTagIndexProcessor.cpp:139–186；src/storage/exec/IndexScanNode.cpp:58–102、221–294、392–415、426–468。

### 4.3 NULL 占位和 bitmap

每个 NULL 字段先以本字段固定宽度的 ff 字节占位。若索引定义中至少一列 nullable，则不论当前行是否真的为 NULL，属性编码末尾都追加 2 字节 bitmap；第 i 列用 0x8000 >> i 标识。没有 nullable 列时不带这 2 字节。

因此“NULL 编成全 ff”并不意味着能仅凭字段字节区分 NULL 与 INT64_MAX：它们的 8 字节占位相同，bitmap 才用于消歧。NULL 对应高值区间，但不能省略 NULL 检查而把它当作普通数值最大值。

例：索引 (age nullable int, name nullable string(5)) 中 (NULL, "Amy")，属性区域是 8 个 ff + 41 6d 79 00 00，bitmap 数值为 0x8000（内存字节序直接追加），后面再接 VID。

源码：src/common/utils/IndexKeyUtils.cpp:17–83；src/common/utils/IndexKeyUtils.h:63–104、398–473；src/storage/exec/IndexScanNode.cpp:89–99、200–214。

### 4.4 复合索引按定义顺序拼接，适合左前缀

以 (age, name(5)) 为例，键按 age 编码优先排序，age 相同再按 name 的前 5 字节排序，最后按实体标识区分。因此 age=20 能拼出一个连续索引前缀；age=20 AND name 在区间内可拼出前缀加范围；仅约束 name 不能跳过前面的 age，直接构造同样窄的连续范围。

PrefixPath 逐个把前导等值 hint 拼入键。RangePath 要求前 n−1 个 hint 为共同等值前缀，再把最后一个 hint 变成 [startKey, endKey) 范围。条件不能完全折叠到扫描边界时，还可能作为 filter 保留在执行树中。这里的结论来自键布局与扫描器，不是说任意查询一定选用该索引；优化器仍会选择实际计划。

源码：src/common/utils/IndexKeyUtils.cpp:31–53；src/storage/exec/IndexScanNode.cpp:40–50、152–191、331–350；src/storage/index/LookupProcessor.cpp:210–214。

### 4.5 地理索引不是一个实体严格一个键

GEOGRAPHY 通过 S2 covering 产生若干 cell ID，每个 cell ID 用大端 uint64 编码生成一个索引键，因此 encodeValues 的返回类型是字符串向量。索引只保存 cell，不保存完整地理形状；查询需要地理属性时回表，多个 cell 命中同一实体时去重。

源码：src/common/utils/IndexKeyUtils.cpp:54–75；src/common/utils/IndexKeyUtils.h:314–330；src/storage/exec/IndexScanNode.cpp:215–217、344–347、404–405。

## 5. TTL：索引 value 并非永远为空

未启用有效 TTL 时，普通索引 value 通常为空；启用有效 ttl_col 且 ttl_duration > 0 后，value 保存 TTL 字段的 Value 序列化结果，前面再加 size_t 长度。它不把全部行属性复制到索引 value，也不是必须把 TTL 列列入索引字段才能过滤。

TTL 的几个时机：

| 时机 | 行为 | 源码 |
| --- | --- | --- |
| 普通新增/覆盖 | 从完整属性行取得 TTL 值，写入新索引 value | AddVerticesProcessor.cpp:301–318；AddEdgesProcessor.cpp:294–310 |
| 全量重建 | 已过期行直接跳过；未过期且有 TTL 则编码索引 value | RebuildTagIndexTask.cpp:111–126；RebuildEdgeIndexTask.cpp:119–135 |
| 索引查询 | 从索引 value 恢复 TTL 值并检查，避免仅为 TTL 回表 | IndexScanNode.cpp:473–484 |
| compaction | 校验 tag/edge/index 元数据和 TTL，满足条件时清理无效键 | src/storage/CompactionFilter.h:35–58、145–179 |

上表未写目录的文件分别位于 src/storage/mutate、src/storage/admin、src/storage/exec。

实际判断是 now > ttlValue + duration；TTL 值不是整数（如 NULL）则不因这段逻辑过期。默认以秒解释，ttl_use_ms 可改成毫秒。索引 TTL 编解码见 src/common/utils/IndexKeyUtils.cpp:155–170，判断见 src/storage/CommonUtils.cpp:10–81。

UpdateNode.h:428、780 附近有“when TTL exists, there is no index”的旧注释，但同函数后面的执行代码明确读取 ttlValue 并写索引。解释本版本行为应以执行代码为准，不能根据这两句注释得出“TTL 与索引不能同时存在”。

## 6. 正常写入时，数据和索引如何一起维护

### 6.1 INSERT / 覆盖已有点

AddVerticesProcessor 先获取这个 space 的索引列表。存在索引时，doProcessWithIndex 为每个分区构造 atomicOp，再调用 asyncAtomicOp；addVerticesWithIndex 负责形成原子批次：

1. 读旧属性行，初始化 oldReader。需要 IF NOT EXISTS 时存在则跳过。显式 ignore_existed_index 的快速路径会跳过这一步，不能把它当成所有写入的常规行为。
2. 逐个匹配这个 tag 的索引，用旧属性生成旧索引键并移除。
3. 用新属性生成新索引键并写入；若有 TTL，生成相应索引 value。
4. 把新属性数据行也放进同一个 BatchHolder，编码后返回。

这里读集合和写集合参与可合并原子操作的冲突处理；核心保证范围是同一分区中这次数据与索引批次。正常状态下没有“先成功写业务数据，稍后独立 RPC 再补索引”的常规异步索引链。

源码：src/storage/mutate/AddVerticesProcessor.cpp:53–67、152–225、227–331；边对应 src/storage/mutate/AddEdgesProcessor.cpp:215–221、226–332。

### 6.2 UPDATE / UPSERT

UpdateNode 根据表达式计算新属性并生成新行，同样按每个匹配索引执行“删旧 + 写新 + 写数据”。代码没有先判断索引字段值是否真的改变再决定是否跳过维护。因此即使只更新一个非索引字段，只要走到该路径并有旧行，也可能重新计算、删除并重写同一个索引键；在 BUILDING 期间，这会变成额外的 delete/modify 操作日志。

源码：src/storage/exec/UpdateNode.h:395–495（点），775–843（边）。

### 6.3 DELETE TAG / DELETE VERTEX / DELETE EDGE

删除需要知道旧索引属性，因此先读旧数据，从旧行计算索引键，把索引删除和数据删除放在批次中。删边时只在实际 edgeType 匹配元数据正类型时处理索引，反向记录不再删一套负类型索引。

源码：src/storage/mutate/DeleteVerticesProcessor.cpp:145–177；src/storage/mutate/DeleteTagsProcessor.cpp:117–156；src/storage/mutate/DeleteEdgesProcessor.cpp:160–229。这些说明是存储处理器级别；图层的 DELETE VERTEX 还要组织相关边删除，不等于只删一个 tag KV。

## 7. BUILDING、LOCKED 与 operation 日志

### 7.1 状态机与作用范围

StorageEnv 中的 IndexGuard 是 ConcurrentHashMap<(GraphSpaceID, PartitionID), IndexState>，没有 indexId 维度。不存在 guard 条目时视为 FINISHED。

| 状态 | 写路径对匹配索引的处理 | 重建侧的用途 |
| --- | --- | --- |
| STARTING | 不属于 checkRebuilding/checkIndexLocked，通常仍按正常索引处理 | 任务预留/开始 |
| BUILDING | 属性数据照常进入原子批次，索引修改改记 operation KV | 全量扫描与后续增量回放 |
| LOCKED | 相关索引写路径返回失败/冲突，不继续完成该批次 | 临时阻止新索引变更，排空在途请求及尾部日志 |
| FINISHED | 直接维护索引 KV | 重建退出后恢复正常 |

LOCKED 的具体错误在不同写路径不完全相同：点插入显式设置 E_DATA_CONFLICT_ERROR；AddEdges 的该分支返回其初始化的失败结果；UpdateNode 返回无批次。不能把所有路径的对外表现概括为同一个错误码。

源码：src/storage/CommonUtils.h:46–100；src/storage/mutate/AddVerticesProcessor.cpp:284–287、311–314；src/storage/mutate/AddEdgesProcessor.cpp:228–230、279–280、304–306；src/storage/exec/UpdateNode.h:448–450、478–480。

### 7.2 operation KV 的精确格式

这里的 operation 是存在同一 space、partition 中的独立 KV 类型，不等于 RocksDB WAL 文件，也不等于 Raft 日志本身。它的持久化当然仍经过底层写入/复制路径，但不能用 WAL 目录大小直接代替该前缀积压条数。

OperationKeyUtils::modifyOperationKey 与 deleteOperationKey 的定义为：

    modify key = H(operation)[4] + timestampUsBigEndian[8] + typeModify[4] + indexKey[K]
    modify value = indexValue[T]

    delete key = H(operation)[4] + timestampUsBigEndian[8] + typeDelete[4]
    delete value = indexKey[K]

timestamp 来自 WallClock::fastNowInMicroSec；operation type 以本机 4 字节数追加，modify=1、delete=2。operationPrefix 只有 H(operation) 的 4 字节，因此一次前缀扫描包含该分区所有索引的操作日志。

时间戳放在 type 和实体键前面，使不同时间戳的记录按时间编码排列。不要据此宣称实现提供了全局唯一序列号：delete key 本身没有实体后缀，也没有额外自增序号。时间戳碰撞属于另一类正确性核查点，本文不将其作为本次长耗时的已复现原因。

源码：src/common/utils/OperationKeyUtils.cpp:13–37、41–67；src/common/utils/Types.h:32–35。

### 7.3 日志数量和回放批次字节数为何容易被低估

BatchHolder::size 的统计是 put 的 key+value 字节、remove 的 key 字节之和，不是操作条数，也不包含编码器额外开销。回放每个 operation 时既修改真实索引，又删除该 operation 键：

| 一条待回放记录 | operation 存储大小 | 对回放 BatchHolder::size 的增量 |
| --- | --- | --- |
| modify | 16 + K + T | (K + T) + (16 + K) = 16 + 2K + T |
| delete | 16 + K | K + 16 |
| 旧行更新（同等长索引的 delete + modify） | 32 + 2K + T | 32 + 3K + T |

K 是完整索引键宽度，T 是索引 value 的宽度。普通无 TTL 时 T=0。

本分支有两组需要区分的参数来源：

| 参数来源 | rebuild_index_batch_size | rebuild_index_part_rate_limit（同时作为 rate/burstSize 参数） | 证据 |
| --- | --- | --- | --- |
| C++ 编译默认，无外部覆盖时 | 131072 B = 128 KiB | 524288 B/s = 512 KiB/s | src/storage/StorageFlags.cpp:25–29 |
| 随仓库提供的 .default 配置 | 1048576 B = 1 MiB | 4194304 B/s = 4 MiB/s | conf/nebula-storaged.conf.default:124–127 |
| 随仓库提供的 .production 配置 | 1048576 B = 1 MiB | 4194304 B/s = 4 MiB/s | conf/nebula-storaged.conf.production:125–128 |

batch 是触发批次提交的字节阈值；rate 是每秒字节速率，传入 RateLimiter 时它的数值也被用作 burst 字节上限。启动脚本以 --flagfile 指定实际配置文件（scripts/nebula.service:147）。若安装部署使用上述模板值，生效的是 1 MiB / 4 MiB，而非 C++ 的 128 KiB / 512 KiB；实际部署还可能有定制配置或其他参数覆盖，应以运行进程实际采用的参数为准。不能仅凭仓库编译默认或文件名推定生产值。

例：VID 宽 8、索引只有一个 INT NOT NULL、无 TTL。点索引 K=4+4+8+8=24 字节；每条新点 modify 在回放批次中增加 64 字节。按 C++ 编译默认，128 KiB 批次阈值在第 2049 条严格越界；因为当前代码中同一轮的 BatchHolder 持续累计，512 KiB 的限流 burst 阈值在第 8193 条严格越界。边索引 K=4+4+8+8+8+8=40 字节，每条 modify 增加 96 字节，两个阈值分别在第 1366、5462 条越界。若采用仓库配置模板的 1 MiB / 4 MiB，上述点阈值改为第 16385 / 65537 条，边阈值改为第 10923 / 43691 条。若该 INT 可空，应额外计入 2 字节 bitmap，阈值条数随之下降。这些是等宽、纯 modify 场景的计算示例，不代表本次生产或本地复现的实际索引宽度。

因此不能把字节配置理解成条数，也不能只累加新索引键本身来估算是否触发回放批次分支。RateLimiter::consume 在 toConsume > burstSize 时每次固定等待 1 秒；要分析长期不结束，应分别看何时开始重复批次提交、何时每次提交都再等待 1 秒。

源码：src/kvstore/LogEncoder.h:159–174、203–216；src/storage/admin/RebuildIndexTask.cpp:155–184、264–267；src/storage/StorageFlags.cpp:25–29；src/kvstore/RateLimiter.h:40–49；键宽来源 src/common/utils/IndexKeyUtils.cpp:87–132。

### 7.4 全量扫描期间为什么积累 backlog

rebuildIndex 先清理遗留 operation 前缀，再将状态设为 BUILDING，调用 buildIndexGlobal，结束后才调用 buildIndexOnOperations。全量扫描逐行读旧视图并批量写索引，期间前台写路径持续写 operation KV，但该阶段没有消费者同时回放这些 KV。

底层 prefix 创建 RocksDB NewIterator；常规 iterator 提供创建时的一致读视图，代码没有启用 tailing iterator。新写的数据不能靠不断调用原迭代器 next 来保证纳入同一轮扫描；同理，回放迭代器创建后新加入的日志要由下一轮新建前缀迭代器处理。即使已回放日志从数据库删除，当前已有迭代器的稳定视图也不会重新变成最新实时列表。

仓库连接点：src/kvstore/RocksEngine.cpp:206–254；src/storage/admin/RebuildTagIndexTask.cpp:53–66、128–143；src/storage/admin/RebuildEdgeIndexTask.cpp:53–66、137–157；src/storage/admin/RebuildIndexTask.cpp:98–123、141–155。本机编译使用的第三方头文件 /opt/vesoft/third-party/3.3/include/rocksdb/options.h:1457–1462 明确说明 snapshot=nullptr 使用读操作开始时的隐式快照，1531–1537 行说明 tailing 是需要单独指定的模式。

积压量的第一阶估计是“全量期间写入持续时间 × 每秒涉及索引的变更数 × 每次变更展开日志数”，再乘各类键宽。还应考虑以下源码事实：

- 全量扫描需要遍历无关 tag/edge 行，反向边也先扫描到再跳过。
- 对该分区的所有匹配索引维护都受同一 BUILDING 状态控制，不限于任务选中的 indexId。
- 覆盖/更新有旧值时通常产生删旧和建新两种记录。
- 更新非索引字段也可重复维护索引。
- geography 一个值可能展开多个 cell key。

这解释了为什么业务新增数据并不算很多，回放却会在全量阶段结束时已经拥有跨过批次阈值的积压。更具体的超线性重复提交缺陷和实测结果应结合本目录根因/复现报告阅读。

### 7.5 尾部排空不是“全量扫描结束就完成”

设计意图是：回放一轮 operation；当本轮字节数不大于批次阈值时，将 BUILDING 切到 LOCKED；等待在途索引写入计数变为零；再新建迭代器回放尾部日志；符合退出条件后完成。onFlyingRequest_ 是 StorageEnv 上的计数器，本身没有分区维度；IndexCountWrapper 用 RAII 增减。

这是另一个排查位置：任务可能已经过了全量与大批量回放，停在等待在途请求的循环。必须用阶段日志或线程栈区分，不能把所有长期 RUNNING 都归为同一个位置。本文不声称生产已经发生这个等待问题。

源码：src/storage/CommonUtils.h:70–76、103–130；src/storage/admin/RebuildIndexTask.cpp:190–213。

## 8. 查询端：扫描、过滤和回表

Graph 层 IndexScanExecutor 向 StorageClient::lookupIndex 传入 indexId、column hints、schemaId、返回列、排序与 limit。存储 LookupProcessor 根据点/边创建 IndexVertexScanNode 或 IndexEdgeScanNode，必要时在上面加 IndexSelectionNode。

源码：src/graph/executor/query/IndexScanExecutor.cpp:69–92；src/storage/index/LookupProcessor.cpp:179–216。

完整读取过程：

1. 用 indexId 从元数据缓存拿到字段定义与所属 schema。
2. 根据 hints 构造 PrefixPath 或 RangePath。没有 hint 时，也可以按 H(index)+indexId 全扫描该索引前缀。
3. 为当前分区替换 H 中的 partitionId，调用 KV prefix/range。
4. 对每个索引候选先检查 TTL，再做 NULL、截断边界、NaN、地理去重等资格判断。
5. 若候选确定满足扫描条件，且所需列可完整从索引取得，直接 decodeFromIndex；否则用实体标识构建原始数据键并 get。
6. 需要时用完整行再次校验 hint，输出请求列；额外 filter 由上层节点处理。

源码：src/storage/exec/IndexScanNode.cpp:40–50、376–497；src/storage/index/LookupProcessor.cpp:210–214。

点索引尾部 VID 直接回到 tagKey(VID, tagId)；边索引尾部 src/rank/dst 回到 edgeKey(src, edgeType, rank, dst)。索引字段里的字符串前缀无法取代完整字符串属性，S2 cell 也无法还原原地理形状。所需普通数值列都在索引中时则可以覆盖读取，避免回表。

源码：src/storage/exec/IndexVertexScanNode.cpp:55–79；src/storage/exec/IndexEdgeScanNode.cpp:50–91；src/storage/exec/IndexScanNode.cpp:392–415。

举例：索引 person(age, name(5)) 下，查询 age=20 且只返回 VID、age，可满足索引覆盖读取的基本条件；如果要返回完整 name，则当前扫描器需要回表；name 的前 5 字节与谓词相同但原始完整字符串不同的行，回表过滤后不会应当被当作等值命中。实际是否采用该索引还需看执行计划，这里展示的是扫描器能力。

BUILDING 时普通索引维护已转为 operation 记录，查询扫描器却读取真实 index 前缀，并不把未回放 operation 日志实时合并进查询结果。直接索引读取也不会逐条核对原始行。因而不能仅凭“有一部分 LOOKUP 结果”来证明重建已经完整成功，或把重建中途的索引视图当作完成态索引来校验。

证据链：AddVerticesProcessor.cpp:304–318（分支），RebuildIndexTask.cpp:161–173（回放），IndexScanNode.cpp:426–468（读取）。

## 9. 源码阅读与生产证据的边界

从本仓库可以确定键格式、状态范围、日志生成与消费的阶段、回放批次的字节计数方式，以及指定控制流中的性能缺陷。没有生产 job/task 明细、相应 storaged 日志、实际索引 schema、流量和线程栈时，不能仅凭“运行两天”排除任务排队、RPC/状态上报问题、磁盘/复制阻塞或其他阶段等待。

对生产实例应把以下证据与实测复现对应：重建分区的全量完成时间、开始 Processing operation logs 的时间、operation 回放批次大小及提交次数、业务是否持续写入、索引字段宽度/nullable/TTL、是否已进入 LOCKED、任务中尚未完成的具体 host/part。机制报告提供的是解释这些现象的源码依据；生产根因确认依赖与实际故障阶段匹配。

## 10. 对 RebuildIndexTest.cpp 的只读审查：为何没有直接揭示本次规模问题

本节只审查 src/storage/test/RebuildIndexTest.cpp 的现有用例，没有运行测试，也不据此断言全仓库或历史 CI 完全没有相关覆盖。

- 基础点、边用例先写入 mock 数据，再提交重建任务，最后检查索引键或计数。文件明确校验 81 条 tag 数据、162 条点索引，以及 334 条边物理记录、334 条边索引；任务分布到 6 个分区。这类校验侧重结果存在性/数量，不等于积累了跨越批次和 burst 阈值的 operation backlog（65–131、152–219、361–400、535–571 行）。全量扫描总行数本身也不是 operation 条数。
- 两个 WithDelete 用例先调用 writer->addTask(delete...).get() 等待删除完成，再创建并提交重建任务。因此它们的名字和 Background Writer 日志不能作为“在 BUILDING 中并发删除”的证据；该时序没有主动制造重建期 delete 积压（点 242–278 行，边 413–447 行）。
- WithAppend 存在异步写入，但点测试是在 addAsyncTask 后提交一次追加，边测试则先提交一次追加再准备重建。它们都没有等待分区明确进入 BUILDING 的同步屏障，也没有持续产生达到目标字节数的操作日志，所以是否进入目标重建阶段取决于调度，无法稳定覆盖大 backlog 条件（点 300–343 行，边 475–516 行）。
- 本文件没有对每轮回放 BatchHolder 的最大字节数、累计提交字节、同一操作重复提交次数、越过 burst 后的睡眠次数作断言，也没有在用例里显式设置小阈值以强制跨界。等待完成使用循环轮询，没有本地耗时上界断言（例如 94–97、340–343、513–516 行）。文件末 main 会初始化命令行参数，因此外部测试启动参数仍可能改变 flags；只读这一个文件不能反推所有实际测试运行配置（582–587 行）。

由这些事实可以解释：小规模、单次、以结果为主的现有用例即使通过，也不能反证“operation 回放跨阈值后，累积批次重复提交导致长耗时”。尤其重复 put/remove 在许多场景下最终值仍然正确，性能放大不会必然表现为这些结果断言失败。本次复现需要单独证实 BUILDING 内日志生成、回放字节跨界和重复提交，而不能只重跑全量索引计数用例作为充分证据。
