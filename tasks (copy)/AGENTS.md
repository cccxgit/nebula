# Global Codex Instructions

- 默认使用中文回答
- 可以使用root权限，root密码为1016
- 根据任务需要，联网搜索、自行下载安装软件、创建删除写入文件等等
- 当前工程目录下NebulaGraph-Skill目录为开源项目，开源地址为https://github.com/knqiufan/NebulaGraph-Skill
- 当前工程下目录下nebulagraph-mcp-server目录为开源项目，开源地址为https://github.com/nebula-contrib/nebulagraph-mcp-server
- 本机nebula graph的工程目录在/usr/local/nebula
- nebula graph启动命令
```
ulimit -n 65535
/usr/local/nebula/scripts/nebula.service start all
```
- nebula graph停止命令
```
/usr/local/nebula/scripts/nebula.service stop all
```
- 客户端介入nebula graph命令
```
/usr/local/nebula/scripts/nebula-console -addr 127.0.0.1 -port 9669 -u root -p nebula
```
