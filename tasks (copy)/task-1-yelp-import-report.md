# Yelp 数据导入与智能系统测试报告

## 1. 本次执行目标

本次工作的目标是：

1. 将 Yelp 数据集导入 NebulaGraph
2. 使用当前已经搭建好的 `Codex + NebulaGraph-Skill + nebulagraph-mcp-server` 智能系统做实际测试

## 2. 数据来源说明

本次实际导入使用的是 `Yelp Open Dataset` 的公开子集样本：

- 来源仓库：`ahegel/yelp-dataset`
- 子集说明：从 Yelp Open Dataset 过滤得到的 `Cleveland, Ohio` 样本

仓库中明确说明：

- 数据来自 Yelp Open Dataset
- 当前 `data/` 目录为 Cleveland 过滤子集

本次使用文件：

- `business_sample_cleveland.json`
- `review_sample_cleveland.json`
- `user_sample_cleveland.json`

说明：

- Yelp 官方全量 `Yelp-JSON.zip` 下载链路已确认可用
- 为了在当前周期内完成入库和智能验证，本次优先导入官方数据的公开派生子集

## 3. 导入策略

本次不是做“全量灌库”，而是做“高价值 PoC 子集导入”，原则包括：

1. 保留 Yelp 数据的真实业务语义
2. 保留用户-评论-商户-品类-城市关系
3. 优先满足智能查询、路径分析、邻居探索和商业演示
4. 控制导入规模，确保当前环境能稳定完成验证

## 4. 图模型

导入到图空间：

- `yelp_graph`

顶点类型：

- `user`
- `business`
- `review`
- `category`
- `city`

边类型：

- `WROTE`
- `REVIEWS`
- `HAS_CATEGORY`
- `LOCATED_IN`

## 5. 导入脚本

已新增脚本目录：

- [yelp_import/README.md](/home/sch/tasks/yelp_import/README.md:1)
- [yelp_import/import_yelp_subset.py](/home/sch/tasks/yelp_import/import_yelp_subset.py:1)
- [yelp_import/validate_yelp_graph.py](/home/sch/tasks/yelp_import/validate_yelp_graph.py:1)

## 6. 实际导入命令

本次执行命令：

```bash
NEBULA_HOST=127.0.0.1 \
NEBULA_PORT=9669 \
NEBULA_USER=root \
NEBULA_PASSWORD=nebula \
YELP_DATA_DIR=/tmp/yelp-dataset/data \
YELP_TARGET_CITIES=Cleveland \
~/.local/bin/uv run --directory /home/sch/tasks/nebulagraph-mcp-server \
python /home/sch/tasks/yelp_import/import_yelp_subset.py
```

## 7. 实际导入结果

脚本输出结果：

```text
Prepared subset: 400 businesses, 4385 reviews, 3154 users, 145 categories, 1 cities
Import completed successfully.
```

因此，本次导入结果为：

- 商户：`400`
- 评论：`4385`
- 用户：`3154`
- 品类：`145`
- 城市：`1`

## 8. 图数据校验

校验脚本：

```bash
NEBULA_HOST=127.0.0.1 \
NEBULA_PORT=9669 \
NEBULA_USER=root \
NEBULA_PASSWORD=nebula \
~/.local/bin/uv run --directory /home/sch/tasks/nebulagraph-mcp-server \
python /home/sch/tasks/yelp_import/validate_yelp_graph.py
```

核心结果：

- `business_count = 400`
- `review_count = 4385`

## 9. 智能系统测试

本次不是只验证“入库成功”，而是进一步用当前智能系统做了实际调用。

### 9.1 `list_spaces()`

结果包含：

- `yelp_graph`

说明：

- MCP 工具已能识别新导入图空间

### 9.2 `get_space_schema("yelp_graph")`

结果显示：

- `business`
- `category`
- `city`
- `review`
- `user`

以及：

- `HAS_CATEGORY`
- `LOCATED_IN`
- `REVIEWS`
- `WROTE`

说明：

- 智能系统已经能识别新图模型结构

### 9.3 `find_neighbors()`

测试对象：

- `biz_uziDWXfTvCYlbQYrJT0iAQ`
- 商户名称：`Yours Truly Restaurant`

结果返回了该商户的一阶邻居，包括：

- 城市节点 `city_cleveland`
- 品类节点如 `Restaurants`
- 多个评论节点

说明：

- 邻居探索能力正常
- 可直接用于商户周边关系解释

### 9.4 `find_path()`

测试路径：

- `user__SEBcjCwgneOV1VV_vESfQ`
- `biz_KhtmCNHqCH6cnxirsenndA`

结果成功找到一条路径：

- `user -> review -> business`

说明：

- 当前系统已可在 Yelp 图中完成真实业务关系路径查询

### 9.5 `execute_query()`

测试查询目标：

- 查询 Cleveland 中高评分且评论数较高的餐馆

返回结果示例：

- `Sabor Miami Cafe & Gallery`
- `Slyman's Restaurant`
- `Cuisine Du Cambodge`
- `George's  Kitchen`
- `Nate's Deli & Restaurant`

说明：

- 当前系统已可对 Yelp 图执行真实聚合和筛选查询
- 可直接用于推荐、榜单和经营分析场景

## 10. 代表性测试结论

本次测试说明：

1. Yelp 数据子集已成功导入 NebulaGraph
2. 图模型与当前智能系统兼容
3. 当前 `Skill + MCP + NebulaGraph` 链路已可直接用于真实业务型图数据
4. 不仅能查路径和邻居，还能执行商业意义明确的业务查询

## 11. 当前限制

本次仍有几个边界需要说明：

1. 当前导入的是 Yelp Open Dataset 的 Cleveland 子集，不是官方全量
2. 尚未引入全文检索与评论主题分析增强
3. 尚未增加 `SIMILAR_TO` 等派生商业关系边
4. `get_space_schema()` 当前输出更偏基础文本，仍可继续优化可读性

## 12. 下一步建议

建议下一步继续做三件事：

1. 从 Yelp 官方全量数据中生成更高价值城市子集
2. 增加 `SIMILAR_TO`、`CO_VISITED` 等派生关系
3. 设计面向推荐、竞对、选址、评论洞察的专用 MCP 工具

## 13. 结论

本次已经完成“Yelp 数据 -> NebulaGraph -> 当前智能系统测试”的闭环验证。

结论是：

- 方案可行
- 导入成功
- 智能系统可直接作用于真实业务语义图数据
- 已具备从技术验证走向业务 PoC 的基础
