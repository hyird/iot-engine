#pragma once

#include <array>
#include <ruvia/web/db/DbMigration.h>
#include <utility>

namespace service::config {

class SchemaMigration final {
  public:
    SchemaMigration(std::string id, std::string sql)
        : id_(std::move(id)), sql_(std::move(sql)) {}

    [[nodiscard]] std::string_view id() const noexcept { return id_; }
    [[nodiscard]] std::string_view sql() const noexcept { return sql_; }

    operator ruvia::DbMigration() const {
        return ruvia::DbMigration(ruvia::DbMigrationOptions{
            .id = id_,
            .sql = sql_,
        });
    }

  private:
    std::string id_;
    std::string sql_;
};

inline const std::array<SchemaMigration, 33> kSchemaMigrations{{
    {"0000_unified_link_boundary", R"sql(
DO $schema$
BEGIN
IF NOT EXISTS (SELECT 1 FROM sys_schema_migrations) THEN
    RETURN;
END IF;

IF EXISTS (
    SELECT 1
    FROM information_schema.columns
    WHERE table_schema = current_schema()
      AND table_name = 'link'
      AND column_name = 'execution'
) AND NOT EXISTS (
    SELECT 1
    FROM information_schema.columns
    WHERE table_schema = current_schema()
      AND table_name = 'device'
      AND column_name = 'edge_node_id'
) THEN
    RETURN;
END IF;

IF NOT EXISTS (
    SELECT 1
    FROM information_schema.columns
    WHERE table_schema = current_schema()
      AND table_name = 'link'
      AND column_name = 'usage'
) OR NOT EXISTS (
    SELECT 1
    FROM information_schema.columns
    WHERE table_schema = current_schema()
      AND table_name = 'device'
      AND column_name = 'edge_node_id'
) OR NOT EXISTS (
    SELECT 1
    FROM information_schema.columns
    WHERE table_schema = current_schema()
      AND table_name = 'device'
      AND column_name = 'edge_endpoint'
) THEN
    RAISE EXCEPTION
        'this iot-engine schema is neither the supported legacy layout nor the unified layout';
END IF;

ALTER TABLE link
    ADD COLUMN execution VARCHAR(16),
    ADD COLUMN edge_node_id UUID;

ALTER TABLE link DROP CONSTRAINT IF EXISTS ck_link_endpoint;

UPDATE link
SET execution = 'collector',
    endpoint = jsonb_set(endpoint, '{transport}', '"tcp"'::jsonb, true);

CREATE TEMP TABLE iot_edge_link_migration ON COMMIT DROP AS
SELECT id AS device_id, gen_random_uuid() AS link_id
FROM device
WHERE edge_node_id IS NOT NULL AND link_id IS NULL;

INSERT INTO link(
    id, name, protocol, endpoint, execution, edge_node_id, status,
    created_by, created_at, updated_at, deleted_at)
SELECT migration.link_id,
       left(device.name, 70) || ' [edge ' || left(device.id::text, 8) || ']',
       protocol_config.protocol,
       device.edge_endpoint,
       'edge',
       device.edge_node_id,
       device.status,
       device.created_by,
       device.created_at,
       device.updated_at,
       device.deleted_at
FROM iot_edge_link_migration AS migration
JOIN device ON device.id = migration.device_id
JOIN protocol_config ON protocol_config.id = device.protocol_config_id;

ALTER TABLE device DROP CONSTRAINT IF EXISTS ck_device_connection_source;

UPDATE device
SET link_id = migration.link_id
FROM iot_edge_link_migration AS migration
WHERE device.id = migration.device_id;

UPDATE device_data
SET link_id = device.link_id
FROM device
WHERE device.id = device_data.device_id AND device_data.link_id IS NULL;

IF EXISTS (SELECT 1 FROM device WHERE link_id IS NULL) OR
   EXISTS (SELECT 1 FROM device_data WHERE link_id IS NULL) THEN
    RAISE EXCEPTION 'legacy device linkage could not be migrated completely';
END IF;

ALTER TABLE link
    ALTER COLUMN execution SET NOT NULL,
    ADD CONSTRAINT fk_link_edge_node
        FOREIGN KEY (edge_node_id) REFERENCES edge_node(id) ON DELETE RESTRICT;

ALTER TABLE link ADD CONSTRAINT ck_link_endpoint CHECK (
    (
        execution = 'collector'
        AND edge_node_id IS NULL
        AND endpoint->>'transport' = 'tcp'
        AND endpoint->>'mode' IN ('TCP Server', 'TCP Client')
        AND COALESCE(jsonb_typeof(endpoint->'targets'), 'array') = 'array'
        AND (
            (
                endpoint->>'mode' = 'TCP Server'
                AND endpoint->>'ip' = '0.0.0.0'
                AND COALESCE(endpoint->>'port', '') ~ '^[0-9]+$'
                AND (endpoint->>'port')::integer BETWEEN 1 AND 65535
                AND jsonb_array_length(COALESCE(endpoint->'targets', '[]'::jsonb)) = 0
            )
            OR
            (
                endpoint->>'mode' = 'TCP Client'
                AND COALESCE(endpoint->>'ip', '') = ''
                AND COALESCE((endpoint->>'port')::integer, 0) = 0
                AND jsonb_array_length(COALESCE(endpoint->'targets', '[]'::jsonb)) > 0
            )
        )
    )
    OR (
        execution = 'edge'
        AND edge_node_id IS NOT NULL
        AND COALESCE(endpoint->>'interface', '') <> ''
        AND (
            endpoint->>'transport' = 'serial'
            OR (
                endpoint->>'transport' = 'tcp'
                AND endpoint->>'mode' IN ('TCP Server', 'TCP Client')
                AND COALESCE(endpoint->>'ip', '') <> ''
                AND COALESCE(endpoint->>'port', '') ~ '^[0-9]+$'
                AND (endpoint->>'port')::integer BETWEEN 1 AND 65535
            )
        )
    )
);

DROP INDEX IF EXISTS idx_link_usage;
CREATE INDEX idx_link_execution ON link(execution) WHERE deleted_at IS NULL;
CREATE INDEX idx_link_edge_node ON link(edge_node_id)
    WHERE execution = 'edge' AND deleted_at IS NULL;

DROP INDEX IF EXISTS idx_device_edge_node;
ALTER TABLE device
    ALTER COLUMN link_id SET NOT NULL,
    DROP COLUMN edge_node_id,
    DROP COLUMN edge_endpoint;
ALTER TABLE device_data ALTER COLUMN link_id SET NOT NULL;

ALTER TABLE link
    DROP COLUMN usage,
    DROP COLUMN agent_id,
    DROP COLUMN agent_interface,
    DROP COLUMN agent_bind_ip,
    DROP COLUMN agent_prefix_length,
    DROP COLUMN agent_gateway;
END
$schema$;
)sql"},
    {"0001_initial_schema", R"sql(
DO $schema$
BEGIN
CREATE EXTENSION IF NOT EXISTS timescaledb;

CREATE TYPE status_enum AS ENUM ('enabled', 'disabled');

CREATE TABLE sys_role (
    id          UUID PRIMARY KEY,
    name        VARCHAR(50) NOT NULL,
    code        VARCHAR(50) NOT NULL,
    description VARCHAR(255),
    status      status_enum NOT NULL DEFAULT 'enabled',
    sort_order  INTEGER NOT NULL DEFAULT 0,
    permissions JSONB NOT NULL DEFAULT '[]'::jsonb
                CHECK (jsonb_typeof(permissions) = 'array'),
    created_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    deleted_at  TIMESTAMPTZ
);
CREATE UNIQUE INDEX idx_sys_role_code_active ON sys_role(code) WHERE deleted_at IS NULL;
CREATE INDEX idx_sys_role_deleted ON sys_role(deleted_at);
CREATE INDEX idx_sys_role_status_deleted ON sys_role(status, deleted_at);

CREATE TABLE sys_department (
    id          UUID PRIMARY KEY,
    name        VARCHAR(100) NOT NULL,
    code        VARCHAR(50),
    parent_id   UUID REFERENCES sys_department(id) ON DELETE RESTRICT,
    leader_id   UUID,
    phone       VARCHAR(20),
    email       VARCHAR(100),
    status      status_enum NOT NULL DEFAULT 'enabled',
    sort_order  INTEGER NOT NULL DEFAULT 0,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    deleted_at  TIMESTAMPTZ
);
CREATE INDEX idx_sys_department_parent ON sys_department(parent_id);
CREATE INDEX idx_sys_department_deleted ON sys_department(deleted_at);
CREATE UNIQUE INDEX idx_sys_department_code ON sys_department(code)
    WHERE code IS NOT NULL AND deleted_at IS NULL;

CREATE TABLE sys_user (
    id            UUID PRIMARY KEY,
    username      VARCHAR(50) NOT NULL,
    password_hash VARCHAR(255) NOT NULL,
    nickname      VARCHAR(100),
    email         VARCHAR(100),
    phone         VARCHAR(20),
    avatar        VARCHAR(255),
    status        status_enum NOT NULL DEFAULT 'enabled',
    department_id UUID REFERENCES sys_department(id) ON DELETE SET NULL,
    profile       JSONB NOT NULL DEFAULT '{}'::jsonb
                  CHECK (jsonb_typeof(profile) = 'object'),
    created_at    TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at    TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    deleted_at    TIMESTAMPTZ
);
CREATE UNIQUE INDEX idx_sys_user_username_active ON sys_user(username)
    WHERE deleted_at IS NULL;
CREATE INDEX idx_sys_user_deleted ON sys_user(deleted_at);
CREATE INDEX idx_sys_user_department ON sys_user(department_id) WHERE deleted_at IS NULL;
ALTER TABLE sys_department ADD CONSTRAINT fk_sys_department_leader
    FOREIGN KEY (leader_id) REFERENCES sys_user(id) ON DELETE SET NULL;

CREATE TABLE sys_user_role (
    id         UUID PRIMARY KEY,
    user_id    UUID NOT NULL REFERENCES sys_user(id) ON DELETE CASCADE,
    role_id    UUID NOT NULL REFERENCES sys_role(id) ON DELETE CASCADE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    UNIQUE (user_id, role_id)
);
CREATE INDEX idx_sys_user_role_user ON sys_user_role(user_id);
CREATE INDEX idx_sys_user_role_role ON sys_user_role(role_id);

CREATE TABLE link (
    id           UUID PRIMARY KEY,
    name         VARCHAR(100) NOT NULL,
    protocol     VARCHAR(20) NOT NULL CHECK (protocol IN ('SL651', 'Modbus', 'S7')),
    endpoint     JSONB NOT NULL CHECK (jsonb_typeof(endpoint) = 'object'),
    execution    VARCHAR(16) NOT NULL CHECK (execution IN ('collector', 'edge')),
    edge_node_id UUID,
    status       status_enum NOT NULL DEFAULT 'enabled',
    created_by   UUID NOT NULL REFERENCES sys_user(id) ON DELETE RESTRICT,
    created_at   TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at   TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    deleted_at   TIMESTAMPTZ,
    CONSTRAINT ck_link_endpoint CHECK (
        (
            execution = 'collector'
            AND edge_node_id IS NULL
            AND endpoint->>'transport' = 'tcp'
            AND endpoint->>'mode' IN ('TCP Server', 'TCP Client')
            AND COALESCE(jsonb_typeof(endpoint->'targets'), 'array') = 'array'
            AND (
                (
                    endpoint->>'mode' = 'TCP Server'
                    AND endpoint->>'ip' = '0.0.0.0'
                    AND COALESCE(endpoint->>'port', '') ~ '^[0-9]+$'
                    AND (endpoint->>'port')::integer BETWEEN 1 AND 65535
                    AND jsonb_array_length(COALESCE(endpoint->'targets', '[]'::jsonb)) = 0
                )
                OR
                (
                    endpoint->>'mode' = 'TCP Client'
                    AND COALESCE(endpoint->>'ip', '') = ''
                    AND COALESCE((endpoint->>'port')::integer, 0) = 0
                    AND jsonb_array_length(COALESCE(endpoint->'targets', '[]'::jsonb)) > 0
                )
            )
        )
        OR (
            execution = 'edge'
            AND edge_node_id IS NOT NULL
            AND COALESCE(endpoint->>'interface', '') <> ''
            AND (
                endpoint->>'transport' = 'serial'
                OR (
                    endpoint->>'transport' = 'tcp'
                    AND endpoint->>'mode' IN ('TCP Server', 'TCP Client')
                    AND COALESCE(endpoint->>'ip', '') <> ''
                    AND COALESCE(endpoint->>'port', '') ~ '^[0-9]+$'
                    AND (endpoint->>'port')::integer BETWEEN 1 AND 65535
                )
            )
        )
    )
);
CREATE INDEX idx_link_deleted ON link(deleted_at);
CREATE UNIQUE INDEX idx_link_name ON link(name) WHERE deleted_at IS NULL;
CREATE INDEX idx_link_status_active ON link(status) WHERE deleted_at IS NULL;
CREATE INDEX idx_link_protocol ON link(protocol);
CREATE INDEX idx_link_execution ON link(execution) WHERE deleted_at IS NULL;
CREATE INDEX idx_link_endpoint_mode ON link((endpoint->>'mode')) WHERE deleted_at IS NULL;
CREATE INDEX idx_link_endpoint_gin ON link USING GIN(endpoint);

CREATE TABLE protocol_config (
    id          UUID PRIMARY KEY,
    protocol    VARCHAR(20) NOT NULL CHECK (protocol IN ('SL651', 'Modbus', 'S7')),
    name        VARCHAR(64) NOT NULL,
    enabled     BOOLEAN NOT NULL DEFAULT TRUE,
    config      JSONB NOT NULL DEFAULT '{}'::jsonb CHECK (jsonb_typeof(config) = 'object'),
    remark      TEXT,
    created_by  UUID NOT NULL REFERENCES sys_user(id) ON DELETE RESTRICT,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    deleted_at  TIMESTAMPTZ
);
CREATE INDEX idx_protocol_config_protocol ON protocol_config(protocol);
CREATE INDEX idx_protocol_config_deleted ON protocol_config(deleted_at);
CREATE UNIQUE INDEX idx_protocol_config_name ON protocol_config(name) WHERE deleted_at IS NULL;

CREATE TABLE device_group (
    id          UUID PRIMARY KEY,
    name        VARCHAR(100) NOT NULL,
    parent_id   UUID REFERENCES device_group(id) ON DELETE RESTRICT,
    status      status_enum NOT NULL DEFAULT 'enabled',
    sort_order  INTEGER NOT NULL DEFAULT 0 CHECK (sort_order >= 0),
    remark      TEXT,
    created_by  UUID NOT NULL REFERENCES sys_user(id) ON DELETE RESTRICT,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    deleted_at  TIMESTAMPTZ
);
CREATE INDEX idx_device_group_parent ON device_group(parent_id);
CREATE INDEX idx_device_group_deleted ON device_group(deleted_at);
CREATE UNIQUE INDEX idx_device_group_name ON device_group(name) WHERE deleted_at IS NULL;

CREATE TABLE device (
    id                  UUID PRIMARY KEY,
    name                VARCHAR(100) NOT NULL,
    link_id             UUID NOT NULL REFERENCES link(id) ON DELETE RESTRICT,
    protocol_config_id  UUID NOT NULL REFERENCES protocol_config(id) ON DELETE RESTRICT,
    group_id            UUID REFERENCES device_group(id) ON DELETE RESTRICT,
    status              status_enum NOT NULL DEFAULT 'enabled',
    protocol_params     JSONB NOT NULL DEFAULT '{}'::jsonb
                        CHECK (jsonb_typeof(protocol_params) = 'object'),
    remark              TEXT,
    created_by          UUID NOT NULL REFERENCES sys_user(id) ON DELETE RESTRICT,
    created_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    deleted_at          TIMESTAMPTZ
);
CREATE INDEX idx_device_link ON device(link_id);
CREATE INDEX idx_device_protocol ON device(protocol_config_id);
CREATE INDEX idx_device_group ON device(group_id) WHERE deleted_at IS NULL;
CREATE INDEX idx_device_deleted ON device(deleted_at);
CREATE UNIQUE INDEX idx_device_name ON device(name) WHERE deleted_at IS NULL;
CREATE UNIQUE INDEX idx_device_protocol_params_code
    ON device((protocol_params->>'device_code'))
    WHERE deleted_at IS NULL
      AND protocol_params->>'device_code' IS NOT NULL
      AND protocol_params->>'device_code' != '';

CREATE TABLE device_data (
    id             UUID NOT NULL,
    device_id      UUID NOT NULL REFERENCES device(id) ON DELETE RESTRICT,
    link_id        UUID NOT NULL REFERENCES link(id) ON DELETE RESTRICT,
    protocol       TEXT NOT NULL CHECK (protocol IN ('SL651', 'Modbus', 'S7')),
    data           JSONB NOT NULL DEFAULT '{}'::jsonb CHECK (jsonb_typeof(data) = 'object'),
    report_time    TIMESTAMPTZ NOT NULL,
    created_at     TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    connection_id  UUID NOT NULL,
    source         TEXT NOT NULL,
    occurred_at    TIMESTAMPTZ NOT NULL,
    raw_payload_hex JSONB NOT NULL DEFAULT '[]'::jsonb
                    CHECK (jsonb_typeof(raw_payload_hex) = 'array'),
    PRIMARY KEY (id, report_time)
);

PERFORM create_hypertable('device_data', 'report_time', if_not_exists => TRUE);
CREATE INDEX idx_device_data_device_time
    ON device_data(device_id, report_time DESC);
CREATE INDEX idx_device_data_link_time
    ON device_data(link_id, report_time DESC);
CREATE INDEX idx_device_data_history
    ON device_data(device_id, (data->>'function_code'), report_time DESC);

INSERT INTO sys_role(id, code, name, description, status, permissions)
VALUES ('00000000-0000-7000-8000-000000000001', 'superadmin', '超级管理员',
        '系统内置角色', 'enabled', '["*"]'::jsonb);

INSERT INTO sys_user(id, username, password_hash, nickname, status)
VALUES (
    '00000000-0000-7000-8000-000000000002',
    'admin',
    'pbkdf2_sha256$210000$c92f36ef05a2afc548b868057a91af3a$9aa29710cf82ac0a23c8b0a112e76e2b860ecb303c949b9eada7045762df5974',
    '系统管理员',
    'enabled'
);

INSERT INTO sys_user_role(id, user_id, role_id)
VALUES ('00000000-0000-7000-8000-000000000003',
        '00000000-0000-7000-8000-000000000002',
        '00000000-0000-7000-8000-000000000001');

EXECUTE format('ALTER DATABASE %I SET timezone TO %L', current_database(), 'UTC');
PERFORM set_config('TimeZone', 'UTC', false);
END
$schema$;
)sql"},
    {"0002_device_access_control", R"sql(
DO $schema$
BEGIN
CREATE TABLE device_access_grant (
    id            UUID PRIMARY KEY,
    device_id     UUID NOT NULL REFERENCES device(id) ON DELETE CASCADE,
    user_id       UUID REFERENCES sys_user(id) ON DELETE CASCADE,
    department_id UUID REFERENCES sys_department(id) ON DELETE CASCADE,
    access_level  VARCHAR(20) NOT NULL
                  CHECK (access_level IN ('view', 'operate', 'manage')),
    granted_by    UUID NOT NULL REFERENCES sys_user(id) ON DELETE RESTRICT,
    created_at    TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at    TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    CONSTRAINT ck_device_access_subject CHECK (
        num_nonnulls(user_id, department_id) = 1
    )
);
CREATE UNIQUE INDEX idx_device_access_user_unique
    ON device_access_grant(device_id, user_id) WHERE user_id IS NOT NULL;
CREATE UNIQUE INDEX idx_device_access_department_unique
    ON device_access_grant(device_id, department_id) WHERE department_id IS NOT NULL;
CREATE INDEX idx_device_access_user_scope
    ON device_access_grant(user_id, device_id) WHERE user_id IS NOT NULL;
CREATE INDEX idx_device_access_department_scope
    ON device_access_grant(department_id, device_id) WHERE department_id IS NOT NULL;
CREATE INDEX idx_device_created_by_active
    ON device(created_by, id) WHERE deleted_at IS NULL;

CREATE TABLE security_audit_log (
    id            UUID PRIMARY KEY,
    actor_user_id UUID REFERENCES sys_user(id) ON DELETE SET NULL,
    action        VARCHAR(100) NOT NULL,
    resource_type VARCHAR(50) NOT NULL,
    resource_id   UUID,
    outcome       VARCHAR(20) NOT NULL CHECK (outcome IN ('success', 'denied', 'failed')),
    reason        TEXT,
    details       JSONB NOT NULL DEFAULT '{}'::jsonb
                  CHECK (jsonb_typeof(details) = 'object'),
    occurred_at   TIMESTAMPTZ NOT NULL DEFAULT NOW()
);
CREATE INDEX idx_security_audit_resource_time
    ON security_audit_log(resource_type, resource_id, occurred_at DESC);
CREATE INDEX idx_security_audit_actor_time
    ON security_audit_log(actor_user_id, occurred_at DESC);
END
$schema$;
)sql"},
    {"0003_device_share_without_manage", R"sql(
DO $schema$
BEGIN
UPDATE device_access_grant
SET access_level = 'operate', updated_at = NOW()
WHERE access_level = 'manage';

ALTER TABLE device_access_grant
    DROP CONSTRAINT IF EXISTS device_access_grant_access_level_check;
ALTER TABLE device_access_grant
    DROP CONSTRAINT IF EXISTS ck_device_access_level;
ALTER TABLE device_access_grant
    ADD CONSTRAINT ck_device_access_level
    CHECK (access_level IN ('view', 'operate'));
END
$schema$;
)sql"},
    {"0004_device_group_access_control", R"sql(
DO $schema$
BEGIN
CREATE TABLE device_group_access_grant (
    id            UUID PRIMARY KEY,
    group_id      UUID NOT NULL REFERENCES device_group(id) ON DELETE CASCADE,
    user_id       UUID REFERENCES sys_user(id) ON DELETE CASCADE,
    department_id UUID REFERENCES sys_department(id) ON DELETE CASCADE,
    access_level  VARCHAR(20) NOT NULL
                  CONSTRAINT ck_device_group_access_level
                  CHECK (access_level IN ('view', 'operate')),
    granted_by    UUID NOT NULL REFERENCES sys_user(id) ON DELETE RESTRICT,
    created_at    TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at    TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    CONSTRAINT ck_device_group_access_subject CHECK (
        num_nonnulls(user_id, department_id) = 1
    )
);
CREATE UNIQUE INDEX idx_device_group_access_user_unique
    ON device_group_access_grant(group_id, user_id) WHERE user_id IS NOT NULL;
CREATE UNIQUE INDEX idx_device_group_access_department_unique
    ON device_group_access_grant(group_id, department_id) WHERE department_id IS NOT NULL;
CREATE INDEX idx_device_group_access_user_scope
    ON device_group_access_grant(user_id, group_id) WHERE user_id IS NOT NULL;
CREATE INDEX idx_device_group_access_department_scope
    ON device_group_access_grant(department_id, group_id) WHERE department_id IS NOT NULL;
END
$schema$;
)sql"},
    {"0005_open_access", R"sql(
DO $schema$
BEGIN
CREATE TABLE open_access_key (
    id                UUID PRIMARY KEY,
    name              VARCHAR(64) NOT NULL,
    access_key_prefix VARCHAR(16) NOT NULL,
    access_key_hash   VARCHAR(64) NOT NULL,
    status            status_enum NOT NULL DEFAULT 'enabled',
    scopes            JSONB NOT NULL DEFAULT '[]'::jsonb
                      CHECK (jsonb_typeof(scopes) = 'array'),
    expires_at        TIMESTAMPTZ,
    last_used_at      TIMESTAMPTZ,
    last_used_ip      VARCHAR(64),
    remark            VARCHAR(200),
    created_by        UUID NOT NULL REFERENCES sys_user(id) ON DELETE RESTRICT,
    created_at        TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at        TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    deleted_at        TIMESTAMPTZ
);
CREATE UNIQUE INDEX idx_open_access_key_hash ON open_access_key(access_key_hash);
CREATE UNIQUE INDEX idx_open_access_key_name_active
    ON open_access_key(name) WHERE deleted_at IS NULL;
CREATE INDEX idx_open_access_key_active
    ON open_access_key(status, expires_at) WHERE deleted_at IS NULL;

CREATE TABLE open_access_key_device (
    access_key_id UUID NOT NULL REFERENCES open_access_key(id) ON DELETE CASCADE,
    device_id     UUID NOT NULL REFERENCES device(id) ON DELETE CASCADE,
    created_at    TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    PRIMARY KEY (access_key_id, device_id)
);
CREATE INDEX idx_open_access_key_device_device ON open_access_key_device(device_id);

CREATE TABLE open_webhook (
    id                UUID PRIMARY KEY,
    access_key_id     UUID NOT NULL REFERENCES open_access_key(id) ON DELETE CASCADE,
    name              VARCHAR(64) NOT NULL,
    url               TEXT NOT NULL,
    status            status_enum NOT NULL DEFAULT 'enabled',
    secret            VARCHAR(255),
    headers           JSONB NOT NULL DEFAULT '{}'::jsonb
                      CHECK (jsonb_typeof(headers) = 'object'),
    event_types       JSONB NOT NULL DEFAULT '["device.data.reported"]'::jsonb
                      CHECK (jsonb_typeof(event_types) = 'array'),
    timeout_seconds   INTEGER NOT NULL DEFAULT 5 CHECK (timeout_seconds BETWEEN 1 AND 30),
    last_triggered_at TIMESTAMPTZ,
    last_success_at   TIMESTAMPTZ,
    last_failure_at   TIMESTAMPTZ,
    last_http_status  INTEGER,
    last_error        TEXT,
    created_at        TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at        TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    deleted_at        TIMESTAMPTZ
);
CREATE UNIQUE INDEX idx_open_webhook_name_active
    ON open_webhook(access_key_id, name) WHERE deleted_at IS NULL;
CREATE INDEX idx_open_webhook_active
    ON open_webhook(access_key_id, status) WHERE deleted_at IS NULL;

CREATE TABLE open_access_log (
    id               UUID PRIMARY KEY,
    access_key_id    UUID REFERENCES open_access_key(id) ON DELETE SET NULL,
    webhook_id       UUID REFERENCES open_webhook(id) ON DELETE SET NULL,
    direction        VARCHAR(20) NOT NULL CHECK (direction IN ('pull', 'push')),
    action           VARCHAR(50) NOT NULL,
    event_type       VARCHAR(100),
    status           VARCHAR(20) NOT NULL CHECK (status IN ('success', 'failed')),
    http_method      VARCHAR(10),
    target           TEXT,
    request_ip       VARCHAR(64),
    http_status      INTEGER,
    device_id        UUID REFERENCES device(id) ON DELETE SET NULL,
    device_code      VARCHAR(100),
    message          TEXT,
    request_payload  JSONB NOT NULL DEFAULT '{}'::jsonb
                     CHECK (jsonb_typeof(request_payload) = 'object'),
    response_payload JSONB NOT NULL DEFAULT '{}'::jsonb
                     CHECK (jsonb_typeof(response_payload) = 'object'),
    created_at       TIMESTAMPTZ NOT NULL DEFAULT NOW()
);
CREATE INDEX idx_open_access_log_key_time
    ON open_access_log(access_key_id, created_at DESC);
CREATE INDEX idx_open_access_log_webhook_time
    ON open_access_log(webhook_id, created_at DESC);
CREATE INDEX idx_open_access_log_device_time
    ON open_access_log(device_id, created_at DESC);

-- 告警引擎尚未迁入 iot-engine；开放接入拥有独立、稳定的读模型，供后续告警投影写入。
CREATE TABLE open_alert_record (
    id           UUID PRIMARY KEY,
    rule_id      UUID,
    device_id    UUID NOT NULL REFERENCES device(id) ON DELETE CASCADE,
    severity     VARCHAR(20) NOT NULL,
    status       VARCHAR(20) NOT NULL,
    message      TEXT NOT NULL,
    detail       JSONB NOT NULL DEFAULT '{}'::jsonb
                 CHECK (jsonb_typeof(detail) = 'object'),
    triggered_at TIMESTAMPTZ NOT NULL,
    resolved_at  TIMESTAMPTZ,
    created_at   TIMESTAMPTZ NOT NULL DEFAULT NOW()
);
CREATE INDEX idx_open_alert_device_time
    ON open_alert_record(device_id, triggered_at DESC);
CREATE INDEX idx_open_alert_status_time
    ON open_alert_record(status, triggered_at DESC);
END
$schema$;
)sql"},
    {"0006_edge_node_management", R"sql(
DO $schema$
BEGIN
CREATE TABLE edge_node (
    id                         UUID PRIMARY KEY,
    platform_id                UUID NOT NULL,
    imei                       VARCHAR(15) NOT NULL,
    name                       VARCHAR(100),
    model                      VARCHAR(128) NOT NULL DEFAULT '',
    software_version           VARCHAR(32) NOT NULL DEFAULT '',
    hostname                   VARCHAR(64) NOT NULL DEFAULT '',
    architecture               VARCHAR(32) NOT NULL DEFAULT '',
    openwrt_release            VARCHAR(64) NOT NULL DEFAULT '',
    enrollment_status          VARCHAR(20) NOT NULL DEFAULT 'pending'
                               CHECK (enrollment_status IN ('pending', 'approved', 'rejected')),
    supports_network_config    BOOLEAN NOT NULL DEFAULT FALSE,
    supports_firmware_update   BOOLEAN NOT NULL DEFAULT FALSE,
    supports_platform_config   BOOLEAN NOT NULL DEFAULT FALSE,
    ttyd_available             BOOLEAN NOT NULL DEFAULT FALSE,
    active_config_version      BIGINT NOT NULL DEFAULT 0,
    outbox_records             BIGINT NOT NULL DEFAULT 0,
    outbox_bytes               BIGINT NOT NULL DEFAULT 0,
    last_seen_at               TIMESTAMPTZ,
    approved_by                UUID REFERENCES sys_user(id) ON DELETE SET NULL,
    approved_at                TIMESTAMPTZ,
    created_at                 TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at                 TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    CONSTRAINT edge_node_imei_digits CHECK (imei ~ '^[0-9]{15}$')
);
CREATE UNIQUE INDEX idx_edge_node_platform_imei ON edge_node(platform_id, imei);
CREATE INDEX idx_edge_node_status_seen ON edge_node(enrollment_status, last_seen_at DESC);
ALTER TABLE link ADD CONSTRAINT fk_link_edge_node
    FOREIGN KEY (edge_node_id) REFERENCES edge_node(id) ON DELETE RESTRICT;
CREATE INDEX idx_link_edge_node ON link(edge_node_id)
    WHERE execution = 'edge' AND deleted_at IS NULL;

CREATE TABLE edge_node_interface (
    node_id       UUID NOT NULL REFERENCES edge_node(id) ON DELETE CASCADE,
    name          VARCHAR(32) NOT NULL,
    display_name  VARCHAR(64) NOT NULL DEFAULT '',
    mac           VARCHAR(17),
    is_up         BOOLEAN NOT NULL DEFAULT FALSE,
    is_bridge     BOOLEAN NOT NULL DEFAULT FALSE,
    ipv4          VARCHAR(15),
    prefix_length INTEGER CHECK (prefix_length BETWEEN 0 AND 32),
    gateway       VARCHAR(15),
    bridge_ports  JSONB NOT NULL DEFAULT '[]'::jsonb
                  CHECK (jsonb_typeof(bridge_ports) = 'array'),
    updated_at    TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    PRIMARY KEY (node_id, name)
);

CREATE TABLE edge_node_serial (
    node_id      UUID NOT NULL REFERENCES edge_node(id) ON DELETE CASCADE,
    path         VARCHAR(96) NOT NULL,
    display_name VARCHAR(64) NOT NULL DEFAULT '',
    available    BOOLEAN NOT NULL DEFAULT FALSE,
    rs485        BOOLEAN NOT NULL DEFAULT FALSE,
    updated_at   TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    PRIMARY KEY (node_id, path)
);

CREATE TABLE edge_firmware (
    id             UUID PRIMARY KEY,
    version        VARCHAR(64) NOT NULL,
    file_name      VARCHAR(255) NOT NULL,
    storage_path   TEXT NOT NULL,
    sha256         VARCHAR(64) NOT NULL,
    size_bytes     BIGINT NOT NULL CHECK (size_bytes > 0 AND size_bytes <= 134217728),
    download_token VARCHAR(64) NOT NULL,
    created_by     UUID NOT NULL REFERENCES sys_user(id) ON DELETE RESTRICT,
    created_at     TIMESTAMPTZ NOT NULL DEFAULT NOW()
);
CREATE INDEX idx_edge_firmware_created ON edge_firmware(created_at DESC);

CREATE TABLE edge_node_platform (
    node_id                UUID NOT NULL REFERENCES edge_node(id) ON DELETE CASCADE,
    platform_id            UUID NOT NULL,
    name                   VARCHAR(32) NOT NULL,
    base_url               VARCHAR(255) NOT NULL,
    enabled                BOOLEAN NOT NULL DEFAULT TRUE,
    priority               INTEGER NOT NULL DEFAULT 100 CHECK (priority BETWEEN 0 AND 65535),
    reconnect_interval_sec INTEGER NOT NULL DEFAULT 5 CHECK (reconnect_interval_sec BETWEEN 1 AND 3600),
    outbox_max_bytes       INTEGER NOT NULL DEFAULT 262144
                           CHECK (outbox_max_bytes BETWEEN 16384 AND 8388608),
    apply_status           VARCHAR(20) NOT NULL DEFAULT 'pending'
                           CHECK (apply_status IN ('pending', 'applied', 'failed')),
    last_message           VARCHAR(256),
    updated_at             TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    PRIMARY KEY (node_id, platform_id)
);

CREATE TABLE edge_task (
    id           UUID PRIMARY KEY,
    node_id      UUID NOT NULL REFERENCES edge_node(id) ON DELETE CASCADE,
    task_type    VARCHAR(30) NOT NULL
                 CHECK (task_type IN ('network', 'firmware', 'platform_upsert', 'platform_delete')),
    status       VARCHAR(20) NOT NULL DEFAULT 'pending'
                 CHECK (status IN ('pending', 'accepted', 'running', 'succeeded', 'failed')),
    request      JSONB NOT NULL DEFAULT '{}'::jsonb CHECK (jsonb_typeof(request) = 'object'),
    result       JSONB NOT NULL DEFAULT '{}'::jsonb CHECK (jsonb_typeof(result) = 'object'),
    created_by   UUID NOT NULL REFERENCES sys_user(id) ON DELETE RESTRICT,
    created_at   TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at   TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    completed_at TIMESTAMPTZ
);
CREATE INDEX idx_edge_task_node_created ON edge_task(node_id, created_at DESC);
END
$schema$;
)sql"},
    {"0007_edge_device_assignment", R"sql(
DO $schema$
BEGIN
ALTER TABLE edge_node
    ADD COLUMN desired_config_version BIGINT NOT NULL DEFAULT 0,
    ADD COLUMN supports_device_config BOOLEAN NOT NULL DEFAULT FALSE,
    ADD COLUMN config_status VARCHAR(20) NOT NULL DEFAULT 'idle'
        CHECK (config_status IN ('idle', 'pending', 'applied', 'rejected')),
    ADD COLUMN config_message VARCHAR(256) NOT NULL DEFAULT '';

CREATE TABLE edge_config_revision (
    node_id      UUID NOT NULL REFERENCES edge_node(id) ON DELETE CASCADE,
    revision     BIGINT NOT NULL CHECK (revision > 0),
    sha256       VARCHAR(64) NOT NULL CHECK (sha256 ~ '^[0-9a-f]{64}$'),
    item_count   INTEGER NOT NULL CHECK (item_count BETWEEN 0 AND 512),
    status       VARCHAR(20) NOT NULL DEFAULT 'pending'
                 CHECK (status IN ('pending', 'applied', 'rejected')),
    message      VARCHAR(256) NOT NULL DEFAULT '',
    created_by   UUID REFERENCES sys_user(id) ON DELETE SET NULL,
    created_at   TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    completed_at TIMESTAMPTZ,
    PRIMARY KEY (node_id, revision)
);
CREATE INDEX idx_edge_config_revision_created
    ON edge_config_revision(node_id, created_at DESC);
END
$schema$;
)sql"},
    {"0008_edge_network_management", R"sql(
DO $schema$
BEGIN
ALTER TABLE edge_node
    ADD COLUMN network_config_version INTEGER NOT NULL DEFAULT 0
        CHECK (network_config_version BETWEEN 0 AND 100);

CREATE TABLE edge_node_network (
    node_id       UUID NOT NULL REFERENCES edge_node(id) ON DELETE CASCADE,
    name          VARCHAR(32) NOT NULL,
    address_mode  VARCHAR(12) NOT NULL
                  CHECK (address_mode IN ('dhcp', 'static', 'none')),
    device        VARCHAR(32) NOT NULL DEFAULT '',
    is_up         BOOLEAN NOT NULL DEFAULT FALSE,
    is_bridge     BOOLEAN NOT NULL DEFAULT FALSE,
    ipv4          VARCHAR(15),
    prefix_length INTEGER CHECK (prefix_length BETWEEN 0 AND 32),
    gateway       VARCHAR(15),
    bridge_ports  JSONB NOT NULL DEFAULT '[]'::jsonb
                  CHECK (jsonb_typeof(bridge_ports) = 'array'),
    updated_at    TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    PRIMARY KEY (node_id, name)
);
END
$schema$;
)sql"},
    {"0009_edge_modem_management", R"sql(
DO $schema$
BEGIN
ALTER TABLE edge_node
    ADD COLUMN modem_available BOOLEAN NOT NULL DEFAULT FALSE,
    ADD COLUMN sim_state VARCHAR(20) NOT NULL DEFAULT 'unknown'
        CHECK (sim_state IN ('unknown', 'ready', 'not_inserted', 'pin_required',
                             'puk_required', 'blocked')),
    ADD COLUMN iccid VARCHAR(22) NOT NULL DEFAULT '',
    ADD COLUMN signal_csq INTEGER NOT NULL DEFAULT 99 CHECK (signal_csq BETWEEN 0 AND 99),
    ADD COLUMN signal_rssi_dbm INTEGER NOT NULL DEFAULT -1,
    ADD COLUMN signal_percent INTEGER NOT NULL DEFAULT 0
        CHECK (signal_percent BETWEEN 0 AND 100),
    ADD COLUMN mobile_registered BOOLEAN NOT NULL DEFAULT FALSE,
    ADD COLUMN mobile_registration_status INTEGER NOT NULL DEFAULT -1,
    ADD COLUMN apn VARCHAR(63) NOT NULL DEFAULT '',
    ADD COLUMN mobile_operator VARCHAR(64) NOT NULL DEFAULT '',
    ADD COLUMN mobile_connected BOOLEAN NOT NULL DEFAULT FALSE,
    ADD COLUMN mobile_ipv4 VARCHAR(15) NOT NULL DEFAULT '',
    ADD COLUMN supports_modem_control BOOLEAN NOT NULL DEFAULT FALSE;

ALTER TABLE edge_task DROP CONSTRAINT edge_task_task_type_check;
ALTER TABLE edge_task ADD CONSTRAINT edge_task_task_type_check
    CHECK (task_type IN ('network', 'firmware', 'modem',
                         'platform_upsert', 'platform_delete'));
END
$schema$;
)sql"},
    {"0010_unified_json_runtime_design", R"sql(
DO $schema$
BEGIN
ALTER TABLE edge_node
    ADD COLUMN status JSONB NOT NULL DEFAULT
        '{"config":{"activeVersion":0,"desiredVersion":0,"state":"idle","message":""},"outbox":{"records":0,"bytes":0}}'::jsonb
        CHECK (jsonb_typeof(status) = 'object'),
    ADD COLUMN capability JSONB NOT NULL DEFAULT '{}'::jsonb
        CHECK (jsonb_typeof(capability) = 'object'),
    ADD COLUMN mobile JSONB NOT NULL DEFAULT '{}'::jsonb
        CHECK (jsonb_typeof(mobile) = 'object');

UPDATE edge_node
SET status = jsonb_build_object(
        'config', jsonb_build_object(
            'activeVersion', active_config_version,
            'desiredVersion', desired_config_version,
            'state', config_status,
            'message', config_message),
        'outbox', jsonb_build_object(
            'records', outbox_records,
            'bytes', outbox_bytes)),
    capability = jsonb_build_object(
        'networkConfig', supports_network_config,
        'networkConfigVersion', network_config_version,
        'firmwareUpdate', supports_firmware_update,
        'platformConfig', supports_platform_config,
        'deviceConfig', supports_device_config,
        'modemControl', supports_modem_control,
        'terminal', ttyd_available),
    mobile = jsonb_build_object(
        'available', modem_available,
        'simState', sim_state,
        'iccid', iccid,
        'signal', jsonb_build_object(
            'csq', signal_csq,
            'rssiDbm', signal_rssi_dbm,
            'percent', signal_percent),
        'registered', mobile_registered,
        'registrationStatus', mobile_registration_status,
        'apn', apn,
        'operator', mobile_operator,
        'connected', mobile_connected,
        'ipv4', mobile_ipv4);

ALTER TABLE edge_node
    DROP COLUMN supports_network_config,
    DROP COLUMN supports_firmware_update,
    DROP COLUMN supports_platform_config,
    DROP COLUMN ttyd_available,
    DROP COLUMN active_config_version,
    DROP COLUMN outbox_records,
    DROP COLUMN outbox_bytes,
    DROP COLUMN desired_config_version,
    DROP COLUMN supports_device_config,
    DROP COLUMN config_status,
    DROP COLUMN config_message,
    DROP COLUMN modem_available,
    DROP COLUMN sim_state,
    DROP COLUMN iccid,
    DROP COLUMN signal_csq,
    DROP COLUMN signal_rssi_dbm,
    DROP COLUMN signal_percent,
    DROP COLUMN mobile_registered,
    DROP COLUMN mobile_registration_status,
    DROP COLUMN apn,
    DROP COLUMN mobile_operator,
    DROP COLUMN mobile_connected,
    DROP COLUMN mobile_ipv4,
    DROP COLUMN supports_modem_control;

CREATE INDEX idx_edge_node_capability_gin ON edge_node USING GIN(capability);

ALTER TABLE edge_node_platform
    ADD COLUMN status JSONB NOT NULL DEFAULT '{"state":"pending","message":""}'::jsonb
        CHECK (jsonb_typeof(status) = 'object');

UPDATE edge_node_platform
SET status = jsonb_build_object(
    'state', apply_status,
    'message', COALESCE(last_message, ''));

ALTER TABLE edge_node_platform
    DROP COLUMN apply_status,
    DROP COLUMN last_message;
END
$schema$;
)sql"},
    {"0011_edge_log_status", R"sql(
DO $schema$
BEGIN
ALTER TABLE edge_node
    ALTER COLUMN status SET DEFAULT
        '{"config":{"activeVersion":0,"desiredVersion":0,"state":"idle","message":""},"outbox":{"records":0,"bytes":0},"log":{"level":"info"}}'::jsonb;

UPDATE edge_node
SET status = jsonb_set(
        jsonb_set(status, '{log}', COALESCE(status->'log', '{}'::jsonb), true),
        '{log,level}', to_jsonb(COALESCE(status->'log'->>'level', 'info')::text), true)
WHERE NOT (status ? 'log') OR status->'log'->>'level' IS NULL;
END
$schema$;
)sql"},
    {"0012_alert_center", R"sql(
DO $schema$
BEGIN
CREATE TABLE alert_rule (
    id                    UUID PRIMARY KEY,
    name                  VARCHAR(128) NOT NULL,
    device_id             UUID NOT NULL REFERENCES device(id) ON DELETE CASCADE,
    severity              VARCHAR(20) NOT NULL
                          CHECK (severity IN ('critical', 'warning', 'info')),
    conditions            JSONB NOT NULL CHECK (jsonb_typeof(conditions) = 'array'),
    logic                 VARCHAR(8) NOT NULL DEFAULT 'and'
                          CHECK (logic IN ('and', 'or')),
    silence_duration      INTEGER NOT NULL DEFAULT 300
                          CHECK (silence_duration BETWEEN 0 AND 86400),
    recovery_condition    VARCHAR(32) NOT NULL DEFAULT 'reverse',
    recovery_wait_seconds INTEGER NOT NULL DEFAULT 60
                          CHECK (recovery_wait_seconds BETWEEN 0 AND 86400),
    status                status_enum NOT NULL DEFAULT 'enabled',
    remark                VARCHAR(500),
    created_by            UUID NOT NULL REFERENCES sys_user(id) ON DELETE RESTRICT,
    created_at            TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at            TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    deleted_at            TIMESTAMPTZ
);
CREATE UNIQUE INDEX idx_alert_rule_name_device_active
    ON alert_rule(device_id, name) WHERE deleted_at IS NULL;
CREATE INDEX idx_alert_rule_device
    ON alert_rule(device_id) WHERE deleted_at IS NULL;
CREATE INDEX idx_alert_rule_status
    ON alert_rule(status) WHERE deleted_at IS NULL;

CREATE TABLE alert_rule_template (
    id                    UUID PRIMARY KEY,
    name                  VARCHAR(128) NOT NULL,
    category              VARCHAR(64),
    description           VARCHAR(500),
    severity              VARCHAR(20) NOT NULL
                          CHECK (severity IN ('critical', 'warning', 'info')),
    conditions            JSONB NOT NULL CHECK (jsonb_typeof(conditions) = 'array'),
    logic                 VARCHAR(8) NOT NULL DEFAULT 'and'
                          CHECK (logic IN ('and', 'or')),
    silence_duration      INTEGER NOT NULL DEFAULT 300
                          CHECK (silence_duration BETWEEN 0 AND 86400),
    recovery_condition    VARCHAR(32) NOT NULL DEFAULT 'reverse',
    recovery_wait_seconds INTEGER NOT NULL DEFAULT 60
                          CHECK (recovery_wait_seconds BETWEEN 0 AND 86400),
    applicable_protocols  JSONB NOT NULL DEFAULT '[]'::jsonb
                          CHECK (jsonb_typeof(applicable_protocols) = 'array'),
    protocol_config_id    UUID REFERENCES protocol_config(id) ON DELETE SET NULL,
    created_by            UUID NOT NULL REFERENCES sys_user(id) ON DELETE RESTRICT,
    created_at            TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at            TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    deleted_at            TIMESTAMPTZ
);
CREATE UNIQUE INDEX idx_alert_template_name_active
    ON alert_rule_template(name) WHERE deleted_at IS NULL;
CREATE INDEX idx_alert_template_category
    ON alert_rule_template(category) WHERE deleted_at IS NULL;

ALTER TABLE open_alert_record
    ADD COLUMN acknowledged_at TIMESTAMPTZ,
    ADD COLUMN acknowledged_by UUID REFERENCES sys_user(id) ON DELETE SET NULL,
    ADD COLUMN updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW();

ALTER TABLE open_alert_record
    ADD CONSTRAINT fk_open_alert_record_rule
        FOREIGN KEY (rule_id) REFERENCES alert_rule(id) ON DELETE SET NULL NOT VALID,
    ADD CONSTRAINT ck_open_alert_record_severity
        CHECK (severity IN ('critical', 'warning', 'info')) NOT VALID,
    ADD CONSTRAINT ck_open_alert_record_status
        CHECK (status IN ('active', 'acknowledged', 'resolved')) NOT VALID;

CREATE UNIQUE INDEX idx_open_alert_one_unresolved_rule
    ON open_alert_record(rule_id)
    WHERE rule_id IS NOT NULL AND status IN ('active', 'acknowledged');
CREATE INDEX idx_open_alert_rule_time
    ON open_alert_record(rule_id, triggered_at DESC);

CREATE TABLE alert_rule_state (
    rule_id             UUID PRIMARY KEY REFERENCES alert_rule(id) ON DELETE CASCADE,
    matched             BOOLEAN NOT NULL DEFAULT FALSE,
    recovery_started_at TIMESTAMPTZ,
    last_evaluated_at   TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at          TIMESTAMPTZ NOT NULL DEFAULT NOW()
);
END
$schema$;
)sql"},
    {"0013_timestamp_contract", R"sql(
DO $schema$
BEGIN
EXECUTE format('ALTER DATABASE %I SET timezone TO %L', current_database(), 'UTC');
EXECUTE format('ALTER DATABASE %I SET datestyle TO %L', current_database(), 'ISO, YMD');
PERFORM set_config('TimeZone', 'UTC', false);
PERFORM set_config('DateStyle', 'ISO, YMD', false);
EXECUTE $definition$
CREATE OR REPLACE FUNCTION iot_utc_timestamp(value TIMESTAMPTZ)
RETURNS TEXT
LANGUAGE SQL
IMMUTABLE
PARALLEL SAFE
RETURNS NULL ON NULL INPUT
AS $function$
SELECT to_char(value AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"')
$function$
$definition$;
EXECUTE $definition$
COMMENT ON FUNCTION iot_utc_timestamp(TIMESTAMPTZ) IS
    'Serializes typed timestamps at the public API boundary as UTC RFC 3339 seconds'
$definition$;
END
$schema$;
)sql"},
    {"0014_gb28181_projection", R"sql(
DO $schema$
BEGIN
CREATE TABLE gb28181_device (
    id                  VARCHAR(128) PRIMARY KEY,
    name                VARCHAR(255) NOT NULL DEFAULT '',
    manufacturer        VARCHAR(255) NOT NULL DEFAULT '',
    remote_address      VARCHAR(255) NOT NULL DEFAULT '',
    registration_source VARCHAR(32) NOT NULL DEFAULT 'sip',
    online              BOOLEAN NOT NULL DEFAULT FALSE,
    last_seen_at        TIMESTAMPTZ NOT NULL,
    mapped_device_id    UUID REFERENCES device(id) ON DELETE SET NULL,
    created_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at          TIMESTAMPTZ NOT NULL DEFAULT NOW()
);
CREATE INDEX idx_gb28181_device_online ON gb28181_device(online, last_seen_at DESC);
CREATE INDEX idx_gb28181_device_mapped ON gb28181_device(mapped_device_id)
    WHERE mapped_device_id IS NOT NULL;

CREATE TABLE gb28181_channel (
    device_id   VARCHAR(128) NOT NULL REFERENCES gb28181_device(id) ON DELETE CASCADE,
    id          VARCHAR(128) NOT NULL,
    name        VARCHAR(255) NOT NULL DEFAULT '',
    manufacturer VARCHAR(255) NOT NULL DEFAULT '',
    online      BOOLEAN NOT NULL DEFAULT FALSE,
    ptz_type    INTEGER NOT NULL DEFAULT -1,
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    PRIMARY KEY (device_id, id)
);
CREATE INDEX idx_gb28181_channel_online
    ON gb28181_channel(device_id, online);

CREATE TABLE gb28181_record (
    device_id   VARCHAR(128) NOT NULL REFERENCES gb28181_device(id) ON DELETE CASCADE,
    channel_id  VARCHAR(128) NOT NULL,
    name        VARCHAR(255) NOT NULL DEFAULT '',
    file_path   TEXT NOT NULL DEFAULT '',
    address     TEXT NOT NULL DEFAULT '',
    start_time  TIMESTAMPTZ NOT NULL,
    end_time    TIMESTAMPTZ NOT NULL,
    record_type VARCHAR(64) NOT NULL DEFAULT '',
    recorder_id VARCHAR(128) NOT NULL DEFAULT '',
    created_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    PRIMARY KEY (device_id, channel_id, start_time, end_time, file_path)
);
CREATE INDEX idx_gb28181_record_range
    ON gb28181_record(device_id, channel_id, start_time DESC, end_time DESC);

CREATE TABLE gb28181_stream (
    app          VARCHAR(128) NOT NULL,
    stream       VARCHAR(255) NOT NULL,
    schema       VARCHAR(32) NOT NULL,
    online       BOOLEAN NOT NULL DEFAULT FALSE,
    reader_count INTEGER NOT NULL DEFAULT 0 CHECK (reader_count >= 0),
    updated_at   TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    PRIMARY KEY (app, stream, schema)
);
CREATE INDEX idx_gb28181_stream_online
    ON gb28181_stream(online, updated_at DESC);
END
$schema$;
)sql"},
    {"0015_sql_query_optimization", R"sql(
DO $schema$
BEGIN
CREATE INDEX idx_alert_rule_enabled_device
    ON alert_rule(device_id)
    WHERE deleted_at IS NULL AND status = 'enabled';

CREATE INDEX idx_gb28181_record_device_time
    ON gb28181_record(device_id, start_time DESC);
END
$schema$;
)sql"},
    {"0016_mass_data_query_optimization", R"sql(
DO $schema$
BEGIN
CREATE INDEX idx_open_access_log_time
    ON open_access_log(created_at DESC, id DESC);

CREATE INDEX idx_open_alert_time
    ON open_alert_record(triggered_at DESC, id DESC);

CREATE INDEX idx_open_alert_unresolved_summary
    ON open_alert_record(severity, device_id)
    WHERE status IN ('active', 'acknowledged');

CREATE INDEX idx_open_alert_resolved_time
    ON open_alert_record(resolved_at DESC)
    WHERE status = 'resolved';
END
$schema$;
)sql"},
    {"0017_device_data_compression_layout", R"sql(
ALTER TABLE device_data SET (
    timescaledb.compress,
    timescaledb.compress_segmentby = 'device_id',
    timescaledb.compress_orderby = 'report_time DESC, id DESC'
);
)sql"},
    {"0018_device_data_ingest_state", R"sql(
DO $schema$
BEGIN
CREATE TABLE device_data_ingest_state (
    device_id          UUID PRIMARY KEY REFERENCES device(id) ON DELETE CASCADE,
    last_stored_at     TIMESTAMPTZ,
    last_observed_at   TIMESTAMPTZ,
    last_observed_id   UUID,
    last_data          JSONB,
    previous_data      JSONB,
    updated_at         TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    CONSTRAINT ck_device_data_ingest_observed_pair CHECK (
        (last_observed_at IS NULL) = (last_observed_id IS NULL)
    )
);

INSERT INTO device_data_ingest_state(
    device_id, last_stored_at, last_observed_at, last_observed_id,
    last_data, previous_data)
SELECT device.id, recent.last_report_time, recent.last_report_time,
       recent.last_report_id, recent.last_data, recent.previous_data
FROM device
CROSS JOIN LATERAL (
    SELECT
        (array_agg(sample.report_time ORDER BY sample.report_time DESC, sample.id DESC))[1]
            AS last_report_time,
        (array_agg(sample.id ORDER BY sample.report_time DESC, sample.id DESC))[1]
            AS last_report_id,
        (jsonb_agg(sample.data ORDER BY sample.report_time DESC, sample.id DESC))->0
            AS last_data,
        (jsonb_agg(sample.data ORDER BY sample.report_time DESC, sample.id DESC))->1
            AS previous_data
    FROM (
        SELECT history.report_time, history.id, history.data
        FROM device_data history
        WHERE history.device_id = device.id
        ORDER BY history.report_time DESC, history.id DESC
        LIMIT 2
    ) sample
) recent
WHERE recent.last_report_time IS NOT NULL
ON CONFLICT (device_id) DO NOTHING;
END
$schema$;
)sql"},
    {"0019_alert_event_outbox", R"sql(
DO $schema$
BEGIN
CREATE TABLE alert_event_outbox (
    event_id        UUID NOT NULL,
    event_type      VARCHAR(64) NOT NULL,
    rule_id         UUID NOT NULL,
    device_id       UUID NOT NULL,
    device_code     VARCHAR(255) NOT NULL DEFAULT '',
    occurred_at_ms  BIGINT NOT NULL,
    data             JSONB NOT NULL,
    created_at       TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    PRIMARY KEY (event_id, event_type)
);
CREATE INDEX idx_alert_event_outbox_created
    ON alert_event_outbox(created_at, event_id);
END
$schema$;
)sql"},
    {"0020_alert_evaluation_receipt", R"sql(
DO $schema$
BEGIN
CREATE TABLE alert_evaluation_receipt (
    message_id  UUID PRIMARY KEY,
    device_id   UUID NOT NULL,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT NOW()
);
CREATE INDEX idx_alert_evaluation_receipt_created
    ON alert_evaluation_receipt(created_at);
END
$schema$;
)sql"},
    {"0021_device_latest_value", R"sql(
DO $schema$
BEGIN
CREATE TABLE device_latest_value (
    device_id    UUID NOT NULL REFERENCES device(id) ON DELETE CASCADE,
    element_id   TEXT NOT NULL,
    value        JSONB NOT NULL,
    observed_at  TIMESTAMPTZ NOT NULL,
    record_id    UUID NOT NULL,
    updated_at   TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    PRIMARY KEY (device_id, element_id)
);

INSERT INTO device_latest_value(
    device_id, element_id, value, observed_at, record_id)
SELECT DISTINCT ON (history.device_id, point.key)
       history.device_id, point.key, point.value, history.report_time, history.id
FROM device_data history
CROSS JOIN LATERAL jsonb_each(
    COALESCE(history.data->'values', '{}'::jsonb)) point
ORDER BY history.device_id, point.key, history.report_time DESC, history.id DESC
ON CONFLICT (device_id, element_id) DO NOTHING;
END
$schema$;
)sql"},
    {"0022_gb28181_custom_names", R"sql(
DO $schema$
BEGIN
ALTER TABLE gb28181_device
    ADD COLUMN custom_name VARCHAR(255);
ALTER TABLE gb28181_channel
    ADD COLUMN custom_name VARCHAR(255);
END
$schema$;
)sql"},
    {"0023_transactional_outbox", R"sql(
DO $schema$
BEGIN
CREATE TABLE outbox_event (
    id              UUID PRIMARY KEY,
    event_type      VARCHAR(100) NOT NULL,
    aggregate_type  VARCHAR(100) NOT NULL,
    aggregate_id    TEXT NOT NULL,
    action          VARCHAR(50) NOT NULL,
    schema_version  INTEGER NOT NULL,
    payload         JSONB NOT NULL DEFAULT '{}'::jsonb,
    occurred_at     TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    available_at    TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    attempts        INTEGER NOT NULL DEFAULT 0,
    last_error      TEXT,
    published_at    TIMESTAMPTZ,
    dead_lettered_at TIMESTAMPTZ
);

CREATE INDEX idx_outbox_event_pending
    ON outbox_event(available_at, occurred_at, id)
    WHERE published_at IS NULL AND dead_lettered_at IS NULL;
END
$schema$;
)sql"},
    {"0024_outbox_consumer_receipt", R"sql(
DO $schema$
BEGIN
CREATE TABLE outbox_consumer_receipt (
    consumer_name VARCHAR(150) NOT NULL,
    event_id      UUID NOT NULL,
    processed_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    PRIMARY KEY (consumer_name, event_id)
);

CREATE INDEX idx_outbox_consumer_receipt_processed_at
    ON outbox_consumer_receipt(processed_at);
END
$schema$;
)sql"},
    {"0025_webhook_tls_verification", R"sql(
ALTER TABLE open_webhook
    ADD COLUMN skip_tls_verify BOOLEAN NOT NULL DEFAULT FALSE;
)sql"},
    {"0026_unify_protocol_read_interval", R"sql(
DO $schema$
BEGIN
UPDATE protocol_config
SET config = jsonb_set(config - 'pollInterval', '{readInterval}',
                       config->'pollInterval', true),
    updated_at = NOW()
WHERE protocol = 'S7' AND config ? 'pollInterval';

ALTER TABLE protocol_config
    ADD CONSTRAINT ck_protocol_config_no_poll_interval
    CHECK (NOT (config ? 'pollInterval'));
END
$schema$;
)sql"},
    {"0027_unify_protocol_storage_policy", R"sql(
DO $schema$
BEGIN
UPDATE protocol_config
SET config = jsonb_set(
        config - 'storageInterval',
        '{storagePolicy}',
        to_jsonb(CASE WHEN config->>'storagePolicy' IN ('report', 'change')
                      THEN config->>'storagePolicy' ELSE 'report' END),
        true),
    updated_at = NOW()
WHERE config ? 'storageInterval'
   OR NOT (config ? 'storagePolicy')
   OR COALESCE(config->>'storagePolicy' NOT IN ('report', 'change'), TRUE);

ALTER TABLE protocol_config
    ADD CONSTRAINT ck_protocol_config_storage_policy
    CHECK (NOT (config ? 'storageInterval')
           AND COALESCE(config->>'storagePolicy' IN ('report', 'change'), FALSE));
END
$schema$;
)sql"},
    {"0028_remove_edge_enrollment_rejection", R"sql(
DO $schema$
BEGIN
UPDATE edge_node
SET enrollment_status = 'pending', approved_by = NULL, approved_at = NULL,
    updated_at = NOW()
WHERE enrollment_status = 'rejected';

ALTER TABLE edge_node
    DROP CONSTRAINT IF EXISTS edge_node_enrollment_status_check;
ALTER TABLE edge_node
    ADD CONSTRAINT edge_node_enrollment_status_check
    CHECK (enrollment_status IN ('pending', 'approved'));
END
$schema$;
)sql"},
    {"0029_vpn_core", R"sql(
DO $schema$
BEGIN
ALTER TABLE edge_task DROP CONSTRAINT IF EXISTS edge_task_task_type_check;
ALTER TABLE edge_task ADD CONSTRAINT edge_task_task_type_check
    CHECK (task_type IN ('network', 'firmware', 'modem', 'vpn',
                         'platform_upsert', 'platform_delete'));

CREATE TABLE vpn_network (
    id                UUID PRIMARY KEY,
    name              VARCHAR(100) NOT NULL,
    overlay_cidr      VARCHAR(18) NOT NULL,
    hub_public_key    VARCHAR(64) NOT NULL DEFAULT '',
    hub_endpoint      VARCHAR(255) NOT NULL DEFAULT '',
    hub_listen_port   INTEGER NOT NULL DEFAULT 51820 CHECK (hub_listen_port BETWEEN 1 AND 65535),
    status            status_enum NOT NULL DEFAULT 'enabled',
    created_by        UUID NOT NULL REFERENCES sys_user(id) ON DELETE RESTRICT,
    created_at        TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at        TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    deleted_at        TIMESTAMPTZ
);
CREATE UNIQUE INDEX idx_vpn_network_name_active ON vpn_network(name) WHERE deleted_at IS NULL;
CREATE INDEX idx_vpn_network_status_active ON vpn_network(status) WHERE deleted_at IS NULL;

CREATE TABLE vpn_peer (
    id                UUID PRIMARY KEY,
    network_id        UUID NOT NULL REFERENCES vpn_network(id) ON DELETE CASCADE,
    peer_type         VARCHAR(16) NOT NULL CHECK (peer_type IN ('windows', 'edge')),
    edge_node_id      UUID REFERENCES edge_node(id) ON DELETE CASCADE,
    user_id           UUID REFERENCES sys_user(id) ON DELETE CASCADE,
    name              VARCHAR(100) NOT NULL,
    public_key        VARCHAR(64) NOT NULL DEFAULT '',
    assigned_ipv4     INET NOT NULL,
    allowed_routes    JSONB NOT NULL DEFAULT '[]'::jsonb
                      CHECK (jsonb_typeof(allowed_routes) = 'array'),
    status            VARCHAR(16) NOT NULL DEFAULT 'active'
                      CHECK (status IN ('pending', 'active', 'revoked')),
    config_revision   BIGINT NOT NULL DEFAULT 1,
    last_handshake_at TIMESTAMPTZ,
    created_at        TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at        TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    revoked_at        TIMESTAMPTZ,
    CONSTRAINT ck_vpn_peer_subject CHECK (
        (peer_type = 'edge' AND edge_node_id IS NOT NULL AND user_id IS NULL)
        OR (peer_type = 'windows' AND edge_node_id IS NULL AND user_id IS NOT NULL)
    )
);
CREATE UNIQUE INDEX idx_vpn_peer_network_ip ON vpn_peer(network_id, assigned_ipv4);
CREATE UNIQUE INDEX idx_vpn_peer_public_key ON vpn_peer(public_key) WHERE public_key <> '';
CREATE INDEX idx_vpn_peer_network_status ON vpn_peer(network_id, status);
CREATE INDEX idx_vpn_peer_edge ON vpn_peer(edge_node_id) WHERE edge_node_id IS NOT NULL;
CREATE INDEX idx_vpn_peer_user ON vpn_peer(user_id) WHERE user_id IS NOT NULL;

CREATE TABLE vpn_route (
    id                UUID PRIMARY KEY,
    network_id        UUID NOT NULL REFERENCES vpn_network(id) ON DELETE CASCADE,
    edge_peer_id      UUID NOT NULL REFERENCES vpn_peer(id) ON DELETE CASCADE,
    lan_interface     VARCHAR(32) NOT NULL,
    target_cidr       VARCHAR(18) NOT NULL,
    virtual_cidr      VARCHAR(18) NOT NULL,
    mode              VARCHAR(16) NOT NULL DEFAULT 'nat' CHECK (mode IN ('nat', 'routed')),
    nat_mode          VARCHAR(16) NOT NULL DEFAULT 'masquerade'
                      CHECK (nat_mode IN ('masquerade', 'none')),
    status            VARCHAR(16) NOT NULL DEFAULT 'active'
                      CHECK (status IN ('active', 'error', 'disabled')),
    enabled           BOOLEAN NOT NULL DEFAULT TRUE,
    last_error        TEXT NOT NULL DEFAULT '',
    created_by        UUID NOT NULL REFERENCES sys_user(id) ON DELETE RESTRICT,
    created_at        TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at        TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    CONSTRAINT ck_vpn_route_mode_nat CHECK (
        (mode = 'nat' AND nat_mode = 'masquerade') OR
        (mode = 'routed' AND nat_mode = 'none')
    ),
    UNIQUE (network_id, virtual_cidr),
    UNIQUE (edge_peer_id, target_cidr)
);
CREATE INDEX idx_vpn_route_network ON vpn_route(network_id, enabled);
CREATE INDEX idx_vpn_route_edge_peer ON vpn_route(edge_peer_id, enabled);

CREATE TABLE vpn_access_rule (
    id                UUID PRIMARY KEY,
    peer_id           UUID REFERENCES vpn_peer(id) ON DELETE CASCADE,
    user_id           UUID REFERENCES sys_user(id) ON DELETE CASCADE,
    role_id           UUID REFERENCES sys_role(id) ON DELETE CASCADE,
    route_id          UUID NOT NULL REFERENCES vpn_route(id) ON DELETE CASCADE,
    protocol          VARCHAR(8) NOT NULL DEFAULT 'any'
                      CHECK (protocol IN ('any', 'tcp', 'udp')),
    port_range        VARCHAR(32) NOT NULL DEFAULT '',
    action            VARCHAR(8) NOT NULL CHECK (action IN ('allow', 'deny')),
    priority          INTEGER NOT NULL DEFAULT 100 CHECK (priority >= 0),
    enabled           BOOLEAN NOT NULL DEFAULT TRUE,
    created_by        UUID NOT NULL REFERENCES sys_user(id) ON DELETE RESTRICT,
    created_at        TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at        TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    CONSTRAINT ck_vpn_access_subject CHECK (num_nonnulls(peer_id, user_id, role_id) = 1)
);
CREATE INDEX idx_vpn_access_route ON vpn_access_rule(route_id, enabled, priority);

CREATE TABLE vpn_enrollment (
    id                UUID PRIMARY KEY,
    token_hash        VARCHAR(64) NOT NULL UNIQUE,
    network_id        UUID NOT NULL REFERENCES vpn_network(id) ON DELETE CASCADE,
    allowed_routes    JSONB NOT NULL DEFAULT '[]'::jsonb
                      CHECK (jsonb_typeof(allowed_routes) = 'array'),
    expires_at        TIMESTAMPTZ NOT NULL,
    used_at           TIMESTAMPTZ,
    created_by        UUID NOT NULL REFERENCES sys_user(id) ON DELETE RESTRICT,
    created_at        TIMESTAMPTZ NOT NULL DEFAULT NOW()
);
CREATE INDEX idx_vpn_enrollment_active ON vpn_enrollment(network_id, expires_at)
    WHERE used_at IS NULL;
END
$schema$;
)sql"},
    {"0030_vpn_iot_server_default", R"sql(
DO $schema$
BEGIN
INSERT INTO vpn_network(id, name, overlay_cidr, hub_public_key, hub_endpoint,
                        hub_listen_port, created_by)
VALUES ('00000000-0000-7000-8000-000000000004', 'iot-server', '100.96.0.0/16',
        '', '', 51820, '00000000-0000-7000-8000-000000000002')
ON CONFLICT (id) DO UPDATE
SET name = 'iot-server', overlay_cidr = '100.96.0.0/16', deleted_at = NULL,
    updated_at = NOW();
END
$schema$;
)sql"},
    {"0031_vpn_hub_database_config", R"sql(
DO $schema$
BEGIN
ALTER TABLE vpn_network
    ADD COLUMN IF NOT EXISTS hub_private_key VARCHAR(44) NOT NULL DEFAULT '';
END
$schema$;
)sql"},
    {"0032_edge_node_group", R"sql(
DO $schema$
BEGIN
CREATE TABLE edge_node_group (
    id          UUID PRIMARY KEY,
    name        VARCHAR(100) NOT NULL,
    parent_id   UUID REFERENCES edge_node_group(id) ON DELETE RESTRICT,
    status      status_enum NOT NULL DEFAULT 'enabled',
    sort_order  INTEGER NOT NULL DEFAULT 0 CHECK (sort_order >= 0),
    remark      VARCHAR(500),
    created_by  UUID NOT NULL REFERENCES sys_user(id) ON DELETE RESTRICT,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    deleted_at  TIMESTAMPTZ
);
CREATE UNIQUE INDEX idx_edge_node_group_name_active
    ON edge_node_group(name) WHERE deleted_at IS NULL;
CREATE INDEX idx_edge_node_group_parent
    ON edge_node_group(parent_id, sort_order) WHERE deleted_at IS NULL;
ALTER TABLE edge_node ADD COLUMN group_id UUID REFERENCES edge_node_group(id) ON DELETE RESTRICT;
CREATE INDEX idx_edge_node_group_id ON edge_node(group_id);
END
$schema$;
)sql"},
}};

} // namespace service::config
