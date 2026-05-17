# Yelp 导入脚本

## 文件

- `import_yelp_subset.py`
  - 从 Yelp 官方 JSON 数据中抽取 `Phoenix / Las Vegas` 的高价值子集并导入 `yelp_graph`
- `validate_yelp_graph.py`
  - 导入后做基础图校验

## 依赖

建议通过已有虚拟环境执行：

```bash
~/.local/bin/uv run --directory /home/sch/tasks/nebulagraph-mcp-server python /home/sch/tasks/yelp_import/import_yelp_subset.py
```

## 说明

当前策略不是全量导入，而是：

- 使用 Yelp 官方下载数据
- 抽取高价值城市和重点品类
- 构建适合智能演示与商业场景测试的图子集

这样可以更快完成 PoC，同时保留真实数据关系结构。
