# 传统手写查询方式 vs 智能系统方式 价值对比

## 第 1 页：对比结论页

### 标题

传统手写查询方式 vs 智能系统方式

### 核心结论

当前系统的价值不在于“替代数据库”，而在于把图数据库从“专家工具”变成“业务可用能力”。

### 传统手写查询方式

- 需要先理解图模型
- 需要知道 tag、edge、属性路径
- 需要掌握 nGQL 语法和图遍历写法
- 需要自己判断是用 `MATCH`、`GO` 还是 `FIND PATH`
- 需要自己解释查询结果的业务意义

### 智能系统方式

- 用户只需要提出自然语言问题
- 系统自动识别业务意图
- 系统自动理解 schema 和关系结构
- 系统自动选择工具或生成查询
- 系统直接输出业务可读结果

### 一句话差异

传统方式是“人适应数据库”，智能系统方式是“数据库能力适应人”。

### 价值总结

1. 降低图数据库使用门槛
2. 降低 schema 和记忆成本
3. 提升探索效率
4. 提升业务人员可用性
5. 为推荐、竞对、选址、评论洞察等商业场景提供直接支撑

---

## 第 2 页：案例对比页

### 案例 1：找 Cleveland 评分高、评论多的餐馆

#### 自然语言问题

- 帮我找 Cleveland 评分高、评论多的餐馆

#### 传统手写 nGQL

```ngql
MATCH (b:business)-[:HAS_CATEGORY]->(cat:category), (b)-[:LOCATED_IN]->(c:city)
WHERE c.city.city_name == "Cleveland"
  AND cat.category.category_name == "Restaurants"
  AND b.business.review_count >= 20
WITH b.business.name AS name, b.business.stars AS stars, b.business.review_count AS review_count
RETURN name, stars, review_count
ORDER BY stars DESC, review_count DESC
LIMIT 10;
```

#### 差异

- 自然语言只表达业务意图
- 手写查询必须知道图关系、属性路径、排序限制和语法细节

### 案例 2：这个用户和这家店是什么关系

#### 自然语言问题

- 这个用户和这家店是什么关系

#### 传统手写 nGQL

```ngql
FIND ALL PATH WITH PROP
FROM "user__SEBcjCwgneOV1VV_vESfQ"
TO "biz_KhtmCNHqCH6cnxirsenndA"
OVER *
BIDIRECT
UPTO 3 STEPS
YIELD PATH AS p;
```

#### 差异

- 自然语言只问关系解释
- 手写查询需要开发者决定路径算法、方向、深度、是否保留属性

### 汇报结论

智能系统真正提升的不是“数据库执行速度”，而是：

- 业务问题到图查询之间的转化效率
- 非专家使用图数据库的可行性
- 图数据在企业内部的可消费性

### 汇报收口

对于专家用户，手写 nGQL 仍然重要。

但对于更广泛的研发、运营、分析和业务人员，智能系统方式更有推广价值，因为它让图数据库从“少数人可用”变成“更多人可用”。
