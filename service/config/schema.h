#pragma once

#include <array>
#include <ruvia/web/db/DbMigration.h>

namespace service::config {

inline constexpr std::array<ruvia::DbMigration, 7> kSchemaMigrations{{
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
    id          UUID PRIMARY KEY,
    name        VARCHAR(100) NOT NULL,
    mode        VARCHAR(20) NOT NULL CHECK (mode IN ('TCP Server', 'TCP Client')),
    protocol    VARCHAR(20) NOT NULL CHECK (protocol IN ('SL651', 'Modbus', 'S7')),
    ip          VARCHAR(50) NOT NULL DEFAULT '',
    port        INTEGER NOT NULL DEFAULT 0 CHECK (port BETWEEN 0 AND 65535),
    targets     JSONB NOT NULL DEFAULT '[]'::jsonb CHECK (jsonb_typeof(targets) = 'array'),
    usage       VARCHAR(20) NOT NULL DEFAULT 'device',
    status      status_enum NOT NULL DEFAULT 'enabled',
    agent_id            UUID,
    agent_interface     VARCHAR(100),
    agent_bind_ip       VARCHAR(50),
    agent_prefix_length INTEGER,
    agent_gateway       VARCHAR(50),
    created_by  UUID NOT NULL REFERENCES sys_user(id) ON DELETE RESTRICT,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    deleted_at  TIMESTAMPTZ,
    CONSTRAINT ck_link_endpoint CHECK (
        (mode = 'TCP Server' AND ip = '0.0.0.0' AND port BETWEEN 1 AND 65535
         AND jsonb_array_length(targets) = 0)
        OR
        (mode = 'TCP Client' AND ip = '' AND port = 0
         AND jsonb_array_length(targets) > 0)
    )
);
CREATE INDEX idx_link_mode ON link(mode);
CREATE INDEX idx_link_deleted ON link(deleted_at);
CREATE UNIQUE INDEX idx_link_name ON link(name) WHERE deleted_at IS NULL;
CREATE INDEX idx_link_status_active ON link(status) WHERE deleted_at IS NULL;
CREATE INDEX idx_link_protocol ON link(protocol);
CREATE INDEX idx_link_usage ON link(usage);

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
ALTER TABLE device ALTER COLUMN link_id DROP NOT NULL;
ALTER TABLE device_data ALTER COLUMN link_id DROP NOT NULL;
ALTER TABLE device
    ADD COLUMN edge_node_id UUID REFERENCES edge_node(id) ON DELETE RESTRICT,
    ADD COLUMN edge_endpoint JSONB NOT NULL DEFAULT '{}'::jsonb
        CHECK (jsonb_typeof(edge_endpoint) = 'object');
ALTER TABLE device ADD CONSTRAINT ck_device_connection_source CHECK (
    (edge_node_id IS NULL AND link_id IS NOT NULL AND edge_endpoint = '{}'::jsonb)
    OR
    (edge_node_id IS NOT NULL AND link_id IS NULL AND edge_endpoint <> '{}'::jsonb)
);
CREATE INDEX idx_device_edge_node ON device(edge_node_id) WHERE deleted_at IS NULL;

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
}};

} // namespace service::config
