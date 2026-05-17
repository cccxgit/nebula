# NebulaGraph 智能交互方案技术附录

## 1. 关键源码定位

### NebulaGraph-Skill

- `/home/sch/tasks/NebulaGraph-Skill/nebulagraph/SKILL.md`
- `/home/sch/tasks/NebulaGraph-Skill/nebulagraph/examples/workflows.md`
- `/home/sch/tasks/NebulaGraph-Skill/nebulagraph/references/*.md`

### nebulagraph-mcp-server

- `/home/sch/tasks/nebulagraph-mcp-server/src/nebulagraph_mcp_server/server.py`
- `/home/sch/tasks/nebulagraph-mcp-server/tests/test_integration.py`
- `/home/sch/tasks/nebulagraph-mcp-server/pyproject.toml`

## 2. 关键技术点与源码片段

### 2.1 Skill 的触发定义

源码位置：

- `NebulaGraph-Skill/nebulagraph/SKILL.md`

代码片段：

```md
description: >-
  This skill should be used when the user asks to "query NebulaGraph",
  ...
  or interacts with nebulagraph-mcp-server
  MCP tools (list_spaces, get_space_schema, execute_query,
  find_path, find_neighbors).
```

说明：

- 这是领域触发器
- 决定 Agent 在何种输入下进入图数据库工作模式

### 2.2 Skill 的工作流约束

代码片段：

```md
- Exploration: `list_spaces()` → `get_space_schema(space)` → `find_neighbors(vertex, space)`
- Query: src+dst path → `find_path()` | around vertex → `find_neighbors()` | pattern/filter → MATCH via `execute_query()`
```

说明：

- 这不是静态文档，而是可执行决策流程
- 直接影响模型的工具调用路径

### 2.3 MCP 生命周期管理

源码位置：

- `nebulagraph-mcp-server/src/nebulagraph_mcp_server/server.py`

代码片段：

```python
@asynccontextmanager
async def nebula_lifespan(server: FastMCP) -> AsyncIterator[NebulaContext]:
    if os.environ["NEBULA_VERSION"] != "v3":
        raise ValueError("NebulaGraph version must be v3")
    global_pool.init([...], config)
    yield NebulaContext(pool=global_pool)
    global_pool.close()
```

说明：

- MCP 服务启动时初始化连接池
- 服务结束时关闭连接池

### 2.4 Schema 工具实现

代码片段：

```python
session.execute(f"USE {space}")
tags = session.execute("SHOW TAGS").column_values("Name")
edges = session.execute("SHOW EDGES").column_values("Name")
```

说明：

- 先切换图空间
- 再拉取 tag 与 edge 元数据
- 最终组装为统一 schema 文本

### 2.5 路径查询工具实现

代码片段：

```python
query = f"""FIND ALL PATH WITH PROP FROM "{src}" TO "{dst}" OVER * BIDIRECT UPTO {depth} STEPS
                  YIELD PATH AS paths | LIMIT {limit}"""
```

说明：

- 封装 NebulaGraph 原生路径能力
- 支持跨所有边类型双向搜索

### 2.6 邻居查询工具实现

代码片段：

```python
query = f"""
MATCH (u)-[e*1..{depth}]-(v)
WHERE id(u) == "{vertex}"
RETURN DISTINCT v, e
"""
```

说明：

- 封装局部图探索能力
- 非常适合自然语言问答的上下文采样

## 3. 已完成验证项

- 本机 NebulaGraph 启动成功
- Codex Skill 安装成功
- Codex MCP 配置成功
- 集成测试通过
- 路径查询与邻居查询人工验证通过

## 4. 当前交付文档索引

- `task-1-usage-guide.md`
- `task-1-test-report.md`
- `task-1-system-report.md`
- `task-1-executive-summary.md`
- `task-1-presentation-outline.md`
- `task-1-defense-qa.md`
- `task-1-architecture-diagrams.md`
- `task-1-technical-appendix.md`

## 5. 建议汇报顺序

建议你汇报时按下面顺序使用材料：

1. 先讲 `task-1-executive-summary.md`
2. 再用 `task-1-presentation-outline.md` 组织 PPT
3. 中间引用 `task-1-architecture-diagrams.md`
4. 技术细节部分讲 `task-1-system-report.md`
5. 专家追问时用 `task-1-defense-qa.md`
