# 开发接手指南 从研究证据到空间级 WAL 配置

更新日期：2026 年 10 月 16 日。适用仓库：`cccxgit/nebula`，分支：`3.6-memory`。

**第一次接触本需求，请从本文开始。** 本文说明先读什么、先验证什么、从哪里开始开发；完整契约以[需求设计](SPACE-LEVEL-WAL-REQUIREMENTS-DESIGN.md)为准，所有材料见[成果索引](ARTIFACT-INDEX.md)。

## 1 用三句话理解问题

生产反馈是：storaged 的容器内存超过 limit 的 80%，查询和写入经常受到内存保护影响，环境中有大量数月不写数据的空间。

目标源码和本地实验已确认：没有业务写入的分片仍可能通过心跳追加空日志；日志持续占用每个副本独立的 WAL 缓存，提交空日志还会更新 RocksDB 提交位点、消耗写缓冲。

计划开发“全局默认容量加图空间覆盖容量”，先只缩小经确认的低频空间 WAL 缓存，同时保留热点空间原容量；是否足以解决生产痛点，需要生产归因、性能和恢复实验继续证明。

这里的 WAL 是 Raft 近期日志的内存缓存及其对应磁盘日志，不是 RocksDB 自己的 WAL。首期调整的是缓存逻辑容量，不能把它当作整个空间或容器的物理内存上限。

## 2 交接时已经完成什么

| 状态 | 可复核结论或交付 | 边界 |
| --- | --- | --- |
| 已确认的源码机制 | 空心跳日志进入复制、缓存和提交路径；容量按逻辑字节计账；标删与释放有不同条件 | 分析基线为 `de9b3ed800a6627d9845e9289b6bbc5b6faf460a` |
| 已完成的服务实验 | 50 空间，每空间 20 分片、3 副本，同机三个 storaged 进程各有 1000 个本地副本；无业务点边写入，观察到持续上涨 | 这是隔离实验，不是生产容器采样 |
| 已保存的原始证据 | 3000 个 WAL 文件，6730574 条记录，payload 全空，解析结构异常为零 | 空 payload 本身不唯一证明日志来源，需结合源码和操作时间线 |
| 已完成的组件实验 | 本构建下，8/4/2/1 MiB 与 256 KiB 空日志容量，对应 Node 分配平台约 28/14/7/3.5/0.875 MiB | 需要正常读者退出、足量填充；不是整个进程 RSS |
| 已完成的产品改动 | `AtomicLogBuffer.cpp` 增加 124 行计数和观测日志，配套完整补丁 | 没有实现空间配置，没有修改原缓存回收条件 |
| 已完成的需求设计 | 配置输入、身份校验、启动冻结、构造传递、动态入口保护、诊断、验收和回退 | 版本 0.9 评审稿；建议数值仍需评审 |
| 尚未完成 | 空间级参数功能、目标商用分支适配、实际开发机运行、生产等价性能与恢复验收 | 不得把设计示例当作现有可用接口 |

尚未证实生产 80% 内存中 WAL 的具体占比，也没有测得“50 空间持续真实业务写入”的整机 RSS 对照。固定 payload 探针只验证缓存行为；不能据此声称持续业务写入的总内存更低。

## 3 按目的阅读 不必先读完所有日志

以下时间仅为建议阅读预算，不是实验或开发工期承诺。

| 建议顺序 | 材料与建议预算 | 读完应能回答 |
| --- | --- | --- |
| 1 | [通俗报告](REPORT.md)，20–30 分钟 | 谁在空闲时写什么，为什么 WAL 和 MemTable 都能增长？ |
| 2 | [需求设计](SPACE-LEVEL-WAL-REQUIREMENTS-DESIGN.md)第 1、6–10、13–14 节，60–90 分钟 | 首期做什么，何时生效，哪些入口必须保护？ |
| 3 | [实验明细](EXPERIMENT-DETAILS.md)与[空闲和写入对比](WRITE-VS-IDLE.md)，45–60 分钟 | 数字来自什么窗口，哪些对照尚未运行？ |
| 4 | [WAL 解读](CODE-WALKTHROUGH-WAL.md)、[Raft 解读](CODE-WALKTHROUGH-RAFT.md)、[RocksDB 解读](CODE-WALKTHROUGH-ROCKSDB.md)，按职责阅读 | 原对象何时创建、追加、提交、标删、释放？ |
| 5 | [迁移说明](DELIVERY-AND-MIGRATION.md)与[脚本审计](migration/SCRIPT-PORTABILITY-AUDIT.md)，30–45 分钟 | 哪些命令可直接离线运行，哪些会操作服务或覆盖输出？ |

[成果索引](ARTIFACT-INDEX.md)用于查找文章、工具、证据和代码；[交付清单](migration/publish-files.txt)定义实际进入本次 Git 交付的路径。不要以工作目录里“恰好存在的全部文件”代替交付范围。

后续分析遵循根目录[分析规范](../../nebula_graph_readable_analysis_prompts.md)：先说明因果，再给目标版本源码和实验，明确事实与待验证判断。

## 4 首日先离线复核 不接触运行集群

前提：取得发布后的完整 checkout，使用 Linux、Python 3.9+，当前终端位于仓库根目录。记录交接时的明确 commit；先保持 checkout 原样完成完整性核验，再开始修改源码。

下面命令只读交付材料，新结果写入唯一的新目录；不启动服务、不编译、不发送 HTTP。任一步失败先停下解释原因，不继续把后续结果记为通过。

```bash
set -euo pipefail
mkdir -p task/wal-investigation/migration/runs
handoff_run=$(mktemp -d "$PWD/task/wal-investigation/migration/runs/handoff.XXXXXX")
git rev-parse HEAD > "$handoff_run/source-commit.txt"
git status --short > "$handoff_run/worktree-status.txt"
python3 task/wal-investigation/migration/verify_delivery.py \
  > "$handoff_run/delivery-check.json"
python3 task/wal-investigation/migration/preflight.py \
  --repo "$PWD" --output "$handoff_run/preflight"
mkdir "$handoff_run/parser-selftest"
cp task/wal-investigation/parse_wal.py \
   task/wal-investigation/test_parse_wal.py "$handoff_run/parser-selftest/"
python3 "$handoff_run/parser-selftest/test_parse_wal.py" \
  > "$handoff_run/parser-selftest.log" 2>&1
```

预期：交付校验 `passed: true`；解析器 10 项合成检查通过。这里的 `migration/preflight.py` 是现有材料静态预检工具，与需求设计中尚未实现的空间策略/集群身份预检查工具不同。静态预检可能提示历史硬编码路径或依赖缺失，逐项判断即可；它不证明服务可启动或编译成功。旧 `SHA256SUMS` 是历史快照，本次以 `DELIVERY-MANIFEST.json` 为交付核验依据。

继续复查已保存 WAL，仍在同一终端使用 `$handoff_run`：

```bash
python3 - <<'PY'
from pathlib import Path
import hashlib
p = Path('task/wal-investigation/wal-evidence.tar.gz')
actual = hashlib.sha256(p.read_bytes()).hexdigest()
expected = 'acb2e767bf72b9b715879c6a911e53ef115e30469f2185e8e4b3205ad988fd55'
assert actual == expected, (actual, expected)
print('WAL archive SHA-256 verified')
PY
mkdir "$handoff_run/wal-unpacked"
tar -xzf task/wal-investigation/wal-evidence.tar.gz \
  -C "$handoff_run/wal-unpacked"
python3 task/wal-investigation/parse_wal.py \
  --data-dir s1="$handoff_run/wal-unpacked/data/storage1" \
  --data-dir s2="$handoff_run/wal-unpacked/data/storage2" \
  --data-dir s3="$handoff_run/wal-unpacked/data/storage3" \
  --output "$handoff_run/reparsed-wal" > "$handoff_run/parse-wal.log" 2>&1
```

核对输出为 3000 文件、6730574 记录、全部空 payload、零结构异常。它证明归档与既有解释一致；目标生产环境是否同因，仍需另采证据。

接着按[迁移说明第 6.3 节](DELIVERY-AND-MIGRATION.md#63-重新构建缓存组件)重新构建探针。显式填写目标机 third-party 和 build 路径，输出继续使用新的子目录；探针源码随 Git 交付，本机 ELF 不随迁移复用。

发布前为通过仓库规范检查，三个研究探针补充了版权头、整理了格式，并把 RSS 计数的 `long` 写成 `int64_t`（已确认本机类型相同）。当前源码已重新编译运行，五档空日志容量、payload/停写对照和 RocksDB 位点覆盖写检查通过；命令、源码摘要和完整文本输出见[发布验证记录](migration/publication-validation.json)。旧实验原件及其摘要保留生成时状态，不能用旧源码摘要核验整理后的源码。

完整服务复现放在独立测试环境进行。`cluster.py` 和历史 shell 驱动保留了固定路径及服务变更操作，先按脚本审计适配安装目录、端口、数据目录和输出位置，再执行需求设计第 5 节。启动前执行 `ulimit -n 65536`；Nebula 编译最多使用 `make -j5`。原机器 `/home/sch/nebula-run/nebula-memory` 不是目标环境的默认安装位置。

## 5 商用分支先做差异检查

把实验基线与待开发商用 commit 的相关改动单独列成 `commercial-diff-review.md`，保存到本次任务的新结果目录。至少检查下列对象是否仍然满足设计前提：

| 检查范围 | 要核对的具体行为 |
| --- | --- |
| RaftPart、FileBasedWal、AtomicLogBuffer | 容量来源、计账、构造时机、空心跳日志、迭代器和回收条件是否改变 |
| StorageServer、NebulaStore、Part、listener | 业务角色与管理空间如何区分，新副本是否经过同一构造入口 |
| MetaClient、GflagsManager、SetFlagsHandler | 实际配置来源、Meta 动态刷新、HTTP 改 flag 是否有自研入口 |
| Part、RocksEngine、RocksEngineConfig | 提交位点、引擎作用域、共享缓存、RocksDB 版本及 option 更新是否改变 |
| MemoryUtils、MemoryTracker、部署配置 | cgroup 口径、保护条件、编译开关、allocator、当前有效参数与原实验是否一致 |

记录“仍成立、已有变化、未确认”及对应源码/实验，不以版本同为 3.6 就假定行为相同。观测补丁要先核对[代码审计](migration/CODE-CHANGE-AUDIT.md)；当前源码已包含时禁止再次 apply，也不能用 `baseline/` 覆盖商用实现。

## 6 开发启动先确认 D1 至 D8

需求设计第 14 节有完整决策表。以下是评审入口；本文不替设计责任人批准数值。

| 决策 | 启动时要明确的事项 |
| --- | --- |
| D1 | 首期仅业务 storage 的 `wal_buffer_size`，启动冻结、重启生效 |
| D2 | 覆盖值范围、JSON 大小和条目数；稿件建议 256 KiB–8 MiB、1 MiB、4096 项 |
| D3 | 冷空间试点名单和试点档位；需生产等价结果后确认，不在开发时直接定现网最小值 |
| D4 | 预检查的集群身份验证边界；权威启动握手必须校验 |
| D5 | 已有业务 SLA、内存目标和性能门槛；稿件比例只是讨论起点 |
| D6 | listener、Meta、空间 0 及其他非目标运行模式的拒绝和兼容行为 |
| D7 | 实际生效接口的权限、字段、监控基数和运维接入 |
| D8 | RocksDB 空间参数另立需求，首期不并入 |

先确认影响本阶段接口的决策，再冻结对应产物。D3、D5 尚无现场输入时，可以推进解析器、构造链、拒绝逻辑与测试框架；不能据此批准上线或宣称性能达标。

## 7 按 W1 至 W7 落实开发

| 工作包 | 具体产物与完成条件 |
| --- | --- |
| W1 配置模型 | 严格 JSON 解析器、不可变策略、规范化 hash、边界/重复键测试；关闭默认兼容，开启错误明确失败 |
| W2 启动和构造链 | 握手 cluster ID、完整空间校验、初始化屏障、类型化参数传递；新建/恢复/迁移副本统一取得正确容量 |
| W3 更新保护 | HTTP 整单预检查、Meta 注册与应用拦截、可查询拒绝记录；冻结后对象和值不被动态入口改写 |
| W4 诊断与预检查 | 离线和只读预检查、实际对象容量分布接口、启动摘要、操作说明；不能拿策略期望值伪装实际生效值 |
| W5 功能和恢复 | T01–T12 自动化、身份误投/空间重建/并发删除/混合版本/故障恢复证据；覆盖失败路径 |
| W6 性能和容量 | A/B/C、冷热混合、50 空间真实写入、容量填充和长稳态报告；保留原始指标、配置和负载完成量 |
| W7 发布和回退 | 生产空间清单、版本与策略分发、逐台健康检查、SLA 签署、参数及二进制回退演练和资源预算 |

拟议构造方向为 `StorageServer → KVOptions → NebulaStore::newPart → Part → RaftPart → FileBasedWalPolicy → AtomicLogBuffer`。先在商用分支核实签名和角色，再改容量来源；不要在共享 Raft 基类里只凭 `spaceId > 0` 自动套策略。

W1 可先独立开发；W2 和 W3 都验收完成后才能允许启用。W4、W5 可提前准备框架；W6、W7 依赖功能和现场输入。每个工作包交付代码、必要测试、设计差异、错误处理说明及可复现记录。

## 8 验收时必须拿到什么

配置正确性按 T01–T12 全部验收，特别关注身份不符时启动失败、HTTP 混合请求整单拒绝、Meta 异步拒绝真实可见、新空间继承、同名重建不误套旧 ID，以及删除并发时诊断安全。

性能分成 A 旧版本原配置、B 新版本关闭、C 新版本开启且仅覆盖冷空间。A/B 衡量新代码开销，B/C 衡量容量策略；保持分片、容器、负载和其他参数相同。不要同时改心跳、RocksDB、水位线等参数来解释 WAL 收益。

最终证据包至少包含明确 commit/二进制标识、配置及 revision/hash、每空间实际容量分布、操作时间线、原始采样、分析程序和失败样本。分别报告 WAL、MemTable、RSS/cgroup、冷热吞吐和尾延迟、复制追赶与快照、回退结果；现有 Debug 观测构建不能代替正式性能构建。

上线前补齐容器 limit/cgroup 版本、分级空间清单与本机副本分布、当前实际参数来源、错误码和 SLA、磁盘与回退余量。缺失这些信息不阻断前述基础开发，但阻断现网选值和上线批准。

## 9 接手时最容易误解的地方

- 每空间的容量值最终用于每个本地分片副本，不是整个空间共享一个预算；统计包含 Leader 和 Follower。
- 8 MiB 是逻辑计账值，不等于实际 Node 分配、RSS 或容器硬上限；28 MiB 平台依赖本次构建和实验条件。
- 重启后的内存低不能证明长期有效：缓存需要重新填充，真实心跳下可能要数十天才到原平台。
- `cleanWAL` 的磁盘 TTL 不会按时间释放活跃内存缓存；空日志写入也可能占用 RocksDB MemTable。
- 小缓存仍可磁盘读日志，但提交、追赶及共享磁盘负载可能变化；正确性路径存在不等于性能零影响。
- `enable_space_kernel_options`、`space_kernel_options` 和生效诊断接口是拟议功能，当前版本不可直接使用。
- 观测开关在首次 WAL 使用前决定；历史脚本与服务路径先适配，分析程序重算到证据副本，保留原件。
- 二进制回退须移除旧程序不认识的新 flag；恢复较大容量还需要为随后重新增长预留内存。

## 10 材料入口与交接完成判断

- 找文章、代码、脚本和原始材料：[成果索引](ARTIFACT-INDEX.md)。
- 查需求、配置和测试契约：[需求设计主文](SPACE-LEVEL-WAL-REQUIREMENTS-DESIGN.md)及[Word 评审版](SPACE-LEVEL-WAL-REQUIREMENTS-DESIGN.docx)。
- 查提交范围、迁移和重建步骤：[交付迁移说明](DELIVERY-AND-MIGRATION.md)、[精确发布清单](migration/publish-files.txt)、[脚本审计](migration/SCRIPT-PORTABILITY-AUDIT.md)、[代码审计](migration/CODE-CHANGE-AUDIT.md)。

接手人能够复述空闲增长的两条因果链、完成离线原件复查、指出商用分支差异，并把 D1–D8 与 W1–W7 落到责任人和交付物，才算完成开发交接。空间级功能和生产收益仍需要后续实现与验收。
