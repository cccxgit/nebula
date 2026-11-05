# 本机构建与复现入口

本页记录原开发机已使用的工具链和隔离部署入口。公司电脑没有 Rust 时请直接使用 [迁移与无 Rust 构建指南](company-migration.md)，其中明确设置了预编译静态库和目标机运行目录。用户最新要求将编译并行度限制为 **最多 5**，仓库 `AGENTS.md` 也已更新为 `make -j5`。正式性能计时期间不要构建、导入其他语料或执行正确性回归。

## 构建前提

本机配置记录为 Ubuntu 22.04、GCC 11.4、Nebula third-party 3.3（`/opt/vesoft/third-party/3.3`）、Rust/Cargo 1.97.1。可核对 `task-logs/tantivy-build/configure.log`、`task-logs/tantivy-bridge/toolchain.log` 与 `build/CMakeCache.txt`。这些是本次环境记录，不是已完成其他平台验收的声明。

Rust 依赖由 `src/common/plugin/fulltext/tantivy/rust/Cargo.lock` 固定。本机源码构建模式使用 `cargo build --release --locked`，内部 Rust 构建限制为 1 个任务；首次源码构建需要 Cargo 依赖缓存或下载访问。公司电脑可改用带源码和 SHA256 校验的预编译静态库模式（`-DNEBULA_TANTIVY_USE_PREBUILT=ON`），不需要 Cargo。保留 lock 文件、`vendor/include-flate` 及其许可和补丁说明；目录内 `.gitignore` 已为 lock 文件设置例外。部署后的数据库不需要 Cargo 或独立 Tantivy 服务。

构建前固定所有源码和头文件，构建期间不要同时修改它们。开发过程中曾因头文件在对象编译中途改变而产生布局不一致；若复用有这种历史的构建目录，应在停止编译后清理并完整重建。以下命令不主动清除已有构建产物。

```bash
cd /home/sch/nebula/nebula-3.6-tantivy
set -o pipefail
mkdir -p build task-logs/tantivy-build
cmake -S . -B build \
  -DCMAKE_INSTALL_PREFIX=/home/sch/nebula-run/3.6-tantivy \
  -DENABLE_TESTING=OFF -DCMAKE_BUILD_TYPE=Debug \
  2>&1 | tee task-logs/tantivy-build/reproduce-configure.log
cmake --build build --parallel 5 \
  2>&1 | tee task-logs/tantivy-build/reproduce-build.log
cmake --install build \
  2>&1 | tee task-logs/tantivy-build/reproduce-install.log
```

安装前停止本次隔离部署的现有服务，避免更新正在使用的二进制；首次部署无需停止不存在的进程。`ENABLE_TESTING=OFF` 不会执行 C++ 单元测试，不能把成功构建解释为测试通过。独立 Rust 回归可在桥接目录执行 `cargo test --locked -j1 --manifest-path rust/Cargo.toml`，不要与数据库性能测量重叠。

## 隔离部署

Python 验证依赖锁定在 `scripts/tantivy-benchmark/requirements.txt`。以下流程创建独立 Python 环境；后续所有管理和回归脚本使用这个环境。

```bash
python3 -m venv /home/sch/nebula-run/3.6-tantivy/python-venv
source /home/sch/nebula-run/3.6-tantivy/python-venv/bin/activate
python3 -m pip install -r scripts/tantivy-benchmark/requirements.txt
ulimit -n 65536
python3 scripts/tantivy-demo/cluster.py init
python3 scripts/tantivy-demo/cluster.py start
python3 scripts/tantivy-demo/cluster.py status
```

`init` 只生成配置，在已有隔离进程运行时拒绝重写。`start` 先启动 meta 和 graph，注册三个 storage 地址，再启动三个 storage；不会删除现有数据。脚本核对 PID 所属二进制和配置，检查端口冲突，并把文件描述符限制提高到至少 65536。

运行数据、配置和日志分置于 `/home/sch/nebula-run/3.6-tantivy/tantivy-demo/` 的五个服务目录。RPC 端口为 graph `9669`、meta `9559`、storage `9779/9789/9799`，HTTP 端口为 `19669/19559/19779/19789/19799`；还需留出 storage 的内部、管理和 Raft 端口。管理日志为 `task-logs/tantivy-demo/cluster.jsonl`。需要停机时使用 `python3 scripts/tantivy-demo/cluster.py stop`。

使用此三 storage 隔离部署时，不要再使用默认 `nebula.service` 启动同端口的另一套服务。`enable_optimizer=true` 从现有 graph 配置模板继承，启动时注册优化规则；运行后仅修改此 flag 不会重新注册规则。原型的普通谓词开关 `enable_tantivy_pushdown` 默认开启。

## 正确性复现

使用未出现过的空间名和新的日志目录。以下命令会写入各自的人工数据空间，不属于性能测量。

```bash
python3 scripts/tantivy-demo/demo.py \
  --space tantivy_reproduce_01 --output task-logs/tantivy-demo/reproduce-01
python3 scripts/tantivy-demo/lifecycle.py \
  --space tantivy_lifecycle_01 --output task-logs/tantivy-lifecycle/reproduce-01
python3 scripts/tantivy-benchmark/tantivy_regression_setup.py \
  --space ft_regression_reproduce_01 --index ftcase_reproduce_01 \
  --output task-logs/tantivy-benchmark/regression-reproduce-01
```

语法入口使用同一份 8 条人工 fixture，检查 20 分区、3 副本，等待 `TEXT_SEARCH(index, "*")` 的完整 8 个 VID 可见，并用 PROFILE 证明 Tantivy 路径，然后调用现有 `regression.py` 执行 18 例。它保存 fixture/cases 的 SHA256、查询日志、计划和 JSON 结果；预期语法错误被拒绝才算通过。密码读取 `NEBULA_PASSWORD`，未设置时使用本地测试约定 `nebula`。

现场演示和完整结果解释见 [demo-guide.md](demo-guide.md)，性能流程见 [benchmark-runbook.md](benchmark-runbook.md)，跨引擎分词差异见 [fulltext-compatibility.md](fulltext-compatibility.md)。
