#pragma once

#include <array>
#include <ruvia/web/db/DbMigration.h>

namespace service::config {

inline constexpr std::array<ruvia::DbMigration, 1> kSchemaMigrations{{
    {"0001_initial_schema", R"sql(
DO $schema$
BEGIN
CREATE EXTENSION IF NOT EXISTS timescaledb;

CREATE TABLE sys_role (
    id          UUID PRIMARY KEY,
    code        VARCHAR(64) NOT NULL UNIQUE,
    name        VARCHAR(128) NOT NULL,
    description VARCHAR(500),
    status      VARCHAR(20) NOT NULL DEFAULT 'enabled'
                CHECK (status IN ('enabled', 'disabled')),
    permissions JSONB NOT NULL DEFAULT '[]'::jsonb
                CHECK (jsonb_typeof(permissions) = 'array'),
    created_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    deleted_at  TIMESTAMPTZ
);

CREATE TABLE sys_user (
    id            UUID PRIMARY KEY,
    username      VARCHAR(50) NOT NULL UNIQUE,
    password_hash VARCHAR(255) NOT NULL,
    nickname      VARCHAR(100),
    phone         VARCHAR(20),
    email         VARCHAR(100),
    status        VARCHAR(20) NOT NULL DEFAULT 'enabled'
                  CHECK (status IN ('enabled', 'disabled')),
    profile       JSONB NOT NULL DEFAULT '{}'::jsonb
                  CHECK (jsonb_typeof(profile) = 'object'),
    created_at    TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at    TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    deleted_at    TIMESTAMPTZ
);

CREATE TABLE sys_user_role (
    user_id UUID NOT NULL REFERENCES sys_user(id) ON DELETE CASCADE,
    role_id UUID NOT NULL REFERENCES sys_role(id) ON DELETE CASCADE,
    PRIMARY KEY (user_id, role_id)
);

CREATE TABLE sys_dept (
    id          UUID PRIMARY KEY,
    name        VARCHAR(128) NOT NULL,
    code        VARCHAR(64) UNIQUE,
    parent_id   UUID REFERENCES sys_dept(id) ON DELETE RESTRICT,
    leader_id   UUID REFERENCES sys_user(id) ON DELETE SET NULL,
    sort_order  INTEGER NOT NULL DEFAULT 0 CHECK (sort_order >= 0),
    status      VARCHAR(20) NOT NULL DEFAULT 'enabled'
                CHECK (status IN ('enabled', 'disabled')),
    created_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    deleted_at  TIMESTAMPTZ
);

CREATE TABLE iot_link (
    id          UUID PRIMARY KEY,
    name        VARCHAR(100) NOT NULL,
    mode        VARCHAR(20) NOT NULL CHECK (mode IN ('TCP Server', 'TCP Client')),
    protocol    VARCHAR(20) NOT NULL CHECK (protocol IN ('SL651', 'Modbus', 'S7')),
    ip          VARCHAR(50) NOT NULL DEFAULT '',
    port        INTEGER NOT NULL DEFAULT 0 CHECK (port BETWEEN 0 AND 65535),
    targets     JSONB NOT NULL DEFAULT '[]'::jsonb CHECK (jsonb_typeof(targets) = 'array'),
    status      VARCHAR(20) NOT NULL DEFAULT 'enabled'
                CHECK (status IN ('enabled', 'disabled')),
    created_by  UUID NOT NULL REFERENCES sys_user(id) ON DELETE RESTRICT,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    deleted_at  TIMESTAMPTZ,
    CONSTRAINT ck_iot_link_endpoint CHECK (
        (mode = 'TCP Server' AND ip <> '' AND port BETWEEN 1 AND 65535
         AND jsonb_array_length(targets) = 0)
        OR
        (mode = 'TCP Client' AND ip = '' AND port = 0
         AND jsonb_array_length(targets) > 0)
    )
);

CREATE TABLE iot_protocol_config (
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

CREATE TABLE iot_device_group (
    id          UUID PRIMARY KEY,
    name        VARCHAR(100) NOT NULL,
    parent_id   UUID REFERENCES iot_device_group(id) ON DELETE RESTRICT,
    status      VARCHAR(20) NOT NULL DEFAULT 'enabled'
                CHECK (status IN ('enabled', 'disabled')),
    sort_order  INTEGER NOT NULL DEFAULT 0 CHECK (sort_order >= 0),
    remark      TEXT,
    created_by  UUID NOT NULL REFERENCES sys_user(id) ON DELETE RESTRICT,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    deleted_at  TIMESTAMPTZ
);

CREATE TABLE iot_device (
    id                  UUID PRIMARY KEY,
    name                VARCHAR(100) NOT NULL,
    device_code         VARCHAR(100) NOT NULL CHECK (device_code ~ '^[A-Za-z0-9]+$'),
    link_id             UUID NOT NULL REFERENCES iot_link(id) ON DELETE RESTRICT,
    target_id           VARCHAR(100),
    protocol_config_id  UUID NOT NULL REFERENCES iot_protocol_config(id) ON DELETE RESTRICT,
    group_id            UUID REFERENCES iot_device_group(id) ON DELETE RESTRICT,
    status              VARCHAR(20) NOT NULL DEFAULT 'enabled'
                        CHECK (status IN ('enabled', 'disabled')),
    online_timeout      INTEGER NOT NULL DEFAULT 300 CHECK (online_timeout BETWEEN 1 AND 86400),
    remote_control      BOOLEAN NOT NULL DEFAULT TRUE,
    modbus_mode         VARCHAR(10) CHECK (modbus_mode IS NULL OR modbus_mode IN ('TCP', 'RTU')),
    slave_id            INTEGER CHECK (slave_id IS NULL OR slave_id BETWEEN 1 AND 247),
    timezone            VARCHAR(6) NOT NULL DEFAULT '+08:00',
    heartbeat           JSONB NOT NULL DEFAULT '{"mode":"OFF"}'::jsonb
                        CHECK (jsonb_typeof(heartbeat) = 'object'),
    registration        JSONB NOT NULL DEFAULT '{"mode":"OFF"}'::jsonb
                        CHECK (jsonb_typeof(registration) = 'object'),
    remark              TEXT,
    created_by          UUID NOT NULL REFERENCES sys_user(id) ON DELETE RESTRICT,
    created_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    deleted_at          TIMESTAMPTZ
);

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

INSERT INTO sys_user_role(user_id, role_id)
VALUES ('00000000-0000-7000-8000-000000000002',
        '00000000-0000-7000-8000-000000000001');

EXECUTE format('ALTER DATABASE %I SET timezone TO %L', current_database(), 'UTC');
PERFORM set_config('TimeZone', 'UTC', false);
END
$schema$;
)sql"},
}};

} // namespace service::config
