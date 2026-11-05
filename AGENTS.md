# 介绍
1、当前代码仓库是nebula graph开源图数据库3.6发布版本。
2、我是一位nebula graph的开发工程师，基于开源项目已经商用

# 注意项
1、启动nebula数据库前需要先执行ulimit -n 65536
2、执行任务时，需在任务所在目录记录关键运行日志

# 其他补充信息
编译命令（注意nebula数据库的工作目录为/home/sch/nebula-run/3.6-tantivy）：
cmake -DCMAKE_INSTALL_PREFIX=/home/sch/nebula-run/3.6-tantivy -DENABLE_TESTING=OFF -DCMAKE_BUILD_TYPE=Debug ..
make -j5
make install

迁移到公司电脑时，请按 `docs/tantivy/company-migration.md` 使用本机路径与预编译 Tantivy 静态库。上面的 `/home/sch/...` 路径仅适用于原开发机；三进程 storage 的 20 分片、3 副本演示部署使用 `scripts/tantivy-demo/cluster.py`，不要将单个默认 `nebula.service start storaged` 当作该部署的替代。编译并行度始终不超过 5。

1. 启动nebula数据库命令
- 启动graphd：/home/sch/nebula-run/3.6-tantivy/scripts/nebula.service start graphd
- 启动metad：/home/sch/nebula-run/3.6-tantivy/scripts/nebula.service start metad
- 启动storaged：/home/sch/nebula-run/3.6-tantivy/scripts/nebula.service start storaged

2. 停止nebula数据库命令
- 停止graphd：/home/sch/nebula-run/3.6-tantivy/scripts/nebula.service stop graphd
- 停止metad：/home/sch/nebula-run/3.6-tantivy/scripts/nebula.service stop metad
- 停止storaged：/home/sch/nebula-run/3.6-tantivy/scripts/nebula.service stop storaged

3. 查看nebula数据库状态
- 查看graphd：/home/sch/nebula-run/3.6-tantivy/scripts/nebula.service status graphd
- 查看metad：/home/sch/nebula-run/3.6-tantivy/scripts/nebula.service status metad
- 查看storaged：/home/sch/nebula-run/3.6-tantivy/scripts/nebula.service status storaged

4. 登陆nebula数据库
   /home/sch/nebula-run/nebula-console -addr 127.0.0.1 -port 9669 -u root -p nebula
