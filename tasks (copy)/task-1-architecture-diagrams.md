# NebulaGraph 智能交互方案架构图

## 1. 四层总体架构图

```mermaid
flowchart TD
    U[用户自然语言请求]
    A[Codex Agent]
    S[NebulaGraph-Skill]
    M[nebulagraph-mcp-server]
    N[NebulaGraph]

    U --> A
    A --> S
    S --> A
    A --> M
    M --> N
    N --> M
    M --> A
    A --> U
```

## 2. 端到端调用时序图

```mermaid
sequenceDiagram
    participant User as 用户
    participant Codex as Codex
    participant Skill as NebulaGraph-Skill
    participant MCP as nebulagraph-mcp-server
    participant Nebula as NebulaGraph

    User->>Codex: 查找 person1 到 person5 的路径
    Codex->>Skill: 匹配图数据库场景，选择工作流
    Skill-->>Codex: 建议调用 find_path
    Codex->>MCP: find_path(src,dst,space,depth,limit)
    MCP->>Nebula: FIND ALL PATH WITH PROP ...
    Nebula-->>MCP: 路径结果
    MCP-->>Codex: 格式化文本结果
    Codex-->>User: 返回自然语言解释
```

## 3. Skill 内部工作流图

```mermaid
flowchart TD
    R[用户请求]
    C{请求分类}
    E[Exploration]
    Q[Query]
    M[Mutation]
    D[Schema Design]

    R --> C
    C --> E
    C --> Q
    C --> M
    C --> D

    E --> E1[list_spaces]
    E1 --> E2[get_space_schema]
    E2 --> E3[find_neighbors]

    Q --> Q1[find_path / find_neighbors]
    Q --> Q2[execute_query]

    M --> M1[先看 schema]
    M1 --> M2[执行 DDL/DML]

    D --> D1[读取 data-modeling]
    D1 --> D2[VID 策略]
    D2 --> D3[Tag / Edge / Index]
```

## 4. MCP Server 内部结构图

```mermaid
flowchart TD
    Main[main()]
    FastMCP[FastMCP]
    Life[Lifespan]
    Pool[ConnectionPool]
    Tools[Tools / Resources]
    Nebula[NebulaGraph]

    Main --> FastMCP
    FastMCP --> Life
    Life --> Pool
    FastMCP --> Tools
    Tools --> Pool
    Pool --> Nebula
```

## 5. 内部项目落地建议架构图

```mermaid
flowchart LR
    User[业务用户 / 研发 / 数据]
    Portal[内部平台前端]
    Agent[Codex / Agent Runtime]
    Skill[Graph Skill Layer]
    MCP[MCP Graph Service]
    Guard[权限 / 审计 / 风控]
    Graph[NebulaGraph]
    Viz[图可视化组件]

    User --> Portal
    Portal --> Agent
    Agent --> Skill
    Agent --> MCP
    MCP --> Guard
    Guard --> Graph
    Graph --> Viz
    Viz --> Portal
```
