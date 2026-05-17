# Yelp 图数据智能系统测试报告

## 1. 报告目的

本文档用于汇总以下工作结果：

1. Yelp 数据子集导入 NebulaGraph 的测试情况
2. 当前智能系统在 Yelp 图数据上的实际验证结果
3. 手写 nGQL 与自然语言问题交互方式的差异对比
4. 当前智能系统相对传统图数据库使用方式的价值分析

## 2. 测试背景

当前系统由以下组件组成：

- `Codex`
- `NebulaGraph-Skill`
- `nebulagraph-mcp-server`
- `NebulaGraph`

系统目标是让用户通过自然语言访问图数据库，而不必先掌握 NebulaGraph 的 schema、nGQL 语法细节和图遍历写法。

## 3. 数据来源与测试范围

### 3.1 数据来源

本次导入使用的是 Yelp Open Dataset 的公开 Cleveland 子集样本，包含：

- 商户数据
- 评论数据
- 用户数据

来源仓库：

- `ahegel/yelp-dataset`

该仓库说明其数据来自 Yelp Open Dataset，并已过滤为 Cleveland 区域样本。

### 3.2 测试范围

本次测试覆盖：

1. Yelp 子集导入 NebulaGraph
2. 图空间与 schema 校验
3. 基于 MCP 的工具调用测试
4. 基于真实业务语义的图查询测试
5. 交互方式价值对比测试

## 4. 数据导入结果

实际导入图空间：

- `yelp_graph`

实际导入结果如下：

- `business` 顶点：`400`
- `review` 顶点：`4385`
- `user` 顶点：`3154`
- `category` 顶点：`145`
- `city` 顶点：`1`

导入完成输出：

```text
Prepared subset: 400 businesses, 4385 reviews, 3154 users, 145 categories, 1 cities
Import completed successfully.
```

## 5. 图模型结构

### 5.1 顶点类型

- `business`
- `review`
- `user`
- `category`
- `city`

### 5.2 边类型

- `WROTE`
- `REVIEWS`
- `HAS_CATEGORY`
- `LOCATED_IN`

### 5.3 校验结果

`SHOW TAGS` 结果：

- `business`
- `category`
- `city`
- `review`
- `user`

`SHOW EDGES` 结果：

- `HAS_CATEGORY`
- `LOCATED_IN`
- `REVIEWS`
- `WROTE`

结论：

- 图模型创建成功
- 结构与预期一致

## 6. 智能系统测试结果

### 6.1 测试项一：`list_spaces()`

测试目标：

- 验证智能系统是否能发现新导入图空间

测试结果：

- 成功返回 `yelp_graph`

结论：

- 通过

### 6.2 测试项二：`get_space_schema("yelp_graph")`

测试目标：

- 验证智能系统是否能识别新图空间 schema

测试结果：

- 成功识别全部 5 类顶点和 4 类边

结论：

- 通过

### 6.3 测试项三：`find_neighbors()`

测试对象：

- `biz_uziDWXfTvCYlbQYrJT0iAQ`
- 商户名称：`Yours Truly Restaurant`

测试结果：

返回了一阶邻居，包括：

- `city_cleveland`
- `cat_restaurants`
- `cat_gluten-free`
- `cat_american_(traditional)`
- 多条 `review` 节点

结论：

- 通过

价值说明：

- 可快速查看某商户周边的品类、地理位置和评论关系

### 6.4 测试项四：`find_path()`

测试路径：

- `user__SEBcjCwgneOV1VV_vESfQ`
- `biz_KhtmCNHqCH6cnxirsenndA`

测试结果：

成功找到一条真实业务路径：

- `user -> review -> business`

结论：

- 通过

价值说明：

- 可以解释用户与商户的真实行为关系链

### 6.5 测试项五：`execute_query()`

测试问题：

- 查询 Cleveland 高评分且评论数较高的餐馆

测试结果示例：

- `Sabor Miami Cafe & Gallery`
- `Slyman's Restaurant`
- `Cuisine Du Cambodge`
- `George's Kitchen`
- `Nate's Deli & Restaurant`

结论：

- 通过

价值说明：

- 当前系统已能在真实业务图上支撑推荐与榜单型查询

## 7. 手写 nGQL 与自然语言问题对比

本节重点说明为什么当前智能系统有实际价值，而不只是“给数据库套了一层界面”。

### 7.1 场景一：查高评分高热度餐馆

#### 用户自然语言问题

- 帮我找 Cleveland 评分高、评论多的餐馆

#### 对应手写 nGQL

```ngql
MATCH (b:business)-[:HAS_CATEGORY]->(cat:category), (b)-[:LOCATED_IN]->(c:city) \
WHERE c.city.city_name == "Cleveland" \
  AND cat.category.category_name == "Restaurants" \
  AND b.business.review_count >= 20 \
WITH b.business.name AS name, b.business.stars AS stars, b.business.review_count AS review_count \
RETURN name, stars, review_count \
ORDER BY stars DESC, review_count DESC \
LIMIT 10;
```

#### 差异分析

用户自然语言只表达了业务意图：

- 城市
- 评分高
- 评论多
- 餐馆

而手写 nGQL 需要开发者额外掌握：

1. 图模型里城市不是字段过滤，而是 `LOCATED_IN -> city`
2. 餐馆不是直接类型，而是 `HAS_CATEGORY -> Restaurants`
3. NebulaGraph 的 `ORDER BY` 需要配合 `WITH` 别名处理
4. 属性访问必须写成 `c.city.city_name`、`b.business.review_count`

结论：

- 自然语言关注的是业务意图
- 手写 nGQL 关注的是图模型细节和语法细节

这就是智能系统的第一层价值：**把业务问题翻译成图查询实现。**

### 7.2 场景二：查某商户周边关系

#### 用户自然语言问题

- 这个商户周边有哪些关联信息

#### 对应手写 nGQL

```ngql
MATCH (b:business)-[e]-(x)
WHERE id(b) == "biz_uziDWXfTvCYlbQYrJT0iAQ"
RETURN type(e), x
LIMIT 20;
```

#### 差异分析

自然语言只描述：

- 某商户
- 周边关系

而手写 nGQL 需要知道：

1. 要用 `MATCH (b)-[e]-(x)` 做无向关系探索
2. 要知道商户的具体 VID
3. 要知道如何返回边类型和邻居节点

结论：

- 智能系统把“局部图探索”从图语法操作变成了可理解的业务动作

这就是智能系统的第二层价值：**把图遍历能力包装成自然语言可调用能力。**

### 7.3 场景三：解释用户与商户的关系

#### 用户自然语言问题

- 这个用户和这家店是什么关系

#### 对应手写 nGQL

```ngql
FIND ALL PATH WITH PROP \
FROM "user__SEBcjCwgneOV1VV_vESfQ" \
TO "biz_KhtmCNHqCH6cnxirsenndA" \
OVER * \
BIDIRECT \
UPTO 3 STEPS \
YIELD PATH AS p;
```

#### 差异分析

自然语言只表达了“关系解释”需求。

但手写 nGQL 需要开发者自己决定：

1. 用 `FIND PATH` 而不是 `MATCH`
2. 是否双向搜索
3. 最多搜索多少跳
4. 是否保留属性
5. 路径结果如何解释

结论：

- 智能系统可以直接把“关系解释”路由成图路径查询

这就是智能系统的第三层价值：**把抽象业务语义映射成图原生能力。**

## 8. 为什么手写 nGQL 不足以替代智能系统

从工程角度讲，手写 nGQL 并不是不能解决问题，但它有几个明显缺点：

1. 使用门槛高
   - 需要熟悉 schema、tag、edge、属性路径和 nGQL 语法
2. 迁移成本高
   - 新人、业务人员、分析师很难直接上手
3. 认知负担大
   - 业务问题必须先转换成图模型问题才能下手
4. 交互效率低
   - 探索性分析时需要频繁试错
5. 可解释性对业务人员不友好
   - 查询语句本身不能直接表达业务结论

而智能系统的价值恰恰在于弥补这些缺点。

## 9. 智能系统价值总结

结合本次测试，我认为当前智能系统相对传统手写 nGQL 的价值主要体现在五点：

### 9.1 降低图数据库使用门槛

业务人员可以从“直接写图查询”转向“直接提业务问题”。

### 9.2 降低 schema 认知成本

用户不需要先记住：

- 哪些是 tag
- 哪些是 edge
- 哪些字段在哪个实体上

### 9.3 提升探索效率

尤其在面对陌生图空间时：

- `list_spaces`
- `get_space_schema`
- `find_neighbors`

这条链路非常适合快速探索。

### 9.4 提升图能力的业务可用性

路径、邻居、多跳关系这些图数据库最有价值的能力，被直接转化成了业务可调用动作。

### 9.5 为商业化场景提供基础

例如：

- 推荐
- 竞对
- 选址
- 评论洞察
- 风险识别

这些场景都不适合要求业务方自己写 nGQL，但适合通过自然语言智能系统承接。

## 10. 最终结论

本次测试可以得出两个层面的结论。

### 10.1 技术结论

- Yelp 图数据已成功导入 NebulaGraph
- 当前 `Codex + Skill + MCP + NebulaGraph` 智能链路可稳定工作
- 已验证真实图数据上的探索、路径、邻居和业务查询能力

### 10.2 业务结论

与手写 nGQL 相比，当前智能系统最重要的价值不是“能不能查”，而是：

- 能否把业务问题直接映射成图分析动作
- 能否降低图数据库使用门槛
- 能否提升图数据在内部项目中的可消费性

因此，当前系统已经证明自己具备成为“智能图数据库交互层”的价值。

## 11. 建议

建议后续继续沿三个方向增强：

1. 增加更多业务专用图工具
2. 引入派生关系，如 `SIMILAR_TO`
3. 增强评论分析与结构化结果返回

这样可以进一步放大当前智能系统相对手写 nGQL 的优势。
