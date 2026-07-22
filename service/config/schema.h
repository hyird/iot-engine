#pragma once

#include <array>
#include <ruvia/web/db/DbMigration.h>

namespace service::config {

inline constexpr std::array<ruvia::DbMigration, 4> kSchemaMigrations{{
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
}};

} // namespace service::config
