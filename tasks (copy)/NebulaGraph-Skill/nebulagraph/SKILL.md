---
name: nebulagraph
description: >-
  This skill should be used when the user asks to "query NebulaGraph",
  "write nGQL", "create a graph space", "design a graph schema",
  "find paths in a graph", "explore graph neighbors",
  "insert vertices or edges", "build a knowledge graph",
  "model graph data", "traverse a graph",
  "查询 NebulaGraph", "写 nGQL", "创建图空间", "设计图模型",
  "查找路径", "探索邻居节点", "插入点边", "构建知识图谱",
  "图数据建模", "图遍历",
  or mentions NebulaGraph, nGQL, graph database, 图数据库,
  graph schema design, or interacts with nebulagraph-mcp-server
  MCP tools (list_spaces, get_space_schema, execute_query,
  find_path, find_neighbors).
version: 0.2.0
---

# NebulaGraph Skill

NebulaGraph is a distributed graph database using nGQL (SQL-like + openCypher) with a strong-schema model (spaces, tags, edge types, properties).

## Workflow

**1. Categorize** the request:
- Exploration — "what spaces/schema/data exist?"
- Query — "find/get/show data"
- Mutation — "insert/create/update/delete"
- Schema Design — "design/model a graph"

**2. Execute** based on category:

- **Exploration**: `list_spaces()` → `get_space_schema(space)` → `find_neighbors(vertex, space)` for sampling
- **Query** (check schema first): src+dst path → `find_path()` | around vertex → `find_neighbors()` | pattern/filter → MATCH via `execute_query()` | known VID traversal → GO | by property → LOOKUP | by VID → FETCH | subgraph → GET SUBGRAPH
- **Mutation**: Check schema → compose DDL/DML → `execute_query()`. Warn about ~20s heartbeat delay after schema changes.
- **Schema Design**: Read `references/data-modeling.md` → gather requirements → VID strategy → tags/edges → indexes → DDL

## MCP Tools

### `list_spaces()` — Discover available graph spaces. No parameters.

### `get_space_schema(space)` — Get tags, edges, indexes for a space. Always call before writing queries.

### `execute_query(query, space)` — Run any nGQL statement.
- Auto-prepends `USE space;` — never include USE in the query
- nGQL uses `==` for equality (not `=`); string literals in `"double quotes"`
- Prefix query with `PROFILE` for execution metrics

### `find_path(src, dst, space, depth=3, limit=10)` — Find all paths between two vertices.
- **Parameters**: `src` (string, required), `dst` (string, required), `space` (string, required), `depth` (int, default 3), `limit` (int, default 10)
- Searches all edge types bidirectionally
- Increase depth to 5 for sparse graphs

### `find_neighbors(vertex, space, depth=1)` — Get connections around a vertex.
- **Parameters**: `vertex` (string, required), `space` (string, required), `depth` (int, default 1)
- Searches all edge types, undirected
- depth=1 for immediate neighbors; increase for broader exploration

## Essential nGQL Patterns

Core one-liners for quick reference. For complete syntax, see `references/ngql-syntax.md`.

```cypher
MATCH (v) WHERE id(v) == "p1" RETURN v;                              -- by VID
MATCH (v:person) WHERE v.person.name == "Tim" RETURN v;              -- by property (needs index)
MATCH (a)-[e:knows*1..3]->(b) WHERE id(a) == "p1" RETURN DISTINCT b; -- variable-length
GO FROM "p1" OVER knows YIELD dst(edge), knows.weight;               -- traversal
LOOKUP ON person WHERE person.age > 30 YIELD id(vertex), person.name; -- index scan
FETCH PROP ON person "p1" YIELD person.name, person.age;             -- direct fetch
INSERT VERTEX person (name, age) VALUES "p1":("Alice", 30);          -- insert
DELETE VERTEX "p1" WITH EDGE;                                         -- delete
```

## Common Pitfalls

1. `==` not `=` for equality in WHERE. `=` is only for SET in UPDATE/UPSERT.
2. Property access needs tag: `v.person.name` not `v.name`.
3. String VIDs need double quotes: `"p1"`. INT64 VIDs are bare: `100`.
4. LOOKUP needs pre-built index — CREATE + REBUILD first.
5. ~20s heartbeat delay after CREATE/ALTER TAG/EDGE before data operations.
6. Variable-length edge is a list — filter with `ALL(x IN e WHERE ...)` not `e.prop`.
7. `DELETE VERTEX ... WITH EDGE` to avoid dangling edges.
8. No `USE` in `execute_query` — the MCP tool handles it automatically.

## Additional Resources

> All file paths below are relative to this skill's directory (`nebulagraph/`).

### Reference Files

Consult these for detailed syntax and guidance:

- **`references/ngql-syntax.md`** — Complete nGQL syntax: types, DDL/DML, MATCH, GO, paths, indexes, EXPLAIN/PROFILE
- **`references/data-modeling.md`** — Schema design: VID strategy, tags, edges, partitions, modeling patterns
- **`references/operators.md`** — Comparison, boolean, string, set, list operators and precedence
- **`references/functions.md`** — All built-in function signatures (math, string, datetime, aggregation, list, type, schema, predicate)
- **`references/clauses.md`** — YIELD, SAMPLE, INNER JOIN, WITH, UNWIND, ORDER BY, GROUP BY
- **`references/composite-queries.md`** — Pipe operator, variables, property references ($- $^ $$)
- **`references/show-statements.md`** — All SHOW statement variants
- **`references/fulltext-index.md`** — Elasticsearch full-text search integration
- **`references/admin.md`** — JOB management, KILL QUERY/SESSION, user/role management

### Example Files

- **`examples/workflows.md`** — Exploration workflows, query composition, schema evolution, graph analysis, bulk operations, error diagnosis, MCP tool patterns, common query recipes

### Online-Only Reference

For edge cases not covered by local files, consult official docs:

- [Graph patterns](https://docs.nebula-graph.com.cn/3.8.0/3.ngql-guide/1.nGQL-overview/3.graph-patterns/)
- [Keywords & reserved words](https://docs.nebula-graph.com.cn/3.8.0/3.ngql-guide/1.nGQL-overview/keywords-and-reserved-words/)
- [Geography functions](https://docs.nebula-graph.com.cn/3.8.0/3.ngql-guide/6.functions-and-expressions/14.geo/)
- [Data type details](https://docs.nebula-graph.com.cn/3.8.0/3.ngql-guide/3.data-types/1.numeric/)
- [Full-text index deployment](https://docs.nebula-graph.com.cn/3.8.0/4.deployment-and-installation/6.deploy-text-based-index/2.deploy-es/)
- [Import/Export tools](https://docs.nebula-graph.com.cn/3.8.0/nebula-importer/use-importer/)
- [Best practices & system design](https://docs.nebula-graph.com.cn/3.8.0/8.service-tuning/2.graph-modeling/)
