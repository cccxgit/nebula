# Task 1 任务三报告：高价值数据集与 AI 商业价值测试设计

## 1. 任务目标

任务三要求：

- 在网上找到一个有价值的数据集
- 设计一些有价值的测试用例
- 凸显本项目的 AI 商业价值

本报告给出数据集选型结论、选型理由、图建模方案、测试用例设计和落地建议。

## 2. 数据集选型结论

本次推荐的数据集为：

- `Yelp Open Dataset`

推荐理由：

1. 数据主题具备明显商业属性
   - 本地生活商户、用户、评论、品类、地理位置天然具有商业价值。
2. 关系结构天然适合图数据库
   - 用户、商户、评论、品类、城市之间是高密度关系网络。
3. 场景丰富
   - 推荐、选址、竞对、经营洞察、风险识别都能做。
4. 对 AI 价值展示友好
   - 很容易通过自然语言问答展示“图智能”的业务效果。

## 3. 为什么不选纯学术图数据

很多公开图数据集虽然适合算法验证，但不适合对专家展示商业价值，例如：

- 引文网络
- 学术合作网络
- 纯 benchmark 图数据

这些数据能证明算法能力，但很难直接说明：

- 对企业经营有什么价值
- 对业务分析有什么提升
- 对智能应用有什么落地意义

而 `Yelp Open Dataset` 更接近真实业务图谱。

## 4. 数据集来源与官方依据

### 4.1 Yelp 开发者官方页面

Yelp Developers 页面明确说明：

- `Yelp Open Dataset` 提供 businesses、reviews、user data 的子集
- 适用于 personal、educational、academic 使用

官方入口：

- https://www.yelp.com/developers

### 4.2 Yelp 工程博客

Yelp 官方工程文章说明开放数据集规模约为：

- 近 500 万 reviews
- 超过 110 万 users
- 超过 15 万 businesses
- 覆盖 12 个 metropolitan areas

官方文章：

- https://engineeringblog.yelp.com/2017/08/yelp-open-dataset-and-dataset-challenge-round-10.html

### 4.3 Yelp 商业数据能力页面

Yelp 官方商业数据页面说明其数据可用于：

- Local Search & Recommendations
- Natural Language Search
- Location Intelligence
- Insurance & Risk Adjustment
- Banking & Finance

相关页面：

- https://business.yelp.com/data/
- https://business.yelp.com/data/products/insights-api/

## 5. 数据集的商业价值判断

如果从内部项目视角看，这个数据集最大的价值在于它覆盖了“本地生活商业图谱”最核心的五类实体：

1. 用户
2. 商户
3. 评论
4. 品类
5. 城市/区域

这意味着它可以回答的不只是“数据里有什么”，而是：

- 用户喜欢什么类型商户
- 某商户和哪些竞品最相近
- 某城市哪些品类增长更快
- 某区域口碑好但供给不足的商户类型有哪些
- 负面评价集中反映了哪些经营问题

这类问题具有直接业务意义。

## 6. 面向 NebulaGraph 的图建模建议

建议建立一个新的图空间，例如：

- `yelp_graph`

### 6.1 顶点设计

建议 Tag：

- `user`
  - `user_id`
  - `review_count`
  - `average_stars`
- `business`
  - `business_id`
  - `name`
  - `stars`
  - `review_count`
  - `city`
  - `state`
  - `is_open`
- `category`
  - `category_name`
- `city`
  - `city_name`
- `review`
  - `review_id`
  - `stars`
  - `text`
  - `date`

### 6.2 边设计

建议 Edge：

- `WROTE`
  - `date`
  - `stars`
- `REVIEWS`
  - `stars`
- `HAS_CATEGORY`
- `LOCATED_IN`
- `SIMILAR_TO`
  - 可由规则或算法离线计算
- `CO_VISITED`
  - 可由用户评论共现推导

### 6.3 为什么这样建模

因为这样既能保留原始业务关系，又能支持后续智能场景：

- 推荐
- 相似商户
- 商圈洞察
- 评论分析
- 竞品关系挖掘

## 7. 最能体现 AI 商业价值的测试方向

我建议重点展示五类测试方向：

1. 自然语言推荐
2. 竞对与相似商户分析
3. 选址与市场空白洞察
4. 评论根因与经营改进建议
5. 风险与机会识别

这些方向比单纯展示“查路径”“查邻居”更有说服力。

## 8. 商业价值测试用例设计

### 用例 1：本地生活智能推荐助手

用户问题：

- 帮我找凤凰城评分高、评论活跃、适合家庭聚餐的中餐馆

系统执行思路：

- 先确定目标城市
- 筛选相关 `category`
- 关联 `business`
- 汇总 `stars`、`review_count`
- 如有评论文本，再结合评论关键词做解释

商业价值：

- 适用于本地生活推荐
- 适用于平台搜索增强
- 适用于 AI 导购/客服

### 用例 2：竞对发现与替代商户推荐

用户问题：

- 如果某家热门餐厅满座，附近最相似、口碑不差的替代商户有哪些

系统执行思路：

- 从目标 `business` 出发
- 查找相同 `category`
- 结合地理位置、评分、评论量和共现关系计算相似性
- 返回 TopN 替代商户

商业价值：

- 提升推荐成功率
- 降低用户流失
- 可用于导流与竞品分析

### 用例 3：新店选址与市场空白分析

用户问题：

- 哪些区域适合新增精品咖啡店，但目前优质供给不足

系统执行思路：

- 按 `city/area/category` 聚合
- 对比商户数量、评分、评论活跃度
- 识别“需求强、供给弱”的区域

商业价值：

- 直接支持选址
- 支持区域拓展决策
- 支持连锁品牌开店策略

### 用例 4：评论驱动的经营问题诊断

用户问题：

- 这类餐馆近期差评主要集中在哪些问题

系统执行思路：

- 提取相关 `business` 的低分 `review`
- 按主题聚类负面文本
- 结合商户类别和城市进行横向对比

商业价值：

- 帮助商户经营改进
- 帮助平台做商户健康度评估
- 帮助运营发现共性问题

### 用例 5：高潜商户识别

用户问题：

- 哪些商户评论增速快、评分高、但当前曝光可能不足

系统执行思路：

- 按时间聚合评论数量
- 结合评分、品类、城市比较
- 查找“增长快但尚未头部”的商户

商业价值：

- 商户线索挖掘
- 平台资源投放
- 广告和招商优先级判断

### 用例 6：银行/保险/风控视角的商户风险辅助分析

用户问题：

- 哪些商户存在停业风险或经营波动迹象

系统执行思路：

- 分析是否营业、评论趋势、评分趋势、负面评论集中度
- 结合区域和品类进行风险对比

商业价值：

- 银行授信辅助
- 保险承保辅助
- 风险预警

## 9. 为什么这些用例能凸显 AI 商业价值

因为这些用例展示的不是“查一条图路径”，而是：

- AI 能理解业务问题
- AI 能自动选择图工具与查询方式
- AI 能将图结构结果解释为业务决策建议

换句话说，它把图数据库从后台存储能力，变成了前台决策支持能力。

## 10. 建议的演示策略

如果用于专家汇报，建议演示顺序如下：

1. 先展示基础能力
   - 查看 space
   - 查看 schema
   - 查路径
   - 查邻居
2. 再展示任务三价值能力
   - 推荐
   - 竞对
   - 选址
   - 评论洞察
3. 最后讲商业价值
   - 平台推荐
   - 商户服务
   - 经营分析
   - 金融风控

这样能形成从“技术可行”到“业务有用”的完整说服链。

## 11. 当前实施建议

当前建议不是一次性导入完整大数据集，而是：

1. 先选一个城市或一个品类做子集试点
2. 建立示范图模型
3. 跑通高价值用例
4. 再扩展到更大规模

原因是任务三的重点是“证明价值”，不是“证明能导入最大数据量”。

## 12. 使用限制说明

需要注意：

- Yelp Open Dataset 官方说明更偏 personal/educational/academic 用途
- 如果未来真的走商业生产化，建议切换到 Yelp 官方商业数据或其他具备商用授权的数据源

所以本次任务三建议把它定位为：

- 商业价值展示数据集
- 内部 PoC 数据集
- 图智能场景样例数据集

## 13. 结论

任务三推荐的最优方向是：

- 选择 `Yelp Open Dataset` 作为高价值测试数据方向
- 以本地生活商户图谱为中心设计 AI 测试用例
- 用推荐、竞对、选址、评论洞察、风险识别五类场景凸显商业价值

这是当前最容易向专家和业务方说明“图数据库 + AI 为什么值得做”的方案。

## 14. 参考链接

- Yelp Developers: https://www.yelp.com/developers
- Yelp Open Dataset engineering blog: https://engineeringblog.yelp.com/2017/08/yelp-open-dataset-and-dataset-challenge-round-10.html
- Yelp Data Licensing: https://business.yelp.com/data/
- Yelp Insights API: https://business.yelp.com/data/products/insights-api/
- Yelp Developer Docs: https://docs.developer.yelp.com/docs
