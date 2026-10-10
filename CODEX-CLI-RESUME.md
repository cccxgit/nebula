# 使用 Codex CLI 继续当前会话

## 本次只读错误的原因

本机于 2026-10-10 排查确认：直接恢复时，桌面 ChatGPT 的后台进程仍持有当前会话的独占写入锁，CLI 连接的另一个后台实例无法取得写入权。这不是工程目录只读，也不是增加文件沙箱权限就能解决的问题。

CLI 显示：

```text
This conversation is read-only or unavailable; no operation was sent.
```

对应的底层错误（北京时间 2026-10-10 07:55:40）：

```text
Failed to create session: thread-store conflict: thread 01a122bc-efa6-7351-8ea5-7a9569f4bce0 already has an active writer
```

当时 `/proc/locks` 确认该会话的 `.lock` 文件由 PID 34358 持有 WRITE/FLOCK 锁；进程为 `/usr/lib/chatgpt/resources/codex`，父进程是 `/usr/lib/chatgpt/ChatGPT`。CLI 恢复请求则来自另一后台进程 PID 114652。PID 是当时的观测值，不应当作今后的固定值。

证据保存在 [恢复失败原始日志](task/codex-cli-resume-diagnostics/active-writer-errors.json) 和 [锁持有进程快照](task/codex-cli-resume-diagnostics/active-writer-owner.json)。

此前本说明把“CLI 支持 resume，且本地存在会话记录”直接描述为“可以续接”，漏掉了运行实例占用条件，现予以修正。

## 在 CLI 中继续原会话

1. 退出已进入只读状态的 CLI 界面。
2. 等桌面端需要保留的运行任务结束，然后使用应用菜单完整退出桌面 ChatGPT。仅最小化或关闭窗口，后台进程可能仍在。
3. 在同一台机器上，以 `sch` 用户打开终端，执行：

```bash
cd /home/sch/nebula/nebula-index
codex resume 01a122bc-efa6-7351-8ea5-7a9569f4bce0
```

释放独占写入锁后，可使用此命令尝试加载原会话并继续。为保留当前桌面会话，本次排查没有自动退出桌面应用，因而尚未实际执行“退出后成功恢复”的完整验证；已确认的是锁冲突根因及命令语法。

如仍出现同一错误，需要检查持锁后台进程是否还在。`--sandbox workspace-write` 只控制工具写文件的权限；`--no-daemon` 只改变 CLI 的运行方式，两者都不会释放另一个进程持有的会话锁。不要通过删除 `.lock` 文件或修改会话数据库强行绕过独占写入保护。

也可以在恢复命令中直接附上下一条要求，例如：

```bash
cd /home/sch/nebula/nebula-index
codex resume 01a122bc-efa6-7351-8ea5-7a9569f4bce0 \
  "继续当前索引重建问题分析，先阅读已有报告；仅分析和复现，不实现修复，源码编译最大并发为 make -j5。"
```

## 保留桌面会话、在 CLI 分支继续

如果需要桌面端保持打开，可尝试创建独立会话分支：

```bash
cd /home/sch/nebula/nebula-index
codex fork 01a122bc-efa6-7351-8ea5-7a9569f4bce0
```

`fork` 会创建新的会话 ID，沿用此前历史；之后两个会话独立发展，不是原会话的实时双向同步。该命令语法已核实，本次未实际执行 fork 或验证占用期间 fork 的结果。

官方命令说明：[resume / fork](https://learn.chatgpt.com/docs/cli/reference)。

## 已核实的本机信息

- 会话 ID：`01a122bc-efa6-7351-8ea5-7a9569f4bce0`。
- 工程目录：`/home/sch/nebula/nebula-index`。
- Codex 数据目录：`/home/sch/.codex`。
- CLI 路径：`/home/sch/.local/npm-global/bin/codex`。
- 首次检查 CLI 版本为 `0.154.0`；本次错误排查时 CLI 及其独立后台实例均为 `0.162.1`。本机 `codex resume --help` 和 `codex fork --help` 均支持按会话 ID 操作。
- 当前会话的 rollout 路径：`/home/sch/.codex/sessions/2026/10/10/rollout-2026-10-10T06-16-09-01a122bc-efa6-7351-8ea5-7a9569f4bce0.jsonl`。本次数据库记录的历史模式为 `paginated`；不要把单个 JSONL 文件视为全部会话状态或手工迁移方案。

如果终端提示找不到 `codex`，可使用完整路径：

```bash
cd /home/sch/nebula/nebula-index
/home/sch/.local/npm-global/bin/codex resume 01a122bc-efa6-7351-8ea5-7a9569f4bce0
```

如果终端设置了其他 `CODEX_HOME`，可只对本次启动指定当前会话所在的数据目录：

```bash
cd /home/sch/nebula/nebula-index
CODEX_HOME=/home/sch/.codex /home/sch/.local/npm-global/bin/codex resume 01a122bc-efa6-7351-8ea5-7a9569f4bce0
```

## 当前分析材料

- [完整报告入口](task/index-rebuild-investigation/README.md)
- [根因分析](task/index-rebuild-investigation/01-root-cause.md)
- [点、边索引实现详解](task/index-rebuild-investigation/02-index-mechanism.md)
- [真实服务复现步骤与证据](task/index-rebuild-investigation/05-live-reproduction.md)

本次任务要求保持为：分析问题并提供复现证据；允许增加日志等观测手段，不实现修复；编译最大并发为 `make -j5`。已有测试实例已停止，数据、配置、报告和原始日志保留在本机。
