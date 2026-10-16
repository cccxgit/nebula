# 会话成果清单与开发环境迁移说明

整理日期：2026 年 10 月 16 日。仓库：`cccxgit/nebula`，工作分支：`3.6-memory`。机制分析基线：`de9b3ed800a6627d9845e9289b6bbc5b6faf460a`。

## 1 这次交接的实际内容

本次成果包括问题分析文章、需求设计、复现与采样脚本、三个 C++ 组件探针、完整原始证据，以及一份可以单独检查的产品观测补丁。它们足以让新开发者先复核现有结论，再在独立开发环境重新构建和复现实验。

**图空间级 WAL 参数机制尚未实现。** 当前真正改动的产品源码只有 `src/kvstore/wal/AtomicLogBuffer.cpp`，新增 124 行计数与日志，没有改变原缓存容量、标删和回收条件。此次迁移验证首先针对原问题、观测能力和既有算法；空间级配置功能的开发与验收按需求设计另行开展。

本文最初形成于提交准备阶段。随后用户明确要求发布研究以供他人开发，新增 [开发接手指南](DEVELOPER-HANDOFF.md) 与 [完整成果索引](ARTIFACT-INDEX.md)。实际发布版本以当前 Git 提交为准；审计记录保留其执行时点的状态，不把整个未跟踪工作区一并加入 Git。

## 2 先让团队按这个顺序阅读

以下文章路径均相对本目录。不要让未接触问题的开发者一开始就从日志和函数调用链阅读。

| 顺序 | 文章 | 解决的问题 |
| --- | --- | --- |
| 1 | [需求设计主文](SPACE-LEVEL-WAL-REQUIREMENTS-DESIGN.md)，[Word 评审版](SPACE-LEVEL-WAL-REQUIREMENTS-DESIGN.docx) | 背景、目标、首期边界、配置格式、开发契约、测试、发布和回退 |
| 2 | [通俗问题报告](REPORT.md) | 为什么无业务写入还会增长，哪些结论已经确认 |
| 3 | [空闲与持续写入比较](WRITE-VS-IDLE.md) | WAL 分配与整个进程内存的区别，组件对照的适用条件 |
| 4 | [完整实验与复现](EXPERIMENT-DETAILS.md) | 50 空间服务实验、真实时间窗口、采样缺口、FLUSH 作业与原始证据 |
| 5 | [组件机制与定量模型](COMPONENT-ANALYSIS.md) | 16 字节计账、64 槽 Node、实际分配和长期填充模型 |
| 6 | [Raft 解读](CODE-WALKTHROUGH-RAFT.md)、[WAL 解读](CODE-WALKTHROUGH-WAL.md)、[RocksDB 解读](CODE-WALKTHROUGH-ROCKSDB.md) | 相关功能的逐段实现和读者、提交、回收等细节 |
| 7 | [生产调参分析](PRODUCTION-PARAMETER-TUNING.md)、[空间级参数可行性](SPACE-LEVEL-KERNEL-PARAMETERS.md) | 三类业务空间的参数取舍，原方案如何演进到首期需求 |

工程根目录的 [分析写作要求](../../nebula_graph_readable_analysis_prompts.md) 约束后续分析：先解释因果，再给固定版本源码和实验；没有运行过的场景只能列为待验证。

## 3 脚本 工具和代码如何归类

| 成果 | 入口 | 提交和迁移方式 |
| --- | --- | --- |
| 参数化只读采样 | `sample_runtime.py` | 提交源码；目标机器指定 PID、HTTP、空间和数据目录。持续采样仍需评估负载 |
| WAL 文件解析与合成边界检查 | `parse_wal.py`、`test_parse_wal.py` | 提交源码；解析旧归档到新目录；自测先复制两个文件再运行 |
| 统计与绘图 | `analyze_runtime_rocks.py`、`analyze_results.py`、`plot-requirements.txt` | 提交源码和依赖清单；在完整证据副本中复算，避免覆盖历史汇总 |
| 原始缓存与 payload 探针 | `atomic-buffer-probe.cpp`、`write-idle-comparison-20261011/payload-probe.cpp` | 提交源码；用新增参数化驱动在目标机器重建 |
| RocksDB 提交位点探针 | `rocks-commit-probe.cpp` | 提交源码、编译记录和 CSV；会创建临时数据库，按新数据目录重建运行 |
| 历史实验驱动 | `cluster.py`、`run-atomic-buffer-probe.sh`、两个子目录的 `run*.sh` | 保留当时操作依据；固定路径、输出及服务变更操作需先适配，不能原样在现有集群执行 |
| 原实验配置与 nGQL | `configs/`、`*.ngql`、`spaces.txt` | 提交作为实验模板；端口、路径、账号和空间名不是目标环境的现成配置 |
| 目标环境静态预检 | [migration/preflight.py](migration/preflight.py) | 新增；查语法、路径、依赖和链接存在性，不启动服务、不编译、不发 HTTP；链接安全由交付校验器核验 |
| 目标环境组件重建 | [migration/run_components.py](migration/run_components.py) | 新增；显式指定依赖和 build 目录，串行编译，支持 smoke/full，新输出拒绝覆盖 |
| 产品观测改动 | `AtomicLogBuffer.cpp`、`observation.patch`、`instrumentation-notes.txt` | 提交源码与完整补丁；含补丁的源码不能再次 apply |
| 未改动的组件基线 | `baseline/kvstore/wal/AtomicLogBuffer.cpp/.h` | 提交固定提交的原始文件，供验证原算法；不可覆盖商用分支源码 |
| Word 出版工具 | `design-evidence/build_design_docx.py` 等 | 提交生成源码和小体积记录；渲染器的本机 Codex 路径需要另行适配 |

工具的实际副作用、依赖和逐项迁移命令见 [脚本迁移审计](migration/SCRIPT-PORTABILITY-AUDIT.md)。代码改动的开关、口径、并发边界与应用/回退验证见 [代码迁移审计](migration/CODE-CHANGE-AUDIT.md)。

## 4 哪些进入 Git 哪些留在本机

原材料初审为 519 个路径：506 个普通文件、13 个日志相对符号链接，普通文件合计约 146.90 MiB。初审候选为 299 个路径、约 71.23 MiB；复查发现统计程序依赖服务日志别名，因此最终保留额外 13 个安全相对链接，原材料交付基数为 312 个路径，再加入本轮迁移工具和记录。它们不是“全部新增文件”：其中已有 8 个文章/图表路径受 Git 管理，计入产品源码则为 9 个受管路径。

这些是固定的初审口径，最终数量与字节以 [发布清单](migration/publish-files.txt) 和 `migration/DELIVERY-MANIFEST.json` 为准。最终清单明确包含唯一产品源码路径、AGENTS.md、分析规范、本机 CLI 续接说明以及任务目录内的选定成果。

进入 Git 的原始证据包括：所有 `phase-*` 的 CSV、完整 HTTP 原始 JSONL、事件时间线、实际服务日志、观测 CSV、WAL 完整解析结果、容量与 payload 组件 CSV，以及停机 WAL 归档。这样新成员拉取后可以独立核对汇总，不依赖原实验机器。

`wal-evidence.tar.gz` 为 14,089,865 字节，约 13.44 MiB；包含 3000 个普通 WAL 文件，解压后 215,378,368 字节。它属于本次无业务点边写入实验的不可变原件，保留一份用于复核；未来长期新增归档宜放制品库或 Release 资产，避免不断扩大 Git 历史。

留在本机的内容包括：历史和新增探针 ELF、四组 RocksDB 临时数据库、Word 多轮 QA 图片/PDF、文字抽取缓存、编辑器锁和 Python 字节码。新增 `.gitignore` 只忽略这些可重建内容，没有删除它们。解析器的 12 个合成测试固件/报告保留在交付中，重跑仍使用副本以免覆盖。13 个日志链接全部指向同目录已提交的真实日志，Git 只保存 746 字节的目标文本，不会再复制一份日志。

历史配置包含本机路径、用户名、实验端口及 `root/nebula` 测试登录示例。有限静态规则没有发现其他疑似凭据，但并非完整秘密识别证明；不要把目标环境真实凭据写回这些模板或公开日志。详细路径、尺寸、旧校验状态见 [初审盘点](migration/INVENTORY-AUDIT.md) 与 [补充审查](migration/INVENTORY-FOLLOWUP.md)。

## 5 提交前怎样检查

先生成或检查本轮独立发布清单。下面命令在仓库根目录执行；不会启动数据库。`build_delivery.py` 的行为是按明确候选规则生成交付文件列表与摘要，传入 `--bundle-dir` 时额外输出选定文件覆盖包。

```bash
python3 task/wal-investigation/migration/build_delivery.py
python3 task/wal-investigation/migration/verify_delivery.py
sha256sum -c task/wal-investigation/migration/PUBLISH-SHA256SUMS
```

需要手工传输时可以额外生成覆盖包：

```bash
python3 task/wal-investigation/migration/build_delivery.py \
  --bundle-dir task/wal-investigation/migration/local-bundles/release-20261016
```

输出目录必须不存在；包只包含清单选中的文件，不是完整 Nebula 源码仓库。它可覆盖到一个匹配基线的独立 checkout，也可先解压到空目录核验再合并。应先核对 Git 分支和商用修改，禁止直接向现有运行数据目录解包。包外附有 SHA256SUMS；两种发布途径都保留原始 WAL 归档。

文件冻结后再做最终打包。打包和最终检查日志写到不纳入交付的 `migration/runs/`，不要一边把输出写入已选文件一边计算它的摘要。若之后修改文档或工具，重新生成清单和覆盖包。

交付目录的 `.gitattributes` 禁止 Git 自动归一化文本换行，以保留 CSV 的 CRLF 和其他证据原字节，保证克隆后摘要一致。归档日志、工具生成 SVG、补丁上下文和历史实验文章另有针对性的空白检查规则；不是清洗或改写证据。产品代码、脚本和当前主文继续进行正常空白检查。

旧 `SHA256SUMS` 是历史交付快照，本轮检查发现其中 6 篇文章因后续重写已变化，还引用了有意不交付的二进制或临时文件。不要改旧清单来让它看起来全绿。新 `DELIVERY-MANIFEST.json` 才说明本次交付范围；其自身不列入自己的摘要，完整性通过 Git 提交及覆盖包外部摘要追溯。新 `PUBLISH-SHA256SUMS` 检查普通文件；符号链接的类型、目标和摘要由 JSON 清单核验。

本机当前分支是 `3.6-memory`。以下是后续提交时的精确操作，不使用 `git add -A`：

```bash
git status -sb
git --literal-pathspecs add \
  --pathspec-from-file=task/wal-investigation/migration/publish-files.txt
git diff --cached --name-only
git diff --cached --stat
git diff --cached --check
```

确认暂存内容仅属于清单，再提交并推送。若索引原先已有其他修改，先明确其归属，不要把它们混入这次提交。下面推送显式指定当前目标分支；若远端已前进，先检查分歧，不强推。

```bash
git commit -m "docs: package WAL investigation and migration validation tools"
git push origin HEAD:3.6-memory
```

若本机 HTTPS 认证不可用，可通过已配置的 GitHub SSH 密钥使用以下一次性地址；它不改 origin URL，不读取私钥：

```bash
git push ssh://git@ssh.github.com:443/cccxgit/nebula.git HEAD:3.6-memory
```

推送完成后记录最终 commit，目标环境应 checkout 这个明确提交，不只依赖可能继续变化的分支名。上面的命令同样供后续维护发布使用，实际是否已发布以 Git 记录为准。

## 6 开发环境按四个阶段验证

### 6.1 拉取并先检查交付完整性

以下命令在目标环境执行。`<published-commit>` 替换为交接时记录的实际提交 ID，`<new-checkout>` 为新目录。必须确认取得包含接手指南和发布清单的交付提交，不能只拉取早期仅含文章的版本。

```bash
git clone --branch 3.6-memory https://github.com/cccxgit/nebula.git <new-checkout>
cd <new-checkout>
git checkout --detach <published-commit>
python3 task/wal-investigation/migration/verify_delivery.py
mkdir -p task/wal-investigation/migration/runs
migration_run=$(mktemp -d "$PWD/task/wal-investigation/migration/runs/target.XXXXXX")
```

后面所有新结果写到 `$migration_run`，不会改原证据。Git 保留源码与记录，不迁移 Codex 的本机会话数据库、编译依赖、安装目录或运行中的服务；在目标机新建 Codex 任务并读取本说明即可继续。

### 6.2 离线核对证据和解析器

```bash
python3 task/wal-investigation/migration/preflight.py \
  --repo "$PWD" --output "$migration_run/preflight"
mkdir "$migration_run/parser-selftest"
cp task/wal-investigation/parse_wal.py \
   task/wal-investigation/test_parse_wal.py "$migration_run/parser-selftest/"
python3 "$migration_run/parser-selftest/test_parse_wal.py" \
  > "$migration_run/parser-selftest.log" 2>&1
```

应得到 10 项合成检查通过。再检查旧归档散列并解压到新的空目录，逐条解析。归档预期 SHA-256 为 `acb2e767bf72b9b715879c6a911e53ef115e30469f2185e8e4b3205ad988fd55`。

```bash
sha256sum task/wal-investigation/wal-evidence.tar.gz
mkdir "$migration_run/wal-unpacked"
tar -xzf task/wal-investigation/wal-evidence.tar.gz \
  -C "$migration_run/wal-unpacked"
python3 task/wal-investigation/parse_wal.py \
  --data-dir s1="$migration_run/wal-unpacked/data/storage1" \
  --data-dir s2="$migration_run/wal-unpacked/data/storage2" \
  --data-dir s3="$migration_run/wal-unpacked/data/storage3" \
  --output "$migration_run/reparsed-wal" > "$migration_run/parse-wal.log" 2>&1
```

预期 3000 个文件、6730574 条记录，全部 payload 为空、结构异常为零。该结果证明旧实验原件与解释一致，不证明目标生产环境也有相同归因。完整统计复算需将本证据目录复制到新位置再运行两个分析程序，它们会写同目录输出，不能直接在唯一原件上重算。

### 6.3 重新构建缓存组件

目标环境需要 Linux、Python 3.9+、C++17 工具链、与项目匹配的 Folly/gflags/glog/gtest 头文件、jemalloc，以及已生成 Thrift 头文件的 Nebula build 目录。源码构建最大 `make -j5`；不要把本机 ELF 复制过去当作编译验证。

以下两处 `/path/to` 必须替换为目标环境路径。新驱动只读产品源码，串行编译两个探针，结果和 ELF 都写到新的输出目录；不启动数据库。

```bash
python3 task/wal-investigation/migration/run_components.py \
  --repo "$PWD" \
  --third-party /path/to/third-party/3.3 \
  --build-dir /path/to/nebula-build \
  --mode smoke --output "$migration_run/component-smoke"
python3 task/wal-investigation/migration/run_components.py \
  --repo "$PWD" \
  --third-party /path/to/third-party/3.3 \
  --build-dir /path/to/nebula-build \
  --mode full --output "$migration_run/component-full"
```

本轮在原实验机器上实际运行了这两个模式：均重新编译并退出成功；full 运行包含 8/4/2/1 MiB、256 KiB 五档容量和四组 payload 对照。五档空日志 Node 分配平台分别为 28、14、7、3.5、0.875 MiB，与旧证据一致。记录在 `migration/component-{smoke,full}-20261016/`，随交付保留 CSV、日志、meta 和 run.json，排除四个新 ELF。

实际开发机尚未运行，因此这里没有承诺其 ABI、链接库、分配器取整或性能与本机相同。若分配量不同，先核对编译器、结构大小、allocator 和读者状态；先解释差异，不强行让报告数字一致。RocksDB 探针需要按其编译记录重建，并使用全新的临时数据库目录；新 WAL 驱动没有宣称覆盖它。

### 6.4 完整服务复现与后续功能验证

完整服务复跑会启停服务、建 50 个空间、动态修改心跳并执行 FLUSH，必须使用隔离的新安装、端口、数据与证据目录。`cluster.py` 原样保存，仍有硬编码，运行前按 [脚本迁移审计](migration/SCRIPT-PORTABILITY-AUDIT.md) 逐项适配，操作次序见 [需求设计第 5 节](SPACE-LEVEL-WAL-REQUIREMENTS-DESIGN.md)。

启动服务前执行 `ulimit -n 65536`。建立 50 空间、20 分片、3 副本后，核对三实例各 1000 个本地副本，不能只数 Leader。先等待 Host ONLINE 和分片就绪，再采样；FLUSH 后核对本次 50 个作业 FINISHED，最后只停止本次 PID。心跳 1 秒仅用于隔离实验加速，不是生产参数建议。

当前补丁开关为 `wal_memory_observe`，需要在首次 WAL 使用前决定是否启用；观测间隔设置也放在启动配置，不在运行中通过 HTTP 修改。诊断计数不是跨线程一致快照，Debug/观测构建也不能代替生产性能验收。具体开销和回退条件见代码审计。

空间级配置实现后，再按需求设计的 T01 至 T12 和 A/B/C 组测试验收：旧版本原配置、新版本关闭、新版本仅冷空间覆盖。高频与中频空间保持现场基线，验证内存分项、热点 P99、吞吐、冷空间突发导入、复制追赶、故障恢复和回退预算。现有迁移工具验证原缓存算法，不会提前验证尚未实现的 flags 或诊断接口。

## 7 本轮核验记录与后续交接

本轮已经执行：原材料分类与逐文件散列；WAL 归档结构检查；产品 diff 与补丁逐字比对及临时目录正反向应用；新工具静态预检；两个组件模式的重新构建与运行；输出覆盖拒绝检查。清单和覆盖包还会在一个不含本机 ELF、临时数据库或 Word QA 的新目录中核验，最终结果见 `migration/delivery-verification.json`。

这些验证没有启动 Nebula 服务，没有修改现网，没有实现空间级参数，也没有进行实际开发机或生产负载测试。下一位开发者应先记录目标商用分支和基线差异、完成前两阶段离线检查，再决定隔离服务复现与需求实现的顺序。
