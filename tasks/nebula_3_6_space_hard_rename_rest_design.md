# NebulaGraph 3.6 图空间硬改名 REST 接口设计与 Codex 开发指导

> 适用版本：NebulaGraph 3.6  
> 目标：在**不新增 nGQL 语法**、**不修改 `space_id`**、**不迁移 Storage 数据**的前提下，在 `nebula-metad` 暴露一个内部 REST 运维接口，支持对图空间执行 **hard rename**：删除旧名称映射，新增新名称映射，并保持原 `space_id` 不变。

---

## 1. 背景与结论

### 1.1 背景

NebulaGraph 3.6 官方文档说明：

- 图空间是彼此隔离的图数据集合，类似 MySQL 的 database。
- `CREATE SPACE` 可以创建新图空间，也可以克隆已有图空间的 Schema。
- `<graph_space_name>` 在 NebulaGraph 实例中唯一标识一个图空间，且“图空间名称设置后无法被修改”。
- Meta 服务负责图空间元数据、分片位置信息、Schema 信息、用户权限和作业管理。
- Meta 集群由 Raft 协议保证 leader/follower 数据一致；只有 leader 对客户端或其他组件提供服务。

参考资料：

- NebulaGraph 3.6 `CREATE SPACE` 文档：`https://docs.nebula-graph.com.cn/3.6.0/3.ngql-guide/9.space-statements/1.create-space/`
- NebulaGraph 3.6 Meta 服务文档：`https://docs.nebula-graph.com.cn/3.6.0/1.introduction/3.nebula-graph-architecture/2.meta-service/`

### 1.2 当前问题

官方 nGQL 层不支持：

```ngql
RENAME SPACE old_space TO new_space;
ALTER SPACE old_space RENAME TO new_space;
```

而 `CREATE SPACE new_space AS old_space` 只是克隆 Schema，不是重命名，也不会复用原 `space_id`。

### 1.3 设计结论

从内核实现角度，Storage 侧图数据主要按 `space_id` 隔离和存储。只要保持 `space_id` 不变，并正确修改 Meta 中的：

```text
space name -> space_id 映射
space_id -> SpaceDesc
```

就可以实现“改图空间名称但不改 `space_id`”。

本方案不新增 nGQL 语法，而是在 `nebula-metad` 暴露一个**内部 REST 运维接口**：

```http
POST /admin/space/rename
```

该接口只支持 hard rename：

```text
执行前：
old_space -> space_id = 100
SpaceDesc.name = old_space

执行后：
new_space -> space_id = 100
SpaceDesc.name = new_space
old_space 映射被删除
```

---

## 2. 需求目标

### 2.1 功能目标

实现一个内部 REST 接口，完成：

```text
old_space hard rename to new_space
space_id 保持不变
旧名称立即不可用
新名称立即生效，或等待 Graph Meta cache 刷新后生效
```

接口能力：

```text
1. 支持 dry-run，只校验和返回执行计划，不修改 Meta。
2. 支持 expected_space_id 防误操作。
3. 支持 token 鉴权。
4. 支持操作审计日志。
5. 只允许请求 Meta leader 执行。
6. 通过 Meta Raft/KV 原子提交，不允许直接修改 RocksDB 文件。
```

### 2.2 明确删除的能力

按当前要求，删除上一版中的：

```text
compatible 模式
finalize 模式
rollback 模式
alias 语义
新旧名称共存能力
```

因此接口语义非常明确：

```text
只做 hard rename。
执行成功后 old_space 不再存在。
```

### 2.3 非目标

本功能不做：

```text
不新增 RENAME SPACE 语法
不新增 ALTER SPACE RENAME 语法
不修改 CREATE SPACE AS 语义
不复制点边数据
不修改 Storage 数据目录
不修改 partition_num、replica_factor、vid_type
不修改 Raft group、part 分布和数据文件
不手工改 Meta RocksDB
不支持跨集群迁移
```

---

## 3. 总体方案

### 3.1 方案概览

```text
HTTP 请求
  |
  v
nebula-metad REST Handler
  |
  | 参数校验 / token 校验 / dry-run 判断
  v
RenameSpaceHardProcessor
  |
  | 校验 leader
  | 查询 old_space 对应 space_id
  | 校验 expected_space_id
  | 校验 new_space 不存在
  | 读取并修改 SpaceDesc.name
  | 构造 put/remove
  v
Meta KVStore / Raft 原子提交
  |
  v
Meta follower 同步
  |
  v
Graph 服务等待心跳刷新或滚动重启
```

### 3.2 修改范围

主要修改 `nebula-metad`，原则上不修改 `graphd` 和 `storaged`。

```text
需要修改：
- Meta REST handler 注册逻辑
- 新增 RenameSpaceHardProcessor
- 新增配置项
- 新增单元测试
- 新增集成验证说明

原则上不修改：
- Storage 数据读写逻辑
- Graph parser / validator / executor
- nGQL 语法
- Storage KV 编码
```

---

## 4. REST 接口设计

### 4.1 Endpoint

```http
POST /admin/space/rename
Content-Type: application/json
X-Nebula-Admin-Token: <token>
```

### 4.2 请求体

```json
{
  "old_name": "old_space",
  "new_name": "new_space",
  "dry_run": true,
  "expected_space_id": 100,
  "operator": "admin",
  "comment": "hard rename space for production workaround"
}
```

### 4.3 字段说明

| 字段 | 类型 | 必填 | 说明 |
|---|---:|---:|---|
| `old_name` | string | 是 | 原图空间名称 |
| `new_name` | string | 是 | 新图空间名称 |
| `dry_run` | bool | 否 | 是否只执行校验，默认 `true` |
| `expected_space_id` | int | 是 | 防误操作字段，必须与 `old_name` 当前 `space_id` 一致 |
| `operator` | string | 否 | 操作者，用于审计日志 |
| `comment` | string | 否 | 操作原因，用于审计日志 |

### 4.4 字段约束

```text
old_name 必须存在
new_name 必须不存在
old_name != new_name
expected_space_id 必须传入
expected_space_id 必须等于 old_name 当前 space_id
dry_run 缺省值为 true
非 dry_run 必须显式传 dry_run=false
```

### 4.5 成功响应

dry-run 成功：

```json
{
  "code": 0,
  "message": "DRY_RUN_SUCCEEDED",
  "operation": "hard_rename_space",
  "dry_run": true,
  "old_name": "old_space",
  "new_name": "new_space",
  "space_id": 100,
  "will_put": [
    "spaceNameKey(new_space) -> 100",
    "spaceKey(100) -> SpaceDesc.name=new_space"
  ],
  "will_remove": [
    "spaceNameKey(old_space)"
  ],
  "warnings": [
    "hard rename will make old_space unavailable immediately",
    "graphd may still cache old meta info before next heartbeat",
    "restart graphd or wait for meta cache refresh before validation"
  ]
}
```

实际执行成功：

```json
{
  "code": 0,
  "message": "SUCCEEDED",
  "operation": "hard_rename_space",
  "dry_run": false,
  "old_name": "old_space",
  "new_name": "new_space",
  "space_id": 100,
  "old_name_removed": true,
  "space_desc_name": "new_space",
  "warnings": [
    "old_space is unavailable after hard rename",
    "graphd cache refresh is required before using new_space reliably"
  ]
}
```

### 4.6 失败响应

```json
{
  "code": -1,
  "message": "NEW_SPACE_ALREADY_EXISTS",
  "operation": "hard_rename_space",
  "old_name": "old_space",
  "new_name": "new_space",
  "detail": "new_space already maps to space_id=101"
}
```

### 4.7 错误码建议

| 错误码 | 含义 |
|---|---|
| `E_SPACE_NOT_FOUND` | `old_name` 不存在 |
| `E_SPACE_ALREADY_EXISTS` | `new_name` 已存在 |
| `E_SPACE_ID_MISMATCH` | `expected_space_id` 与实际 `space_id` 不一致 |
| `E_INVALID_SPACE_NAME` | `old_name` 或 `new_name` 不符合命名规则 |
| `E_OLD_NEW_NAME_SAME` | `old_name` 与 `new_name` 相同 |
| `E_NOT_META_LEADER` | 当前 metad 不是 leader |
| `E_PERMISSION_DENIED` | token 缺失或校验失败 |
| `E_DRY_RUN_REQUIRED` | 未执行 dry-run 或缺少显式确认 |
| `E_CONFLICT` | 与正在运行的 DDL/Job 冲突 |
| `E_STORE_FAILURE` | Meta KV/Raft 写入失败 |
| `E_INTERNAL_ERROR` | 其他内部错误 |

---

## 5. 元数据变更设计

### 5.1 关键 Meta 信息

需要关注两类 Meta KV：

```text
spaceNameKey(old_name) -> space_id
spaceKey(space_id)     -> SpaceDesc
```

hard rename 必须在同一次原子提交中完成：

```text
put    spaceNameKey(new_name) -> space_id
put    spaceKey(space_id)     -> SpaceDesc.name = new_name
remove spaceNameKey(old_name)
```

### 5.2 执行前后状态

执行前：

```text
spaceNameKey("old_space") -> 100
spaceKey(100).name        -> "old_space"
spaceNameKey("new_space") -> 不存在
```

执行后：

```text
spaceNameKey("old_space") -> 不存在
spaceNameKey("new_space") -> 100
spaceKey(100).name        -> "new_space"
```

### 5.3 不变项

以下内容不应改变：

```text
space_id
partition_num
replica_factor
vid_type
parts 分布
tag_id / edge_type_id
schema version
index id
Storage 数据
Storage Raft group
```

### 5.4 为什么不改 Storage

Storage 侧点边数据按 `space_id` 和 `part_id` 组织。hard rename 只改变图空间名称到 `space_id` 的解析关系，不改变 `space_id`，因此 Storage 数据无需迁移。

---

## 6. 核心处理流程

### 6.1 流程图

```text
POST /admin/space/rename
  |
  v
检查 enable_space_rename_rest
  |
  v
检查 X-Nebula-Admin-Token
  |
  v
解析 JSON 请求
  |
  v
校验 old_name/new_name/expected_space_id
  |
  v
确认当前 Meta 是 leader
  |
  v
getSpaceId(old_name)
  |
  v
getSpaceId(new_name)，必须不存在
  |
  v
getSpaceDesc(space_id)
  |
  v
修改 SpaceDesc.name = new_name
  |
  v
构造 puts/removes
  |
  +-- dry_run=true  -> 返回执行计划，不落盘
  |
  +-- dry_run=false -> Raft 原子提交
                       |
                       v
                    返回结果并写审计日志
```

### 6.2 Processor 伪代码

```cpp
StatusOr<RenameSpaceHardResult>
RenameSpaceHardProcessor::process(const RenameSpaceHardReq& req) {
    // 1. 参数校验
    if (req.old_name().empty() || req.new_name().empty()) {
        return Status::Error("E_INVALID_SPACE_NAME");
    }

    if (req.old_name() == req.new_name()) {
        return Status::Error("E_OLD_NEW_NAME_SAME");
    }

    NG_RETURN_IF_ERROR(validateSpaceName(req.old_name()));
    NG_RETURN_IF_ERROR(validateSpaceName(req.new_name()));

    if (!req.has_expected_space_id()) {
        return Status::Error("E_SPACE_ID_MISMATCH: expected_space_id is required");
    }

    // 2. 只允许 leader 执行
    if (!isLeader()) {
        return Status::Error("E_NOT_META_LEADER");
    }

    // 3. old_name 必须存在
    auto oldSpaceIdRet = getSpaceId(req.old_name());
    if (!oldSpaceIdRet.ok()) {
        return Status::Error("E_SPACE_NOT_FOUND");
    }
    auto spaceId = oldSpaceIdRet.value();

    // 4. expected_space_id 必须匹配
    if (req.expected_space_id() != spaceId) {
        return Status::Error("E_SPACE_ID_MISMATCH");
    }

    // 5. new_name 必须不存在
    auto newSpaceIdRet = getSpaceId(req.new_name());
    if (newSpaceIdRet.ok()) {
        return Status::Error("E_SPACE_ALREADY_EXISTS");
    }

    // 6. 读取 SpaceDesc
    auto spaceDescRet = getSpaceDesc(spaceId);
    if (!spaceDescRet.ok()) {
        return Status::Error("E_SPACE_DESC_NOT_FOUND");
    }
    auto spaceDesc = spaceDescRet.value();

    // 7. 修改 SpaceDesc.name
    spaceDesc.set_space_name(req.new_name());

    // 8. 构造原子写入
    std::vector<KV> puts;
    std::vector<std::string> removes;

    puts.emplace_back(MetaKeyUtils::spaceNameKey(req.new_name()),
                      encodeSpaceId(spaceId));
    puts.emplace_back(MetaKeyUtils::spaceKey(spaceId),
                      MetaKeyUtils::spaceVal(spaceDesc));
    removes.emplace_back(MetaKeyUtils::spaceNameKey(req.old_name()));

    // 9. dry-run 只返回计划
    if (req.dry_run()) {
        return RenameSpaceHardResult::dryRun(spaceId, puts, removes);
    }

    // 10. 原子提交，必须走 Meta KVStore/Raft，不允许直接改 RocksDB
    auto code = doSyncMultiPutAndRemove(std::move(puts), std::move(removes));
    if (code != nebula::cpp2::ErrorCode::SUCCEEDED) {
        return Status::Error("E_STORE_FAILURE");
    }

    return RenameSpaceHardResult::succeeded(spaceId);
}
```

> 注意：函数名以 3.6 代码仓实际实现为准。Codex 需要先搜索 `CreateSpaceProcessor`、`DropSpaceProcessor`、`MetaKeyUtils`、`BaseProcessor`、`doSyncPut`、`doSyncMultiRemove`、`doSyncMultiPutAndRemove` 等实际接口。

---

## 7. REST Handler 设计

### 7.1 Handler 职责

REST Handler 只做入口逻辑，不直接操作底层 KV：

```text
1. 检查开关。
2. 检查 token。
3. 解析 JSON。
4. 校验 required fields。
5. 调用 RenameSpaceHardProcessor。
6. 返回 JSON。
7. 打印审计日志。
```

不要让 Handler 直接拼接并写入 Meta KV，避免绕过 Processor 的统一校验和测试。

### 7.2 建议文件

Codex 需要先根据 3.6 实际代码结构定位 HTTP handler 所在目录。可能路径包括：

```text
src/meta/http/
src/meta/
src/common/http/
src/webservice/
```

建议新增或修改：

```text
src/meta/processors/admin/RenameSpaceHardProcessor.h
src/meta/processors/admin/RenameSpaceHardProcessor.cpp
src/meta/http/RenameSpaceHandler.h
src/meta/http/RenameSpaceHandler.cpp
src/meta/test/RenameSpaceHardProcessorTest.cpp
```

实际路径以代码仓为准。

### 7.3 Handler 伪代码

```cpp
void RenameSpaceHandler::onRequest(const HttpRequest& req, HttpResponse& resp) {
    if (!FLAGS_enable_space_rename_rest) {
        resp.status = 404;
        resp.body = jsonError("DISABLED");
        return;
    }

    auto token = req.header("X-Nebula-Admin-Token");
    if (token.empty() || token != FLAGS_space_rename_admin_token) {
        resp.status = 403;
        resp.body = jsonError("E_PERMISSION_DENIED");
        return;
    }

    auto body = parseJson(req.body());
    RenameSpaceHardReq renameReq;
    renameReq.set_old_name(body["old_name"].getString());
    renameReq.set_new_name(body["new_name"].getString());
    renameReq.set_dry_run(body.value("dry_run", true));
    renameReq.set_expected_space_id(body["expected_space_id"].getInt());
    renameReq.set_operator(body.value("operator", ""));
    renameReq.set_comment(body.value("comment", ""));

    auto result = RenameSpaceHardProcessor::instance(kvstore_, ...)->process(renameReq);
    if (!result.ok()) {
        resp.status = 400;
        resp.body = jsonError(result.status());
        auditLogFailed(renameReq, result.status());
        return;
    }

    resp.status = 200;
    resp.body = jsonSuccess(result.value());
    auditLogSuccess(renameReq, result.value());
}
```

---

## 8. 配置项设计

### 8.1 新增配置项

```cpp
DEFINE_bool(enable_space_rename_rest, false,
            "Enable internal REST API to hard rename graph space without changing space id.");

DEFINE_string(space_rename_admin_token, "",
              "Admin token for hard space rename REST API.");

DEFINE_bool(space_rename_require_expected_space_id, true,
            "Require expected_space_id in hard rename request.");

DEFINE_bool(space_rename_require_dry_run_first, true,
            "Require operator to execute dry-run before actual hard rename.");
```

### 8.2 生产推荐配置

```text
--enable_space_rename_rest=true
--space_rename_admin_token=<strong-token>
--space_rename_require_expected_space_id=true
--space_rename_require_dry_run_first=true
```

### 8.3 默认安全策略

```text
默认不开启 REST rename
默认 dry_run=true
默认必须传 expected_space_id
默认拒绝空 token
默认只允许 Meta leader 执行
```

---

## 9. dry-run 与执行确认

### 9.1 为什么 hard 模式必须强化 dry-run

hard rename 执行后：

```text
old_space 立即不可用
所有仍然 USE old_space 的新请求都会失败
```

因此必须用充分验证替代 compatible/rollback 模式的灰度兜底。

### 9.2 dry-run 必须检查

dry-run 不落盘，但必须完成所有关键校验：

```text
old_name 存在
new_name 不存在
expected_space_id 匹配
SpaceDesc 可读取
当前 Meta 是 leader
名称合法
无明显 DDL/Job 冲突
可构造 puts/removes
```

### 9.3 dry-run 输出

dry-run 响应应包含：

```text
space_id
old_name
new_name
will_put
will_remove
风险提示
建议验证步骤
```

### 9.4 可选：dry-run token

如果想更安全，可以让 dry-run 返回一个短期 token：

```json
{
  "dry_run_token": "sha256(old,new,spaceId,timestamp,secret)"
}
```

实际执行时必须携带：

```json
{
  "dry_run_token": "..."
}
```

第一版可以不实现，但文档建议保留扩展点。

---

## 10. Graph Cache 与会话影响

### 10.1 Graph Meta cache

Graph 服务可能缓存：

```text
space name -> space_id
space_id -> schema
session 当前 space_id
```

hard rename 完成后，存在短时间不一致：

```text
Meta 已经只有 new_space
graphd 可能仍缓存 old_space
graphd 可能暂时不知道 new_space
```

建议操作流程中明确：

```text
执行 hard rename 后，滚动重启所有 graphd，或等待至少两个 Meta heartbeat 周期后再验证。
```

### 10.2 已有 session

已经执行过：

```ngql
USE old_space;
```

的旧 session 可能还持有 `space_id`。底层读写可能继续成功，但它与新的空间名语义不一致。

生产要求：

```text
执行 hard rename 前，业务停止写入并断开旧连接。
执行 hard rename 后，业务使用 new_space 创建新连接。
```

---

## 11. DDL / Job 并发控制

hard rename 期间建议禁止：

```text
CREATE SPACE
DROP SPACE
CLEAR SPACE
CREATE TAG
ALTER TAG
DROP TAG
CREATE EDGE
ALTER EDGE
DROP EDGE
CREATE INDEX
DROP INDEX
REBUILD INDEX
BALANCE
COMPACT
SUBMIT JOB STATS
```

第一版可以采用人工操作规程保证，第二版可增加程序化检查。

建议 Codex 先搜索 3.6 作业管理相关代码：

```bash
grep -R "Job" -n src/meta | head -100
grep -R "JobManager" -n src | head -100
grep -R "ListJobs" -n src | head -100
grep -R "AdminJob" -n src | head -100
```

如果容易获取 running job，可在实际执行前拒绝：

```text
存在 running / queued job 时，拒绝 hard rename
```

---

## 12. 审计日志

每次请求必须打印审计日志。

成功：

```text
[SPACE_HARD_RENAME] result=SUCCEEDED dry_run=false
operator=admin old_name=old_space new_name=new_space space_id=100
remote_ip=10.1.2.3 comment="production workaround"
```

失败：

```text
[SPACE_HARD_RENAME] result=FAILED dry_run=false
operator=admin old_name=old_space new_name=new_space
reason=E_SPACE_ALREADY_EXISTS remote_ip=10.1.2.3
```

不要打印：

```text
admin token
完整请求 header
敏感配置
```

---

## 13. 测试设计

### 13.1 单元测试

新增：

```text
RenameSpaceHardProcessorTest
RenameSpaceHandlerTest
```

核心用例：

| 用例 | 预期 |
|---|---|
| old 存在，new 不存在，dry_run=true | 不修改 KV，返回执行计划 |
| old 存在，new 不存在，dry_run=false | old 映射删除，new 映射存在，`space_id` 不变 |
| old 不存在 | 返回 `E_SPACE_NOT_FOUND` |
| new 已存在 | 返回 `E_SPACE_ALREADY_EXISTS` |
| expected_space_id 不匹配 | 返回 `E_SPACE_ID_MISMATCH` |
| old_name == new_name | 返回 `E_OLD_NEW_NAME_SAME` |
| token 缺失 | 返回 403 |
| REST 开关关闭 | 返回 404 或 disabled |
| 非 leader 执行 | 返回 `E_NOT_META_LEADER` |
| SpaceDesc 读取失败 | 返回内部错误 |
| 原子提交失败 | 返回 `E_STORE_FAILURE` |

### 13.2 元数据一致性测试

执行 hard rename 后必须验证：

```text
getSpaceId(old_space) 失败
getSpaceId(new_space) 成功
getSpaceId(new_space) == 原 old space_id
getSpaceDesc(space_id).name == new_space
partition_num 不变
replica_factor 不变
vid_type 不变
parts 分布不变
tags 不变
edges 不变
indexes 不变
roles/permissions 行为符合预期
```

### 13.3 集成测试流程

#### 1. 创建测试图空间

```ngql
CREATE SPACE old_space(partition_num=3, replica_factor=1, vid_type=FIXED_STRING(32));
```

等待 Meta 同步后：

```ngql
USE old_space;
CREATE TAG person(name string, age int);
CREATE EDGE follow(degree int);
CREATE TAG INDEX person_name_index ON person(name(20));
INSERT VERTEX person(name, age) VALUES "v1":("Tom", 18), "v2":("Bob", 20);
INSERT EDGE follow(degree) VALUES "v1"->"v2":(90);
```

#### 2. 记录 rename 前信息

```ngql
SHOW SPACES;
DESCRIBE SPACE old_space;
SHOW TAGS;
SHOW EDGES;
SHOW CREATE TAG person;
SHOW CREATE EDGE follow;
SHOW INDEXES;
FETCH PROP ON person "v1";
FETCH PROP ON follow "v1"->"v2";
```

记录：

```text
space_id
partition_num
replica_factor
vid_type
tag schema
edge schema
index schema
样例点边查询结果
```

#### 3. dry-run

```bash
curl -X POST http://<metad-leader>:19559/admin/space/rename \
  -H 'Content-Type: application/json' \
  -H 'X-Nebula-Admin-Token: <token>' \
  -d '{
    "old_name": "old_space",
    "new_name": "new_space",
    "dry_run": true,
    "expected_space_id": 100,
    "operator": "test",
    "comment": "dry run hard rename"
  }'
```

预期：

```text
返回 DRY_RUN_SUCCEEDED
Meta 不发生修改
USE old_space 仍成功
USE new_space 失败
```

#### 4. 执行 hard rename

```bash
curl -X POST http://<metad-leader>:19559/admin/space/rename \
  -H 'Content-Type: application/json' \
  -H 'X-Nebula-Admin-Token: <token>' \
  -d '{
    "old_name": "old_space",
    "new_name": "new_space",
    "dry_run": false,
    "expected_space_id": 100,
    "operator": "test",
    "comment": "execute hard rename"
  }'
```

预期：

```text
返回 SUCCEEDED
space_id 保持 100
old_space 映射被删除
new_space 映射被创建
SpaceDesc.name 变为 new_space
```

#### 5. 刷新 graphd cache

建议测试中采用明确动作：

```text
重启 graphd
或等待两个 Meta heartbeat 周期
```

#### 6. 验证新名称可用

```ngql
SHOW SPACES;
USE new_space;
DESCRIBE SPACE new_space;
SHOW TAGS;
SHOW EDGES;
SHOW CREATE TAG person;
SHOW CREATE EDGE follow;
SHOW INDEXES;
FETCH PROP ON person "v1";
FETCH PROP ON follow "v1"->"v2";
```

预期：

```text
所有 Schema 和样例数据可访问
space_id 与 rename 前一致
```

#### 7. 验证旧名称不可用

```ngql
USE old_space;
DESCRIBE SPACE old_space;
```

预期：

```text
返回 space not found
```

### 13.4 读写验证

hard rename 后继续验证写入：

```ngql
USE new_space;
INSERT VERTEX person(name, age) VALUES "v3":("Alice", 22);
INSERT EDGE follow(degree) VALUES "v2"->"v3":(88);
FETCH PROP ON person "v3";
FETCH PROP ON follow "v2"->"v3";
```

预期：

```text
写入和查询均成功
```

### 13.5 统计与索引验证

```ngql
REBUILD TAG INDEX person_name_index;
SHOW INDEX STATUS;
SUBMIT JOB STATS;
SHOW STATS;
LOOKUP ON person WHERE person.name == "Tom" YIELD id(vertex);
```

预期：

```text
索引和统计任务能在 new_space 下正常执行
```

### 13.6 异常场景验证

| 场景 | 操作 | 预期 |
|---|---|---|
| new_space 已存在 | rename old -> existing | 拒绝 |
| expected_space_id 错误 | 传错误 id | 拒绝 |
| old_space 不存在 | rename missing -> new | 拒绝 |
| graphd 未刷新 | rename 后立即 USE new | 可能暂时失败，等待/重启后成功 |
| 旧连接未断开 | 旧 session 继续执行 | 记录行为，生产流程要求断开旧连接 |
| Meta leader 切换 | 非 leader 请求 | 拒绝或提示 leader |

---

## 14. 生产操作流程

### 14.1 执行前强制检查

```text
1. 确认当前版本为 NebulaGraph 3.6 分支。
2. 确认 old_space 当前 space_id。
3. 确认 new_space 不存在。
4. 确认业务已停止写入。
5. 确认业务连接可重连，并已准备切换到 new_space。
6. 确认无 DDL、无 Job、无导入导出任务。
7. 备份 Meta 数据目录或创建可回退快照。
8. 准备反向修复方案。
9. 确认 metad leader 地址。
10. 确认所有 graphd 可滚动重启。
```

### 14.2 执行步骤

第一步，查询 old_space 信息：

```ngql
SHOW SPACES;
DESCRIBE SPACE old_space;
SHOW PARTS;
```

第二步，dry-run：

```bash
curl -X POST http://<metad-leader>:19559/admin/space/rename \
  -H 'Content-Type: application/json' \
  -H 'X-Nebula-Admin-Token: <token>' \
  -d '{
    "old_name": "old_space",
    "new_name": "new_space",
    "dry_run": true,
    "expected_space_id": 100,
    "operator": "ops",
    "comment": "dry run before production hard rename"
  }'
```

第三步，确认 dry-run 输出：

```text
space_id 正确
will_put 正确
will_remove 正确
new_space 不存在
old_space 存在
```

第四步，停止业务写入并断开旧连接。

第五步，执行 hard rename：

```bash
curl -X POST http://<metad-leader>:19559/admin/space/rename \
  -H 'Content-Type: application/json' \
  -H 'X-Nebula-Admin-Token: <token>' \
  -d '{
    "old_name": "old_space",
    "new_name": "new_space",
    "dry_run": false,
    "expected_space_id": 100,
    "operator": "ops",
    "comment": "execute production hard rename"
  }'
```

第六步，刷新 graphd cache：

```text
滚动重启 graphd
或等待至少两个 Meta heartbeat 周期
```

第七步，验证：

```ngql
SHOW SPACES;
USE new_space;
DESCRIBE SPACE new_space;
SHOW TAGS;
SHOW EDGES;
FETCH PROP ON <tag> <vid>;
```

第八步，业务改用 new_space 重连。

第九步，观察：

```text
graphd 日志
metad 日志
storaged 日志
查询成功率
错误码
慢查询
业务请求量
```

### 14.3 回退预案

由于本方案不提供 rollback 模式，回退依赖以下两种方式：

#### 方案 A：反向 hard rename

如果执行后发现业务配置无法立即切换，但 Meta 状态正常，可以反向执行：

```text
new_space hard rename to old_space
space_id 仍保持 100
```

请求：

```bash
curl -X POST http://<metad-leader>:19559/admin/space/rename \
  -H 'Content-Type: application/json' \
  -H 'X-Nebula-Admin-Token: <token>' \
  -d '{
    "old_name": "new_space",
    "new_name": "old_space",
    "dry_run": false,
    "expected_space_id": 100,
    "operator": "ops",
    "comment": "reverse hard rename after failed validation"
  }'
```

前提：

```text
old_space 当前不存在
new_space 当前存在
space_id 匹配
```

#### 方案 B：恢复 Meta 快照

如果 Meta 状态异常，使用执行前准备的 Meta 快照恢复。

要求：

```text
恢复前停止集群或至少停止相关服务
严格按照内部恢复流程执行
恢复后验证 old_space 可用
```

---

## 15. Codex 开发指导

### 15.1 给 Codex 的任务描述

可以直接复制给 Codex：

```text
我使用的是 NebulaGraph 3.6 代码分支。请帮我实现一个内部 REST 接口，用于 hard rename 图空间名称，但保持 space_id 不变。

要求：
1. 不新增 nGQL 语法。
2. 不修改 Storage 数据。
3. 不支持 compatible/finalize/rollback 模式，只支持 hard rename。
4. 在 nebula-metad 暴露 POST /admin/space/rename。
5. 请求 JSON 包括 old_name、new_name、dry_run、expected_space_id、operator、comment。
6. dry_run 默认 true。
7. expected_space_id 必须传入，并必须与 old_name 当前 space_id 一致。
8. 执行时：新增 new_name -> old space_id，修改 SpaceDesc.name=new_name，删除 old_name -> old space_id。
9. 必须通过 Meta leader/Raft 原子写入，不能直接改 RocksDB。
10. 默认配置关闭，需要 enable_space_rename_rest 开关。
11. 需要 token 鉴权，header 为 X-Nebula-Admin-Token。
12. 增加审计日志。
13. 增加单元测试，覆盖成功、dry-run、old 不存在、new 已存在、space_id 不匹配、old/new 相同、token 缺失、非 leader 等场景。
14. 输出修改文件列表、核心逻辑说明、测试结果和风险说明。

请先阅读 3.6 代码中的 CreateSpaceProcessor、DropSpaceProcessor、MetaKeyUtils、BaseProcessor、HTTP handler 注册逻辑，再给出实现方案，确认后再修改代码。
```

### 15.2 Codex 第一步搜索命令

```bash
grep -R "CreateSpaceProcessor" -n src | head -50
grep -R "DropSpaceProcessor" -n src | head -50
grep -R "spaceNameKey" -n src | head -50
grep -R "spaceKey" -n src | head -50
grep -R "MetaKeyUtils" -n src | head -50
grep -R "doSync" -n src/meta | head -100
grep -R "BaseProcessor" -n src/meta | head -50
grep -R "registerHandler" -n src | head -100
grep -R "HttpHandler" -n src | head -100
grep -R "WebService" -n src | head -100
grep -R "DEFINE_bool" -n src/meta | head -50
grep -R "SHOW META LEADER" -n src | head -50
```

### 15.3 Codex 第二步阅读重点

```text
CreateSpaceProcessor：
- space name key 如何生成
- SpaceDesc 如何编码
- part 信息如何创建
- 已存在空间如何判断

DropSpaceProcessor：
- 删除空间时删除了哪些 key
- 是否有 job/schema/index/role 等相关清理逻辑
- 哪些逻辑不能复用到 rename

MetaKeyUtils：
- spaceNameKey
- spaceKey
- spaceVal
- parse space id / name 的方法

BaseProcessor：
- 如何获取 kvstore
- 如何通过 Raft 提交
- 如何处理 leader/follower
- 如何返回 ErrorCode

HTTP Handler：
- metad 当前如何注册 HTTP endpoint
- 当前 HTTP 端口配置
- JSON 解析库
- 响应格式
```

### 15.4 建议开发拆分

```text
提交 1：新增 RenameSpaceHardProcessor，并写单元测试。
提交 2：新增 REST Handler，只支持 dry-run。
提交 3：支持 dry_run=false 实际写入。
提交 4：增加配置项和 token 鉴权。
提交 5：补充集成验证脚本和文档。
```

### 15.5 编译建议

结合你的本地开发习惯，建议 Codex 使用 Debug 编译：

```bash
mkdir -p build
cd build

cmake \
  -DCMAKE_INSTALL_PREFIX=/home/sch/fjh/nebula-debug \
  -DNEBULA_THIRDPARTY_ROOT=/home/sch/sch/3.3/ \
  -DCMAKE_C_COMPILER=/usr/bin/gcc \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
  -DENABLE_TESTING=ON \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-std=c++17" \
  -DENABLE_WERROR=OFF \
  ..

make -j$(nproc) nebula-metad
```

如果你的 3.6 工程目录是：

```text
/home/sch/sch/sch_nebula_graph_1
```

则 Codex 应在该目录下操作，不要使用其他工程目录。

---

## 16. 风险清单与规避

| 风险 | 说明 | 规避 |
|---|---|---|
| old_space 立即不可用 | hard rename 删除旧名称映射 | 执行前停写、切配置、断旧连接 |
| Graph cache 不刷新 | graphd 可能暂时不知道 new_space | 重启 graphd 或等待心跳 |
| Meta 写一半 | 只改一个 key 会不一致 | 必须同一 Raft 原子提交 |
| new_name 冲突 | 新名称已有空间 | 执行前和代码中双重校验 |
| space_id 误操作 | old_name 指向非预期空间 | 强制 expected_space_id |
| DDL/Job 并发 | rename 与 DDL/Job 同时执行 | 低峰停写，检查 Job |
| 权限展示异常 | 角色/权限可能与 space_id/name 展示有关 | 测试 SHOW ROLES、用户访问 |
| 回退失败 | 不内置 rollback | 准备反向 hard rename 和 Meta 快照 |

---

## 17. 最小可交付版本

第一版必须完成：

```text
POST /admin/space/rename
只支持 hard rename
dry_run=true
dry_run=false
expected_space_id 必填
token 鉴权
默认关闭开关
Meta leader 校验
原子修改 Meta KV
审计日志
Processor 单元测试
REST Handler 基本测试
生产验证脚本
```

第一版不做：

```text
compatible
finalize
rollback
alias
主动刷新 graphd cache
自动检查所有 running jobs
复杂权限迁移
图形化管理入口
```

---

## 18. 最终建议

按你当前要求，方案收敛为：

```text
只开发 hard 模式。
接口执行后 old_space 不再可用。
new_space 复用原 space_id。
Storage 数据完全不动。
通过 dry-run、expected_space_id、停写、Meta 备份、graphd 重启、全量验证保证正确性。
```

生产执行前必须完成：

```text
1. 测试集群完整验证。
2. 生产 Meta 快照。
3. 停止写入。
4. dry-run 成功。
5. 执行 hard rename。
6. 重启/刷新 graphd。
7. 新名称全量验证。
8. 业务使用 new_space 重连。
```
