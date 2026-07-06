# 介绍
1、当前代码仓库是nebula graph开源图数据库3.6发布版本。
2、我是一位nebula graph的开发工程师，基于开源项目已经商用

# 注意项
1、启动nebula数据库前需要先执行ulimit -n 65536
2、执行任务时，需在任务所在目录记录关键运行日志

# 其他补充信息
编译命令（注意nebula数据库的工作目录为/usr/local/nebula）：
cmake -DCMAKE_INSTALL_PREFIX=/usr/local/nebula -DENABLE_TESTING=OFF -DCMAKE_BUILD_TYPE=Debug ..
make -j10
make install

1. 启动nebula数据库命令
- 启动graphd：/usr/local/nebula/scripts/nebula.service start graphd
- 启动metad：/usr/local/nebula/scripts/nebula.service start metad
- 启动storaged：/usr/local/nebula/scripts/nebula.service start storaged

2. 停止nebula数据库命令
- 停止graphd：/usr/local/nebula/scripts/nebula.service stop graphd
- 停止metad：/usr/local/nebula/scripts/nebula.service stop metad
- 停止storaged：/usr/local/nebula/scripts/nebula.service stop storaged

3. 查看nebula数据库状态
- 查看graphd：/usr/local/nebula/scripts/nebula.service status graphd
- 查看metad：/usr/local/nebula/scripts/nebula.service status metad
- 查看storaged：/usr/local/nebula/scripts/nebula.service status storaged

4. 登陆nebula数据库
   /usr/local/nebula/scripts/nebula-console -addr 127.0.0.1 -port 9669 -u root -p nebula
