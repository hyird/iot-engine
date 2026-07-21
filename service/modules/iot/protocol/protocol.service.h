#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/web/ModelObject.h>
#include <ruvia/web/db/Db.h>

#include "service/common/http.h"
#include "service/middleware/auth.h"

namespace service::protocol {

class ProtocolService {
  public:
    static ProtocolService& instance() {
        static ProtocolService service;
        return service;
    }

    ruvia::Task<std::string> list(ruvia::Context& c, std::int64_t page, std::int64_t pageSize,
                                  std::optional<std::string> protocol) {
        page = std::max<std::int64_t>(1, page);
        pageSize = std::clamp<std::int64_t>(pageSize, 1, 1000);
        std::string where = " WHERE deleted_at IS NULL";
        std::vector<ruvia::DbValue> params;
        if (protocol && !protocol->empty()) {
            params.emplace_back(*protocol);
            where += " AND protocol = $1";
        }

        const auto countRows =
            co_await c.db().query("SELECT COUNT(*) FROM iot_protocol_config" + where, params);
        const auto total = toInt(countRows.rows().front()[0].text());
        auto listParams = params;
        listParams.emplace_back(pageSize);
        const auto limitIndex = listParams.size();
        listParams.emplace_back((page - 1) * pageSize);
        const auto offsetIndex = listParams.size();
        const auto rows =
            co_await c.db().query("SELECT " + itemExpression() + "::text FROM iot_protocol_config" +
                                      where + " ORDER BY id LIMIT $" + std::to_string(limitIndex) +
                                      " OFFSET $" + std::to_string(offsetIndex),
                                  listParams);

        std::string result = "{\"list\":[";
        bool first = true;
        for (const auto& row : rows.rows()) {
            if (!first)
                result.push_back(',');
            first = false;
            result.append(row[0].text());
        }
        result += "],\"total\":" + std::to_string(total) + ",\"page\":" + std::to_string(page) +
                  ",\"pageSize\":" + std::to_string(pageSize) + ",\"totalPages\":" +
                  std::to_string(total == 0 ? 0 : (total + pageSize - 1) / pageSize) + "}";
        co_return result;
    }

    ruvia::Task<std::string> detail(ruvia::Context& c, std::int64_t id) {
        const auto rows = co_await c.db().query(
            "SELECT " + itemExpression() +
                "::text FROM iot_protocol_config WHERE id = $1 AND deleted_at IS NULL LIMIT 1",
            service::common::dbParams(id));
        if (rows.rows().empty())
            service::common::fail(16001, "协议配置不存在", 404);
        co_return std::string(rows.rows().front()[0].text());
    }

    ruvia::Task<std::string> options(ruvia::Context& c, const std::string& protocol,
                                     std::int64_t page, std::int64_t pageSize) {
        page = std::max<std::int64_t>(1, page);
        pageSize = std::clamp<std::int64_t>(pageSize, 1, 1000);
        const auto countRows = co_await c.db().query(
            "SELECT COUNT(*) FROM iot_protocol_config WHERE deleted_at IS NULL AND protocol = $1",
            service::common::dbParams(protocol));
        const auto total = toInt(countRows.rows().front()[0].text());
        const auto rows = co_await c.db().query(
            R"sql(
SELECT jsonb_build_object('id', id, 'name', name)::text
FROM iot_protocol_config
WHERE deleted_at IS NULL AND enabled = TRUE AND protocol = $1
ORDER BY name LIMIT $2 OFFSET $3)sql",
            service::common::dbParams(protocol, pageSize, (page - 1) * pageSize));
        std::string result = "{\"list\":[";
        bool first = true;
        for (const auto& row : rows.rows()) {
            if (!first)
                result.push_back(',');
            first = false;
            result.append(row[0].text());
        }
        result += "],\"total\":" + std::to_string(total) + ",\"page\":" + std::to_string(page) +
                  ",\"pageSize\":" + std::to_string(pageSize) + "}";
        co_return result;
    }

    ruvia::Task<void> create(ruvia::Context& c, const ruvia::JsonValue& payload) {
        if (!payload.isObject())
            service::common::fail(16002, "请求体必须是对象", 400);
        const auto protocol = requiredString(payload, "protocol", "协议不能为空");
        const auto name = requiredString(payload, "name", "配置名称不能为空");
        validateProtocol(protocol);
        validateName(name);
        co_await validateConfig(c, payload.view(), protocol, true);
        co_await ensureNameAvailable(c, name, std::nullopt);
        const auto principal = service::middleware::requireAuth(c);
        (void)co_await c.db().execute(R"sql(
WITH body AS (SELECT $1::jsonb AS value)
INSERT INTO iot_protocol_config(protocol, name, enabled, config, remark, created_by)
SELECT value->>'protocol', value->>'name', COALESCE((value->>'enabled')::boolean, TRUE),
       value->'config', NULLIF(value->>'remark', ''), $2
FROM body)sql",
                                      service::common::dbParams(payload.view(), principal.userId));
    }

    ruvia::Task<void> update(ruvia::Context& c, std::int64_t id, const ruvia::JsonValue& payload) {
        if (!payload.isObject())
            service::common::fail(16002, "请求体必须是对象", 400);
        const auto existing = co_await c.db().query(
            "SELECT protocol, created_by FROM iot_protocol_config WHERE id = $1 AND deleted_at IS "
            "NULL LIMIT 1",
            service::common::dbParams(id));
        if (existing.rows().empty())
            service::common::fail(16001, "协议配置不存在", 404);
        co_await requireOwner(c, toInt(existing.rows().front()[1].text()));
        const std::string protocol(existing.rows().front()[0].text());
        if (const auto requested = payload.get<ruvia::String>("protocol");
            requested && !requested->view().empty() && requested->view() != protocol)
            service::common::fail(16006, "协议类型不可修改", 409);
        if (const auto name = payload.get<ruvia::String>("name")) {
            validateName(name->view());
            co_await ensureNameAvailable(c, std::string(name->view()), id);
        }
        co_await validateConfig(c, payload.view(), protocol, false);
        (void)co_await c.db().execute(R"sql(
WITH body AS (SELECT $1::jsonb AS value)
UPDATE iot_protocol_config p
SET name = CASE WHEN body.value ? 'name' THEN body.value->>'name' ELSE p.name END,
    enabled = CASE WHEN body.value ? 'enabled' THEN (body.value->>'enabled')::boolean ELSE p.enabled END,
    config = CASE WHEN body.value ? 'config' THEN p.config || (body.value->'config') ELSE p.config END,
    remark = CASE WHEN body.value ? 'remark' THEN NULLIF(body.value->>'remark', '') ELSE p.remark END,
    updated_at = NOW()
FROM body WHERE p.id = $2)sql",
                                      service::common::dbParams(payload.view(), id));
    }

    ruvia::Task<void> remove(ruvia::Context& c, std::int64_t id) {
        const auto existing = co_await c.db().query("SELECT created_by FROM iot_protocol_config "
                                                    "WHERE id = $1 AND deleted_at IS NULL LIMIT 1",
                                                    service::common::dbParams(id));
        if (existing.rows().empty())
            service::common::fail(16001, "协议配置不存在", 404);
        co_await requireOwner(c, toInt(existing.rows().front()[0].text()));
        (void)co_await c.db().execute(
            "UPDATE iot_protocol_config SET deleted_at = NOW(), updated_at = NOW() WHERE id = $1",
            service::common::dbParams(id));
    }

  private:
    static std::int64_t toInt(std::string_view value) { return std::stoll(std::string(value)); }

    static std::string itemExpression() {
        return R"sql(jsonb_build_object(
    'id', id, 'protocol', protocol, 'name', name, 'enabled', enabled,
    'config', config, 'remark', COALESCE(remark, ''),
    'created_at', created_at, 'updated_at', updated_at))sql";
    }

    static std::string requiredString(const ruvia::JsonValue& payload, std::string_view field,
                                      std::string_view message) {
        const auto value = payload.get<ruvia::String>(field);
        if (!value || value->view().empty())
            service::common::fail(16002, std::string(message), 400);
        return std::string(value->view());
    }

    static void validateProtocol(std::string_view protocol) {
        if (protocol != "SL651" && protocol != "Modbus" && protocol != "S7")
            service::common::fail(16003, "不支持的协议类型", 400);
    }

    static void validateName(std::string_view name) {
        if (name.empty() || name.size() > 64)
            service::common::fail(16002, "配置名称长度必须在 1 - 64 之间", 400);
    }

    ruvia::Task<void> validateConfig(ruvia::Context& c, std::string_view body,
                                     const std::string& protocol, bool required) {
        const auto shape = co_await c.db().query(R"sql(
WITH body AS (SELECT $1::jsonb AS value)
SELECT body.value ? 'config', jsonb_typeof(body.value->'config')
FROM body)sql",
                                                 service::common::dbParams(body));
        const bool present = shape.rows().front()[0].text() == "t";
        if (!present) {
            if (required)
                service::common::fail(16004, "config 不能为空", 400);
            co_return;
        }
        if (shape.rows().front()[1].text() != "object")
            service::common::fail(16004, "config 必须是对象", 400);
        if (protocol == "SL651") {
            const auto rows = co_await c.db().query(R"sql(
WITH cfg AS (SELECT ($1::jsonb)->'config' AS value)
SELECT
  (NOT (value ? 'responseMode') OR value->>'responseMode' IN ('M1', 'M2', 'M3', 'M4')),
  (NOT (value ? 'funcs') OR jsonb_typeof(value->'funcs') = 'array'),
  value ? 'responseMode', value ? 'funcs'
FROM cfg)sql",
                                                    service::common::dbParams(body));
            const auto& row = rows.rows().front();
            if (row[0].text() != "t" || row[1].text() != "t" ||
                (required && (row[2].text() != "t" || row[3].text() != "t")))
                service::common::fail(16004, "SL651 配置无效", 400);
            co_return;
        }
        if (protocol == "S7") {
            const auto rows = co_await c.db().query(R"sql(
WITH cfg AS (SELECT ($1::jsonb)->'config' AS value)
SELECT
  (NOT (value ? 'plcModel') OR value->>'plcModel' IN
      ('S7-200', 'S7-300', 'S7-400', 'S7-1200', 'S7-1500')),
  (NOT (value ? 'connection') OR jsonb_typeof(value->'connection') = 'object'),
  (NOT (value ? 'areas') OR jsonb_typeof(value->'areas') = 'array'),
  value ? 'plcModel', value ? 'connection', value ? 'areas'
FROM cfg)sql",
                                                    service::common::dbParams(body));
            const auto& row = rows.rows().front();
            if (row[0].text() != "t" || row[1].text() != "t" || row[2].text() != "t" ||
                (required &&
                 (row[3].text() != "t" || row[4].text() != "t" || row[5].text() != "t")))
                service::common::fail(16004, "S7 配置无效", 400);
            co_return;
        }
        if (protocol != "Modbus")
            co_return;

        const auto configRows = co_await c.db().query(R"sql(
WITH cfg AS (SELECT ($1::jsonb)->'config' AS value)
SELECT
    COALESCE(value->>'byteOrder', '') IN
        ('BIG_ENDIAN', 'LITTLE_ENDIAN', 'BIG_ENDIAN_BYTE_SWAP', 'LITTLE_ENDIAN_BYTE_SWAP'),
    COALESCE(jsonb_typeof(value->'registers') = 'array', FALSE),
    COALESCE(jsonb_typeof(value->'packet') = 'object', TRUE)
FROM cfg)sql",
                                                      service::common::dbParams(body));
        const auto& config = configRows.rows().front();
        if (config[0].text() != "t")
            service::common::fail(16004, "Modbus 配置的 byteOrder 无效", 400);
        if (config[1].text() != "t")
            service::common::fail(16004, "Modbus 配置的 registers 必须是数组", 400);
        if (config[2].text() != "t")
            service::common::fail(16004, "Modbus 配置的 packet 必须是对象", 400);

        const auto registers = co_await c.db().query(R"sql(
WITH cfg AS (SELECT ($1::jsonb)->'config' AS value),
registers AS (SELECT item FROM cfg, jsonb_array_elements(value->'registers') AS item)
SELECT COALESCE(bool_and(
    jsonb_typeof(item) = 'object'
    AND jsonb_typeof(item->'id') = 'string' AND COALESCE(item->>'id', '') <> ''
    AND jsonb_typeof(item->'name') = 'string' AND COALESCE(item->>'name', '') <> ''
    AND item->>'registerType' IN ('COIL', 'DISCRETE_INPUT', 'HOLDING_REGISTER', 'INPUT_REGISTER')
    AND item->>'dataType' IN ('BOOL', 'INT16', 'UINT16', 'INT32', 'UINT32', 'FLOAT32',
                              'INT64', 'UINT64', 'DOUBLE')
    AND CASE WHEN jsonb_typeof(item->'address') = 'number'
             THEN (item->>'address')::numeric BETWEEN 0 AND 65535 ELSE FALSE END
    AND CASE WHEN jsonb_typeof(item->'quantity') = 'number'
             THEN (item->>'quantity')::numeric BETWEEN 1 AND 4 ELSE FALSE END
), TRUE) FROM registers)sql",
                                                     service::common::dbParams(body));
        if (registers.rows().front()[0].text() != "t")
            service::common::fail(16004, "Modbus 寄存器配置无效", 400);
    }

    ruvia::Task<void> ensureNameAvailable(ruvia::Context& c, const std::string& name,
                                          std::optional<std::int64_t> excludedId) {
        std::string sql =
            "SELECT 1 FROM iot_protocol_config WHERE name = $1 AND deleted_at IS NULL";
        auto params = service::common::dbParams(name);
        if (excludedId) {
            params.emplace_back(*excludedId);
            sql += " AND id <> $2";
        }
        sql += " LIMIT 1";
        const auto rows = co_await c.db().query(sql, params);
        if (!rows.rows().empty())
            service::common::fail(16005, "配置名称已存在", 409);
    }

    ruvia::Task<void> requireOwner(ruvia::Context& c, std::int64_t ownerId) {
        const auto principal = service::middleware::requireAuth(c);
        if (principal.userId == ownerId)
            co_return;
        const auto rows = co_await c.db().query(R"sql(
SELECT EXISTS (
    SELECT 1 FROM sys_user_role ur JOIN sys_role r ON r.id = ur.role_id
    WHERE ur.user_id = $1 AND r.code = 'superadmin'
      AND r.status = 'enabled' AND r.deleted_at IS NULL
))sql",
                                                service::common::dbParams(principal.userId));
        if (rows.rows().empty() || rows.rows().front()[0].text() != "t")
            service::common::fail(16007, "只能修改或删除自己创建的协议配置", 403);
    }
};

inline ProtocolService& protocolService() { return ProtocolService::instance(); }

} // namespace service::protocol
