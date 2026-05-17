# 任务
当前仓库是nebula graph数据库3.6版本的开源仓库，定位到一个死锁问题。在同时并发创建图空间和执行leader均衡会出现死锁，并且cpu占用到100%。bug详细描述如下
1. 创建图空间占用写锁nebula::kvstore::NebulaStore::lock_和调用nebula::kvstore::NebulaStore::newEngine代码涉及线程池folly::getGlobalIOExecutor()
2. 执行leader均衡任务，storage涉及多个分片transfer leader会启动多次线程TransLeaderProcessor，也涉及线程池folly::getGlobalIOExecutor()和读锁nebula::kvstore::NebulaStore::lock_
----
编译命令（注意nebula数据库的工作命令为/usr/local/nebula）： cmake -DCMAKE_INSTALL_PREFIX=/usr/local/nebula -DENABLE_TESTING=OFF -DCMAKE_BUILD_TYPE=Debug .. make -j10 make install

启动nebula数据库命令： 启动graphd：/usr/local/nebula/scripts/nebula.service start graphd 启动metad：/usr/local/nebula/scripts/nebula.service start metad 启动storaged：/usr/local/nebula/scripts/nebula.service start storaged

停止nebula数据库命令： 停止graphd：/usr/local/nebula/scripts/nebula.service stop graphd 停止metad：/usr/local/nebula/scripts/nebula.service stop metad 停止storaged：/usr/local/nebula/scripts/nebula.service stop storaged

查看nebula数据库状态 查看graphd：/usr/local/nebula/scripts/nebula.service status graphd 查看metad：/usr/local/nebula/scripts/nebula.service status metad 查看storaged：/usr/local/nebula/scripts/nebula.service status storaged