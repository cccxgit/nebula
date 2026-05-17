# NebulaGraph 智能交互方案 PPT 汇报提纲

## 1. 项目背景

- NebulaGraph 作为图数据库，功能强但使用门槛较高
- 业务和研发团队希望通过自然语言提升图数据使用效率
- 需要一种低侵入、可扩展、可验证的智能交互方案

## 2. 项目目标

- 打通 `Codex + MCP + NebulaGraph`
- 基于开源 `NebulaGraph-Skill` 与 `nebulagraph-mcp-server` 完成系统验证
- 评估其在内部项目中的技术价值与可扩展性

## 3. 总体架构

- 用户自然语言请求
- Codex 通过 `NebulaGraph-Skill` 做任务理解和工具选择
- `nebulagraph-mcp-server` 暴露 NebulaGraph 能力为 MCP 工具
- NebulaGraph 执行真实图查询与图遍历

建议配图：

- 四层架构图
- 调用时序图

## 4. 核心组件说明

### 4.1 NebulaGraph-Skill

- 本质是领域化 Agent 运行规则
- 负责触发、分类、知识装载、错误规约
- 将通用大模型约束成图数据库专家代理

### 4.2 nebulagraph-mcp-server

- 本质是 NebulaGraph 到 MCP 的协议适配器
- 将图数据库能力标准化成工具
- 兼顾专用图工具与通用执行能力

### 4.3 NebulaGraph

- 提供图空间、tag、edge、路径查找、邻居探索、nGQL 执行

## 5. Skill 设计优势

- 有明确的触发机制
- 有任务分类工作流
- 有渐进式知识加载
- 有 NebulaGraph 易错点约束

结论：

- Skill 的价值不只是“提示词”，而是“领域行为编排”

## 6. MCP Server 设计优势

- 使用 MCP 标准协议，适配多种 Agent
- 提供 `list_spaces/get_space_schema/execute_query/find_path/find_neighbors`
- 同时支持 `tool` 与 `resource` 建模
- 连接池与生命周期设计简单清晰

## 7. 源码关键点

可重点讲三段代码：

1. Skill 的触发与工作流定义
2. MCP Server 的生命周期与连接池
3. 路径查找与邻居查询工具实现

## 8. 本次系统验证结果

- 本地 NebulaGraph 服务正常启动
- MCP 服务依赖安装成功
- Codex Skill 安装成功
- Codex MCP 注册成功
- 集成测试通过

测试结果：

```text
4 passed in 44.88s
```

## 9. 技术竞争力

- 面向图数据库的原生适配能力
- 自然语言到图查询的低门槛交互
- Skill 与 MCP 解耦，便于换模型、换平台
- 适合构建内部图数据库 Copilot

## 10. 当前风险与不足

- `execute_query` 安全边界不足
- 结果输出偏文本，结构化不足
- 连接治理与异常治理较轻
- 缺少权限审计与生产级运维能力

## 11. 建议落地路径

### 第一阶段

- 作为研发与数据团队内部辅助工具

### 第二阶段

- 增强权限、安全、审计、结构化输出

### 第三阶段

- 接入内部知识平台、图谱平台、运维平台

## 12. 最终结论

- 技术路线正确
- PoC 验证成功
- 有较强内部应用潜力
- 建议继续推进工程化与平台化建设
