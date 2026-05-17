# Task 1 Verification

## Goal

Validate local intelligent interaction between:

- local NebulaGraph
- `NebulaGraph-Skill`
- `nebulagraph-mcp-server`
- Codex as the agent runtime

## What was changed

1. Adapt `NebulaGraph-Skill` documentation from Claude Code to Codex.
2. Install the skill under `~/.codex/skills/nebulagraph`.
3. Register `nebulagraph-mcp-server` in Codex MCP config.
4. Start the local NebulaGraph service.
5. Use the MCP server test suite and direct tool calls for verification.

## Expected local prerequisites

- NebulaGraph installed under `/usr/local/nebula`
- Python 3.12 available for `nebulagraph-mcp-server`
- Codex installed and logged in

## Verification commands

### 1. Start NebulaGraph

```bash
ulimit -n 65535
/usr/local/nebula/scripts/nebula.service start all
```

### 2. Confirm service ports

```bash
ss -ltnp | rg ':9559|:9669|:9779'
```

### 3. Install MCP server dependencies

From `/home/sch/tasks/nebulagraph-mcp-server`:

```bash
uv sync
```

### 4. Install the skill into Codex

```bash
mkdir -p ~/.codex/skills
cp -r /home/sch/tasks/NebulaGraph-Skill/nebulagraph ~/.codex/skills/
```

### 5. Register the MCP server in Codex

```bash
codex mcp add nebulagraph \
  --env NEBULA_VERSION=v3 \
  --env NEBULA_HOST=127.0.0.1 \
  --env NEBULA_PORT=9669 \
  --env NEBULA_USER=root \
  --env NEBULA_PASSWORD=nebula \
  -- ~/.local/bin/uv run --directory /home/sch/tasks/nebulagraph-mcp-server python -m nebulagraph_mcp_server
```

### 6. Run integration verification

```bash
RUN_INTEGRATION_TESTS=true ~/.local/bin/uv run --directory /home/sch/tasks/nebulagraph-mcp-server pytest tests/test_integration.py
```

## Validation criteria

- `list_spaces()` returns `test_graph`
- `execute_query()` can read inserted test vertices
- `find_path()` can find or report path results between seeded vertices
- `find_neighbors()` returns direct neighbors for `person1`
- Codex MCP list shows a configured `nebulagraph` server
