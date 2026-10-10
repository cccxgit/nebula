# 运行记录说明

- `configure.log`、`build.log`、`install.log`：完整构建记录。build.log 中的 Interrupted 是收到用户 `make -j5` 上限后主动中断旧 `-j8`，之后续编成功；不是源码编译失败。
- `build-and-runtime-verification.json`、`toolchain.json`、`binaries.sha256`：构建退出结果、实际进程路径、文件描述符上限、工具链和二进制校验。
- `start.log`、`register-host.log`、`hosts-before.log`：专用实例的启动和注册过程。刚 ADD HOSTS 时短暂 OFFLINE，后续采样已 ONLINE。
- `http-proxy-failed.log`、`failed-init-tag-flags/`：首次脚本在 GET 本机 flags 时被宿主 HTTP 代理返回 502，尚未创建空间或提交作业。随后脚本改为明确直连 loopback，完整重新开始实验；这不是 Nebula 回放故障。
- `live-tag-flags.log`、`live-edge-flags.log`、`live-tag-template.log`：三组驱动的完整输出，与对应 live 目录中的 events.jsonl 相互对应。
- `live-summary-progress.log`：运行中间态校验，未完成组会报告缺失结束事件；最终应以 `live-summary.log` 和 live/summary.json 的最终校验结果为准。
- `runtime-resources.jsonl`：仅本专用实例三个 daemon 的进程累计计数与主机 loadavg，2 秒间隔。不是系统隔离的性能基准。
- `stop-and-status.log`、`*-full.INFO.log`：结束时停止专用实例与保存的完整服务日志。不会停止或读取其他实例的数据。

本机最初预选端口在启动前发现被另一项实验占用，因此从未在那些端口启动本实例或执行 nGQL；最终端口以 config/、toolchain.json 和实际 events 参数为准（graph 21531、storage 21631）。
