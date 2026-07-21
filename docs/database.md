# PostgreSQL / TimescaleDB 数据模型

## 当前主键与迁移规则

- 所有业务表主键和外键统一使用 PostgreSQL 原生 `UUID`。
- UUID 由应用内线程安全、单调递增的 UUIDv7 生成器创建，不依赖特定 PostgreSQL
  版本的 `uuidv7()` 函数。
- 不保留旧整数 ID、双写列、映射表或兼容迁移；全新部署只执行
  `0001_initial_schema`。
- UUIDv7 可直接用于稳定排序；需要游标分页的接口使用
  `WHERE id < $1::uuid ORDER BY id DESC LIMIT $2`，现有页码接口仍可按 UUIDv7 排序。
- 所有时间字段使用 `TIMESTAMPTZ`，数据库和连接会话时区固定为 UTC。
- 初始迁移只创建运行必需的超级管理员角色、admin 用户及二者关系，不生成演示业务数据。

## JSONB 优先原则

业务数据采用 JSONB 优先设计，以适应不同厂商、协议和型号的设备结构。

适合使用普通列的数据：

- 主键、外键和创建者；
- 设备 ID、用户 ID、事件 ID和命令 ID；
- 时间戳；
- 权限级别、处理状态和消息类型；
- 经常用于过滤、关联、唯一约束和分区的字段。

适合使用 JSONB 的数据：

- 设备元数据和能力描述；
- 协议连接配置；
- 遥测指标和标签；
- 查询命令参数；
- 查询响应和设备错误；
- 审计详情和扩展字段。

不把所有内容塞入单个 JSONB。所有权、ACL、时间分区键等数据仍使用强类型普通
列，以保留数据库约束和可预测的查询性能。

## users

用户身份核心字段使用普通列，个人资料和偏好使用 JSONB：

```sql
CREATE TABLE users (
    id             UUID PRIMARY KEY,
    username       TEXT NOT NULL UNIQUE,
    password_hash  TEXT NOT NULL,
    status         TEXT NOT NULL DEFAULT 'active',
    profile        JSONB NOT NULL DEFAULT '{}',
    preferences    JSONB NOT NULL DEFAULT '{}',
    created_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at     TIMESTAMPTZ NOT NULL DEFAULT now()
);
```

## devices

```sql
CREATE TABLE devices (
    id             TEXT PRIMARY KEY,
    created_by     UUID NOT NULL REFERENCES users(id),
    name           TEXT NOT NULL,
    mode           TEXT NOT NULL,
    protocol       TEXT NOT NULL,
    enabled        BOOLEAN NOT NULL DEFAULT TRUE,
    connection     JSONB NOT NULL DEFAULT '{}',
    capabilities   JSONB NOT NULL DEFAULT '{}',
    collection     JSONB NOT NULL DEFAULT '{}',
    metadata       JSONB NOT NULL DEFAULT '{}',
    last_state     JSONB NOT NULL DEFAULT '{}',
    last_seen_at   TIMESTAMPTZ,
    created_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    CHECK (mode IN ('push', 'query_response', 'hybrid'))
);

CREATE INDEX devices_created_by_idx ON devices(created_by);
CREATE INDEX devices_metadata_gin_idx ON devices USING GIN (metadata jsonb_path_ops);
CREATE INDEX devices_capabilities_gin_idx ON devices USING GIN (capabilities jsonb_path_ops);
```

示例：

```json
{
  "connection": {
    "host": "192.168.1.20",
    "port": 502,
    "unit_id": 1
  },
  "capabilities": {
    "metrics": ["voltage", "current", "power"],
    "operations": ["read_metrics", "reset_energy"]
  },
  "collection": {
    "interval_seconds": 30,
    "timeout_ms": 3000,
    "retry": {"maximum": 3, "backoff_ms": 500}
  }
}
```

密码、令牌和证书私钥不能以明文保存到 `connection`。JSONB 中仅保存密钥引用，
实际秘密由密钥存储管理。

## telemetry_events

每一条 Redis 遥测或成功查询响应对应一条事件记录，不再把每个指标拆成独立行。
完整指标集合保存在 `data` JSONB 中：

```sql
CREATE TABLE telemetry_events (
    observed_at  TIMESTAMPTZ NOT NULL,
    event_id     UUID NOT NULL,
    device_id    TEXT NOT NULL,
    source       TEXT NOT NULL,
    command_id   UUID,
    data         JSONB NOT NULL,
    metadata     JSONB NOT NULL DEFAULT '{}',
    ingested_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (observed_at, event_id),
    CHECK (source IN ('push', 'query_response')),
    CHECK (jsonb_typeof(data) = 'object')
);

SELECT create_hypertable(
    'telemetry_events',
    'observed_at',
    if_not_exists => TRUE
);

CREATE INDEX telemetry_device_time_idx
    ON telemetry_events (device_id, observed_at DESC);

CREATE INDEX telemetry_data_gin_idx
    ON telemetry_events USING GIN (data jsonb_path_ops);
```

示例 `data`：

```json
{
  "metrics": {
    "temperature": 24.6,
    "humidity": 61.2
  },
  "attributes": {
    "quality": "good",
    "site": "taipei-1"
  }
}
```

常用指标可以建立表达式索引，无需改变基础表结构：

```sql
CREATE INDEX telemetry_temperature_idx
    ON telemetry_events (((data #>> '{metrics,temperature}')::double precision))
    WHERE data #> '{metrics}' ? 'temperature';
```

查询单个指标：

```sql
SELECT
    observed_at,
    (data #>> '{metrics,temperature}')::double precision AS value
FROM telemetry_events
WHERE device_id = $1
  AND observed_at >= $2
  AND data #> '{metrics}' ? 'temperature'
ORDER BY observed_at;
```

如果以后某个指标的查询量非常大，可以为它增加连续聚合或派生窄表；基础事件仍以
JSONB 保存，不丢失设备原始结构。

## device_commands

```sql
CREATE TABLE device_commands (
    command_id      UUID PRIMARY KEY,
    correlation_id  UUID NOT NULL UNIQUE,
    device_id       TEXT NOT NULL REFERENCES devices(id),
    requested_by    UUID NOT NULL REFERENCES users(id),
    operation       TEXT NOT NULL,
    status          TEXT NOT NULL,
    request         JSONB NOT NULL DEFAULT '{}',
    execution       JSONB NOT NULL DEFAULT '{}',
    requested_at    TIMESTAMPTZ NOT NULL,
    dispatched_at   TIMESTAMPTZ,
    completed_at    TIMESTAMPTZ
);

CREATE INDEX device_commands_request_gin_idx
    ON device_commands USING GIN (request jsonb_path_ops);
```

`request` 保存命令参数，`execution` 保存重试、协议路由、耗时和诊断信息。

## device_command_responses

```sql
CREATE TABLE device_command_responses (
    event_id        UUID PRIMARY KEY,
    command_id      UUID NOT NULL REFERENCES device_commands(command_id),
    correlation_id  UUID NOT NULL,
    device_id       TEXT NOT NULL REFERENCES devices(id),
    status          TEXT NOT NULL,
    response        JSONB NOT NULL DEFAULT '{}',
    error           JSONB,
    responded_at    TIMESTAMPTZ NOT NULL,
    ingested_at     TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX command_responses_response_gin_idx
    ON device_command_responses USING GIN (response jsonb_path_ops);
```

成功时将设备原始响应和标准化结果保存到 `response`；失败或超时时将结构化错误
保存到 `error`。

## device_acl

ACL 是强关系数据，不使用单个 JSONB 数组保存，否则难以可靠地建立唯一约束、
外键和并发更新规则：

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

`subject_type=user` 表示个人共享，`subject_type=department` 表示部门共享。由于目标
是多态引用，普通外键无法根据类型引用不同表；迁移应使用约束触发器验证
`subject_id` 对应有效用户或部门。

用户同时命中个人和部门授权时，查询按 `editor > viewer` 合并有效权限。

`policy` 可限制编辑用户可执行的操作：

```json
{
  "allowed_operations": ["read_metrics"],
  "denied_operations": ["factory_reset"],
  "expires_at": "2026-12-31T16:00:00Z"
}
```

## security_audit_log

```sql
CREATE TABLE security_audit_log (
    id             UUID PRIMARY KEY,
    actor_user_id  UUID REFERENCES users(id),
    action         TEXT NOT NULL,
    resource_type  TEXT NOT NULL,
    resource_id    TEXT NOT NULL,
    outcome        TEXT NOT NULL,
    context        JSONB NOT NULL DEFAULT '{}',
    occurred_at    TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX security_audit_context_gin_idx
    ON security_audit_log USING GIN (context jsonb_path_ops);
```

## JSONB 版本管理

Redis Envelope 已包含 `schema_version`。写入数据库时建议将业务版本保存在 JSONB
内部或 `metadata` 中。后端读取历史数据时按版本转换，避免直接原地重写大量
TimescaleDB 历史记录。

生产迁移还应加入数据保留、压缩/列存储和连续聚合策略，具体周期根据数据量与
查询范围确定。

设备创建者不变量和安全审计规则见 [权限模型](authorization.md)。
