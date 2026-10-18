# 任务名称
## 背景
为解决nebula graph的性能问题，我计划将部分rocksdb_column_family_options参数开发成图空间级别，且支持通过rest接口实现热生效

## 方案
1. 支持的rocksdb_column_family_options列表：disable_auto_compactions、write_buffer_size、max_write_buffer_number
2. 接口定义：
  - 查询图空间级别配置：GET /admin/v1/spaces/{spaceId}/rocksdb/cf-options
  - 修改图空间级别配置：PATCH /admin/v1/spaces/{spaceId}/rocksdb/cf-options
3. 修改图空间级别配置接口入参
