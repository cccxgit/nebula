# Task 1 使用文档

## 1. 目标

本方案用于在本机完成以下组件的联动：

- `NebulaGraph` 数据库
- `nebulagraph-mcp-server`
- `NebulaGraph-Skill`
- `Codex`

目标是让 Codex 通过 MCP 工具对本机 NebulaGraph 进行智能交互，包括：

- 查看图空间
- 查看 schema
- 执行 nGQL
- 查找路径
- 查询邻居

## 2. 目录说明

当前工程目录：

- `/home/sch/tasks/NebulaGraph-Skill`
- `/home/sch/tasks/nebulagraph-mcp-server`

其中：

- `NebulaGraph-Skill` 已适配为 Codex 使用方式
- `nebulagraph-mcp-server` 作为本地 MCP 服务接入 NebulaGraph

## 3. 环境要求

需要满足以下条件：

- NebulaGraph 已安装在 `/usr/local/nebula`
- 本机可使用 Codex
- 本机可联网安装 Python 依赖
- 建议使用 Python `3.12`

本次验证实际使用：

- Ubuntu 22.04.3 LTS
- Python 3.12.13
- `uv 0.11.14`
- NebulaGraph 本地实例

## 4. 启动 NebulaGraph

启动命令：

```bash
ulimit -n 65535
/usr/local/nebula/scripts/nebula.service start all
```

停止命令：

```bash
/usr/local/nebula/scripts/nebula.service stop all
```

检查端口：

```bash
ss -ltnp | rg ':9559|:9669|:9779'
```

正常情况下应看到：

- `9559` 对应 `nebula-metad`
- `9669` 对应 `nebula-graphd`
- `9779` 对应 `nebula-storaged`

## 5. 安装 MCP 服务依赖

进入目录：

```bash
cd /home/sch/tasks/nebulagraph-mcp-server
```

如本机未安装 `uv`，可先安装：

```bash
curl -LsSf https://astral.sh/uv/install.sh | sh
```

安装 Python 3.12：

```bash
~/.local/bin/uv python install 3.12
```

同步依赖：

```bash
~/.local/bin/uv sync --python 3.12
```

## 6. 安装 Skill 到 Codex

执行：

```bash
mkdir -p ~/.codex/skills
cp -r /home/sch/tasks/NebulaGraph-Skill/nebulagraph ~/.codex/skills/
```

安装完成后，Skill 路径为：

```bash
~/.codex/skills/nebulagraph
```

## 7. 配置 Codex MCP

执行以下命令注册本地 NebulaGraph MCP：

```bash
codex mcp add nebulagraph \
  --env NEBULA_VERSION=v3 \
  --env NEBULA_HOST=127.0.0.1 \
  --env NEBULA_PORT=9669 \
  --env NEBULA_USER=root \
  --env NEBULA_PASSWORD=nebula \
  -- ~/.local/bin/uv run --directory /home/sch/tasks/nebulagraph-mcp-server python -m nebulagraph_mcp_server
```

查看配置是否成功：

```bash
codex mcp list
```

本次验证中，Codex 已成功识别：

- MCP 名称：`nebulagraph`

## 8. 可用能力

该方案当前已验证以下 MCP 工具：

- `list_spaces()`
- `get_space_schema(space)`
- `execute_query(query, space)`
- `find_path(src, dst, space, depth, limit)`
- `find_neighbors(vertex, space, depth)`

## 9. 使用方式

完成 Skill 与 MCP 配置后，在 Codex 中可直接使用自然语言触发，例如：

- 查询 NebulaGraph 里有哪些 space
- 查看 `test_graph` 的 schema
- 在 `test_graph` 中执行一条 nGQL 查询
- 查找 `person1` 到 `person5` 的路径
- 查询 `person1` 的邻居节点

也可以直接要求 Codex：

- 帮我写一条查询 NebulaGraph 的 nGQL
- 帮我设计一个图模型
- 帮我分析当前图空间 schema

## 10. 测试数据

本次系统验证使用了 `nebulagraph-mcp-server` 仓库自带的集成测试数据，创建了图空间：

- `test_graph`

其中包含：

- Tag: `person`
- Edge: `knows`
- Edge: `reports_to`

示例顶点：

- `person1`
- `person2`
- `person3`
- `person4`
- `person5`

## 11. 常用验证命令

查看图空间：

```bash
printf 'SHOW SPACES;\n' | /usr/local/nebula/scripts/nebula-console -addr 127.0.0.1 -port 9669 -u root -p nebula
```

运行集成测试：

```bash
cd /home/sch/tasks
RUN_INTEGRATION_TESTS=true ~/.local/bin/uv run --directory /home/sch/tasks/nebulagraph-mcp-server pytest tests/test_integration.py -q
```

## 12. 注意事项

- `nebulagraph-mcp-server` 工程声明要求 Python `>= 3.12`
- `NEBULA_VERSION` 当前必须配置为 `v3`
- 执行 schema 变更后，NebulaGraph 可能存在短暂元数据同步延迟
- `execute_query(query, space)` 场景下，不要在 query 中重复写 `USE space`

## 13. 当前交付状态

当前工程已完成：

- Skill 从 Claude Code 适配为 Codex
- Codex Skill 安装验证
- Codex MCP 注册验证
- 本机 NebulaGraph 联通验证
- 基于测试数据的 MCP 工具链验证

可以直接作为本机系统验证基线使用。
