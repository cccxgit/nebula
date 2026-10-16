# 脚本与组件工具迁移审计

审计日期：2026-10-16。适用仓库：Nebula Graph 3.6 的 `3.6-memory` 分支。实验组件原始源码固定为 `de9b3ed800a6627d9845e9289b6bbc5b6faf460a`。本次检查文件、新增迁移工具并重建运行了独立 WAL 组件，没有启动服务、接触业务数据或实现空间级参数功能。

## 1. 可以怎样迁移

**文章、源码、原始 CSV/JSON/日志可以随 Git 迁移；历史安装目录、预编译二进制和现成的自动化脚本不能直接当成异机运行环境。** 两个参数化的只读工具 `sample_runtime.py`、`parse_wal.py` 最容易复用。完整服务复现脚本 `cluster.py` 和三个 shell 驱动记载的是当时那台机器的实验流程，换机器前需要在新的实验副本中调整路径与端口。

原因很具体：旧脚本中写死了原机器的安装位置和依赖位置，输出又直接落在历史证据目录；其中还有服务启停、建空间、修改心跳和 FLUSH。把这些脚本原样运行，可能失败，也可能覆盖原证据，或者操作路径恰好对应的现有实例。因此先离线核对证据，再重建组件，最后在独立部署中做完整服务验证。

新加的 [preflight.py](preflight.py) 帮助开发者检查文件、语法、路径和依赖是否就绪。它不编译、不启动服务、不访问 HTTP、不导入旧脚本，仅向指定的新目录写报告。预检通过不等于编译通过、功能正确或性能达标。

## 2. 工具清单与实际行为

以下路径均相对 `task/wal-investigation/`。这张表依据实际文件内容，不按名称猜测安全性。

| 文件 | 做什么 | 依赖和迁移边界 |
| --- | --- | --- |
| `sample_runtime.py` | 读取进程 RSS、RocksDB HTTP 属性、WAL 文件首尾，写采样 CSV/JSON | Python 标准库；在线进程采样依赖 Linux `/proc` 和对应权限。PID、endpoint、空间、数据目录、输出均可参数化；不修改服务配置，但持续请求和遍历文件仍有开销 |
| `parse_wal.py` | 逐条读取 WAL，校验记录边界、ID/term 顺序，输出 JSON/CSV | Python 标准库；输入目录和输出可指定。已有报告会拒绝覆盖。在线读取不是跨文件一致快照，异常尾部不能直接定性为损坏 |
| `test_parse_wal.py` | 构造 10 项合成边界检查 | 标准库；会写固定相邻目录 `wal-parser-selftest/`。只能复制到新目录后重跑，不能当作完全不落盘的检查 |
| `analyze_results.py` | 从保留日志与采样重算汇总和图表 | Python 3.9+（`zoneinfo`）、系统时区数据、Matplotlib/NumPy。固定以脚本目录为输入/输出，会覆盖 `summary.json`、`wal-observe-*.csv` 和图表 |
| `analyze_runtime_rocks.py` | 重算实际窗口、RSS 斜率、FLUSH 对照和 WAL 诊断指标 | 标准库；固定相邻证据路径，覆盖 `runtime-metrics.json`；依赖 `service-logs/s*/nebula-storaged.INFO` 相对符号链接保留且有效 |
| `cluster.py` | 完整的本地 1 Meta、1 Graph、3 Storage、50 空间实验编排 | Python 标准库与 Linux；固定安装目录、console、账号、端口、空间数量。包含写配置、启停、元数据变更、在线 flag 修改与 FLUSH，不是生产部署工具 |
| `run-atomic-buffer-probe.sh` | 从固定 Git 提交提取原始缓存源码，编译两个分配器版本并运行组件矩阵 | 固定仓库根目录、`/usr/bin/c++`、`/opt/vesoft/third-party/3.3`、`build/src`。会直接覆盖 baseline、环境记录、二进制、CSV/meta；禁止在历史证据原位执行 |
| `production-tuning-evidence/run-capacity-probe.sh` | 用现成 jemalloc 缓存组件运行五档容量 | 固定仓库根目录和现成二进制；已有首档 CSV 时拒绝执行。没有异机编译步骤，不能用删除旧 CSV 的方式绕过保护 |
| `write-idle-comparison-20261011/run.sh` | 编译并比较空 payload、256 B、1 KiB、停写后空日志 | 固定仓库与依赖位置；已有指定结果时拒绝执行。需要复制后修改输出位置；`x` 填充 payload 是缓存负载，不是可用于真实图状态机的业务编码 |
| `atomic-buffer-probe.cpp` | 直接构造原始 `AtomicLogBuffer`，观察节点、引用、计账容量和分配器字节 | C++17；Folly、gflags、glog、gtest 头文件、仓库头文件；jemalloc 版本另需 jemalloc。仅组件进程，不启动 Nebula |
| `write-idle-comparison-20261011/payload-probe.cpp` | 带 payload 和停写后的缓存组件比较 | 同上；面向原始结构字段，跨商用改动或其他版本必须重新核对布局和行为 |
| `rocks-commit-probe.cpp` | 在新建独立 RocksDB 中覆盖系统提交位置键，再 FLUSH | C++17、同工程 RocksDB/压缩库/jemalloc；Linux RSS 采样，驱动要求小端。会创建数据库；拒绝复用已存在数据目录 |
| `design-evidence/build_design_docx.py` | 从主 Markdown 生成 Word 设计文档 | `python-docx`、中文字体 `Noto Sans CJK SC`；输出固定，覆盖 Word 和 `docx-build.json`，属出版工具，不是内核验证工具 |
| `design-evidence/render_with_host_libreoffice.py` | 针对原主机运行时兼容性问题的 Word 渲染回退 | 写死 Codex 文档插件版本路径，使用 `/usr/bin/soffice` 和 Poppler；只记录当时的出版环境，异机需改为目标机器可用渲染工具 |
| `migration/preflight.py` | 静态只读迁移预检 | Python 3.9+、Linux/Unix `resource`；可选检查依赖/安装路径和本机 PID，不请求服务，新输出目录拒绝覆盖 |
| `migration/run_components.py` | 参数化重建两个原始 WAL 组件，并运行 smoke 或完整对照 | Python 3.9+、Linux、前述 C++ 依赖；repo/third-party/build-dir/compiler/output 均可指定，固定 baseline 摘要校验，单次串行编译；只写新目录，不启服务 |
| `baseline/kvstore/wal/AtomicLogBuffer.cpp/.h` | 固定提交的原始 WAL 缓存实现，供组件对照 | 保持原样；不是改造后的内核实现，也不能将它覆盖到商用源码后称为安全合并 |
| `observation.patch` | 完整服务中新增 WAL 节点等计数与日志 | 应先检查目标源码差异再决定是否应用；它用于观测，没有实现空间级缓存配置。Debug 构建、日志与计数开销不能直接代表生产性能 |

其他 nGQL 文件也需要区分：`create-spaces.ngql` 创建 50 个图空间；`flush.ngql` 提交 50 个 FLUSH 作业；`inspect.ngql` 与 `jobs.ngql` 主要查询已有状态。`IF NOT EXISTS` 只避免重复创建报错，不证明同名已有空间的参数符合本实验。

任务目录保留的四个 ELF 工件是 `atomic-buffer-probe`、`atomic-buffer-probe-jemalloc`、`rocks-commit-probe` 和 `write-idle-comparison-20261011/payload-probe`。它们是历史构建产物。异机需要从源码重建，不能将“文件在 Git 中存在”当作 ABI、依赖库或分配器兼容的证明。

## 3. 第一步 在目标环境做只读预检

在目标机器的工程根目录执行。以下命令为建议验证，本轮没有在另一台开发机运行。`migration_run` 是本次输出目录，`preflight` 子目录必须不存在。

```bash
migration_run=$(mktemp -d "$PWD/task/wal-investigation/migration/target.XXXXXX")
python3 task/wal-investigation/migration/preflight.py \
  --repo "$PWD" \
  --output "$migration_run/preflight"
```

如需检查实验安装位置，显式添加实际路径；示例 `/path/to/...` 必须先替换，工具不会创建或修改这些位置。

```bash
python3 task/wal-investigation/migration/preflight.py \
  --repo "$PWD" \
  --install /path/to/isolated-nebula-install \
  --console /path/to/nebula-console \
  --third-party /path/to/third-party/3.3 \
  --output "$migration_run/preflight-with-paths"
```

报告记录 Git HEAD、固定 baseline 的 Git blob 校验、Python/shell 语法、硬编码位置、历史 ELF 工件、符号链接、进程句柄上限及依赖位置。只有显式加 `--pid` 才检查指定的本机进程，读取内容仅为 exe/cwd 和少量内存状态，不发送信号，不读取进程命令行或环境变量。

返回码 0 表示静态必需检查未发现错误，警告仍需阅读；返回码 1 表示检查出语法/版本错误；命令参数或输出已存在时返回码 2。路径存在不代表文件版本一致、动态库可链接或服务可启动。预检不会扫描开放端口、验证业务网络、查询集群元数据或评估磁盘容量。

## 4. 第二步 先复核旧证据 再进行新实验

### 4.1 在新目录运行解析器合成检查

解析器测试没有目标参数，输出固定跟随脚本目录，因此复制两个文件后运行。复制的必要性是保护既有合成结果，不涉及更改被测算法。

```bash
mkdir "$migration_run/parser-selftest"
cp task/wal-investigation/parse_wal.py \
   task/wal-investigation/test_parse_wal.py \
   "$migration_run/parser-selftest/"
python3 "$migration_run/parser-selftest/test_parse_wal.py" \
  > "$migration_run/parser-selftest.log" 2>&1
```

预期为 10 项通过。它证明解析器能识别这些合成边界，不证明任意磁盘文件内容的完整性。

### 4.2 重读已停止服务的历史 WAL 归档

先核对归档摘要应为 `acb2e767bf72b9b715879c6a911e53ef115e30469f2185e8e4b3205ad988fd55`。只有摘要符合本次已知工件后，才执行后续解包命令。解包目录为新的目录，不是实际运行数据目录。

```bash
sha256sum task/wal-investigation/wal-evidence.tar.gz
mkdir "$migration_run/archive"
tar -xzf task/wal-investigation/wal-evidence.tar.gz \
  -C "$migration_run/archive"
python3 task/wal-investigation/parse_wal.py \
  --data-dir "s1=$migration_run/archive/data/storage1" \
  --data-dir "s2=$migration_run/archive/data/storage2" \
  --data-dir "s3=$migration_run/archive/data/storage3" \
  --output "$migration_run/reparsed" \
  > "$migration_run/reparsed.log" 2>&1
```

预期：退出码 0，3000 个文件、6,730,574 条记录、215,378,368 B，全部 payload 为空且 `has_anomalies=false`。路径与扫描时间随机器变化，不应逐字比较整个 JSON。该格式没有 payload checksum；结构和顺序通过不能替代摘要校验，也不能证明空日志全部来自某一种后台动作。

### 4.3 原始汇总和图表应在复制目录中重算

`analyze_results.py` 和 `analyze_runtime_rocks.py` 按相邻目录查找历史数据并覆写衍生结果。要重算，就把完整证据复制到一个新的工作目录并保留相对符号链接，再运行复制版。不要将单个脚本移到空目录后期待它自动发现原证据。

`plot-requirements.txt` 固定 Matplotlib 3.10.9 / NumPy 2.2.6。迁移时应使用单独 Python 虚拟环境；这些版本有各自 Python 支持范围和可用 wheel 限制，不能仅凭本机 Python 3.9 满足 stdlib 脚本就保证画图依赖能安装。不能安装时可先完成 CSV/JSON 复核，保留已有图；需要更改依赖版本时单独记录该环境与输出差异。

## 5. 第三步 在目标机重建组件

推荐使用新加的可参数化工具。默认 smoke 模式会重建两个组件，检查短序列跨 64 槽节点边界及 payload 读回；`full` 模式重建后运行五档容量与四组 payload/停写对照。每次必须给不同的新输出目录，参数中的实际路径需替换。

```bash
python3 task/wal-investigation/migration/run_components.py \
  --repo "$PWD" \
  --third-party /path/to/third-party/3.3 \
  --build-dir "$PWD/build" \
  --output "$migration_run/components-smoke" \
  --mode smoke
```

```bash
python3 task/wal-investigation/migration/run_components.py \
  --repo "$PWD" \
  --third-party /path/to/third-party/3.3 \
  --build-dir "$PWD/build" \
  --output "$migration_run/components-full" \
  --mode full
```

`run.json` 记录精确命令、编译器、驱动与 baseline 摘要、二进制摘要和检查结果；编译/运行标准输出与标准错误分别保存。工具只对本次组件进程的 `LD_LIBRARY_PATH` 添加指定依赖的 `lib/lib64`，不会修改机器环境配置。出错保留输出目录供排查，不能原目录重试。`--compiler` 只接受可执行文件名或路径，不支持一段 shell 命令。

下面另列出依据实际编译脚本整理的手工命令，便于开发者理解依赖；本轮实际验证使用上面的新工具。先将 `deps_prefix` 换成实际第三方依赖前缀，核对 Folly、gflags、glog、jemalloc 等是否与目标 Nebula 构建一致。`build/src` 是历史构建目录约定，若商用工程换了构建目录，需替换。编译器、标准库、架构或分配器变化可能改变对象大小和 RSS，实测应记录新值，不强求新机器上的每一个字节与旧机器相同。

```bash
deps_prefix=/path/to/third-party/3.3
c++ -std=c++17 -O2 -g \
  -DENABLE_JEMALLOC -DENABLE_MEMORY_TRACKER \
  -DS2_USE_GFLAGS -DS2_USE_GLOG -D__STDC_FORMAT_MACROS \
  -I task/wal-investigation/baseline -I src -I build/src \
  -isystem "$deps_prefix/include" \
  task/wal-investigation/atomic-buffer-probe.cpp \
  task/wal-investigation/baseline/kvstore/wal/AtomicLogBuffer.cpp \
  -L "$deps_prefix/lib" -lfolly -lglog -lgflags -ljemalloc -ldl -pthread \
  -o "$migration_run/atomic-buffer-probe-jemalloc" \
  > "$migration_run/component-build.log" 2>&1
```

```bash
"$migration_run/atomic-buffer-probe-jemalloc" \
  1 1048576 each 0,1,64,65,65536,65537,65856,131072,262144 \
  > "$migration_run/capacity-1m.csv" \
  2> "$migration_run/capacity-1m.meta"
```

比较 `logical_bytes`、`nodes`、`dirty_nodes`、`refs` 和分配器字节：正常读者退出后，充分填充时应出现平台，而不是每条空记录都持续增加活节点。旧机器 1 MiB 样本平台为 1024 个节点、3,670,016 B 可用分配；异机字节值可能变化。`destroy_all` 应使节点数归零，RSS 不要求立即下降。

此命令验证的是固定基线原始算法，**没有运行新空间级配置机制**。验证商用代码时需要另外标记 commit 和修改集合，不能悄悄把 baseline 换成商用代码却沿用旧实验身份。

payload 组件按原 `write-idle-comparison-20261011/run.sh` 的编译参数，将 `-o` 和四组结果路径改为新目录。RocksDB 组件的实际链接库及 `-no-pie` 命令见 [COMPONENT-ANALYSIS.md](../COMPONENT-ANALYSIS.md) 第 5.2 节；同时改用新的数据库目录与 CSV/meta 文件。不同链接布局可能需要目标工程导出的实际依赖，缺库时不能用系统中任意版本的库补齐后仍声称“同工程组件对照”。

## 6. 第四步 完整服务复现仅在独立部署运行

完整流程见 [reproduction-audit.md](../design-evidence/reproduction-audit.md) 第 6 节。本次整理不新增自动启动入口，避免将一条看似普通的迁移检查变成创建 50 个空间、改变心跳的操作。

准备步骤必须包括：

1. 使用全新专用安装位置及独立数据/PID/日志目录。将 `cluster.py`、`sample_runtime.py` 复制到仓库 `task/` 下与 `wal-investigation` 同级的新目录，保留 `HERE.parent.parent` 指向工程根目录的关系。
2. 修改复制版的 `INSTALL`、`CONSOLE`、端口和凭据；除文件顶部端口变量外，`prepare()` 中直接写入的 HTTP 39559/39669/39779 也需核对。还要核对服务派生的内部/Raft 端口，不能只看 HTTP。
3. `prepare` 仅在 `INSTALL/data` 非空时拒绝；它不检查自定义外部数据位置、已有配置或运行中实例，随后会覆写安装目录配置。因此仅能用于已人工核对的全新部署。
4. 原版脚本启用了 `wal_memory_observe` 新诊断 flag；二进制需包含对应观测补丁。未加补丁的发布二进制不会提供这些内部计数，不应直接沿用配置。
5. 若重编译完整 Nebula，使用用户要求的 **`make -j5` 最大并行度**。启动前执行 `ulimit -n 65536`，并确认硬上限允许。使用目标实验安装目录的服务脚本。
6. 脚本启动只固定等待，不代表服务、空间分片和 Leader 已就绪；需核对全部 HOST ONLINE、空间/副本数量及 nGQL 逐条结果。控制台进程退出成功不等于每条 nGQL 成功。
7. 1 秒心跳只是实验加速，不是生产调优建议。恢复参数后保留过渡窗口；FLUSH 是异步作业，必须核对所有作业 `FINISHED`。结束时验证本次进程退出，再归档 WAL。

50 空间 × 20 分区 × 3 副本会创建总计 3000 个本地副本；原同机三 storage 部署每进程约 1000 个。目标机是否有足够内存、磁盘和文件句柄，需要单独评估。原拓扑是机制实验，不代表跨机器、带容器限额的生产容量与性能。

## 7. 本次实际检查记录与结论边界

新增预检已经在本机运行，最终完整记录位于 [preflight-complete-20261016/preflight.json](preflight-complete-20261016/preflight.json) 与对应 `.log`。本轮完整安全检查记录见 [portability-validation.log](portability-validation.log)。

本机最终静态结果：13 个 Python/shell 文件通过语法检查；固定 baseline 的两个文件与记录提交 Git blob 相符；已有相对日志链接在本机可解引用。首轮报告在组件重建前记录了 11 处历史主机路径和 4 个历史 ELF 工件；最终报告还会列出本轮新生成的组件二进制。该结果只证明当前交付文件可读取、相关语法可解析；不能替代新机器上的预检。

新组件工具实际完成了 [smoke 运行](component-smoke-20261016/run.json) 与 [full 运行](component-full-20261016/run.json)，均重新编译两个驱动且退出 0。full 模式五档容量的最后一个 push 平台分别为：

| 计账容量 | 节点数 | 分配器可用字节 | refs / dirty |
| --- | ---: | ---: | --- |
| 8 MiB | 8192 | 29,360,128 | 0 / 0 |
| 4 MiB | 4096 | 14,680,064 | 0 / 0 |
| 2 MiB | 2048 | 7,340,032 | 0 / 0 |
| 1 MiB | 1024 | 3,670,016 | 0 / 0 |
| 256 KiB | 256 | 917,504 | 0 / 0 |

五档销毁后的节点均归零；四组 payload/停写对照均完成，结果保存在 full 目录中。最后一步还从 `/tmp` 调用预检验证默认工程定位，并实际确认两个工具面对已存在的输出目录均返回 2，原报告摘要保持不变。

这次重建验证了新迁移工具在原机器的可执行性，容量平台与旧证据吻合；没有测量新空间级功能、跨机器兼容性或商用性能。新生成二进制也应按构建产物处理，提交 Git 时纳入工具源码和运行记录即可，目标机器重新编译。

最小迁移验收顺序是：新目录的只读预检 → 解析器合成检查 → 历史 WAL 归档复核 → 目标机组件重建 → 独立服务基线复现 → 新机制功能/故障/混部性能验证。空间级参数设计目前是需求与方案，最后一步仍需要开发实现后执行。
