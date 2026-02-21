# 任务
先前我让你开发一套解决大查询下graph服务OOM问题的代码。我在自测此代码时，发现触发内存超限比较难，因此做了关键优化： 将原先根据水位线比率判断内存是否超限的方式改为根据内存是否超出gflag参数one_query_max_memory_usage，其代表查询时最大内存占用
我实测下来在设置为100MB时会在collectResponse触发内存超限，在设置为500MB时会在buildRequestDataSet触发内存超限
现在我需要你帮我阅读代码，分析代码实现是否存在不足和优化点，请提供建议先不着急修改代码
我自测时memory_tracker是开启的，asan是关闭的
请帮我阅读所有修改的代码排查是否存在内存泄漏
# 注意项
1、请将执行过程信息输出到代码工程目录下，以确保运行中断后能回忆和任务继续
2、电脑密码为1016，如需提权请使用
3、启动nebula数据库前需要先执行ulimit -n 65536

# 其他补充信息
编译命令（注意nebula数据库的工作命令为/usr/local/nebula）：
cmake -DCMAKE_INSTALL_PREFIX=/usr/local/nebula -DENABLE_TESTING=OFF -DCMAKE_BUILD_TYPE=Debug ..
make -j10
make install

启动nebula数据库命令：
    启动graphd：/usr/local/nebula/scripts/nebula.service start graphd
    启动metad：/usr/local/nebula/scripts/nebula.service start metad
    启动storaged：/usr/local/nebula/scripts/nebula.service start storaged

停止nebula数据库命令：
    停止graphd：/usr/local/nebula/scripts/nebula.service stop graphd
    停止metad：/usr/local/nebula/scripts/nebula.service stop metad
    停止storaged：/usr/local/nebula/scripts/nebula.service stop storaged

查看nebula数据库状态
    查看graphd：/usr/local/nebula/scripts/nebula.service status graphd
    查看metad：/usr/local/nebula/scripts/nebula.service status metad
    查看storaged：/usr/local/nebula/scripts/nebula.service status storaged

