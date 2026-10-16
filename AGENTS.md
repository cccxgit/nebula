# 介绍
1、当前代码仓库是nebula graph开源图数据库3.6发布版本。
2、我是一位nebula graph的开发工程师，基于开源项目已经商用

# 注意项
1、启动nebula数据库前需要先执行ulimit -n 65536
2、执行任务时，需在任务所在目录记录关键运行日志
3、后续所有分析结论必须遵循工程根目录的 `nebula_graph_readable_analysis_prompts.md`：先讲清楚因果，再给目标版本源码和实验依据，区分已确认事实与待验证推断。

# 其他补充信息
编译命令（注意nebula数据库的工作目录为/home/sch/nebula-run/nebula-memory）：
cmake -DCMAKE_INSTALL_PREFIX=/home/sch/nebula-run/nebula-memory -DENABLE_TESTING=OFF -DCMAKE_BUILD_TYPE=Debug ..
make -j5
make install

编译最多使用 5 个并行进程，遵守用户在本会话中的明确要求。上述安装和控制台路径是原实验机器的本地路径；迁移后应改为目标环境的独立测试目录。成果盘点与迁移验证入口见 `task/wal-investigation/DELIVERY-AND-MIGRATION.md`。

1. 启动nebula数据库命令
- 启动graphd：/home/sch/nebula-run/nebula-memory/scripts/nebula.service start graphd
- 启动metad：/home/sch/nebula-run/nebula-memory/scripts/nebula.service start metad
- 启动storaged：/home/sch/nebula-run/nebula-memory/scripts/nebula.service start storaged

2. 停止nebula数据库命令
- 停止graphd：/home/sch/nebula-run/nebula-memory/scripts/nebula.service stop graphd
- 停止metad：/home/sch/nebula-run/nebula-memory/scripts/nebula.service stop metad
- 停止storaged：/home/sch/nebula-run/nebula-memory/scripts/nebula.service stop storaged

3. 查看nebula数据库状态
- 查看graphd：/home/sch/nebula-run/nebula-memory/scripts/nebula.service status graphd
- 查看metad：/home/sch/nebula-run/nebula-memory/scripts/nebula.service status metad
- 查看storaged：/home/sch/nebula-run/nebula-memory/scripts/nebula.service status storaged

4. 登陆nebula数据库
   /home/sch/nebula-run/nebula-console -addr 127.0.0.1 -port 9669 -u root -p nebula
