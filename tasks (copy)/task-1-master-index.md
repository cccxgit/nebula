# Task 1 总目录索引

## 1. 文档目的

本文档用于系统整理 `task-1.md` 中全部任务，以及执行过程中新增的所有有效材料，帮助你快速了解、学习和复用本次工作成果。

适用对象包括：

- 需要快速了解项目全貌的人
- 需要准备汇报材料的人
- 需要继续开发和扩展系统的人
- 需要复现实验和测试的人

## 2. 原始任务说明

原始任务文件：

- [task-1.md](/home/sch/tasks/task-1.md:1)

包含三项任务：

### 任务一

基于本机 `NebulaGraph`、开源 `NebulaGraph-Skill`、开源 `nebulagraph-mcp-server`，实现一套智能交互系统，并把 Skill 中使用的 `Claude Code` 适配为 `Codex`。

### 任务二

详细阅读 `NebulaGraph-Skill` 和 `nebulagraph-mcp-server` 源码，输出架构原理、实现机制、关键技术点、竞争力和专家汇报级系统报告。

### 任务三

联网寻找有价值的数据集，设计高价值测试用例，体现本项目的 AI 商业价值。后续又继续推进到了：

- Yelp 数据图建模
- Yelp 数据导入 NebulaGraph
- 使用当前智能系统做真实测试
- 输出传统手写查询方式与智能系统方式的价值对比材料

## 3. 整体成果地图

本次产出可以分为 8 大类：

1. 基础任务与验证材料
2. 使用与运维材料
3. 源码与系统分析材料
4. 汇报与演讲材料
5. 任务三数据集与商业价值材料
6. Yelp 图建模与导入材料
7. Yelp 智能测试与价值对比材料
8. 索引与导航材料

## 4. 基础任务与验证材料

### 4.1 原始任务

- [task-1.md](/home/sch/tasks/task-1.md:1)

用途：

- 查看最原始需求和任务边界

### 4.2 系统验证说明

- [task-1-verification.md](/home/sch/tasks/task-1-verification.md:1)

用途：

- 了解任务一系统验证的目标、步骤和标准

### 4.3 测试结果报告

- [task-1-test-report.md](/home/sch/tasks/task-1-test-report.md:1)

用途：

- 查看任务一在本机的完整验证结果

## 5. 使用与运维材料

### 5.1 使用文档

- [task-1-usage-guide.md](/home/sch/tasks/task-1-usage-guide.md:1)

用途：

- 快速上手当前 `Codex + Skill + MCP + NebulaGraph` 使用方式

内容包括：

- 环境要求
- NebulaGraph 启停
- MCP 依赖安装
- Skill 安装
- Codex MCP 配置
- 基础使用方式

## 6. 源码与系统分析材料

### 6.1 系统技术分析报告

- [task-1-system-report.md](/home/sch/tasks/task-1-system-report.md:1)

用途：

- 对任务二进行完整技术分析

内容包括：

- 整体架构
- Skill 原理分析
- MCP Server 原理分析
- 核心源码解释
- 竞争力分析
- 风险与边界
- 内部项目建议

### 6.2 技术附录

- [task-1-technical-appendix.md](/home/sch/tasks/task-1-technical-appendix.md:1)

用途：

- 快速查源码位置、关键技术点和推荐汇报顺序

## 7. 汇报与演讲材料

### 7.1 领导摘要

- [task-1-executive-summary.md](/home/sch/tasks/task-1-executive-summary.md:1)

用途：

- 给领导快速浏览项目价值和结论

### 7.2 PPT 提纲

- [task-1-presentation-outline.md](/home/sch/tasks/task-1-presentation-outline.md:1)

用途：

- 搭建正式 PPT 结构

### 7.3 PPT 逐页正文

- [task-1-ppt-full-content.md](/home/sch/tasks/task-1-ppt-full-content.md:1)

用途：

- 直接复制到 PPT 页面的正文草稿

### 7.4 汇报讲稿

- [task-1-presentation-script.md](/home/sch/tasks/task-1-presentation-script.md:1)

用途：

- 作为正式技术汇报讲稿

### 7.5 Demo 脚本

- [task-1-demo-script.md](/home/sch/tasks/task-1-demo-script.md:1)

用途：

- 演示系统时的步骤说明与话术参考

### 7.6 架构图材料

- [task-1-architecture-diagrams.md](/home/sch/tasks/task-1-architecture-diagrams.md:1)

用途：

- 直接给 PPT 使用的 Mermaid 架构图

### 7.7 答辩问答

- [task-1-defense-qa.md](/home/sch/tasks/task-1-defense-qa.md:1)

用途：

- 面对专家追问时快速组织回答

### 7.8 建设路线图

- [task-1-implementation-roadmap.md](/home/sch/tasks/task-1-implementation-roadmap.md:1)

用途：

- 说明从 PoC 到平台化的推进路径

## 8. 任务三：数据集与商业价值材料

### 8.1 数据集选型报告

- [task-1-task3-dataset-report.md](/home/sch/tasks/task-1-task3-dataset-report.md:1)

用途：

- 说明为什么选择 `Yelp Open Dataset`
- 说明商业价值场景和测试思路

### 8.2 商业测试用例清单

- [task-1-task3-test-cases.md](/home/sch/tasks/task-1-task3-test-cases.md:1)

用途：

- 查看推荐、竞对、选址、评论洞察、风险识别等测试场景

## 9. Yelp 图建模与导入材料

### 9.1 Yelp 到 NebulaGraph 的建模方案

- [task-1-yelp-nebulagraph-modeling.md](/home/sch/tasks/task-1-yelp-nebulagraph-modeling.md:1)

用途：

- 查看 Yelp 数据在 NebulaGraph 中如何建模

### 9.2 Yelp 示例 nGQL 查询集

- [task-1-yelp-ngql-examples.md](/home/sch/tasks/task-1-yelp-ngql-examples.md:1)

用途：

- 查看围绕 Yelp 商业场景的示例查询模板

### 9.3 Yelp 导入与验证脚本说明

- [yelp_import/README.md](/home/sch/tasks/yelp_import/README.md:1)

用途：

- 了解 Yelp 子集导入脚本和验证脚本的用法

### 9.4 Yelp 导入脚本

- [yelp_import/import_yelp_subset.py](/home/sch/tasks/yelp_import/import_yelp_subset.py:1)

用途：

- 将 Yelp 子集导入 `yelp_graph`

### 9.5 Yelp 校验脚本

- [yelp_import/validate_yelp_graph.py](/home/sch/tasks/yelp_import/validate_yelp_graph.py:1)

用途：

- 对导入后的图做基础校验

### 9.6 Yelp 导入测试报告

- [task-1-yelp-import-report.md](/home/sch/tasks/task-1-yelp-import-report.md:1)

用途：

- 查看 Yelp 图数据从导入到验证的闭环结果

## 10. Yelp 智能测试与价值对比材料

### 10.1 Yelp 智能系统测试报告

- [task-1-yelp-intelligent-test-report.md](/home/sch/tasks/task-1-yelp-intelligent-test-report.md:1)

用途：

- 查看 Yelp 图数据上的智能系统测试结果
- 查看手写 nGQL 与自然语言问题的对比分析

### 10.2 对比压缩版

- [task-1-yelp-value-comparison-summary.md](/home/sch/tasks/task-1-yelp-value-comparison-summary.md:1)

用途：

- 用于 1 到 2 页 PPT 的对比材料

### 10.3 领导口播版

- [task-1-yelp-value-comparison-leader-script.md](/home/sch/tasks/task-1-yelp-value-comparison-leader-script.md:1)

用途：

- 领导现场讲述时使用

### 10.4 3 分钟正式汇报版

- [task-1-yelp-value-comparison-3min-script.md](/home/sch/tasks/task-1-yelp-value-comparison-3min-script.md:1)

用途：

- 在正式会议中完整陈述对比价值

### 10.5 90 秒极简版

- [task-1-yelp-value-comparison-90s-script.md](/home/sch/tasks/task-1-yelp-value-comparison-90s-script.md:1)

用途：

- 快速压缩表达核心观点

### 10.6 专家追问应答版

- [task-1-yelp-value-comparison-expert-qa.md](/home/sch/tasks/task-1-yelp-value-comparison-expert-qa.md:1)

用途：

- 回答“为什么不用手写 nGQL 就够了”等专家问题

### 10.7 PPT 标题与金句版

- [task-1-yelp-value-comparison-ppt-copy.md](/home/sch/tasks/task-1-yelp-value-comparison-ppt-copy.md:1)

用途：

- 快速从中摘取标题、金句、收口句放进 PPT

## 11. 现有索引材料

### 11.1 材料总索引

- [task-1-material-index.md](/home/sch/tasks/task-1-material-index.md:1)

用途：

- 按“领导看什么、专家看什么、演示用什么”进行分类导航

## 12. 推荐阅读路径

### 12.1 如果你想 10 分钟快速了解全局

建议按顺序阅读：

1. [task-1.md](/home/sch/tasks/task-1.md:1)
2. [task-1-executive-summary.md](/home/sch/tasks/task-1-executive-summary.md:1)
3. [task-1-material-index.md](/home/sch/tasks/task-1-material-index.md:1)
4. [task-1-yelp-intelligent-test-report.md](/home/sch/tasks/task-1-yelp-intelligent-test-report.md:1)

### 12.2 如果你想学习系统是怎么做出来的

建议按顺序阅读：

1. [task-1-usage-guide.md](/home/sch/tasks/task-1-usage-guide.md:1)
2. [task-1-system-report.md](/home/sch/tasks/task-1-system-report.md:1)
3. [task-1-technical-appendix.md](/home/sch/tasks/task-1-technical-appendix.md:1)
4. [yelp_import/import_yelp_subset.py](/home/sch/tasks/yelp_import/import_yelp_subset.py:1)

### 12.3 如果你想准备汇报

建议按顺序阅读：

1. [task-1-presentation-outline.md](/home/sch/tasks/task-1-presentation-outline.md:1)
2. [task-1-ppt-full-content.md](/home/sch/tasks/task-1-ppt-full-content.md:1)
3. [task-1-presentation-script.md](/home/sch/tasks/task-1-presentation-script.md:1)
4. [task-1-architecture-diagrams.md](/home/sch/tasks/task-1-architecture-diagrams.md:1)
5. [task-1-defense-qa.md](/home/sch/tasks/task-1-defense-qa.md:1)

### 12.4 如果你想重点看任务三商业价值

建议按顺序阅读：

1. [task-1-task3-dataset-report.md](/home/sch/tasks/task-1-task3-dataset-report.md:1)
2. [task-1-task3-test-cases.md](/home/sch/tasks/task-1-task3-test-cases.md:1)
3. [task-1-yelp-nebulagraph-modeling.md](/home/sch/tasks/task-1-yelp-nebulagraph-modeling.md:1)
4. [task-1-yelp-intelligent-test-report.md](/home/sch/tasks/task-1-yelp-intelligent-test-report.md:1)

### 12.5 如果你想看“手写查询 vs 智能系统”对比

建议按顺序阅读：

1. [task-1-yelp-value-comparison-summary.md](/home/sch/tasks/task-1-yelp-value-comparison-summary.md:1)
2. [task-1-yelp-value-comparison-leader-script.md](/home/sch/tasks/task-1-yelp-value-comparison-leader-script.md:1)
3. [task-1-yelp-value-comparison-3min-script.md](/home/sch/tasks/task-1-yelp-value-comparison-3min-script.md:1)
4. [task-1-yelp-value-comparison-expert-qa.md](/home/sch/tasks/task-1-yelp-value-comparison-expert-qa.md:1)
5. [task-1-yelp-value-comparison-ppt-copy.md](/home/sch/tasks/task-1-yelp-value-comparison-ppt-copy.md:1)

## 13. 关键知识点索引

### 13.1 想看 Skill 原理

看：

- [task-1-system-report.md](/home/sch/tasks/task-1-system-report.md:48)

### 13.2 想看 MCP Server 原理

看：

- [task-1-system-report.md](/home/sch/tasks/task-1-system-report.md:207)

### 13.3 想看 Yelp 图建模

看：

- [task-1-yelp-nebulagraph-modeling.md](/home/sch/tasks/task-1-yelp-nebulagraph-modeling.md:1)

### 13.4 想看 Yelp 导入结果

看：

- [task-1-yelp-import-report.md](/home/sch/tasks/task-1-yelp-import-report.md:1)

### 13.5 想看智能系统实测效果

看：

- [task-1-yelp-intelligent-test-report.md](/home/sch/tasks/task-1-yelp-intelligent-test-report.md:1)

## 14. 一句话总结

如果只保留最核心的三份材料，建议优先看：

1. [task-1-system-report.md](/home/sch/tasks/task-1-system-report.md:1)
2. [task-1-yelp-intelligent-test-report.md](/home/sch/tasks/task-1-yelp-intelligent-test-report.md:1)
3. [task-1-ppt-full-content.md](/home/sch/tasks/task-1-ppt-full-content.md:1)

这三份基本可以帮助你快速理解：

- 这个系统是什么
- 它是怎么实现的
- 它验证了什么
- 它为什么有业务价值
