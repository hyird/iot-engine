#pragma once

#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <ruvia/web/ModelObject.h>
#include <ruvia/web/db/Db.h>

#include "service/common/http.h"
#include "service/middleware/auth.h"

namespace service::device {

class DeviceService {
  public:
    static DeviceService& instance() {
        static DeviceService service;
        return service;
    }

    ruvia::Task<std::string> list(ruvia::Context& c) {
        const auto rows = co_await c.db().query(
            "SELECT " + itemExpression() +
            "::text FROM iot_device d JOIN iot_link l ON l.id = d.link_id "
            "JOIN iot_protocol_config p ON p.id = d.protocol_config_id "
            "WHERE d.deleted_at IS NULL ORDER BY d.group_id NULLS LAST, d.created_at, d.id");
        co_return page(rows);
    }

    ruvia::Task<std::string> realtime(ruvia::Context& c) {
        const auto rows = co_await c.db().query(R"sql(
SELECT jsonb_build_object(
  'id', id, 'connected', FALSE, 'connectionState', 'offline',
  'elements', '[]'::jsonb, 'can_edit', TRUE, 'can_delete', TRUE,
  'can_share', FALSE, 'can_command', FALSE, 'access_level', 'owner')::text
FROM iot_device WHERE deleted_at IS NULL ORDER BY id)sql");
        co_return page(rows);
    }

    ruvia::Task<std::string> detail(ruvia::Context& c, std::int64_t id) {
        const auto rows = co_await c.db().query(
            "SELECT " + itemExpression() +
                "::text FROM iot_device d JOIN iot_link l ON l.id = d.link_id "
                "JOIN iot_protocol_config p ON p.id = d.protocol_config_id "
                "WHERE d.id = $1 AND d.deleted_at IS NULL LIMIT 1",
            service::common::dbParams(id));
        if (rows.rows().empty())
            service::common::fail(18001, "设备不存在", 404);
        co_return std::string(rows.rows().front()[0].text());
    }

    ruvia::Task<std::string> options(ruvia::Context& c) {
        const auto rows = co_await c.db().query(R"sql(
SELECT jsonb_build_object(
  'id', id, 'name', name, 'device_code', device_code,
  'can_edit', TRUE, 'can_delete', TRUE, 'can_share', FALSE,
  'can_command', FALSE, 'access_level', 'owner')::text
FROM iot_device WHERE deleted_at IS NULL AND status = 'enabled' ORDER BY name)sql");
        co_return page(rows);
    }

    ruvia::Task<void> create(ruvia::Context& c, const ruvia::JsonValue& payload) {
        const auto relation = co_await validate(c, payload, true);
        co_await ensureUnique(c, payload, std::nullopt);
        const auto principal = service::middleware::requireAuth(c);
        (void)relation;
        (void)co_await c.db().execute(R"sql(
WITH body AS (SELECT $1::jsonb AS value)
INSERT INTO iot_device(
  name, device_code, link_id, target_id, protocol_config_id, group_id, status,
  online_timeout, remote_control, modbus_mode, slave_id, timezone,
  heartbeat, registration, remark, created_by)
SELECT value->>'name', value->>'device_code', (value->>'link_id')::bigint,
  NULLIF(value->>'target_id', ''), (value->>'protocol_config_id')::bigint,
  NULLIF(value->>'group_id', '')::bigint, COALESCE(value->>'status', 'enabled'),
  COALESCE((value->>'online_timeout')::integer, 300),
  COALESCE((value->>'remote_control')::boolean, TRUE), NULLIF(value->>'modbus_mode', ''),
  NULLIF(value->>'slave_id', '')::integer, COALESCE(NULLIF(value->>'timezone', ''), '+08:00'),
  COALESCE(value->'heartbeat', '{"mode":"OFF"}'::jsonb),
  COALESCE(value->'registration', '{"mode":"OFF"}'::jsonb), NULLIF(value->>'remark', ''), $2
FROM body)sql",
                                      service::common::dbParams(payload.view(), principal.userId));
    }

    ruvia::Task<void> update(ruvia::Context& c, std::int64_t id, const ruvia::JsonValue& payload) {
        const auto rows =
            co_await c.db().query("SELECT created_by, link_id, protocol_config_id FROM iot_device "
                                  "WHERE id = $1 AND deleted_at IS NULL",
                                  service::common::dbParams(id));
        if (rows.rows().empty())
            service::common::fail(18001, "设备不存在", 404);
        co_await requireOwner(c, toInt(rows.rows().front()[0].text()));
        if (const auto linkId = payload.get<ruvia::Int64>("link_id");
            linkId && static_cast<std::int64_t>(*linkId) != toInt(rows.rows().front()[1].text()))
            service::common::fail(18003, "设备所属链路不可修改", 409);
        if (const auto configId = payload.get<ruvia::Int64>("protocol_config_id");
            configId &&
            static_cast<std::int64_t>(*configId) != toInt(rows.rows().front()[2].text()))
            service::common::fail(18003, "设备类型不可修改", 409);
        (void)co_await validate(c, payload, false);
        co_await ensureUnique(c, payload, id);
        (void)co_await c.db().execute(R"sql(
WITH body AS (SELECT $1::jsonb AS value)
UPDATE iot_device d SET
  name = CASE WHEN body.value ? 'name' THEN body.value->>'name' ELSE d.name END,
  device_code = CASE WHEN body.value ? 'device_code' THEN body.value->>'device_code' ELSE d.device_code END,
  link_id = CASE WHEN body.value ? 'link_id' THEN (body.value->>'link_id')::bigint ELSE d.link_id END,
  target_id = CASE WHEN body.value ? 'target_id' THEN NULLIF(body.value->>'target_id', '') ELSE d.target_id END,
  protocol_config_id = CASE WHEN body.value ? 'protocol_config_id' THEN (body.value->>'protocol_config_id')::bigint ELSE d.protocol_config_id END,
  group_id = CASE WHEN body.value ? 'group_id' THEN NULLIF(body.value->>'group_id', '')::bigint ELSE d.group_id END,
  status = CASE WHEN body.value ? 'status' THEN body.value->>'status' ELSE d.status END,
  online_timeout = CASE WHEN body.value ? 'online_timeout' THEN (body.value->>'online_timeout')::integer ELSE d.online_timeout END,
  remote_control = CASE WHEN body.value ? 'remote_control' THEN (body.value->>'remote_control')::boolean ELSE d.remote_control END,
  modbus_mode = CASE WHEN body.value ? 'modbus_mode' THEN NULLIF(body.value->>'modbus_mode', '') ELSE d.modbus_mode END,
  slave_id = CASE WHEN body.value ? 'slave_id' THEN NULLIF(body.value->>'slave_id', '')::integer ELSE d.slave_id END,
  timezone = CASE WHEN body.value ? 'timezone' THEN body.value->>'timezone' ELSE d.timezone END,
  heartbeat = CASE WHEN body.value ? 'heartbeat' THEN body.value->'heartbeat' ELSE d.heartbeat END,
  registration = CASE WHEN body.value ? 'registration' THEN body.value->'registration' ELSE d.registration END,
  remark = CASE WHEN body.value ? 'remark' THEN NULLIF(body.value->>'remark', '') ELSE d.remark END,
  updated_at = NOW()
FROM body WHERE d.id = $2)sql",
                                      service::common::dbParams(payload.view(), id));
    }

    ruvia::Task<void> remove(ruvia::Context& c, std::int64_t id) {
        const auto rows = co_await c.db().query(
            "SELECT created_by FROM iot_device WHERE id = $1 AND deleted_at IS NULL",
            service::common::dbParams(id));
        if (rows.rows().empty())
            service::common::fail(18001, "设备不存在", 404);
        co_await requireOwner(c, toInt(rows.rows().front()[0].text()));
        (void)co_await c.db().execute(
            "UPDATE iot_device SET deleted_at = NOW(), updated_at = NOW() WHERE id = $1",
            service::common::dbParams(id));
    }

  private:
    struct Relation {
        std::string linkProtocol;
        std::string linkMode;
        std::string configProtocol;
    };

    static std::int64_t toInt(std::string_view value) { return std::stoll(std::string(value)); }

    static std::string page(const ruvia::QueryResult& rows) {
        std::string result = "{\"list\":[";
        bool first = true;
        for (const auto& row : rows.rows()) {
            if (!first)
                result.push_back(',');
            first = false;
            result.append(row[0].text());
        }
        result += "],\"total\":" + std::to_string(rows.rows().size()) + "}";
        return result;
    }

    static std::string itemExpression() {
        return R"sql(jsonb_build_object(
  'id', d.id, 'name', d.name, 'device_code', d.device_code, 'link_id', d.link_id,
  'target_id', d.target_id, 'protocol_config_id', d.protocol_config_id,
  'group_id', d.group_id, 'status', d.status, 'online_timeout', d.online_timeout,
  'remote_control', d.remote_control, 'modbus_mode', d.modbus_mode, 'slave_id', d.slave_id,
  'timezone', d.timezone, 'heartbeat', d.heartbeat, 'registration', d.registration,
  'remark', COALESCE(d.remark, ''), 'created_by', d.created_by,
  'created_at', d.created_at, 'updated_at', d.updated_at,
  'link_name', l.name, 'link_mode', l.mode, 'link_protocol', l.protocol,
  'protocol_name', p.name, 'protocol_type', p.protocol,
  'read_interval', COALESCE((p.config->>'readInterval')::numeric, (p.config->>'pollInterval')::numeric),
  'storage_interval', (p.config->>'storageInterval')::numeric,
  'element_count', CASE p.protocol
      WHEN 'Modbus' THEN jsonb_array_length(COALESCE(p.config->'registers', '[]'::jsonb))
      WHEN 'S7' THEN jsonb_array_length(COALESCE(p.config->'areas', '[]'::jsonb))
      ELSE jsonb_array_length(COALESCE(p.config->'funcs', '[]'::jsonb)) END,
  'connected', FALSE, 'connectionState', 'offline', 'elements', '[]'::jsonb,
  'can_edit', TRUE, 'can_delete', TRUE, 'can_share', FALSE,
  'can_command', FALSE, 'access_level', 'owner'))sql";
    }

    ruvia::Task<Relation> validate(ruvia::Context& c, const ruvia::JsonValue& payload,
                                   bool required) {
        if (!payload.isObject())
            service::common::fail(18002, "请求体必须是对象", 400);
        const auto name = payload.get<ruvia::String>("name");
        if (required && (!name || name->view().empty()))
            service::common::fail(18002, "设备名称不能为空", 400);
        if (name && (name->view().empty() || name->view().size() > 100))
            service::common::fail(18002, "设备名称长度必须在 1 - 100 之间", 400);
        const auto shape = co_await c.db().query(R"sql(
WITH body AS (SELECT $1::jsonb AS value)
SELECT
  NOT (value ? 'status') OR value->>'status' IN ('enabled', 'disabled'),
  NOT (value ? 'online_timeout') OR (jsonb_typeof(value->'online_timeout') = 'number'
      AND (value->>'online_timeout')::numeric BETWEEN 1 AND 86400),
  NOT (value ? 'timezone') OR value->>'timezone' ~ '^([+-](0[0-9]|1[0-3]):[0-5][0-9]|[+-]14:00)$',
  NOT (value ? 'modbus_mode') OR value->'modbus_mode' = 'null'::jsonb
      OR value->>'modbus_mode' IN ('TCP', 'RTU'),
  NOT (value ? 'slave_id') OR value->'slave_id' = 'null'::jsonb
      OR (jsonb_typeof(value->'slave_id') = 'number' AND (value->>'slave_id')::numeric BETWEEN 1 AND 247),
  NOT (value ? 'heartbeat') OR jsonb_typeof(value->'heartbeat') = 'object',
  NOT (value ? 'registration') OR jsonb_typeof(value->'registration') = 'object'
FROM body)sql",
                                                 service::common::dbParams(payload.view()));
        for (std::size_t i = 0; i < 7; ++i)
            if (shape.rows().front()[i].text() != "t")
                service::common::fail(18002, "设备参数无效", 400);

        const auto linkId = payload.get<ruvia::Int64>("link_id");
        const auto configId = payload.get<ruvia::Int64>("protocol_config_id");
        if (required && (!linkId || !configId))
            service::common::fail(18002, "链路和设备类型不能为空", 400);
        if (!linkId || !configId)
            co_return Relation{};
        const auto relation =
            co_await c.db().query(R"sql(
SELECT l.protocol, l.mode, p.protocol
FROM iot_link l CROSS JOIN iot_protocol_config p
WHERE l.id = $1 AND l.deleted_at IS NULL AND p.id = $2 AND p.deleted_at IS NULL LIMIT 1)sql",
                                  service::common::dbParams(static_cast<std::int64_t>(*linkId),
                                                            static_cast<std::int64_t>(*configId)));
        if (relation.rows().empty())
            service::common::fail(18003, "链路或设备类型不存在", 400);
        Relation result{std::string(relation.rows().front()[0].text()),
                        std::string(relation.rows().front()[1].text()),
                        std::string(relation.rows().front()[2].text())};
        if (result.linkProtocol != result.configProtocol)
            service::common::fail(18003, "链路协议与设备类型不一致", 409);
        const auto code = payload.get<ruvia::String>("device_code");
        if (!code || code->view().empty() || code->view().size() > 100)
            service::common::fail(18002, "设备编码长度必须在 1 - 100 之间", 400);
        for (const auto character : code->view())
            if (!std::isalnum(static_cast<unsigned char>(character)))
                service::common::fail(18002, "设备编码只能包含字母和数字", 400);
        if (const auto groupId = payload.get<ruvia::Int64>("group_id")) {
            const auto group = co_await c.db().query(
                "SELECT 1 FROM iot_device_group WHERE id = $1 AND deleted_at IS NULL",
                service::common::dbParams(static_cast<std::int64_t>(*groupId)));
            if (group.rows().empty())
                service::common::fail(18003, "设备分组不存在", 400);
        }
        co_return result;
    }

    ruvia::Task<void> ensureUnique(ruvia::Context& c, const ruvia::JsonValue& payload,
                                   std::optional<std::int64_t> excludedId) {
        const auto name = payload.get<ruvia::String>("name");
        const auto code = payload.get<ruvia::String>("device_code");
        if (!name && !code)
            co_return;
        const std::string nameValue = name ? std::string(name->view()) : std::string{};
        const std::string codeValue = code ? std::string(code->view()) : std::string{};
        const auto rows = co_await c.db().query(
            R"sql(
SELECT 1 FROM iot_device
WHERE deleted_at IS NULL AND ($1::bigint = 0 OR id <> $1)
  AND (($2 <> '' AND name = $2) OR ($3 <> '' AND device_code = $3)) LIMIT 1)sql",
            service::common::dbParams(excludedId.value_or(0), std::string_view(nameValue),
                                      std::string_view(codeValue)));
        if (!rows.rows().empty())
            service::common::fail(18004, "设备名称或编码已存在", 409);
    }

    ruvia::Task<void> requireOwner(ruvia::Context& c, std::int64_t ownerId) {
        const auto principal = service::middleware::requireAuth(c);
        if (principal.userId == ownerId)
            co_return;
        const auto rows = co_await c.db().query(R"sql(
SELECT EXISTS (SELECT 1 FROM sys_user_role ur JOIN sys_role r ON r.id = ur.role_id
WHERE ur.user_id = $1 AND r.code = 'superadmin' AND r.status = 'enabled'
AND r.deleted_at IS NULL))sql",
                                                service::common::dbParams(principal.userId));
        if (rows.rows().front()[0].text() != "t")
            service::common::fail(18005, "只能管理自己创建的设备", 403);
    }
};

inline DeviceService& deviceService() { return DeviceService::instance(); }

} // namespace service::device
