# NebulaGraph Skill

A Codex skill that enables intelligent interaction with NebulaGraph databases through the `nebulagraph-mcp-server`. It provides nGQL query generation, schema design guidance, and workflow orchestration for graph database operations.

## File Structure

```
nebulagraph/
├── SKILL.md                          # Entry point — workflow, MCP tools, pitfalls, resource index
├── references/                       # Detailed reference (read on demand)
│   ├── ngql-syntax.md               # Core nGQL syntax — types, DDL/DML, MATCH, GO, functions, indexes
│   ├── data-modeling.md             # Schema design — VID strategy, tags, edges, partitions, patterns
│   ├── operators.md                 # Comparison, boolean, string, set, list operators & precedence
│   ├── functions.md                 # All built-in function signatures & examples
│   ├── clauses.md                   # YIELD, SAMPLE, INNER JOIN, WITH, UNWIND, ORDER BY, GROUP BY
│   ├── composite-queries.md         # Pipe, variables, property references ($- $^ $$)
│   ├── show-statements.md           # All SHOW statement variants
│   ├── fulltext-index.md            # Elasticsearch full-text search integration
│   └── admin.md                     # JOB management, KILL QUERY/SESSION, users & roles
└── examples/                         # Scenario-based examples
    └── workflows.md                 # Exploration, query composition, error diagnosis, common recipes
```

### Three-Layer Progressive Disclosure

| Layer | Content | When Loaded |
|-------|---------|-------------|
| **L1** | `SKILL.md` — workflow, essential patterns, common pitfalls, resource index | Always (on skill activation) |
| **L2** | `references/*.md`, `examples/*.md` | On demand (when deeper syntax or examples needed) |
| **L3** | Official doc URLs in SKILL.md | Fallback (edge cases: geography, keywords, deployment, etc.) |

## Prerequisites

1. A running NebulaGraph cluster (v3.x)
2. [nebulagraph-mcp-server](https://github.com/nebula-contrib/nebulagraph-mcp-server) installed and configured

Install the MCP server:

```bash
pip install nebulagraph-mcp-server
```

Add the MCP server to Codex:

```bash
codex mcp add nebulagraph \
  --env NEBULA_VERSION=v3 \
  --env NEBULA_HOST=127.0.0.1 \
  --env NEBULA_PORT=9669 \
  --env NEBULA_USER=root \
  --env NEBULA_PASSWORD=nebula \
  -- python -m nebulagraph_mcp_server
```

> **Note**: `NEBULA_VERSION` must be `v3`. Adjust `NEBULA_HOST` and `NEBULA_PORT` to match your cluster.

This command writes the MCP server entry into `~/.codex/config.toml`.

## Installation

Copy the `nebulagraph/` directory to one of these locations:

**Global** (available in all projects):
```
~/.codex/skills/nebulagraph/
```

**Project-level** (available only in this project):
```
<project-root>/.codex/skills/nebulagraph/
```

## Verifying Installation

1. Start a new Codex session
2. Try a trigger phrase like "Query NebulaGraph for all spaces" or "Design a graph schema for user relationships"
3. Codex should activate the skill and use the MCP tools

## Trigger Phrases

The skill activates when you mention:
- NebulaGraph, nGQL, graph database schema
- Graph space, graph paths, graph neighbors
- Knowledge graph, graph data modeling
- MCP tool names: `list_spaces`, `get_space_schema`, `execute_query`, `find_path`, `find_neighbors`
- Actions: query/write nGQL, create graph space, design graph schema, find paths, explore neighbors, insert vertices/edges, traverse a graph

## MCP Tools Available

| Tool | Purpose |
|------|---------|
| `list_spaces()` | Discover available graph spaces |
| `get_space_schema(space)` | Inspect tags, edges, indexes |
| `execute_query(query, space)` | Run any nGQL statement |
| `find_path(src, dst, space, depth, limit)` | Find paths between two vertices |
| `find_neighbors(vertex, space, depth)` | Explore connections around a vertex |

## Customization

- Edit `SKILL.md` frontmatter `description` to adjust trigger conditions
- Add domain-specific patterns to `examples/workflows.md`
- Extend `references/ngql-syntax.md` with custom functions or UDFs
- Add organization-specific modeling conventions to `references/data-modeling.md`
- Add new reference files under `references/` and register them in the Additional Resources section of `SKILL.md`
- Add official doc URLs to the Online-Only Reference list for topics without local files
