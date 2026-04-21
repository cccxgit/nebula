# 介绍
1、当前代码仓库是nebula graph开源图数据库3.6发布版本。
2、我是一位nebula graph的开发工程师，基于开源项目已经商用
3、当前我在分析数据均衡功能，包括数据均衡的本身功能机制、可靠性与稳定性、现存问题、性能/调优等等，目标是将此项功能商业化

# 环境介绍
我当前现网环境配置：3节点环境，k8s容器化部署nebula graph，其中graph/meta/storage均为3实例部署

# 数据均衡操作介绍
背景是当前商用现网环境出现storage存储服务性能瓶颈和磁盘资源瓶颈，考虑使用水平扩容storage解决瓶颈问题。
阅读nebula graph的官方文章https://docs.nebula-graph.com.cn/3.1.0/synchronization-and-migration/2.balance-syntax/，在数据均衡任务完成后，还需要手动下发DATA BALANCE任务，将数据在新扩的节点上打平
