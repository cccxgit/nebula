# PR 标题建议

`kvstore: avoid waiting on global IO executor while holding NebulaStore write lock`

# PR 描述中文稿

## 背景

这个 PR 修复了 NebulaGraph 3.6 中“并发创建图空间和执行 leader balance 时可能出现死锁”的问题。

问题链路如下：

1. `NebulaStore::addSpace()` 持有 `NebulaStore::lock_` 写锁。
2. `addSpace()` 内部调用 `newEngine()`。
3. 原实现中 `newEngine()` 会同步等待 `newEngineAsync().get()`。
4. `newEngineAsync()` 会把任务投递到 `folly::getGlobalIOExecutor()`。
5. transfer leader 成功后的 leader 检查回调也运行在 `folly::getGlobalIOExecutor()`，并调用 `partLeader()`，而 `partLeader()` 需要申请 `NebulaStore::lock_` 读锁。

在高并发场景下，上述链路会形成“持写锁等待线程池，线程池任务等待读锁”的依赖环，进而导致死锁和 CPU 占用升高。

## 修复方案

本次修复采用最小改动方式：

- 抽取同步 engine 创建函数 `createEngine()`
- 保留 `newEngineAsync()`，继续用于启动阶段扫描磁盘时的异步 engine 加载
- 将 `newEngine()` 改为直接调用 `createEngine()`，不再同步等待 `newEngineAsync().get()`

这样可以保证：

- `addSpace()` 在持有写锁时不再依赖 `folly::getGlobalIOExecutor()`
- 启动阶段原有的异步并行 engine 加载逻辑不受影响

## 为什么这样修改

- 修复点集中，行为边界清晰
- 不扩大锁范围变更，降低引入新竞态的风险
- 不修改 transfer leader 的现有语义
- 直接消除导致死锁的锁和线程池依赖关系

## 测试验证

新增回归测试：

- `NebulaStoreTest.AddSpaceDoesNotDependOnGlobalIOExecutor`

测试思路：

- 先用阻塞任务占满 `folly::getGlobalIOExecutor()`
- 再调用 `addSpace(1)`
- 验证 `addSpace()` 仍然可以及时返回，并成功创建 engine

已执行验证命令：

```bash
cmake --build build --target nebula_store_test -j4
./build/bin/test/nebula_store_test --gtest_filter=NebulaStoreTest.AddSpaceDoesNotDependOnGlobalIOExecutor
./build/bin/test/nebula_store_test --gtest_filter=NebulaStoreTest.TransLeaderTest
```

验证结果：

- 新增回归测试通过
- 原有 `TransLeaderTest` 通过

## 风险评估

风险较低。

本次修改只影响同步建 space 的路径，没有改变启动阶段的异步 engine 加载流程，也没有改变 transfer leader 的原有行为。
