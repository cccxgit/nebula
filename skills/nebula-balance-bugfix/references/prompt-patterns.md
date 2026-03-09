# Prompt Patterns for Nebula Balance Bugfix

## 1. Read the codebase first

```text
请先不要修改代码。

我在排查 Nebula Graph 的一个复杂工程 bug：
storage 服务水平扩容后，执行数据均衡不稳定，经常失败。

请先做这几件事：
1. 解释与 data balance / storage balance 最相关的模块
2. 找出入口、核心执行链路、任务状态流转位置
3. 列出最值得先读的 10 个文件，并按重要性排序
4. 说明哪些地方最可能与扩容后执行失败有关

请不要改代码，只做代码库理解。
