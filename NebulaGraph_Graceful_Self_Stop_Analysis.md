# Nebula Graph 源码中如何优雅停止自身进程

## 1. 结论先行

在 Nebula Graph 当前源码里，“优雅停止自身进程”的主线不是直接调用 `exit()`，也不是业务线程直接 `kill(getpid(), SIGKILL)`，而是：

1. 入口 daemon 安装 `SIGINT` / `SIGTERM` 信号处理器。
2. 收到信号后只触发服务层的 stop/notify 逻辑，让主线程从阻塞等待中退出。
3. 主线程继续执行已有的 shutdown 路径，按顺序停止 Thrift/Web/KVStore/MetaClient/JobManager 等资源。
4. `main()` 最后自然返回 `EXIT_SUCCESS`，由进程生命周期完成退出。

如果需要在 Nebula Graph **源码内部让当前进程优雅退出**，推荐复用同一条控制流：

- Graph / Storage：优先调用对应 server 的 `notifyStop()`，让 `waitUntilStop()` 接管真正的资源释放。
- Meta：调用 `ThriftServer::stop()` 让 `serve()` 返回，然后走 `waitForStop()` 做 Meta 侧清理。
- Standalone：调用 `stopAllDaemon()` 统一停止 Graph、Storage、Meta 与 Meta KVStore。
- 如果你的触发点不方便直接拿到 server 指针，可以发送 `SIGTERM` 给自身进程来复用 daemon 已注册的信号处理器，但不要用 `SIGKILL`，也不要绕开 shutdown 流程直接 `_exit()` / `exit()`。

## 2. 为什么不建议直接 `exit()` 或 `SIGKILL`

Nebula 的 daemon 在停止时不仅要让 RPC 服务停止接收请求，还要关闭后台线程、KVStore、Raft 服务、MetaClient、JobManager、WebService 等组件。直接退出进程会跳过这些有序清理，风险包括：

- Thrift/Raft/HTTP 服务未完成关闭；
- KVStore / RocksEngine / RaftPart 背景任务没有执行 stop；
- Meta 心跳线程、后台 job、admin task 未按预期收尾；
- pid 文件、日志、线程池和异步任务状态更难诊断。

因此更优雅的做法是让进程自己进入已有的 stop 状态机，而不是硬杀。

## 3. 通用信号入口：`SignalHandler`

`SignalHandler::install()` 会为指定信号注册 `sigaction`，并且初始化时忽略 `SIGPIPE` 和 `SIGHUP`。它显式禁止注册 `SIGKILL` 和 `SIGSTOP`，因为这两个信号不能被用户态捕获。注册后，`handlerHook()` 会分发到已保存的 handler。

这意味着 Nebula 的 daemon 约定是：用可捕获的 `SIGINT` / `SIGTERM` 做优雅停止入口，用 `SIGKILL` 只作为外部强制兜底，而不是应用内的正常退出方式。

## 4. Graph 进程的优雅停止路径

### 4.1 daemon 入口

`GraphDaemon.cpp` 在启动 `GraphServer` 后安装信号处理器。收到 `SIGINT` 或 `SIGTERM` 时，handler 打日志并调用：

```cpp
GraphServer::notifyStop();
```

随后 `main()` 中阻塞的：

```cpp
GraphServer::waitUntilStop();
```

会被唤醒。停止完成后 `main()` 打印 `The graph Daemon stopped` 并返回 `EXIT_SUCCESS`。

### 4.2 `GraphServer` 内部 stop 状态机

`GraphServer::notifyStop()` 只做轻量通知：拿锁、把 `serverStatus_` 改成 `STATUS_STOPPED`、唤醒条件变量。

真正停止动作在 `GraphServer::waitUntilStop()` 中完成：

1. 等待 `serverStatus_ != STATUS_RUNNING`；
2. 调用 `thriftServer_->stop()`；
3. `join()` Graph 服务线程。

`GraphServer::stop()` 是幂等兜底：如果已停止则直接返回；否则把状态切到 stopped，并调用 `thriftServer_->stop()`。

### 4.3 Graph 内部触发建议

如果业务代码位于 Graph 进程内，并且能拿到 `GraphServer*` 或可封装一个 stop callback，建议：

```cpp
graphServer->notifyStop();
```

让主线程继续走 `waitUntilStop()`，不要在业务线程里直接 `exit()`。

## 5. Storage 进程的优雅停止路径

### 5.1 daemon 入口

`StorageDaemon.cpp` 与 Graph 类似：启动 `StorageServer` 后安装 `SIGINT` / `SIGTERM` handler。收到信号后调用：

```cpp
storageServer->notifyStop();
```

主线程阻塞在：

```cpp
storageServer->waitUntilStop();
```

被唤醒后继续执行停止流程，最后返回 `EXIT_SUCCESS`。

### 5.2 `StorageServer::notifyStop()` 为什么只做通知

`StorageServer.h` 对 `notifyStop()` 有明确注释：这是给 signal handler 设置内部停止标志用的，里面“不允许任何 wait”。当前实现符合这个约束：它只更新状态、通知条件变量，并额外调用 `metaClient_->notifyStop()` 唤醒 MetaClient 相关等待。

这点很重要：信号处理路径应尽量短，避免在 handler 中做复杂阻塞式释放。

### 5.3 真正的 Storage 清理顺序

`StorageServer::waitUntilStop()` 被唤醒后调用 `this->stop()`。`StorageServer::stop()` 按顺序做：

1. reset WebService；
2. 调用 `kvstore_->stop()` 停止 KVStore 后台任务和 Raft 服务；
3. clean up admin/storage Thrift server；
4. shutdown admin task manager；
5. stop MetaClient；
6. reset KVStore。

### 5.4 Storage 内部触发建议

如果在 Storage 进程内部需要自停止，优先：

```cpp
storageServer->notifyStop();
```

不要直接在业务线程调用 `storageServer->stop()` 后再 `exit()`，否则容易和主线程状态机、条件变量等待、服务线程清理顺序产生竞态。若确实需要同步等待停止完成，也应由拥有 daemon 生命周期的主线程做等待和资源释放。

## 6. Meta 进程的优雅停止路径

MetaDaemon 的结构和 Graph/Storage 略不同：它直接持有 `apache::thrift::ThriftServer`，调用 `serve()` 阻塞等待。信号 handler 收到 `SIGINT` / `SIGTERM` 后调用：

```cpp
metaServer->stop();
```

这会让 `serve()` 返回。随后 main 线程调用 `waitForStop()`，其中：

1. `JobManager::shutDown()` 停止 Meta 后台 job；
2. `gKVStore->stop()` 停止 Meta KVStore；
3. reset `gKVStore`。

因此 Meta 内部触发优雅停止时，如果能拿到 `ThriftServer*`，应调用：

```cpp
metaServer->stop();
```

并让 daemon 主流程继续执行 `waitForStop()`。

## 7. Standalone 模式

`StandAloneDaemon.cpp` 里 Graph、Storage、Meta 在同一进程内启动。它的 `SIGINT` / `SIGTERM` handler 调用 `stopAllDaemon()`。该函数按顺序停止并 reset：

1. Graph Thrift server；
2. StorageServer；
3. Meta Thrift server；
4. JobManager；
5. Meta KVStore。

所以 standalone 里更合适的统一入口是复用 `stopAllDaemon()`，而不是只停止其中一个组件后直接退出整个进程。

## 8. 如果没有 server 指针：可用 `SIGTERM` 复用现有路径

如果触发点在比较底层，拿不到 daemon/server 指针，也不希望引入反向依赖，可以发送 `SIGTERM` 给当前进程：

```cpp
::kill(::getpid(), SIGTERM);
```

优点：

- 复用 daemon 已安装的信号处理器；
- Graph/Storage/Meta/Standalone 各自走已有停止逻辑；
- 和外部 `kill -TERM <pid>` 行为一致。

注意：

- 只适合作为“请求自身优雅退出”的触发，不要用 `SIGKILL`；
- 发送信号后不要紧接着 `exit()`，应让主线程自然退出；
- 如果代码运行在测试或工具进程中，需要先确认该进程确实安装了对应 `SignalHandler`。

## 9. 推荐封装方式

为了让业务代码更清晰，可以在 daemon/server 层引入一个明确的 stop callback，而不是让深层模块知道具体 server 类型：

```cpp
using StopCallback = std::function<void()>;

class SomeComponent {
 public:
  explicit SomeComponent(StopCallback stopCb) : stopCb_(std::move(stopCb)) {}

  void requestProcessStop() {
    if (stopCb_) {
      stopCb_();  // Graph/Storage 里绑定到 notifyStop，Meta 里绑定到 thriftServer->stop。
    }
  }

 private:
  StopCallback stopCb_;
};
```

绑定建议：

```cpp
// Graph
auto stopCb = [graphServer] { graphServer->notifyStop(); };

// Storage
auto stopCb = [storageServer] { storageServer->notifyStop(); };

// Meta
auto stopCb = [metaServer] { metaServer->stop(); };
```

这样可以保持模块解耦，也能保证停止请求进入 daemon 原生生命周期。

## 10. 一句话实践建议

- **首选**：调用当前 daemon/server 的“通知停止”接口，让 main 线程完成清理。
- **Graph / Storage**：`notifyStop()`。
- **Meta**：`ThriftServer::stop()`，然后走 `waitForStop()`。
- **Standalone**：`stopAllDaemon()`。
- **兜底触发**：`kill(getpid(), SIGTERM)` 复用信号链路。
- **避免**：`exit()`、`_exit()`、`abort()`、`SIGKILL`、在信号 handler 或业务线程里做复杂阻塞式释放。

## 11. 关键源码索引

- `src/common/base/SignalHandler.cpp`：通用信号安装、忽略 `SIGPIPE` / `SIGHUP`、禁止 `SIGKILL` / `SIGSTOP`、分发 handler。
- `src/daemons/GraphDaemon.cpp`：Graph daemon 安装 `SIGINT` / `SIGTERM`，收到信号调用 `GraphServer::notifyStop()`。
- `src/graph/service/GraphServer.{h,cpp}`：Graph 的 `notifyStop()`、`waitUntilStop()`、`stop()` 状态机。
- `src/daemons/StorageDaemon.cpp`：Storage daemon 安装 `SIGINT` / `SIGTERM`，收到信号调用 `StorageServer::notifyStop()`。
- `src/storage/StorageServer.{h,cpp}`：Storage 的 `notifyStop()`、`waitUntilStop()`、`stop()` 清理顺序。
- `src/daemons/MetaDaemon.cpp`：Meta daemon 收到信号调用 `ThriftServer::stop()`，随后 `waitForStop()` 清理 JobManager 和 KVStore。
- `src/daemons/StandAloneDaemon.cpp`：Standalone 统一入口 `stopAllDaemon()`。
- `src/common/process/ProcessUtils.cpp`：pid 文件和 daemonize 逻辑；源码中也使用 `kill(pid, 0)` 检查 pid 是否存在，说明正常停止不依赖强杀进程。
