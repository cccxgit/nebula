# `getLeaderParts` 作为 Storage client 接口是否涉及 `folly::RWSpinLock lock_`

## 1. 结论

**涉及。** `getLeaderParts` 是 Storage AdminService 暴露给 Meta `AdminClient` 的 RPC 接口。它在 Storage 端处理请求时会调用：

```cpp
env_->kvstore_->allLeader(allLeaders);
```

当前 `NebulaStore::allLeader()` 的实现会创建：

```cpp
folly::RWSpinLock::ReadHolder rh(&lock_);
```

因此，**每次 Storage 端执行 `getLeaderParts` 时，都会获取一次 `NebulaStore::lock_` 的读锁**，然后遍历 `spaces_ -> parts_`，筛选本机当前为 leader 的 partition。

不过这个接口拿的是**读锁**，不是写锁；它不创建 engine、不调用 `newEngineAsync()`、不等待 `folly::getGlobalIOExecutor()`。它的直接影响主要是：当有写锁持有者（例如 add/remove space/part 路径）正在修改 `spaces_` / `parts_` 时，`getLeaderParts` 会被阻塞；反过来，大量并发的 `getLeaderParts` 读锁也可能让写锁等待更久。

## 2. 调用链路

从 Meta 侧 client 到 Storage 侧 `NebulaStore::lock_` 的链路如下：

```text
Meta AdminClient::getLeaderDist(host)
  -> storage admin client future_getLeaderParts(req)
    -> StorageAdminServiceHandler::future_getLeaderParts(req)
      -> GetLeaderProcessor::process(req)
        -> env_->kvstore_->allLeader(allLeaders)
          -> NebulaStore::allLeader(allLeaders)
            -> folly::RWSpinLock::ReadHolder rh(&lock_)
            -> 遍历 spaces_ / parts_
            -> part->isLeader(), part->termId()
```

## 3. Storage client 接口入口

`storage.thrift` 中 `getLeaderParts` 的定义说明它是 Storage admin service 的接口，用于返回当前 host 上所有 leader partitions：

```thrift
// Return all leader partitions on this host
GetLeaderPartsResp getLeaderParts(1: GetLeaderReq req);
```

Meta 侧 `AdminClient::getLeaderDist(const HostAddr& host)` 会构造 `GetLeaderReq`，然后通过 Storage admin client 调用 `future_getLeaderParts(request)`。返回值里只取 `resp.get_leader_parts()`。

## 4. Storage handler 与 processor

Storage 侧 `StorageAdminServiceHandler::future_getLeaderParts()` 不直接扫描 leader，而是创建 `GetLeaderProcessor` 并调用 `processor->process(req)`。

`GetLeaderProcessor::process()` 的关键逻辑是：

1. 忽略空的 `GetLeaderReq`；
2. 检查 `env_->kvstore_` 非空；
3. 调用 `env_->kvstore_->allLeader(allLeaders)`；
4. 将 `meta::cpp2::LeaderInfo` 转成 response 里的 `space -> vector<partId>`；
5. 通过 promise 返回 `GetLeaderPartsResp`。

因此，是否加锁不在 handler 层，而是在 `kvstore_->allLeader()` 的具体实现里。

## 5. `NebulaStore::allLeader()` 的锁行为

`NebulaStore::allLeader()` 第一行就是：

```cpp
folly::RWSpinLock::ReadHolder rh(&lock_);
```

随后它遍历 `spaces_`，再遍历每个 space 的 `parts_`：

```cpp
for (const auto& spaceIt : spaces_) {
  auto spaceId = spaceIt.first;
  for (const auto& partIt : spaceIt.second->parts_) {
    auto partId = partIt.first;
    if (partIt.second->isLeader()) {
      meta::cpp2::LeaderInfo partInfo;
      partInfo.part_id_ref() = partId;
      partInfo.term_ref() = partIt.second->termId();
      leaderIds[spaceId].emplace_back(std::move(partInfo));
      ++count;
    }
  }
}
```

这说明 `getLeaderParts` 不只是读 Raft leader 状态，它还需要在 `NebulaStore::lock_` 保护下读取 `spaces_` / `parts_` 容器结构。

## 6. 与 `partLeader()` 的区别

另一个常见 leader 查询接口是 `NebulaStore::partLeader(spaceId, partId)`。它同样会获取 `lock_` 的读锁，但只查询单个 partition：

```cpp
folly::RWSpinLock::ReadHolder rh(&lock_);
auto it = spaces_.find(spaceId);
...
auto partIt = parts.find(partId);
...
return getStoreAddr(partIt->second->leader());
```

对比来看：

| 接口 | 是否使用 `lock_` | 锁类型 | 扫描范围 |
| --- | --- | --- | --- |
| `getLeaderParts` -> `NebulaStore::allLeader()` | 是 | 读锁 | 全部 spaces / parts |
| `partLeader(spaceId, partId)` | 是 | 读锁 | 单个 space / part |

所以如果问题关注“Storage client 接口 `getLeaderParts` 被调用时是否会碰 `NebulaStore::lock_`”，答案是肯定的；而且它比单 partition 的 `partLeader()` 读锁覆盖的遍历范围更大。

## 7. 与 GlobalIO / 写锁死锁分析的关系

`getLeaderParts` 自身没有以下行为：

- 不持有写锁；
- 不调用 `addSpace()` / `addPart()`；
- 不调用 `newEngineAsync()`；
- 不同步等待 `folly::getGlobalIOExecutor()` 上的任务。

因此，**它不是“写锁内等待 GlobalIO”的直接触发点**。

但它属于“会拿 `NebulaStore::lock_` 读锁的 Storage admin RPC”。在以下场景中仍然相关：

1. 如果某线程已经持有 `NebulaStore::lock_` 写锁，`getLeaderParts` 会在 `allLeader()` 的读锁入口等待。
2. 如果大量 `getLeaderParts` 并发执行，它们会频繁进入读锁区间，可能增加写锁获取延迟。
3. 如果未来某个调用方把 `getLeaderParts` 所在任务投递到 `folly::getGlobalIOExecutor()`，那么它也会成为 GlobalIO 上的“读锁任务”，与已有的 `TransLeaderProcessor` 读锁类任务类似，可能放大“GlobalIO 线程被等待读锁的任务占满”的风险窗口。

所以更准确的表述是：

> `getLeaderParts` 当前路径会获取 `NebulaStore::lock_` 读锁；它本身不是写锁等待 GlobalIO 的源头，但可以作为读锁侧参与者，影响写锁等待和潜在的 executor/lock 交互风险。

## 8. 关键源码索引

- `src/interface/storage.thrift`：定义 Storage admin RPC `getLeaderParts`，用于返回当前 host 的所有 leader partitions。
- `src/meta/processors/admin/AdminClient.cpp`：Meta `AdminClient::getLeaderDist(host)` 调用 Storage admin client 的 `future_getLeaderParts()`。
- `src/storage/StorageAdminServiceHandler.cpp`：Storage handler 将 RPC 转给 `GetLeaderProcessor`。
- `src/storage/admin/GetLeaderProcessor.cpp`：processor 调用 `env_->kvstore_->allLeader(allLeaders)`。
- `src/kvstore/KVStore.h`：声明虚接口 `allLeader()`，语义是获取 leader distribution。
- `src/kvstore/NebulaStore.cpp`：`NebulaStore::allLeader()` 用 `folly::RWSpinLock::ReadHolder` 获取 `lock_` 读锁并遍历 `spaces_` / `parts_`。
- `src/kvstore/NebulaStore.cpp`：`NebulaStore::partLeader()` 也使用 `lock_` 读锁，但只查询单个 partition。
