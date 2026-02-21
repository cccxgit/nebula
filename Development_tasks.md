# 任务
先前我让你开发一套解决大查询下graph服务OOM问题的代码，请阅读代码修改点和技术报告（在代码工程跟目录下NebulaGraph_GraphStorage_OOM_Analysis_Report.md、NebulaGraph_OOM_Pressure_Execution_Report.md、NebulaGraph_OOM_Protection_Targeted_Test_Report.md、NebulaGraph_OOM_组合方案_函数级源码讲解.md、NebulaGraph_OOM_组合方案_技术实现方案书.md）
我需要你做的是：
1、根据当前的技术实现方案，设计完整的测试方案和测试用例（建议在关键功能处添加日志，通过日志服务可精准确保新功能代码的有效性）
2、根据设计的用例依次执行，并验证代码功能的可靠性和正确性

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

# 新增信息
1、我预先导入了LDBC v0.3.3 的标准数据集在数据库的图空间stress_test_0221和stress_test_0220中。建议使用此数据集测试以确保有足有的数据开展查询

请阅读

