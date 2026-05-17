# Yelp Open Dataset 到 NebulaGraph 的导入与建模方案

## 1. 目标

本文档用于说明如何将 `Yelp Open Dataset` 设计成适合 NebulaGraph 的图模型，并为后续高价值 AI 测试场景提供数据基础。

## 2. 建议图空间

建议创建图空间：

```ngql
CREATE SPACE IF NOT EXISTS yelp_graph(
  vid_type = FIXED_STRING(128),
  partition_num = 20,
  replica_factor = 1
);
```

说明：

- `FIXED_STRING(128)` 便于直接使用业务自然键
- 开发验证阶段可使用 `replica_factor = 1`

## 3. 建议 VID 策略

为了避免不同实体类型 ID 冲突，建议统一做前缀化：

- 用户：`user_<user_id>`
- 商户：`biz_<business_id>`
- 评论：`review_<review_id>`
- 品类：`cat_<normalized_category_name>`
- 城市：`city_<normalized_city_name>`

这样有三个好处：

1. 可读性更强
2. 不同实体不会冲突
3. 更利于自然语言解释

## 4. 顶点模型设计

### 4.1 `user`

```ngql
CREATE TAG IF NOT EXISTS user(
  raw_user_id STRING NOT NULL,
  review_count INT64,
  average_stars DOUBLE,
  useful INT64,
  funny INT64,
  cool INT64,
  fans INT64
);
```

### 4.2 `business`

```ngql
CREATE TAG IF NOT EXISTS business(
  raw_business_id STRING NOT NULL,
  name STRING NOT NULL,
  stars DOUBLE,
  review_count INT64,
  city STRING,
  state STRING,
  postal_code STRING,
  latitude DOUBLE,
  longitude DOUBLE,
  is_open BOOL
);
```

### 4.3 `review`

```ngql
CREATE TAG IF NOT EXISTS review(
  raw_review_id STRING NOT NULL,
  stars DOUBLE,
  review_date STRING,
  text STRING
);
```

### 4.4 `category`

```ngql
CREATE TAG IF NOT EXISTS category(
  category_name STRING NOT NULL
);
```

### 4.5 `city`

```ngql
CREATE TAG IF NOT EXISTS city(
  city_name STRING NOT NULL,
  state STRING
);
```

## 5. 边模型设计

### 5.1 `WROTE`

```ngql
CREATE EDGE IF NOT EXISTS WROTE(
  review_date STRING,
  stars DOUBLE
);
```

含义：

- 用户写了某条评论

### 5.2 `REVIEWS`

```ngql
CREATE EDGE IF NOT EXISTS REVIEWS(
  stars DOUBLE
);
```

含义：

- 评论对应某个商户

### 5.3 `HAS_CATEGORY`

```ngql
CREATE EDGE IF NOT EXISTS HAS_CATEGORY();
```

含义：

- 商户属于某个品类

### 5.4 `LOCATED_IN`

```ngql
CREATE EDGE IF NOT EXISTS LOCATED_IN();
```

含义：

- 商户位于某个城市

### 5.5 `SIMILAR_TO`

```ngql
CREATE EDGE IF NOT EXISTS SIMILAR_TO(
  score DOUBLE
);
```

含义：

- 商户与商户之间的相似关系
- 可由离线算法或规则推导生成

### 5.6 `CO_VISITED`

```ngql
CREATE EDGE IF NOT EXISTS CO_VISITED(
  score DOUBLE
);
```

含义：

- 基于用户共评或共访问行为生成的关联关系

## 6. 索引建议

### 6.1 Tag 索引

```ngql
CREATE TAG INDEX IF NOT EXISTS idx_business ON business();
CREATE TAG INDEX IF NOT EXISTS idx_business_name ON business(name(128));
CREATE TAG INDEX IF NOT EXISTS idx_business_city ON business(city(64));
CREATE TAG INDEX IF NOT EXISTS idx_category ON category(category_name(128));
CREATE TAG INDEX IF NOT EXISTS idx_city ON city(city_name(128));
```

### 6.2 是否建立全文索引

如果需要做评论语义检索或关键词检索，建议后续引入 Elasticsearch 做全文索引，而不是直接依赖原生精确匹配。

适用对象：

- `review.text`
- `business.name`
- 部分 category 文本

## 7. 导入顺序建议

建议按照以下顺序导入：

1. 创建 `space`
2. 创建 `tag`
3. 创建 `edge`
4. 等待 schema 同步
5. 导入顶点
6. 导入边
7. 再创建索引并 `REBUILD`

原因：

- 大数据导入时先建大量索引会拖慢加载速度

## 8. 推荐的数据处理流程

### 第一步：原始 JSON 清洗

将 Yelp 原始数据分成：

- 用户表
- 商户表
- 评论表
- 品类映射表
- 城市维表

### 第二步：生成 Nebula 导入文件

可转成 CSV 或 Nebula Importer 所需格式。

### 第三步：导入实体

- 用户顶点
- 商户顶点
- 评论顶点
- 品类顶点
- 城市顶点

### 第四步：导入关系

- 用户写评论
- 评论关联商户
- 商户属于品类
- 商户位于城市

### 第五步：构造派生关系

例如：

- `SIMILAR_TO`
- `CO_VISITED`

这一步是 AI 商业价值场景的关键增强项。

## 9. 为什么要保留 `review` 作为独立顶点

很多人会把评论直接做成边属性，但这里建议保留为独立顶点，原因是：

1. 评论文本是高价值分析对象
2. 评论本身具备时间、分值、内容等独立属性
3. 后续可做评论主题聚类、负面问题分析、增长趋势分析

如果只放在边属性中，后续分析会受限。

## 10. 场景支持映射

### 场景 1：推荐

依赖：

- `business`
- `category`
- `city`
- 评分与评论数

### 场景 2：竞品分析

依赖：

- `SIMILAR_TO`
- `HAS_CATEGORY`
- `LOCATED_IN`

### 场景 3：评论洞察

依赖：

- `review`
- `REVIEWS`
- `WROTE`

### 场景 4：选址分析

依赖：

- `business`
- `category`
- `city`

### 场景 5：风险识别

依赖：

- 商户营业状态
- 评论趋势
- 评论内容
- 评分趋势

## 11. 与当前 MCP 工具的适配方式

当前工具链可以直接支持：

- `list_spaces`
- `get_space_schema`
- `execute_query`
- `find_path`
- `find_neighbors`

其中：

- 基础图探索适合 `find_neighbors`
- 商户相似关系与多跳关系适合 `find_path`
- 聚合分析与筛选查询适合 `execute_query`

## 12. 建议扩展的业务工具

如果未来进一步增强 MCP Server，建议补充以下工具：

- `find_similar_businesses`
- `search_business_by_city_and_category`
- `analyze_negative_reviews`
- `find_market_gap_by_city`
- `rank_high_potential_businesses`

这样可以让商业场景直接变成一等工具，而不是完全依赖模型拼装原始 nGQL。

## 13. 结论

`Yelp Open Dataset` 非常适合作为 NebulaGraph 商业场景图谱的样例数据源。

它的优势在于：

- 商业语义强
- 关系结构丰富
- 适合图建模
- 适合展示 AI 价值

如果任务三需要继续推进到样例实现阶段，这份建模方案已经可以作为导入和 PoC 的设计基线。
