# Yelp Open Dataset 场景示例 nGQL 查询集

## 1. 说明

本文件给出基于 `yelp_graph` 的示例 nGQL，用于支撑任务三中“AI 商业价值”的典型场景演示。

以下语句以建议图模型为前提：

- `business`
- `user`
- `review`
- `category`
- `city`
- `WROTE`
- `REVIEWS`
- `HAS_CATEGORY`
- `LOCATED_IN`
- `SIMILAR_TO`

## 2. 基础探索类查询

### 2.1 查看所有商户相关 schema

```ngql
SHOW TAGS;
SHOW EDGES;
```

### 2.2 查看 `business` 结构

```ngql
DESCRIBE TAG business;
```

### 2.3 抽样查看某城市商户

```ngql
MATCH (b:business)-[:LOCATED_IN]->(c:city)
WHERE c.city.city_name == "Las Vegas"
RETURN b.business.name, b.business.stars, b.business.review_count
LIMIT 20;
```

## 3. 推荐场景

### 3.1 推荐某城市高评分高热度商户

```ngql
MATCH (b:business)-[:HAS_CATEGORY]->(cat:category),
      (b)-[:LOCATED_IN]->(c:city)
WHERE c.city.city_name == "Las Vegas"
  AND cat.category.category_name == "Steakhouses"
  AND b.business.review_count > 100
RETURN b.business.name, b.business.stars, b.business.review_count
ORDER BY b.business.stars DESC, b.business.review_count DESC
LIMIT 10;
```

### 3.2 查询某类商户中的高活跃优质商户

```ngql
MATCH (b:business)-[:HAS_CATEGORY]->(cat:category)
WHERE cat.category.category_name == "Coffee & Tea"
RETURN b.business.name, b.business.city, b.business.stars, b.business.review_count
ORDER BY b.business.review_count DESC, b.business.stars DESC
LIMIT 20;
```

## 4. 竞品与相似商户场景

### 4.1 查某商户的相似商户

```ngql
MATCH (b:business)-[s:SIMILAR_TO]->(b2:business)
WHERE id(b) == "biz_target_business"
RETURN b2.business.name, s.score, b2.business.stars, b2.business.review_count
ORDER BY s.score DESC
LIMIT 10;
```

### 4.2 在同城同品类中查竞品

```ngql
MATCH (b:business)-[:HAS_CATEGORY]->(cat:category),
      (b)-[:LOCATED_IN]->(c:city),
      (b2:business)-[:HAS_CATEGORY]->(cat),
      (b2)-[:LOCATED_IN]->(c)
WHERE id(b) == "biz_target_business"
  AND id(b2) != id(b)
RETURN b2.business.name, b2.business.stars, b2.business.review_count
ORDER BY b2.business.stars DESC, b2.business.review_count DESC
LIMIT 10;
```

## 5. 选址与市场空白分析场景

### 5.1 各城市某品类商户供给情况

```ngql
MATCH (b:business)-[:HAS_CATEGORY]->(cat:category),
      (b)-[:LOCATED_IN]->(c:city)
WHERE cat.category.category_name == "Desserts"
RETURN c.city.city_name, COUNT(b) AS biz_count, AVG(b.business.stars) AS avg_stars
ORDER BY biz_count ASC, avg_stars DESC
LIMIT 20;
```

### 5.2 找出供给较少但评分较高的潜力城市

```ngql
MATCH (b:business)-[:HAS_CATEGORY]->(cat:category),
      (b)-[:LOCATED_IN]->(c:city)
WHERE cat.category.category_name == "Coffee & Tea"
RETURN c.city.city_name, COUNT(b) AS biz_count, AVG(b.business.stars) AS avg_stars, SUM(b.business.review_count) AS total_reviews
ORDER BY avg_stars DESC, total_reviews DESC
LIMIT 20;
```

## 6. 评论洞察场景

### 6.1 查看某商户低分评论

```ngql
MATCH (r:review)-[:REVIEWS]->(b:business)
WHERE id(b) == "biz_target_business"
  AND r.review.stars <= 2
RETURN r.review.review_date, r.review.stars, r.review.text
LIMIT 20;
```

### 6.2 查看某类商户的差评样本

```ngql
MATCH (r:review)-[:REVIEWS]->(b:business)-[:HAS_CATEGORY]->(cat:category)
WHERE cat.category.category_name == "Hot Pot"
  AND r.review.stars <= 2
RETURN b.business.name, r.review.stars, r.review.text
LIMIT 30;
```

### 6.3 统计某品类低分评论数量

```ngql
MATCH (r:review)-[:REVIEWS]->(b:business)-[:HAS_CATEGORY]->(cat:category)
WHERE cat.category.category_name == "Hot Pot"
RETURN COUNT(CASE WHEN r.review.stars <= 2 THEN 1 END) AS low_score_reviews,
       COUNT(r) AS total_reviews;
```

## 7. 高潜商户识别场景

### 7.1 查找高评分但评论量尚未极高的成长型商户

```ngql
MATCH (b:business)-[:LOCATED_IN]->(c:city)
WHERE c.city.city_name == "Phoenix"
  AND b.business.stars >= 4.5
  AND b.business.review_count >= 30
  AND b.business.review_count <= 300
RETURN b.business.name, b.business.stars, b.business.review_count
ORDER BY b.business.stars DESC, b.business.review_count DESC
LIMIT 20;
```

### 7.2 识别细分品类中的高潜商户

```ngql
MATCH (b:business)-[:HAS_CATEGORY]->(cat:category)
WHERE cat.category.category_name == "Bubble Tea"
  AND b.business.stars >= 4.5
RETURN b.business.name, b.business.city, b.business.stars, b.business.review_count
ORDER BY b.business.review_count DESC
LIMIT 20;
```

## 8. 风险识别场景

### 8.1 查询疑似经营风险商户

```ngql
MATCH (b:business)-[:LOCATED_IN]->(c:city)
WHERE c.city.city_name == "Phoenix"
  AND b.business.is_open == false
RETURN b.business.name, b.business.stars, b.business.review_count
ORDER BY b.business.review_count DESC
LIMIT 20;
```

### 8.2 低评分高评论量商户识别

```ngql
MATCH (b:business)
WHERE b.business.stars <= 2.5
  AND b.business.review_count >= 100
RETURN b.business.name, b.business.city, b.business.stars, b.business.review_count
ORDER BY b.business.review_count DESC
LIMIT 20;
```

## 9. 路径和邻居场景

### 9.1 查看某商户周边关系

```ngql
MATCH (b:business)-[e]-(x)
WHERE id(b) == "biz_target_business"
RETURN type(e), x
LIMIT 20;
```

### 9.2 查某用户与某商户之间的行为关联路径

```ngql
FIND ALL PATH WITH PROP
FROM "user_target_user"
TO "biz_target_business"
OVER *
BIDIRECT
UPTO 4 STEPS
YIELD PATH AS p;
```

### 9.3 查某商户与某品类头部商户之间的相似路径

```ngql
FIND ALL PATH WITH PROP
FROM "biz_target_business"
TO "biz_head_business"
OVER *
BIDIRECT
UPTO 4 STEPS
YIELD PATH AS p;
```

## 10. MCP 调用建议

这些查询在当前系统里建议这样使用：

- 环境探索：`list_spaces`、`get_space_schema`
- 局部关系探索：`find_neighbors`
- 多跳关系查找：`find_path`
- 复杂筛选和聚合：`execute_query`

## 11. 结论

这组示例 nGQL 可以直接作为任务三演示和后续 PoC 的基础查询模板，用于把 `Yelp Open Dataset` 的商业价值场景映射到 NebulaGraph 智能交互能力上。
