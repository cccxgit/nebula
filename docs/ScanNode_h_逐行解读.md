# `src/storage/exec/ScanNode.h` 逐行解读分析

> 目标：对 `ScanNode.h` 的结构、关键逻辑与每段代码职责做“按代码顺序”的详细说明，帮助你快速建立对扫描执行节点（Scan Vertex / Scan Edge）的完整认知。

---

## 1. 文件定位与总体职责

该头文件定义了存储层查询执行计划中的两个节点：

- `ScanVertexPropNode`：按分区扫描顶点属性。
- `ScanEdgePropNode`：按分区扫描边属性。

二者都继承自 `QueryNode<Cursor>`，都通过 KV 前缀扫描（`rangeWithPrefix`）遍历底层存储，并把读取到的数据按查询请求拼成 `DataSet` 行，同时支持：

- limit 限制
- 游标（cursor）分页续扫
- filter 过滤表达式
- 读 follower 副本（可选）

---

## 2. 逐段解读（按源码顺序）

### 2.1 头部与 include

- 版权和 Apache 2.0 许可声明：标准项目头。
- include guard：`STORAGE_EXEC_SCANNODE_H`，防止重复包含。
- `#include "common/base/Base.h"`：基础类型、日志、工具等。
- `#include "storage/exec/GetPropNode.h"`：依赖 `TagNode` / `FetchEdgeNode` / `PropContext` 等属性提取节点定义。

### 2.2 命名空间和别名

- `namespace nebula { namespace storage { ... } }`
- `using Cursor = std::string;`
  - 扫描续读游标本质就是字符串 key（通常是下一次扫描起始 key）。

---

## 3. `ScanVertexPropNode` 详解

### 3.1 类声明

```cpp
class ScanVertexPropNode : public QueryNode<Cursor>
```

- 表示“扫描某个 partition 的顶点属性”的执行节点。
- 模板参数 `Cursor` 表示 `doExecute` 的第二个参数类型。

### 3.2 `using RelNode<Cursor>::doExecute;`

- 把父类中的重载 `doExecute` 显式引入当前作用域，避免被当前 `doExecute(partId, cursor)` 隐藏。

### 3.3 构造函数参数含义

- `RuntimeContext* context`：运行上下文（spaceId、env、vid 类型等）。
- `std::vector<std::unique_ptr<TagNode>> tagNodes`：每个 tag 的读取节点。
- `bool enableReadFollower`：是否允许读 follower。
- `int64_t limit`：结果上限。
- `std::unordered_map<PartitionID, cpp2::ScanCursor>* cursors`：输出每个分区的续扫游标。
- `nebula::DataSet* resultDataSet`：输出结果容器。
- `StorageExpressionContext* expCtx`：表达式上下文（过滤时注入属性）。
- `Expression* filter`：过滤表达式。

### 3.4 构造函数初始化逻辑

- 成员初始化后，`name_ = "ScanVertexPropNode"`。
- 遍历 `tagNodes_`，建立 `tagId -> index` 映射 `tagNodesIndex_`：
  - 扫描时可 O(1) 找到某 tag 对应的 `TagNode`，避免线性查找。

### 3.5 `doExecute(partId, cursor)` 主流程

#### (1) 执行父节点

```cpp
auto ret = RelNode::doExecute(partId);
```

- 先跑依赖节点（若有）。失败直接返回。

#### (2) 计算扫描起点与前缀

- `prefix = NebulaKeyUtils::tagPrefix(partId)`：当前分区的 tag key 前缀。
- 若传入 `cursor` 为空，则从 `prefix` 开始全量扫；否则从游标 key 继续扫。

#### (3) 拉取 KV 迭代器

```cpp
rangeWithPrefix(spaceId, partId, start, prefix, &iter, enableReadFollower_)
```

- 在同一前缀范围内迭代；失败返回 KV 错误码。

#### (4) 扫描循环核心

循环条件：
- `iter->valid()`
- `resultDataSet_->rowSize() < rowLimit`

循环内关键步骤：

1. 取 `key`，解析 `tagId`。
2. 如果 `tagId` 不在请求集合（`tagNodesIndex_`）中，跳过。
3. 取 `vertexId`。
4. **按 vertex 分组聚合**：
   - 若当前 key 的 `vertexId` 与上一条 `currentVertexId` 不同，且旧 ID 非空，说明上一个顶点的数据段结束，调用 `collectOneRow(...)` 把上一顶点写成一行。
5. 更新 `currentVertexId = vertexId`。
6. 读取 `value`，交给对应 `TagNode` 的 `doExecute(key, value)` 累积属性状态。

> 这个设计依赖底层 key 的有序性：同一顶点不同 tag 的记录会相邻，从而能在“顶点切换时”完成聚合出行。

#### (5) 循环后收尾

- 若结果还未到 limit，再对最后一个 `currentVertexId` 调一次 `collectOneRow`（处理“循环结束但最后一个顶点尚未 flush”）。

#### (6) 写回 cursor

- `iter->valid()` 仍为 true，说明是因为达到 limit 或人为中断，记录 `next_cursor = iter->key()` 便于下次续扫。
- `cursors_->emplace(partId, std::move(c))` 保存分区级 cursor。

### 3.6 `collectOneRow(...)` 顶点行组装

#### (1) 初始化

- 新建 `List row`。
- 首列固定为 vertexId：
  - `isIntId` 时把字节 reinterpret 成 `int64_t`。
  - 否则按字符串写入。

#### (2) 是否有有效 tag

```cpp
std::any_of(tagNodes_.begin(), tagNodes_.end(), ... valid())
```

- 如果一个有效 tag 都没有（例如 TTL 过期导致节点无效），整行不写入。

#### (3) 遍历每个 `TagNode` 收集属性

`collectOneRow` 的核心是调用：

```cpp
tagNode->collectTagPropsIfValid(nullHandler, valueHandler)
```

而 `TagNode::collectTagPropsIfValid` 的分发规则是：

- `valid() == false`：调用 `nullHandler(props_)`
- `valid() == true`：调用 `valueHandler(key_, reader_.get(), props_)`

这套分发可以在 `TagNode.h` 看到，逻辑非常直接：无效就走“空值分支”，有效就走“真实值分支”。

---

##### A. `valid()==false` 时：`nullHandler` 细节

`nullHandler` 对每个属性 `prop` 做两件事：

1. 如果 `prop.returned_ == true`：
   - 向 `row` 里 append 一个 `Value()` 空占位。
   - 目的：保持返回列位次稳定（哪怕该 tag 记录无效、TTL 过期或 decode 失败）。

2. 如果 `prop.filtered_ == true && expCtx_ != nullptr`：
   - 向表达式上下文写入该 tag/prop 的空值：
     `expCtx_->setTagProp(tagName, propName, Value())`。
   - 目的：让 filter 求值阶段能“看到字段存在但值为空”，而不是缺失键。

`nullHandler` **不会**触发任何 `readVertexProp`，因为 `valid()==false` 时 reader 不可用。

---

##### B. `valid()==true` 时：`valueHandler` 细节

`valueHandler` 处理路径更“重”：

1. 筛选是否需要读取：
   - 仅当 `prop.returned_` 或（`prop.filtered_ && expCtx_!=nullptr`）时才读取。
   - 这是性能优化：不返回也不参与过滤的列不解码。

2. 实际读取：
   - 调 `QueryUtils::readVertexProp(key, vIdLen, isIntId, reader, prop)`。
   - 若读取失败，立即返回 `E_TAG_PROP_NOT_FOUND`，上层终止本行处理。

3. 写回过滤上下文：
   - 若 `prop.filtered_`，把真实值注入 `expCtx_`。

4. 写回结果行：
   - 若 `prop.returned_`，把真实值 append 到 `row`。

---

##### C. 两条分支的关键差异（你最关心）

1. **数据来源**
   - `nullHandler`：不读存储，直接填空。
   - `valueHandler`：从 reader 解码真实值。

2. **错误语义**
   - `nullHandler`：基本不会产生属性缺失错误。
   - `valueHandler`：读失败会返回 `E_TAG_PROP_NOT_FOUND`。

3. **结果列内容**
   - `nullHandler`：returned 列是空值占位。
   - `valueHandler`：returned 列是实际属性值。

4. **过滤输入**
   - `nullHandler`：filtered 列注入空值。
   - `valueHandler`：filtered 列注入真实值。

---

##### D. 为什么这样设计

- 统一列对齐：无论 tag 是否有效，返回列数和顺序稳定。
- 统一 filter 行为：无论数据是否缺失，表达式求值上下文都完整可求值。
- 统一调用接口：上层不用关心 `TagNode` 是否有效，只需注册两种 handler。

> 这也是 `collectTagPropsIfValid` 的价值：把“有效/无效分支控制”封装在 `TagNode` 内部，把“该怎么填 row / expCtx”交给上层回调实现。

#### (4) filter 处理

- 若 `filter_ == nullptr`：直接入结果。
- 否则执行：
  - `filter_->eval(*expCtx_)`
  - `QueryUtils::vTrue(...)` 转布尔真值
  - 为真才入 `resultDataSet_`。

#### (5) 清理状态

- `expCtx_->clear()` 清理表达式上下文。
- 每个 `tagNode->clear()` 清理本轮缓存。

> 注意：这里默认 `expCtx_` 可用；虽然参数允许 `nullptr`，但末尾直接 `expCtx_->clear()`，意味着调用方在使用 filter 路径时应保证上下文存在。

### 3.7 成员变量语义

- `context_`：运行时全局上下文。
- `tagNodes_`：每个 tag 的执行节点。
- `tagNodesIndex_`：`tagId -> tagNodes_` 下标。
- `enableReadFollower_`：是否 follower 读。
- `limit_`：全局输出上限。
- `cursors_`：续扫游标输出。
- `resultDataSet_`：结果集。
- `expCtx_`：过滤表达式上下文。
- `filter_`：过滤表达式 AST。

---

## 4. `ScanEdgePropNode` 详解

`ScanEdgePropNode` 与顶点版本结构高度对称，差异主要在“边 key 解析”和“逐 key 出行（而不是按 vertex 聚合）”。

### 4.1 构造函数

- 参数与顶点版本对应，只是节点类型换成 `FetchEdgeNode`。
- 建立 `edgeType -> index` 的 `edgeNodesIndex_`。
- `name_` 设为 `ScanEdgePropNode`。

### 4.2 `doExecute(partId, cursor)`

#### (1) 父节点执行

- 与顶点节点一致。

#### (2) 前缀与起点

- `prefix = NebulaKeyUtils::edgePrefix(partId)`。
- cursor 为空则从 prefix 开始，否则从 cursor 续扫。

#### (3) KV 扫描

- `rangeWithPrefix(context_->spaceId(), ...)` 获取迭代器。

#### (4) 循环扫描逻辑

每次迭代：

1. 先检查 `NebulaKeyUtils::isEdge(vIdLen, key)`，不是 edge key 就跳过。
2. 解析 `edgeType`，若不在 `edgeNodesIndex_` 则跳过。
3. 调对应 `edgeNode->doExecute(key, value)`。
4. 立刻 `collectOneRow(...)`。

> 与顶点扫描不同：边扫描通常“一条边记录对应一行”，不需要跨多条记录按 ID 聚合。

#### (5) cursor 回填

- 同顶点逻辑，若迭代未完记录 `next_cursor`。

### 4.3 `collectOneRow(...)` 边行组装

#### (1) 空有效节点短路

- 若所有 `edgeNodes_` 均无效（例如 TTL 过期），直接返回，不写空行。

#### (2) 收集属性

- 调 `edgeNode->collectEdgePropsIfValid`，同样两个 lambda：
  - 无 reader：returned 放空值、filtered 写空上下文。
  - 有 reader：`QueryUtils::readEdgeProp(...)` 读值，filtered 写 `expCtx_`，returned 写 `row`。
- 属性缺失报 `E_EDGE_PROP_NOT_FOUND`。

#### (3) 过滤与入集

- 无 filter 直接插入；有 filter 则 eval 后真值通过才插入。

#### (4) 清理

- `expCtx_->clear()` + 每个 `edgeNode->clear()`。

### 4.4 成员变量

与顶点版本一一对应，只是 `edgeNodes_` 和 `edgeNodesIndex_` 类型不同。

---

## 5. 关键设计观察（实现意图）

1. **前缀扫描 + 游标续扫**
   - 通过 `rangeWithPrefix` 和 `next_cursor` 支持大表分批扫描。

2. **执行节点复用 GetProp 子节点**
   - `TagNode`/`FetchEdgeNode` 负责单条记录属性解析；`Scan*Node` 负责扫描驱动和行拼装。

3. **返回列与过滤列解耦**
   - `returned_` 决定是否出现在结果。
   - `filtered_` 决定是否注入 `expCtx_` 供表达式求值。

4. **TTL/无效数据防空行**
   - `any_of(valid())` 保护逻辑避免输出全空行。

5. **顶点扫描的“跨 tag 聚合”**
   - 依赖 key 排序，在 vertex 切换时 flush。

6. **边扫描的“逐条产行”**
   - 每次迭代直接产出，结构更直。

---

## 6. 易错点与阅读建议

1. `expCtx_` 空指针风险：
   - 虽多处判空，但尾部 `expCtx_->clear()` 未判空，调用者应确保传入有效上下文。

2. `vertexId` 整数转换：
   - `reinterpret_cast<const int64_t*>` 假设底层编码与对齐符合预期，这是 Nebula 内部 key 编码约定的一部分。

3. `rowLimit` 的作用范围：
   - 限制的是 `resultDataSet_` 最终行数，而不是 KV 扫描条数。

4. 错误传播：
   - 属性读取失败会立即返回错误码，中断当前分区扫描。

---

## 7. 一句话总结

`ScanNode.h` 的两个执行节点本质是“**KV 前缀遍历器 + 属性解码器 + 行构造器 + 过滤器 + 分页器**”：顶点版本多一步按 vertex 聚合，边版本则逐 key 直接出行。
