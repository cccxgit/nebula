# NebulaGraph 3.6 死锁修复手动测试报告

## 1. 测试背景

- 测试日期：2026-05-17 09:16:57 CST
- 测试目录：`/home/sch/nebula/nebula-release-3.6`
- 测试目标：验证 `addSpace()` 在 `folly::getGlobalIOExecutor()` 被占满时，不再因为同步等待 engine 创建而阻塞，从而规避“并发创建图空间 + leader balance”场景下的死锁风险。
- 对应修复文件：
  - `src/kvstore/NebulaStore.cpp`
  - `src/kvstore/NebulaStore.h`
  - `src/kvstore/test/NebulaStoreTest.cpp`

## 2. 问题摘要

原问题表现为：

- `NebulaStore::addSpace()` 持有 `lock_` 写锁时，会同步等待 `newEngineAsync().get()`。
- `newEngineAsync()` 与 transfer leader 成功后的 leader 检查回调都依赖 `folly::getGlobalIOExecutor()`。
- 当 transfer leader 并发较高且线程池资源被占用时，可能出现：
  - 建空间线程持有写锁等待线程池任务完成。
  - transfer leader 回调线程等待 `partLeader()` 的读锁。
  - 双方互相等待，导致死锁和 CPU 占用升高。

## 3. 修复说明

本次修复将 engine 构建逻辑提取为同步方法 `createEngine()`：

- `newEngineAsync()` 仍保留异步行为，用于启动阶段扫描磁盘时的并行 engine 创建。
- `newEngine()` 改为当前线程直接调用 `createEngine()`，不再同步等待投递到 `folly::getGlobalIOExecutor()` 的 future。

这样可以避免 `addSpace()` 在持有写锁期间继续依赖全局 IO 线程池。

## 4. 新增测试用例

- 用例名称：`NebulaStoreTest.AddSpaceDoesNotDependOnGlobalIOExecutor`
- 用例目的：
  - 先使用大量阻塞任务占满 `folly::getGlobalIOExecutor()`。
  - 再调用 `store->addSpace(1)`。
  - 验证 `addSpace()` 仍可在限定时间内返回，并成功创建 space engine。

## 5. 手动执行步骤

### 5.1 编译测试目标

执行命令：

```bash
cmake --build build --target nebula_store_test -j4
```

实际结果：

- 编译成功。
- 生成测试二进制：`build/bin/test/nebula_store_test`

### 5.2 执行新增回归测试

执行命令：

```bash
./bin/test/nebula_store_test --gtest_filter=NebulaStoreTest.AddSpaceDoesNotDependOnGlobalIOExecutor
```

执行目录：

```bash
cd build
```

实际结果：

- 用例执行成功。
- 关键现象：
  - 即使 `folly::getGlobalIOExecutor()` 被阻塞任务占用，`store->addSpace(1)` 仍在 2 秒内返回。
  - space `1` 成功创建，并生成预期的 engine 目录。

测试结论：

- 通过。

### 5.3 执行既有 leader transfer 回归测试

执行命令：

```bash
./bin/test/nebula_store_test --gtest_filter=NebulaStoreTest.TransLeaderTest
```

实际结果：

- 用例执行成功。
- 说明本次修复未破坏既有 transfer leader 行为。

测试结论：

- 通过。

## 6. 结果汇总

| 测试项 | 命令 | 结果 |
| --- | --- | --- |
| 编译 `nebula_store_test` | `cmake --build build --target nebula_store_test -j4` | 通过 |
| 新增回归测试 | `./bin/test/nebula_store_test --gtest_filter=NebulaStoreTest.AddSpaceDoesNotDependOnGlobalIOExecutor` | 通过 |
| 原有 leader transfer 回归测试 | `./bin/test/nebula_store_test --gtest_filter=NebulaStoreTest.TransLeaderTest` | 通过 |

## 7. 最终结论

本次修复有效切断了 `addSpace()` 在持有 `NebulaStore::lock_` 写锁期间对 `folly::getGlobalIOExecutor()` 的同步依赖。新增回归测试与原有 transfer leader 测试均通过，说明修复能够覆盖本次死锁问题的关键触发链路，且未引入明显功能回归。
