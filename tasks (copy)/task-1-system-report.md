# NebulaGraph 智能交互系统技术分析报告

## 1. 报告概述

本文档面向专家汇报，基于以下两个开源组件源码进行系统级技术分析：

- `NebulaGraph-Skill`
- `nebulagraph-mcp-server`

分析目标不是停留在“能不能用”，而是说明这套方案为什么成立、技术边界在哪里、适合在内部项目中承担什么角色、后续应如何演进。

## 2. 一句话结论

这套方案本质上是一个“`LLM 推理层 + MCP 协议层 + NebulaGraph 执行层`”的三层架构：

1. `NebulaGraph-Skill` 负责把通用大模型约束成“懂 NebulaGraph 的代理”。
2. `nebulagraph-mcp-server` 负责把图数据库能力包装成标准 MCP 工具。
3. `NebulaGraph` 负责真实的图数据存储、遍历、路径计算和 nGQL 执行。

它的核心价值不是重新发明查询引擎，而是把“图数据库专业知识、工具调用协议、查询执行能力”三者解耦，形成一个可插拔、可扩展、可迁移的智能图谱交互中间层。

## 3. 总体架构

### 3.1 架构分层

系统可抽象为四层：

1. 用户交互层
   - 用户通过自然语言提出问题，例如“查询有哪些 space”“查找两点路径”“设计图模型”。
2. Skill 编排层
   - `NebulaGraph-Skill` 提供领域提示词、工作流、错误规约、知识分层装载规则。
3. MCP 工具层
   - `nebulagraph-mcp-server` 将 NebulaGraph 能力暴露为标准 MCP `tool/resource`。
4. 数据执行层
   - NebulaGraph 集群执行 `SHOW/MATCH/GO/FIND PATH/FETCH/LOOKUP` 等 nGQL。

### 3.2 端到端调用链

以“查找 `person1` 到 `person5` 的路径”为例：

1. 用户发起自然语言请求。
2. Skill 根据触发词和工作流判断这是“Query”类型请求。
3. Skill 引导代理优先调用 `find_path(src, dst, space, depth, limit)`。
4. MCP Server 将参数翻译为 NebulaGraph 原生 nGQL：
   - `FIND ALL PATH WITH PROP ... OVER * BIDIRECT UPTO N STEPS`
5. NebulaGraph 返回图路径结果。
6. MCP Server 将结果格式化为文本。
7. LLM 再将文本整理为用户能理解的答案。

这个调用链说明：大模型并不直接连接数据库，也不直接拼接所有底层协议细节，而是通过 Skill 决策、MCP 抽象、数据库执行三步完成。

## 4. NebulaGraph-Skill 源码分析

### 4.1 Skill 的本质不是“文档”，而是“代理行为约束器”

`NebulaGraph-Skill` 的核心文件是 [SKILL.md](/home/sch/tasks/NebulaGraph-Skill/nebulagraph/SKILL.md:1)。它不是普通 README，而是 Agent 的运行时行为说明书。

其前置元数据直接定义了触发条件：

```md
name: nebulagraph
description: >-
  This skill should be used when the user asks to "query NebulaGraph",
  ...
  or interacts with nebulagraph-mcp-server
  MCP tools (list_spaces, get_space_schema, execute_query,
  find_path, find_neighbors).
```

来源：[SKILL.md](/home/sch/tasks/NebulaGraph-Skill/nebulagraph/SKILL.md:2)

技术意义：

- 这段配置等价于“领域路由规则”。
- 它决定模型何时切换到图数据库专家模式。
- 它避免通用模型在没有约束时误用 SQL 习惯去写 nGQL。

这就是 Skill 的第一个核心优势：**把领域知识前置为激活条件，而不是等模型犯错后再补救。**

### 4.2 Skill 的核心设计：任务分类驱动工具选择

`SKILL.md` 并没有把所有操作都交给模型自由发挥，而是先进行任务分类：

```md
**1. Categorize** the request:
- Exploration
- Query
- Mutation
- Schema Design
```

来源：[SKILL.md](/home/sch/tasks/NebulaGraph-Skill/nebulagraph/SKILL.md:23)

随后给出分类到工具的映射：

```md
- Exploration: `list_spaces()` → `get_space_schema(space)` → `find_neighbors(...)`
- Query: src+dst path → `find_path()` | around vertex → `find_neighbors()` | pattern/filter → MATCH via `execute_query()`
- Mutation: Check schema → compose DDL/DML → `execute_query()`
- Schema Design: Read `references/data-modeling.md` → gather requirements → VID strategy → tags/edges → indexes → DDL
```

来源：[SKILL.md](/home/sch/tasks/NebulaGraph-Skill/nebulagraph/SKILL.md:31)

技术意义：

- 这是一种显式“决策图”，不是隐式 prompt。
- 它把“任务理解”与“工具调用策略”绑定。
- 对 LLM 而言，这比单纯列出工具更有效，因为模型得到的是步骤式决策框架。

这就是 Skill 的第二个核心优势：**把自然语言任务转化为可执行的图数据库操作路径。**

### 4.3 Skill 的核心竞争力：三层渐进式知识加载

README 中明确给出 `Three-Layer Progressive Disclosure`：

```md
| L1 | `SKILL.md` — workflow, essential patterns, common pitfalls, resource index |
| L2 | `references/*.md`, `examples/*.md` |
| L3 | Official doc URLs in SKILL.md |
```

来源：[README.md](/home/sch/tasks/NebulaGraph-Skill/README.md:24)

这个设计非常关键，原因有三点：

1. 控制上下文体积
   - 不把所有 NebulaGraph 文档一次性塞给模型，避免上下文膨胀。
2. 提高回答稳定性
   - 先用高频规则和工作流回答，只有遇到深语法时再展开二级资料。
3. 兼顾准确性与成本
   - 在大模型推理成本和知识完备性之间做平衡。

这也是 Skill 设计最值得在内部项目复用的点：**它不是“知识库堆砌”，而是“按需加载的领域推理框架”。**

### 4.4 Skill 把 NebulaGraph 易错点固化为运行规则

`Common Pitfalls` 是 Skill 工程质量的关键部分：

```md
1. `==` not `=` for equality in WHERE.
2. Property access needs tag: `v.person.name` not `v.name`.
3. String VIDs need double quotes.
4. LOOKUP needs pre-built index.
5. ~20s heartbeat delay after CREATE/ALTER TAG/EDGE.
8. No `USE` in `execute_query` — the MCP tool handles it automatically.
```

来源：[SKILL.md](/home/sch/tasks/NebulaGraph-Skill/nebulagraph/SKILL.md:74)

这段内容的技术价值很高，因为它把 NebulaGraph 的非直觉性规则前置约束了：

- SQL 工程师最容易错把 `=` 当成比较符。
- openCypher 与 nGQL 的属性访问风格不同。
- 索引构建和心跳同步存在时序要求。
- MCP 封装后 `USE` 的职责边界发生了变化。

这类规则如果不放进 Skill，模型很容易生成“看起来对、执行会错”的查询。

因此 Skill 的第三个核心优势是：**把数据库特有语义坑位显式编码进代理行为约束。**

### 4.5 Skill 为什么适合图数据库，而不是普通 SQL 数据库

`examples/workflows.md` 展示了它更偏向“图探索工作流”而不是“表结构问答”：

```md
1. list_spaces()
2. get_space_schema(space="my_graph")
3. find_neighbors(vertex="any_known_id", space="my_graph")
4. execute_query(query="MATCH ...", space="my_graph")
```

来源：[workflows.md](/home/sch/tasks/NebulaGraph-Skill/nebulagraph/examples/workflows.md:9)

这个顺序非常符合图数据库认知方式：

- 先看图空间
- 再看 schema
- 再采样邻居
- 最后做定向查询

这与关系型数据库“先表、后 SQL”不同，图数据库经常需要先理解节点和边的结构，再写查询。因此这个 Skill 的工作流设计与图数据库使用心智高度一致。

## 5. nebulagraph-mcp-server 源码分析

### 5.1 这是一个典型的“协议适配器”

项目依赖定义非常清楚：

```toml
dependencies = ["mcp>=1.0.0", "nebula3-python>=3.8.0", "python-dotenv>=1.0.1"]
```

来源：[pyproject.toml](/home/sch/tasks/nebulagraph-mcp-server/pyproject.toml:14)

这说明它只做三件事：

1. 使用 `mcp` 提供协议能力。
2. 使用 `nebula3-python` 连接 NebulaGraph。
3. 使用 `.env`/环境变量完成配置装配。

换句话说，它不是复杂的中台服务，而是一个轻量适配层。这样的好处是边界清晰、部署简单、可替换性强。

### 5.2 服务生命周期设计

核心连接初始化逻辑在：

```python
@asynccontextmanager
async def nebula_lifespan(server: FastMCP) -> AsyncIterator[NebulaContext]:
    if os.environ["NEBULA_VERSION"] != "v3":
        raise ValueError("NebulaGraph version must be v3")
    global_pool.init([(host, port)], config)
    yield NebulaContext(pool=global_pool)
    global_pool.close()
```

来源：[server.py](/home/sch/tasks/nebulagraph-mcp-server/src/nebulagraph_mcp_server/server.py:38)

技术解释：

- `FastMCP` 启动时进入 `lifespan`。
- 在服务生命周期开始时初始化 Nebula 连接池。
- 在服务结束时统一关闭连接池。

优点：

- 连接复用，减少每次调用重复握手。
- 连接池作为全局资源，符合 MCP 工具多次调用场景。
- 生命周期清晰，利于服务化部署。

### 5.3 全局连接池设计

```python
config = Config()
config.max_connection_pool_size = 10
global_pool = ConnectionPool()
```

来源：[server.py](/home/sch/tasks/nebulagraph-mcp-server/src/nebulagraph_mcp_server/server.py:27)

技术解释：

- 连接池大小固定为 10。
- 所有工具函数通过 `get_connection_pool()` 获取共享池。
- 每次具体执行时再从池中取 `session`，结束后 `release()`。

这个设计对当前阶段是务实的：

- 足以支撑单机验证和轻量并发。
- 避免每个工具各自维护连接状态。

但也暴露后续扩展点：

- 池大小没有配置化。
- 没有按租户或空间隔离连接。
- 异常恢复和连接健康检查较弱。

### 5.4 MCP 服务对象的创建方式

```python
mcp = FastMCP(
    "NebulaGraph MCP Server", lifespan=nebula_lifespan, log_level=default_log_level
)
```

来源：[server.py](/home/sch/tasks/nebulagraph-mcp-server/src/nebulagraph_mcp_server/server.py:65)

这里体现的架构思想是：

- 不手写协议收发细节。
- 直接用 `FastMCP` 将 Python 函数暴露成标准化 MCP 工具或资源。

这使得后续接入 Codex、Claude、LlamaIndex、任意支持 MCP 的 Agent 都非常自然。

### 5.5 工具与资源双暴露设计

源码同时使用了 `@mcp.tool()` 和 `@mcp.resource(...)`：

- `schema://space/{space}` 对应 schema 资源
- `path://space/{space}/from/{src}/to/{dst}/...` 对应路径资源
- `neighbors://space/{space}/vertex/{vertex}/depth/{depth}` 对应邻居资源
- `list_spaces/get_space_schema/execute_query/find_path/find_neighbors` 则对外作为工具

来源：

- [server.py](/home/sch/tasks/nebulagraph-mcp-server/src/nebulagraph_mcp_server/server.py:71)
- [server.py](/home/sch/tasks/nebulagraph-mcp-server/src/nebulagraph_mcp_server/server.py:114)
- [server.py](/home/sch/tasks/nebulagraph-mcp-server/src/nebulagraph_mcp_server/server.py:242)

技术意义：

- `resource` 更偏“可寻址数据对象”
- `tool` 更偏“显式动作能力”

这是一种比较好的 MCP 建模方式，说明作者理解的不只是“把函数暴露出来”，而是区分了“可引用资源”和“可执行动作”。

## 6. 核心工具源码详解

### 6.1 `list_spaces()`

源码：

```python
@mcp.tool()
def list_spaces() -> str:
    result = session.execute("SHOW SPACES")
    if result.is_succeeded():
        spaces = result.column_values("Name")
        return "Available spaces:\n" + "\n".join(f"- {space}" for space in spaces)
```

来源：[server.py](/home/sch/tasks/nebulagraph-mcp-server/src/nebulagraph_mcp_server/server.py:158)

实现原理：

- 直接执行 NebulaGraph 的 `SHOW SPACES`
- 从返回结果中取 `Name` 列
- 格式化成人类可读字符串

优势：

- 极简
- 稳定
- 直接支撑 Skill 的探索型工作流第一步

本质上它把“数据库枚举入口”变成了 Agent 的图谱认知起点。

### 6.2 `get_space_schema(space)`

源码：

```python
session.execute(f"USE {space}")
tags = session.execute("SHOW TAGS").column_values("Name")
edges = session.execute("SHOW EDGES").column_values("Name")

for tag in tags:
    tag_result = session.execute(f"DESCRIBE TAG {tag}")
for edge in edges:
    edge_result = session.execute(f"DESCRIBE EDGE {edge}")
```

来源：[server.py](/home/sch/tasks/nebulagraph-mcp-server/src/nebulagraph_mcp_server/server.py:71)

实现原理：

1. 先切换到目标 `space`
2. 拉取所有 `tag`
3. 拉取所有 `edge`
4. 分别执行 `DESCRIBE`
5. 拼装成统一 schema 文本

技术价值：

- 这一步把底层 schema 元信息转换成 LLM 能理解的结构化上下文。
- 对图数据库来说，这是比关系库 `SHOW TABLES` 更关键的能力，因为模型需要知道：
  - 顶点类型
  - 边类型
  - 属性字段
  - 属性类型

这也是 Skill 中要求“写查询前先看 schema”的技术基础。

### 6.3 `execute_query(query, space)`

源码：

```python
session.execute(f"USE {space}")
result = session.execute(query)
if result.is_succeeded():
    columns = result.keys()
    output = "Results:\n"
    output += " | ".join(columns) + "\n"
    ...
```

来源：[server.py](/home/sch/tasks/nebulagraph-mcp-server/src/nebulagraph_mcp_server/server.py:190)

实现原理：

- 自动处理 `USE space`
- 接收任意 nGQL
- 将结果列名和结果行转成文本表格

技术定位：

- 这是“通用后门工具”
- 当前四个专用工具覆盖不了的复杂查询，都可以落到这里

为什么这很重要：

- 如果只有专用工具，系统灵活性不足。
- 如果只有 `execute_query`，系统又过于依赖模型自己写 nGQL。

所以当前设计采用“专用工具 + 通用执行器”并存，是非常合理的折中。

### 6.4 `find_path(src, dst, space, depth, limit)`

源码：

```python
query = f"""FIND ALL PATH WITH PROP FROM "{src}" TO "{dst}" OVER * BIDIRECT UPTO {depth} STEPS
                  YIELD PATH AS paths | LIMIT {limit}"""
```

来源：[server.py](/home/sch/tasks/nebulagraph-mcp-server/src/nebulagraph_mcp_server/server.py:134)

技术解释：

- `OVER *` 表示跨所有边类型搜索
- `BIDIRECT` 表示双向路径遍历
- `WITH PROP` 表示路径中保留点边属性
- `UPTO depth STEPS` 控制搜索深度
- `LIMIT` 控制输出规模

这是一个典型的“图原语封装”：

- 对业务用户而言，不必懂复杂路径语法
- 对大模型而言，直接有现成的路径工具可用
- 对系统而言，把高频图能力标准化为可复用 API

这正是这套方案相对普通 SQL Agent 的核心竞争力之一：**内置图能力，而不是把图查询全部退化为文本生成。**

### 6.5 `find_neighbors(vertex, space, depth)`

源码：

```python
query = f"""
MATCH (u)-[e*1..{depth}]-(v)
WHERE id(u) == "{vertex}"
RETURN DISTINCT v, e
"""
```

来源：[server.py](/home/sch/tasks/nebulagraph-mcp-server/src/nebulagraph_mcp_server/server.py:260)

技术解释：

- 使用 `MATCH` 进行无向多跳邻居探索
- `e*1..depth` 支持一跳到多跳
- `RETURN DISTINCT v, e` 返回邻居点和路径边

这个工具非常适合：

- 未知图结构时的数据采样
- 业务排障时查看某实体周边关系
- 作为自然语言问答中的“局部上下文抽样器”

对于图智能应用，这是非常高频的交互模式。

## 7. 测试代码体现的设计理念

测试文件 [test_integration.py](/home/sch/tasks/nebulagraph-mcp-server/tests/test_integration.py:1) 体现了三个关键思想。

### 7.1 测试不是 mock，而是真实图库验证

测试直接创建：

- `test_graph`
- `person` tag
- `knows` edge
- `reports_to` edge

来源：[test_integration.py](/home/sch/tasks/nebulagraph-mcp-server/tests/test_integration.py:21)

说明项目作者不是只做接口单测，而是在验证：

- schema 创建
- 数据写入
- 路径查询
- 邻居查询

这能更真实地反映 MCP Server 对 NebulaGraph 的适配质量。

### 7.2 显式处理 NebulaGraph 的元数据传播延迟

测试里直接写死了等待时间：

```python
if group_index == 0:
    time.sleep(30)
elif group_index == 1:
    time.sleep(10)
```

来源：[test_integration.py](/home/sch/tasks/nebulagraph-mcp-server/tests/test_integration.py:121)

这说明作者对 NebulaGraph 的时序特性是有理解的，知道：

- 创建 space 后不能立刻建 schema
- 建 schema 后不能立刻稳定写数据

这与 Skill 中的“heartbeat delay”规则是相互印证的，说明两个项目并不是松散拼接，而是隐含存在一致的数据库认知模型。

### 7.3 选取的测试场景非常贴近业务语义

测试项包括：

- `list_spaces`
- `execute_query`
- `find_path`
- `find_neighbors`

来源：[test_integration.py](/home/sch/tasks/nebulagraph-mcp-server/tests/test_integration.py:156)

这四类正好覆盖了智能图谱交互的四种基本行为：

1. 环境发现
2. 通用查询
3. 路径分析
4. 局部关系探索

测试集虽然小，但抽象层次选得对。

## 8. 技术竞争力分析

### 8.1 相对“纯 Prompt + 直连数据库”的优势

如果不采用 MCP，而是让大模型直接生成 nGQL 并由应用执行，会有几个明显问题：

1. 模型必须同时负责意图理解、语法生成、数据库约束记忆。
2. 无法形成标准工具边界，换模型或换框架成本高。
3. 路径查找、邻居探索等图原语不容易沉淀成复用能力。

当前方案通过 Skill + MCP 把这几个问题拆开了。

### 8.2 相对“只有通用 SQL Agent”的优势

图数据库交互和关系数据库交互差异很大，关键在：

- 查询入口不只是表，而是 `space/tag/edge`
- 路径、邻居、多跳遍历是第一类能力
- Schema 认知对查询正确性影响更大

`NebulaGraph-Skill` 的优势就在于它针对图数据库单独设计了：

- 分类工作流
- 易错点约束
- 数据建模参考
- 图分析示例

这让它比通用数据库 Agent 更贴近图场景。

### 8.3 相对“只提供 MCP Server、不提供 Skill”的优势

单独的 MCP Server 只能提供工具，不会指导模型正确使用工具。

Skill 则提供了：

- 何时先看 schema
- 何时用 `find_path`
- 何时退化到 `execute_query`
- 何时需要索引与 heartbeat 等等待条件

因此 Skill 的竞争力在于：**把工具可用性提升为工具可正确使用性。**

### 8.4 组合后的系统级竞争力

这两个项目组合后，形成了三类竞争力：

1. 领域适配力
   - 对 NebulaGraph 语义和图查询模式有原生适配。
2. 协议兼容力
   - 基于 MCP，天然适配多种 Agent 平台。
3. 工程复用力
   - 专用工具与通用执行器并存，兼顾标准化与灵活性。

## 9. 现有局限与技术风险

### 9.1 `execute_query` 没有安全边界

当前 `execute_query` 可以执行任意 nGQL，包括 DDL 和 DML。

优点是灵活，缺点是：

- 没有只读模式
- 没有危险语句拦截
- 没有审计增强

如果用于内部生产环境，建议增加：

- 读写分级
- 关键字黑白名单
- 审计日志
- 多租户隔离

### 9.2 字符串拼接生成查询，存在注入风险

例如：

```python
session.execute(f"USE {space}")
```

以及：

```python
FIND ALL PATH WITH PROP FROM "{src}" TO "{dst}" ...
```

来源：

- [server.py](/home/sch/tasks/nebulagraph-mcp-server/src/nebulagraph_mcp_server/server.py:85)
- [server.py](/home/sch/tasks/nebulagraph-mcp-server/src/nebulagraph_mcp_server/server.py:134)

在当前验证环境可接受，但生产场景至少要补：

- 参数合法性校验
- 变量转义
- 空间名/VID 白名单或正则约束

### 9.3 结果格式目前偏文本，不够结构化

当前所有工具几乎都返回文本字符串，而不是 JSON/结构化对象。

好处：

- 人类可读
- 快速接入 Agent

不足：

- 机器二次消费困难
- 前端图可视化不方便
- 精准字段抽取能力有限

建议演进为“双通道输出”：

- 给 Agent 的自然语言摘要
- 给系统的结构化 JSON 载荷

### 9.4 连接与异常治理还偏轻量

当前只有基础连接池与错误字符串返回：

- 没有统一异常码
- 没有熔断/超时治理
- 没有服务端指标埋点

因此更适合：

- PoC
- 单团队内部工具
- 原型系统

如果要进入核心生产链路，需要再做一层企业级封装。

## 10. 对内部项目的适用性判断

### 10.1 适合的场景

这套方案很适合以下内部项目：

1. 知识图谱探索平台
2. 图数据运维助手
3. 业务人员自然语言查图
4. 图模型设计辅助工具
5. 图谱调试与数据验证平台

### 10.2 当前最适合的定位

建议当前定位为：

- “智能图谱交互中间层”
- 或“图数据库 Copilot 基座”

而不是直接定义成最终生产系统。

原因很简单：

- 它已经具备很好的交互抽象
- 但还没具备完整的安全、治理、审计、权限、可观测性能力

### 10.3 内部演进建议

建议分三阶段推进：

#### 第一阶段：工具化落地

- 作为研发/数据团队内部助手使用
- 以只读查询、路径分析、schema 解释为主

#### 第二阶段：平台化增强

- 增加鉴权、审计、读写隔离、结构化返回
- 引入业务语义模板和领域词典

#### 第三阶段：产品化接入

- 接入内部知识平台、运维平台、数据资产平台
- 结合前端图可视化和工单闭环

## 11. 专家汇报视角下的核心结论

### 11.1 Skill 的原理优势

`NebulaGraph-Skill` 的核心不是“写了很多 NebulaGraph 文档”，而是：

- 用触发条件做领域路由
- 用分类工作流做任务编排
- 用渐进式知识装载控制上下文成本
- 用易错点规则提升查询正确率

它的最大优势在于：**把大模型从“会聊天”约束成“会按图数据库工程规则工作”。**

### 11.2 MCP Server 的原理优势

`nebulagraph-mcp-server` 的核心不是“做了数据库连接”，而是：

- 用 MCP 统一工具协议
- 用专用图工具沉淀高频图能力
- 用通用查询工具保留灵活性
- 用轻量连接池与生命周期管理实现快速落地

它的最大优势在于：**把 NebulaGraph 能力标准化为 Agent 可调用的协议接口。**

### 11.3 二者组合后的系统优势

组合后形成的是一个“面向图数据库的智能交互框架”，而不是简单脚本集合。其系统级优势包括：

- 架构解耦清晰
- 多模型/多 Agent 可迁移
- 图领域知识表达完整
- 高价值图操作内置化
- 适合作为内部智能图谱中间层基座

## 12. 最终结论

从源码实现、架构设计和系统验证结果来看，这套方案在当前阶段已经具备较高的工程合理性：

1. `NebulaGraph-Skill` 解决了“模型如何正确理解并使用图数据库工具”的问题。
2. `nebulagraph-mcp-server` 解决了“图数据库能力如何标准化暴露给 Agent”的问题。
3. NebulaGraph 提供了真实的图计算与查询能力。
4. 三者组合形成了一条完整、清晰、可扩展的智能图谱交互链路。

如果用于内部项目，我的判断是：

- 作为 PoC、研发提效工具、图谱助手基座，已经可用。
- 作为生产级统一智能图谱服务，还需要继续补安全治理、结构化输出、审计与运维能力。

总体评价：

- 技术方向正确
- 架构抽象清晰
- 对图数据库场景有针对性优势
- 具备继续工程化深化的价值
