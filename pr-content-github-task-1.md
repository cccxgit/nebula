# Title

`kvstore: avoid waiting on global IO executor while holding NebulaStore write lock`

# Description

## Summary

This PR fixes a deadlock risk when space creation happens concurrently with leader balance in NebulaGraph 3.6.

`NebulaStore::addSpace()` used to hold `NebulaStore::lock_` as a write lock and then wait on `newEngineAsync().get()`. Since `newEngineAsync()` runs on `folly::getGlobalIOExecutor()`, and transfer-leader follow-up checks also run on the same executor and call `partLeader()` (which needs the store lock), this could create a lock/executor dependency cycle under concurrency.

## Fix

- Extract engine creation logic into a synchronous helper `createEngine()`
- Keep `newEngineAsync()` for the startup path that scans existing engines from disk
- Change `newEngine()` to call `createEngine()` directly instead of waiting on `newEngineAsync().get()`

This removes the dependency on `folly::getGlobalIOExecutor()` from the `addSpace()` path while the store write lock is held.

## Tests

Added regression test:

- `NebulaStoreTest.AddSpaceDoesNotDependOnGlobalIOExecutor`

Verified with:

```bash
cmake --build build --target nebula_store_test -j4
./build/bin/test/nebula_store_test --gtest_filter=NebulaStoreTest.AddSpaceDoesNotDependOnGlobalIOExecutor
./build/bin/test/nebula_store_test --gtest_filter=NebulaStoreTest.TransLeaderTest
```

## Risk

Low.

The change only affects the synchronous space-creation path. Startup-time async engine loading remains unchanged.

# Suggested Commit Message

`fix(kvstore): avoid global IO executor wait in addSpace`
