# 代码与组件工具迁移审计

审计日期：2026-10-16。本文对应迁移盘点时的工作树，不表示 GitHub 已经收到提交。机器可读的核验结果见 [code-change-verification.json](code-change-verification.json)，命令输出见 [code-change-verification.log](code-change-verification.log)。

## 1 先说明当前交付到哪一步

本次调查只有一处产品源码改动：`src/kvstore/wal/AtomicLogBuffer.cpp` 新增 124 行观测代码、删除 0 行。它记录 WAL 缓存节点的创建、删除、空消息、逻辑字节和 GC 次数；没有改变缓存容量、心跳生成、标脏条件、读者退出后的回收条件或 RocksDB 写入方式。空间级 WAL 配置、配置校验、生效诊断等仍是需求设计，没有可供直接启用的实现。

“仅观测”不等于没有运行成本。关闭时仍有首次初始化和分支检查；开启时，每条日志、每次读者释放都可能增加共享原子操作和取时，输出日志也会占用 CPU、锁和磁盘。现有证据说明它能辅助解释测试现象，没有证明开启观测对生产高并发读写没有性能影响。

迁移时应同时保留两条验证路径：完整服务使用观测补丁解释对象变化；独立组件程序使用固定版本的原始实现交叉验证缓存机制。不要把组件程序编译时使用的原始实现替换为带观测版本，否则就失去了这层独立性。

## 2 版本与文件对应关系

| 对象 | 审计时确认的值 |
| --- | --- |
| 产品源码基线 | `de9b3ed800a6627d9845e9289b6bbc5b6faf460a` |
| 盘点时 HEAD | `194958131042df77301072eb99dccbc64fb34246` |
| 本地产品改动 | 仅 `src/kvstore/wal/AtomicLogBuffer.cpp`，124 行新增 |
| 原始源码副本 | `task/wal-investigation/baseline/kvstore/wal/AtomicLogBuffer.cpp` 和 `.h` |
| 观测补丁 | `task/wal-investigation/observation.patch` |
| 文档状态 | `SPACE-LEVEL-WAL-REQUIREMENTS-DESIGN.md` 明确标注首期功能尚未实现 |

已核验：baseline 的 `.cpp`、`.h` 与固定产品基线逐字节一致；盘点时 HEAD 中的这两个产品文件也与基线一致。工作树 `.cpp` 与补丁正向应用后的结果完全一致；头文件未改动。

关键 SHA-256：

| 文件或数据 | SHA-256 |
| --- | --- |
| 原始 AtomicLogBuffer.cpp | `3af5641a37c66d5a80ed27670857c9beb8538a6406ff7fa1bd74ccc4b7a2e47a` |
| 工作树 AtomicLogBuffer.cpp | `bd4a0b0f3c850ac8c2bcf0c542db1cb3663a906e9cba42f62cf7abd87a5388a2` |
| AtomicLogBuffer.h | `da9731116d69c04cb5800c72ec375fe2ba4164b502b773636639a4feeeac28fd` |
| observation.patch 及当前该文件的 git diff | `c9004dbb122f887c37b9bfd3ee9f35618f95dbcafc384aa433ff02756478acc7` |

以上是提交前快照。后续 Git 提交可以改变 HEAD；若只是提交这些文件，源码及补丁内容哈希应保持不变。若开发人员修改了产品源码，应重新核验，不能继续引用此表证明新改动仍然仅用于观测。

## 3 观测补丁具体做了什么

### 3.1 开关与触发方式

`wal_memory_observe` 默认 `false`；`wal_memory_observe_interval_secs` 默认 10 秒，小于 1 时按 1 秒处理。参见 [当前实现](../../../src/kvstore/wal/AtomicLogBuffer.cpp) 的 `WalMemoryObservations`、`memoryObservations()` 和 `observeMemory()`。

开关实际在本进程第一次调用 `memoryObservations()` 时复制到 `const bool enabled`，随后不再改变。这通常发生在首次 push、reset、releaseRef 或析构，不能严格解释为“服务刚启动就已经锁定”。因此应只通过启动配置设置两个观测参数，并在需要切换时重启测试进程；在线改 `wal_memory_observe` 可能只改变通用 flags 展示值，已经锁存的观测行为仍保持原值。

间隔参数并没有锁存，而是采样时直接读取普通 gflag 变量。现有 [HTTP 设置入口](../../../src/webservice/SetFlagsHandler.cpp) 的 `onEOM()` 第 79—86 行会调用 `gflags::SetCommandLineOption`，本补丁没有为这两个 flag 增加在线修改拦截。对直接读取该普通变量的线程与在线写入之间，当前审计不能证明存在同步保证；观测期间禁止在线修改这些参数。这个并发风险尚未通过 ThreadSanitizer 验证，不应当写成已经发生的现场故障。

采样限频属于整个进程。多个 buffer 共享一个 `lastLogSecond`，使用 CAS 竞争输出资格，不是每个空间或每个分片每 10 秒各打一条日志。只有已有 push、reset、releaseRef 事件会尝试输出；没有额外采样线程，静默进程可能不再输出。析构和节点删除只更新计数，不保证输出最后一条统计，所以不能根据最后一条日志推断进程退出时还有同样数量的存活节点。

### 3.2 插入位置与原算法的关系

| 位置 | 新增观测 | 原有动作是否被改写 |
| --- | --- | --- |
| `push()` 入口 | pushes、empty_pushes | 没有改写消息内容或 push 分支 |
| 原 `new Node()` 之后 | created、live_nodes | 没有新增 Node 分配点 |
| 原记录写入后 | live_logical_bytes、限频输出 | 没有改变容量和淘汰判断 |
| `reset()` 原动作结束后 | resets、限频输出 | 没有新增实际删除或改变 reset 语义 |
| `releaseRef()` | releases、限频输出 | 没有改变引用数及提前返回条件 |
| 原 GC 成功取得执行权之后 | gc_runs | 没有改变 GC 触发条件 |
| 原 delete 前 | live_nodes、freed、gc_freed、live_logical_bytes | 只在原删除位置计数，没有新增释放点 |

原始类只允许一个写者、多个读者；`reset()` 与 `push()` 仍须由调用方保证线程安全。本补丁依赖这些既有不变量，没有新增同步来支持多个写者，也没有修复原算法已有的超额留存、GC 延迟或整数边界问题。原始算法可在 [baseline 实现](../baseline/kvstore/wal/AtomicLogBuffer.cpp) 和 [原始头文件](../baseline/kvstore/wal/AtomicLogBuffer.h) 中核对。

### 3.3 计数能证明什么

`live_nodes` 是成功分配尚未删除的 Node 数；`created`、`freed` 是累计数。`live_logical_bytes` 在写入时加 `Record::size()`，直到节点被实际 delete 才减去节点内记录的逻辑字节。因此它包含已标脏但仍未释放的记录，不能与单个 buffer 的有效逻辑大小 `buffer_size` 等同。

`node_object_bytes = live_nodes × sizeof(Node)` 只统计 Node 对象本身，包含内嵌 64 个 Record 的存储和对齐。它不包含字符串额外堆分配、allocator 分配档位、碎片、空闲页、RocksDB 等其他对象；不能当作 jemalloc allocated，更不能当作 RSS 或容器 memory usage。

`releases` 表示内存迭代器释放引用次数，不是磁盘读取次数。`gc_runs` 表示原 GC 条件成立且成功取得执行权的次数，一次 GC 可以删除 0 个节点；`gc_freed` 才是 GC 删除的节点数，`freed` 还包含 buffer 析构删除。`empty_pushes` 只依据移动前消息是否为空，不单独证明每条空消息都是某一次心跳。

多数计数是本进程全部 AtomicLogBuffer 的汇总；`buffer_size/capacity/dirty_nodes/refs` 只属于触发当前日志的那个 buffer。日志中的 buffer 地址不能代替图空间 ID，地址还可能被新对象复用。该补丁没有提供按空间完整归属的诊断接口。

### 3.4 并发精度和成本边界

累计数使用 `int64_t` relaxed 原子，单次原子增减不会丢失更新；日志逐字段加载，并不是同一时刻的一致快照。并发分配和释放期间，某一行 `created - freed` 与同一行 `live_nodes` 可能暂时不一致；应结合多个采样点和稳定阶段分析。

新增逻辑不遍历运行中的链表；`node->size_` 只在原有 delete 前读取，安全性依赖原实现此时已经拥有删除权。单 buffer 的 `size_`、`dirtyNodes_`、`refs_` 本来就是原子变量；`capacity_` 构造后不变。这里的源码审查不能替代并发运行验证，尤其不能在破坏原类单写者约束时继续宣称计数可靠。

开启观测后，多个分片会争用同一组进程级计数所在的缓存行；限频只减少日志输出，没有减少每次 push/release 的计数和取时。新增开销可能影响调度和实验斜率。目前没有 on/off 性能对照、ThreadSanitizer 或生产负载等价测试。原始实现的组件结果可以交叉验证机制，但不能替这些验证给出性能保证。

## 4 C++ 组件程序如何归类

| 程序源码 | 使用的实现与作用 | 必须保留的边界 |
| --- | --- | --- |
| `atomic-buffer-probe.cpp` | 固定 baseline 的 AtomicLogBuffer；多 buffer、不同容量、读者 each/none/held；统计 Node 和分配大小 | 单线程驱动；不是完整 Raft 或 storaged；链表遍历仅用于该隔离程序 |
| `write-idle-comparison-20261011/payload-probe.cpp` | 固定 baseline；比较空消息、256/1024 字节消息及停止写入后的替换过程 | payload 是固定字符 x，不是合法业务写入日志；不能代表完整图查询写入 RSS |
| `rocks-commit-probe.cpp` | 直接调用 RocksDB，构造与系统提交位置相同的 key/value，观察覆盖写与 flush | 没有 Raft 或 WAL 缓存；只模拟相关 RocksDB 机制，不能代替服务归因 |

两个 AtomicLogBuffer 驱动在专用翻译单元使用 `#define private public` 读取内部状态；它们没有修改产品头文件，且驱动只在自己控制的单线程时点遍历。不要把这段“遍历所有节点”的方法直接搬入正在运行的服务诊断接口，它没有为并发写入建立安全快照。

Atomic 驱动依赖仓库源码、`build/src` 生成头文件、Folly/gflags/glog 和匹配分配器；Rocks 驱动依赖本机第三方 RocksDB、压缩库和 jemalloc。原路径指向 `/opt/vesoft/third-party/3.3`。使用新环境的生成头文件、ABI、编译器、分配器及库版本重新构建，并记录 `sizeof(Record)`、`sizeof(Node)`、链接方式和库版本，才有条件比较分配数据。

Rocks 驱动显式校验 little-endian，并拒绝复用数据目录；它的选项、系统 key/value 字节布局和 RocksDB 7.5.3 只证明原实验配置下的行为。不要把这些假设迁移到其他字节序、其他系统键格式或不同商用内核后仍当作已经核验。

当前 4 个本地程序均为 Linux x86-64 ELF，带调试信息；它们是本机产物，不是跨环境安装包。建议迁移源码、编译命令和原始结果，目标环境重新构建；是否另行归档二进制由迁移清单决定，不应将“复制旧 ELF 能启动”当成构建验证完成。

相关脚本的迁移边界：`run-atomic-buffer-probe.sh` 会写原 baseline 和结果目录，原先没有证据防覆盖；payload 和容量脚本有已有结果检查，但仍写固定目录，而且容量脚本复用已编译 probe。三者都含原机器绝对工作路径。请使用迁移总说明提供的新输出目录入口；不要在归档目录直接重复执行原脚本。

## 5 观测补丁在目标环境如何处理

### 5.1 先识别当前状态

先进入新环境仓库根目录，检查两个方向，不要直接应用：

```bash
git status --short
git apply --check task/wal-investigation/observation.patch
git apply --reverse --check task/wal-investigation/observation.patch
```

| 正向检查 | 反向检查 | 解释和下一步 |
| --- | --- | --- |
| 成功 | 失败 | 与补丁基线兼容、尚未加入；在专用验证分支正向应用 |
| 失败 | 成功 | 补丁已经存在；直接编译，不要重复应用 |
| 失败 | 失败 | 源码有分歧或部分改动；人工对照审查，先停止自动应用 |
| 成功 | 成功 | 状态不符合当前单一补丁预期；先检查命令工作目录和补丁内容 |

当前这份调查工作树属于第二种状态。若 GitHub 提交将产品观测代码一起提交，目标环境 checkout 后也属于第二种；`observation.patch` 是审计和跨基线手工迁移材料，不是 checkout 后必须再次执行的安装步骤。

只有正向检查成功且确定要在该专用验证分支增加观测时，才执行：

```bash
git apply task/wal-investigation/observation.patch
git diff --check
```

本轮已在临时目录用固定原始文件实际验证：正向应用结果与当前工作树逐字节一致；反向应用后与原始文件逐字节一致。未修改原始产品文件，也未启动数据库。

### 5.2 编译与启动验证

完整工程使用目标环境已经准备好的依赖和独立 build/install 目录，沿用 AGENTS.md 的 CMake 参数；源码 `make` 最大并行度必须为 `make -j5`。不要直接安装到现有商业服务安装路径。启动任何 Nebula 服务之前执行 `ulimit -n 65536`，完整运行日志保存到本次独立输出目录。

设置观测参数前先确认使用的是带补丁二进制；原版程序不认识新增 flag。对照组和实验组应使用相同负载、数据、副本分布及采样方式，分别在启动时关闭/开启观测。若需要比较原版与补丁默认关闭的差异，还要单独构建无补丁版本；仅比较同一带补丁二进制的 true/false，不能量化所有默认关闭开销。

本轮仅检查了历史 `build-j5.log`、`install.log` 等记录，没有重新编译或运行；这些旧日志不能证明新开发环境的二进制已经通过验证。

### 5.3 回退观测

仅想停止计数和日志：设置 `--wal_memory_observe=false` 后重启专用测试进程。由于首次使用锁存机制，在线将 flag 改为 false 不是可靠的停止方法。

要构建完全没有观测补丁的对照组，在单独验证分支/工作树执行：

```bash
git apply --reverse --check task/wal-investigation/observation.patch
git apply --reverse task/wal-investigation/observation.patch
git diff --check
```

重新构建和安装专用对照版本；删除测试配置中的两个 `wal_memory_observe*` 参数，原版程序不接受它们。保留其他业务配置和数据路径的受控对照关系，不通过 reset/clean 或覆盖安装目录来混合回退。若产品源码已加入空间级功能或其他商业修改，反向检查失败就停止，按实际 diff 人工移除观测部分。

## 6 迁移后哪些验证还没有完成

已完成的是源码范围、精确哈希、原始副本一致性、补丁正反向可还原性和既有证据可追溯性核验。尚未完成的是目标环境构建、运行、生产等价性能、观测并发验证，以及空间级配置功能开发和验收。

迁移验证时优先区分三个问题：原版在同样空闲副本规模下是否出现同类增长；观测是否显示相应 Node/空日志趋势且与 RocksDB、RSS 指标相互印证；开启观测是否改变了吞吐和延迟。任何一个结果与旧实验不同，都先核对版本、数据、分片、分配器和采样条件，再解释差异，不直接把旧实验斜率当成新环境预期值。
