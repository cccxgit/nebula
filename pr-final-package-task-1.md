# Final Submission Package

## Recommended Commit Message

### Subject

`fix bug`

### Body

```text
Avoid waiting on folly::getGlobalIOExecutor() while holding
NebulaStore write lock in addSpace().

Extract engine creation into a synchronous helper so the
space-creation path no longer blocks on the global IO executor.
This removes the lock/executor dependency that can deadlock with
transfer leader follow-up checks under concurrency.

Add a regression test to verify addSpace() still completes when
the global IO executor is saturated.
```

## Recommended PR Title

`Fix deadlock between addSpace and transfer leader follow-up checks`

## Recommended PR Body

```markdown
## Summary

This PR fixes a deadlock risk when space creation happens concurrently with leader balance in NebulaGraph 3.6.

`NebulaStore::addSpace()` used to hold `NebulaStore::lock_` as a write lock and then wait on `newEngineAsync().get()`. Since `newEngineAsync()` runs on `folly::getGlobalIOExecutor()`, and transfer-leader follow-up checks also run on the same executor and call `partLeader()` (which needs the store lock), this could create a lock/executor dependency cycle under concurrency.

## Root Cause

The problematic sequence was:

1. `addSpace()` acquires the store write lock.
2. `newEngine()` waits on `newEngineAsync().get()`.
3. `newEngineAsync()` depends on `folly::getGlobalIOExecutor()`.
4. transfer leader follow-up checks also depend on `folly::getGlobalIOExecutor()`.
5. those checks call `partLeader()`, which needs the store read lock.

This can form a cycle where the space-creation path holds the write lock while waiting for the global executor, and the executor is busy running tasks that need the store lock.

## Fix

- Extract engine creation logic into `createEngine()`
- Keep `newEngineAsync()` for startup-time async engine loading
- Change `newEngine()` to call `createEngine()` directly instead of waiting on `newEngineAsync().get()`

This removes the dependency on `folly::getGlobalIOExecutor()` from the synchronous `addSpace()` path.

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

The change only affects the synchronous space-creation path. Startup-time async engine loading remains unchanged, and the existing transfer-leader regression test still passes.
```

## Suggested PR Comment If Reviewers Ask “Why Not Change TransLeaderProcessor?”

```text
The deadlock is caused by the synchronous addSpace() path holding
NebulaStore::lock_ while waiting on work scheduled to the global IO
executor. Changing the transfer leader path would be broader and would
not remove that dependency from the write-locked addSpace() path.

This fix keeps behavior changes minimal and removes the dependency at
the source of the lock/executor cycle.
```

## Suggested Pre-PR Checklist

- Confirm only these code files are staged:
  - `src/kvstore/NebulaStore.cpp`
  - `src/kvstore/NebulaStore.h`
  - `src/kvstore/test/NebulaStoreTest.cpp`
- Do not include:
  - `task-1.md`
  - local report files
  - local PR draft files
- Re-run:

```bash
cmake --build build --target nebula_store_test -j4
./build/bin/test/nebula_store_test --gtest_filter=NebulaStoreTest.AddSpaceDoesNotDependOnGlobalIOExecutor
./build/bin/test/nebula_store_test --gtest_filter=NebulaStoreTest.TransLeaderTest
git diff --check -- src/kvstore/NebulaStore.cpp src/kvstore/NebulaStore.h src/kvstore/test/NebulaStoreTest.cpp
```

## Final Recommendation

The current code change is already appropriate for an upstream PR:

- the fix is minimal
- the root cause is directly addressed
- regression coverage is included
- no additional code refactor is recommended before submission

If you want to keep the upstream patch especially conservative, submit only the three code file changes and keep all generated markdown documents out of the commit.
