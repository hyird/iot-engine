# 权限模型

## 设计目标

系统是现有 `iot-manager` 的重写。授权不能只依赖平台角色，还必须控制用户对每一
台设备的访问权限。

授权由两层组成：

1. **RBAC**：决定用户在平台层面可以使用哪些功能；
2. **设备 ACL**：决定用户可以访问哪些具体设备，以及是只读还是可编辑。

一次请求必须同时满足 RBAC 和设备 ACL，不能因为拥有普通平台角色而绕过设备
所有权。

## 设备角色

### 创建者 `owner`

创建设备的用户自动成为创建者。创建者标识不可通过普通业务接口修改。

创建者拥有：

- 查看设备；
- 查看遥测和查询响应；
- 执行设备查询命令；
- 编辑设备信息、连接配置和采集策略；
- 分享或撤销其他用户的设备权限；
- 删除设备。

### 只读共享 `viewer`

只读用户拥有：

- 查看设备基础信息；
- 查看设备状态；
- 查询历史遥测和查询响应。

只读用户不能：

- 编辑设备；
- 执行会改变设备状态的命令；
- 分享设备；
- 删除设备。

### 编辑共享 `editor`

编辑用户继承只读权限，并可以：

- 编辑允许共享编辑的设备资料；
- 修改采集和查询计划；
- 执行被策略允许的设备查询或控制命令。

编辑用户不能：

- 删除设备；
- 分享或撤销权限；
- 修改创建者；
- 修改设备所有权；
- 授予自己或他人任何权限。

## 权限矩阵

| 操作 | 创建者 | 只读共享 | 编辑共享 |
|---|---:|---:|---:|
| 查看设备 | 是 | 是 | 是 |
| 查看遥测 | 是 | 是 | 是 |
| 查看查询响应 | 是 | 是 | 是 |
| 执行只读查询 | 是 | 否 | 是 |
| 执行控制命令 | 是 | 否 | 受策略限制 |
| 编辑设备 | 是 | 否 | 是 |
| 修改连接密钥 | 是 | 否 | 否 |
| 分享/撤销权限 | 是 | 否 | 否 |
| 删除设备 | 是 | 否 | 否 |
| 修改创建者 | 否 | 否 | 否 |

连接密码、令牌、证书私钥等敏感字段仅创建者可更新，读取接口永不返回明文。

## 分享规则

- 只有 `devices.created_by` 对应的创建者可以新增、修改或撤销分享；
- 分享对象可以是状态正常的用户或有效部门；
- 一次分享操作可以同时包含多个用户和多个部门；
- 用户共享不能以设备创建者本人为目标；
- 每个设备与同一用户或同一部门最多存在一条有效授权；
- 分享权限只能是 `viewer` 或 `editor`；
- 被分享者不能进行二次分享；
- 部门共享对该部门当前所有有效成员生效；
- 用户离开部门后立即失去仅由该部门带来的设备权限；
- 删除用户或部门时应撤销对应的设备授权；
- 删除设备前由数据库外键级联删除分享记录；
- 分享、修改、撤销和拒绝访问都应写入安全审计日志。

混合批量分享必须使用一个数据库事务。任意用户、部门或权限值无效时全部回滚。
重复目标应在请求校验阶段拒绝，已经存在的授权可按明确的 upsert 语义更新。

## 分享界面

设备分享弹窗同时提供：

- 用户多选；
- 部门树多选；
- 为每个目标设置 `viewer` 或 `editor`；
- 已分享目标列表，统一显示用户/部门图标、名称、权限和授权时间；
- 创建者可修改或撤销任意目标，被分享者看不到管理操作。

选择部门只授权该部门自身的当前成员，默认不递归包含子部门。若以后需要包含
子部门，应作为显式策略字段，而不是隐式改变部门分享语义。

用户可能同时命中个人共享和一个或多个部门共享。有效权限按以下优先级合并：

```text
owner > editor > viewer > none
```

例如，部门授权为 `viewer`、个人授权为 `editor` 时，最终权限为 `editor`。部门
共享不会赋予删除、分享或修改创建者的权限。

## RBAC 与 ACL 的组合

推荐判定顺序：

```text
用户身份有效？
  -> RBAC 允许使用该类 API？
  -> 用户是设备创建者？是：按 owner 权限处理
  -> 加载个人共享和当前部门共享
  -> 合并得到有效设备权限
  -> 有效权限满足本次操作？否：拒绝
  -> 命令策略允许？否：拒绝
  -> 执行业务操作
```

平台管理员默认不能以业务身份静默取得任意设备访问权。若保留超级管理员能力，
应作为单独的 `break-glass` 运维流程：要求填写原因、强制审计，并在界面中明确
显示，不能复用普通设备分享逻辑。

## 数据库模型

### devices 扩展

```sql
ALTER TABLE devices
    ADD COLUMN created_by UUID NOT NULL REFERENCES users(id),
    ADD COLUMN created_at TIMESTAMPTZ NOT NULL DEFAULT now();

CREATE INDEX devices_created_by_idx ON devices(created_by);
```

`created_by` 创建后不可由业务 API 更新。数据库迁移应增加触发器或收紧数据库
角色权限，防止普通写入路径修改该字段。

### device_acl

```sql
CREATE TABLE device_acl (
    device_id   TEXT NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    subject_type TEXT NOT NULL CHECK (subject_type IN ('user', 'department')),
    subject_id  UUID NOT NULL,
    access      TEXT NOT NULL CHECK (access IN ('viewer', 'editor')),
    granted_by  UUID NOT NULL REFERENCES users(id),
    policy      JSONB NOT NULL DEFAULT '{}',
    granted_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (device_id, subject_type, subject_id)
);

CREATE INDEX device_acl_subject_idx
    ON device_acl (subject_type, subject_id, device_id);
```

写入 `device_acl` 时必须在同一事务内验证：

```text
devices.created_by = 当前用户
device_acl.granted_by = devices.created_by
subject_type=user       -> subject_id 对应有效用户且不是创建者
subject_type=department -> subject_id 对应有效部门
```

该验证不能只依赖前端隐藏按钮，必须由后端服务执行。建议进一步使用 PostgreSQL
触发器保证多态目标存在性和数据库级不变量。

`policy` 使用 JSONB 保存编辑用户允许执行的设备操作、拒绝操作和授权到期时间，
但 `access` 本身仍使用普通列，以便权限过滤和数据库约束。

### security_audit_log

```sql
CREATE TABLE security_audit_log (
    id            UUID PRIMARY KEY,
    actor_user_id UUID REFERENCES users(id),
    action        TEXT NOT NULL,
    resource_type TEXT NOT NULL,
    resource_id   TEXT NOT NULL,
    outcome       TEXT NOT NULL,
    reason        TEXT,
    details       JSONB NOT NULL DEFAULT '{}',
    occurred_at   TIMESTAMPTZ NOT NULL DEFAULT now()
);
```

## Redis 消息中的身份上下文

北向层完成授权后，写入 `iot:commands` 的命令必须包含：

```json
{
  "requested_by": "user-uuid",
  "authorized_as": "owner",
  "authorization_version": 1
}
```

南向层不负责重新实现完整 ACL，但必须保留该身份上下文并将其复制到响应和审计
事件中。对于可能改变设备状态的命令，命令消费者应验证签名或可信生产者标识，
避免其他 Redis 客户端伪造已授权命令。

## 缓存与撤权

可以在 Redis 中缓存设备权限，但 PostgreSQL 是权限事实来源。缓存键必须带权限
版本或较短 TTL。撤销分享、用户调换部门或部门删除时，应在数据库事务提交后立即
删除相关用户的设备权限缓存，避免组织关系变化后继续访问。
