# PR Title

`kvstore: avoid waiting on global IO executor while holding NebulaStore write lock`

# PR Description

## Summary

This change fixes a deadlock risk between concurrent space creation and leader balance in NebulaGraph 3.6.

Previously, `NebulaStore::addSpace()` could hold `NebulaStore::lock_` as a write lock and then synchronously wait for `newEngineAsync().get()`. Since `newEngineAsync()` schedules work on `folly::getGlobalIOExecutor()`, and transfer-leader follow-up checks also use the same executor and need `partLeader()` to acquire the same store lock as a read lock, the system could end up in a lock/executor dependency cycle under concurrency.

This patch removes that synchronous dependency from the `addSpace()` path.

## Root Cause

The deadlock risk comes from the following sequence:

1. `NebulaStore::addSpace()` acquires the store write lock.
2. `addSpace()` calls `newEngine()`.
3. `newEngine()` waits on `newEngineAsync().get()`.
4. `newEngineAsync()` runs on `folly::getGlobalIOExecutor()`.
5. Transfer leader completion callbacks also run on `folly::getGlobalIOExecutor()` and call `partLeader()`, which needs the store read lock.

Under sufficient concurrency, this creates a lock wait plus executor starvation cycle:

- the space-creation path holds the write lock while waiting for the global IO executor
- the transfer-leader callback waits for the store lock while occupying the same executor

## Fix

This patch extracts the engine construction logic into a synchronous helper:

- add `NebulaStore::createEngine()`
- keep `newEngineAsync()` for the startup path that scans existing engines from disk
- change `newEngine()` to call `createEngine()` directly instead of waiting on `newEngineAsync().get()`

With this change, `addSpace()` no longer depends on `folly::getGlobalIOExecutor()` while holding the write lock.

## Why This Approach

- It is a minimal change with a narrow behavioral impact.
- It preserves the existing async engine-loading behavior during startup.
- It avoids changing transfer-leader behavior or lock ownership semantics more broadly.
- It directly removes the lock/executor dependency that makes the deadlock possible.

## Tests

### Added

- `NebulaStoreTest.AddSpaceDoesNotDependOnGlobalIOExecutor`

This test saturates `folly::getGlobalIOExecutor()` with blocking tasks, then calls `addSpace(1)` and verifies that the call still completes promptly and creates the expected engine.

### Verified

```bash
cmake --build build --target nebula_store_test -j4
./build/bin/test/nebula_store_test --gtest_filter=NebulaStoreTest.AddSpaceDoesNotDependOnGlobalIOExecutor
./build/bin/test/nebula_store_test --gtest_filter=NebulaStoreTest.TransLeaderTest
```

## Risk

Low.

The change only affects the synchronous space-creation path. Startup-time async engine loading remains unchanged, and existing transfer-leader behavior is covered by the regression run above.

# Optional Commit Message

`fix(kvstore): avoid global IO executor wait in addSpace`
