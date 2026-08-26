#pragma once

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/web/ModelObject.h>
#include <ruvia/web/db/Db.h>

#include "service/features/edge/config.h"
#include "service/features/access/contract.h"
#include "service/features/event/config.h"
#include "service/features/telemetry/latest.h"
#include "service/common/http.h"
#include "service/common/uuid.h"
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
            co_await c.db().query("SELECT COUNT(*) FROM protocol_config" + where, params);
        const auto total = toInt(countRows.front()[0].value().value_or(std::string_view{}));
        auto listParams = params;
        listParams.emplace_back(pageSize);
        const auto limitIndex = listParams.size();
        listParams.emplace_back((page - 1) * pageSize);
        const auto offsetIndex = listParams.size();
        const auto rows = co_await c.db().query(
            "SELECT " + itemExpression() + "::text FROM protocol_config" + where +
                " ORDER BY id DESC LIMIT $" + std::to_string(limitIndex) + " OFFSET $" +
                std::to_string(offsetIndex),
            listParams);

        std::string result = "{\"list\":[";
        bool first = true;
        for (const auto& row : rows) {
            if (!first)
                result.push_back(',');
            first = false;
            result.append(row[0].value().value_or(std::string_view{}));
        }
        result += "],\"total\":" + std::to_string(total) + ",\"page\":" + std::to_string(page) +
                  ",\"pageSize\":" + std::to_string(pageSize) + ",\"totalPages\":" +
                  std::to_string(total == 0 ? 0 : (total + pageSize - 1) / pageSize) + "}";
        co_return result;
    }

    ruvia::Task<std::string> detail(ruvia::Context& c, std::string_view id) {
        const auto rows = co_await c.db().query(
            "SELECT " + itemExpression() +
                "::text FROM protocol_config WHERE id = $1 AND deleted_at IS NULL LIMIT 1",
            service::common::dbParams(id));
        if (rows.empty())
            service::common::fail(16001, "协议配置不存在", 404);
        co_return std::string(rows.front()[0].value().value_or(std::string_view{}));
    }

    ruvia::Task<std::string> options(ruvia::Context& c, const std::string& protocol,
                                     std::int64_t page, std::int64_t pageSize) {
        page = std::max<std::int64_t>(1, page);
        pageSize = std::clamp<std::int64_t>(pageSize, 1, 1000);
        const auto countRows = co_await c.db().query(
            "SELECT COUNT(*) FROM protocol_config WHERE deleted_at IS NULL AND protocol = $1",
            service::common::dbParams(protocol));
        const auto total = toInt(countRows.front()[0].value().value_or(std::string_view{}));
        const auto rows = co_await c.db().query(
            R"sql(
SELECT jsonb_build_object('id', id, 'name', name)::text
FROM protocol_config
WHERE deleted_at IS NULL AND enabled = TRUE AND protocol = $1
ORDER BY name LIMIT $2 OFFSET $3)sql",
            service::common::dbParams(protocol, pageSize, (page - 1) * pageSize));
        std::string result = "{\"list\":[";
        bool first = true;
        for (const auto& row : rows) {
            if (!first)
                result.push_back(',');
            first = false;
            result.append(row[0].value().value_or(std::string_view{}));
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
        validateOptionalNullableString(payload, "remark", "remark 必须是字符串或 null", 500);
        co_await validateConfig(c, payload.view(), protocol, true);
        co_await ensureNameAvailable(c, name, std::nullopt);
        const auto principal = service::middleware::requireAuth(c);
        const auto id = service::common::nextUuidV7();
        auto transaction = co_await c.db().beginTransaction();
        (void)co_await transaction.execute(
            R"sql(
WITH body AS (SELECT $1::jsonb AS value)
INSERT INTO protocol_config(id, protocol, name, enabled, config, remark, created_by)
SELECT $3::uuid, value->>'protocol', value->>'name',
       CASE WHEN NOT (value ? 'enabled') THEN TRUE
            WHEN jsonb_typeof(value->'enabled') = 'boolean' THEN value->>'enabled' = 'true'
            ELSE FALSE END,
       value->'config',
       NULLIF(value->>'remark', ''), $2
FROM body)sql",
            service::common::dbParams(payload.view(), principal.userId, id));
        co_await service::message::enqueueConfigEvent(transaction, "protocol", "created", id);
        co_await transaction.commit();
    }

    ruvia::Task<void> update(ruvia::Context& c, std::string_view id,
                             const ruvia::JsonValue& payload) {
        if (!payload.isObject())
            service::common::fail(16002, "请求体必须是对象", 400);
        const auto existing = co_await c.db().query(
            "SELECT protocol, created_by FROM protocol_config WHERE id = $1 AND deleted_at IS "
            "NULL LIMIT 1",
            service::common::dbParams(id));
        if (existing.empty())
            service::common::fail(16001, "协议配置不存在", 404);
        co_await requireOwner(c, existing.front()[1].value().value_or(std::string_view{}));
        const std::string protocol(existing.front()[0].value().value_or(std::string_view{}));
        if (const auto requested =
                optionalString(payload, "protocol", "protocol 必须是字符串", 16)) {
            if (requested->empty())
                service::common::fail(16003, "protocol 不能为空", 400);
            if (*requested != protocol)
                service::common::fail(16006, "协议类型不可修改", 409);
        }
        if (const auto name = optionalString(payload, "name", "name 必须是字符串", 64)) {
            validateName(*name);
            co_await ensureNameAvailable(c, *name, std::string(id));
        }
        validateOptionalNullableString(payload, "remark", "remark 必须是字符串或 null", 500);
        co_await validateConfig(c, payload.view(), protocol, false);
        auto transaction = co_await c.db().beginTransaction();
        (void)co_await transaction.execute(R"sql(
WITH body AS (SELECT $1::jsonb AS value)
UPDATE protocol_config p
SET name = CASE WHEN body.value ? 'name' THEN body.value->>'name' ELSE p.name END,
    enabled = CASE WHEN NOT (body.value ? 'enabled') THEN p.enabled
                   WHEN jsonb_typeof(body.value->'enabled') = 'boolean'
                   THEN body.value->>'enabled' = 'true'
                   ELSE p.enabled END,
    config = CASE WHEN body.value ? 'config' THEN p.config || (body.value->'config') ELSE p.config END,
    remark = CASE WHEN body.value ? 'remark' THEN NULLIF(body.value->>'remark', '') ELSE p.remark END,
    updated_at = NOW()
FROM body WHERE p.id = $2)sql",
                                       service::common::dbParams(payload.view(), id));
        co_await service::message::enqueueConfigEvent(transaction, "protocol", "updated", id);
        co_await transaction.commit();
        try {
            co_await service::telemetry::latest::projectProtocol(c, id);
        } catch (...) {
            // Startup hydration repairs Redis read models if Redis is temporarily unavailable.
        }
        try {
            co_await syncEdgeNodes(c, id);
        } catch (const std::exception& error) {
            logPostUpdateFailure("edge-sync", id, error.what());
        } catch (...) {
            logPostUpdateFailure("edge-sync", id, "unknown exception");
        }
    }

    ruvia::Task<void> remove(ruvia::Context& c, std::string_view id) {
        const auto existing = co_await c.db().query("SELECT created_by FROM protocol_config "
                                                    "WHERE id = $1 AND deleted_at IS NULL LIMIT 1",
                                                    service::common::dbParams(id));
        if (existing.empty())
            service::common::fail(16001, "协议配置不存在", 404);
        co_await requireOwner(c, existing.front()[0].value().value_or(std::string_view{}));
        const auto used = co_await c.db().query(
            "SELECT EXISTS (SELECT 1 FROM device WHERE protocol_config_id = $1::uuid "
            "AND deleted_at IS NULL)",
            service::common::dbParams(id));
        if (used.front()[0].value().value_or(std::string_view{}) == "t")
            service::common::fail(16008, "协议配置已被设备使用，请先删除关联设备", 409);
        auto transaction = co_await c.db().beginTransaction();
        (void)co_await transaction.execute(
            "UPDATE protocol_config SET deleted_at = NOW(), updated_at = NOW() WHERE id = $1",
            service::common::dbParams(id));
        co_await service::message::enqueueConfigEvent(transaction, "protocol", "deleted", id);
        co_await transaction.commit();
    }

  private:
    static ruvia::Task<void> syncEdgeNodes(ruvia::Context& c, std::string_view configId) {
        const auto rows = co_await c.db().query(R"sql(
SELECT DISTINCT l.edge_node_id::text
FROM device d
JOIN link l ON l.id = d.link_id AND l.execution = 'edge' AND l.deleted_at IS NULL
WHERE d.protocol_config_id = $1::uuid AND d.deleted_at IS NULL
  AND l.edge_node_id IS NOT NULL
ORDER BY l.edge_node_id::text)sql",
                                                service::common::dbParams(configId));
        for (const auto& row : rows) {
            const auto nodeId = row[0].value().value_or(std::string_view{});
            if (nodeId.empty())
                continue;
            try {
                (void)co_await service::edge::configService().queueSnapshot(c, nodeId);
            } catch (const std::exception& error) {
                std::cerr << "protocol edge config sync failed: node=" << nodeId
                          << " config=" << configId << " error=" << error.what() << '\n';
            } catch (...) {
                std::cerr << "protocol edge config sync failed: node=" << nodeId
                          << " config=" << configId << " error=unknown exception\n";
            }
        }
    }

    static void logPostUpdateFailure(std::string_view stage, std::string_view configId,
                                     std::string_view message) {
        std::cerr << "protocol post-update " << stage << " failed: config=" << configId
                  << " error=" << message << '\n';
    }

    static std::int64_t toInt(std::string_view value) {
        return service::common::parseInt64(std::optional<std::string_view>{value}).value_or(0);
    }

    static std::string itemExpression() {
        return R"sql(jsonb_build_object(
    'id', id, 'protocol', protocol, 'name', name, 'enabled', enabled,
    'config', config, 'remark', COALESCE(remark, ''),
    'created_at', iot_utc_timestamp(created_at),
    'updated_at', iot_utc_timestamp(updated_at)))sql";
    }

    static std::string requiredString(const ruvia::JsonValue& payload, std::string_view field,
                                      std::string_view message) {
        const auto value = payload.get<ruvia::String>(field);
        if (!value || value->view().empty())
            service::common::fail(16002, std::string(message), 400);
        return std::string(value->view());
    }

    static std::optional<std::string> optionalString(const ruvia::JsonValue& payload,
                                                     std::string_view field,
                                                     std::string_view typeMessage,
                                                     std::size_t maximum) {
        const auto raw = service::access::jsonField(payload, field);
        if (!raw)
            return std::nullopt;
        const auto value = payload.get<ruvia::String>(field);
        if (!value)
            service::common::fail(16002, std::string(typeMessage), 400);
        if (value->view().size() > maximum)
            service::common::fail(16002, std::string(field) + " 长度超出限制", 400);
        return std::string(value->view());
    }

    static void validateOptionalNullableString(const ruvia::JsonValue& payload,
                                               std::string_view field,
                                               std::string_view typeMessage,
                                               std::size_t maximum) {
        const auto raw = service::access::jsonField(payload, field);
        if (!raw || raw->isNull())
            return;
        const auto value = payload.get<ruvia::String>(field);
        if (!value)
            service::common::fail(16002, std::string(typeMessage), 400);
        if (value->view().size() > maximum)
            service::common::fail(16002, std::string(field) + " 长度超出限制", 400);
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
        const auto enabled = co_await c.db().query(R"sql(
WITH body AS (SELECT $1::jsonb AS value)
SELECT NOT (value ? 'enabled') OR jsonb_typeof(value->'enabled') = 'boolean'
FROM body)sql",
                                                   service::common::dbParams(body));
        if (enabled.front()[0].value().value_or(std::string_view{}) != "t")
            service::common::fail(16004, "enabled 必须是布尔值", 400);

        const auto shape = co_await c.db().query(R"sql(
WITH body AS (SELECT $1::jsonb AS value)
SELECT body.value ? 'config', jsonb_typeof(body.value->'config')
FROM body)sql",
                                                 service::common::dbParams(body));
        const bool present = shape.front()[0].value().value_or(std::string_view{}) == "t";
        if (!present) {
            if (required)
                service::common::fail(16004, "config 不能为空", 400);
            co_return;
        }
        if (shape.front()[1].value().value_or(std::string_view{}) != "object")
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
            const auto& row = rows.front();
            if (row[0].value().value_or(std::string_view{}) != "t" || row[1].value().value_or(std::string_view{}) != "t" ||
                (required && (row[2].value().value_or(std::string_view{}) != "t" || row[3].value().value_or(std::string_view{}) != "t")))
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
  (NOT COALESCE(value->'connection' ? 'probeMode', FALSE) OR
       value->'connection'->>'probeMode' IN ('STANDARD', 'COMPATIBLE', 'AUTO')),
  CASE WHEN NOT COALESCE(value->'connection' ? 'handshakeTimeout', FALSE) THEN TRUE
       WHEN jsonb_typeof(value->'connection'->'handshakeTimeout') <> 'number' THEN FALSE
       ELSE (value->'connection'->>'handshakeTimeout')::numeric BETWEEN 1000 AND 30000 END,
  CASE WHEN NOT COALESCE(value->'connection' ? 'directProbeTimeout', FALSE) THEN TRUE
       WHEN jsonb_typeof(value->'connection'->'directProbeTimeout') <> 'number' THEN FALSE
       ELSE (value->'connection'->>'directProbeTimeout')::numeric BETWEEN 1000 AND 30000 END,
  value ? 'plcModel', value ? 'connection', value ? 'areas'
FROM cfg)sql",
                                                    service::common::dbParams(body));
            const auto& row = rows.front();
            if (row[0].value().value_or(std::string_view{}) != "t" || row[1].value().value_or(std::string_view{}) != "t" || row[2].value().value_or(std::string_view{}) != "t" ||
                row[3].value().value_or(std::string_view{}) != "t" || row[4].value().value_or(std::string_view{}) != "t" || row[5].value().value_or(std::string_view{}) != "t" ||
                (required &&
                 (row[6].value().value_or(std::string_view{}) != "t" || row[7].value().value_or(std::string_view{}) != "t" || row[8].value().value_or(std::string_view{}) != "t")))
                service::common::fail(16004, "S7 配置无效", 400);
            const auto areas = co_await c.db().query(R"sql(
WITH cfg AS (SELECT ($1::jsonb)->'config' AS value),
areas AS (
  SELECT item FROM cfg, jsonb_array_elements(value->'areas') AS item
  WHERE value ? 'areas'
)
SELECT COALESCE(bool_and(
    jsonb_typeof(item) = 'object'
    AND jsonb_typeof(item->'id') = 'string' AND COALESCE(item->>'id', '') <> ''
    AND jsonb_typeof(item->'name') = 'string' AND COALESCE(item->>'name', '') <> ''
    AND item->>'area' IN ('DB', 'V', 'MK', 'PE', 'PA', 'CT', 'TM')
    AND (NOT (item ? 'dataType') OR item->>'dataType' IN
        ('BOOL', 'INT8', 'UINT8', 'INT16', 'UINT16', 'INT32', 'UINT32', 'FLOAT', 'LREAL', 'STRING'))
    AND CASE WHEN item->>'area' = 'DB'
             THEN CASE WHEN jsonb_typeof(item->'dbNumber') = 'number'
                            AND item->>'dbNumber' ~ '^[0-9]{1,5}$'
                       THEN (item->>'dbNumber')::integer BETWEEN 1 AND 65535 ELSE FALSE END
             ELSE TRUE END
    AND CASE WHEN jsonb_typeof(item->'start') = 'number'
                  AND item->>'start' ~ '^[0-9]{1,10}$'
             THEN (item->>'start')::bigint BETWEEN 0 AND 2147483647 ELSE FALSE END
    AND CASE WHEN NOT (item ? 'startBit') THEN TRUE
             WHEN jsonb_typeof(item->'startBit') = 'number'
                  AND item->>'startBit' ~ '^[0-7]$'
             THEN TRUE ELSE FALSE END
    AND CASE WHEN jsonb_typeof(item->'size') = 'number'
                  AND item->>'size' ~ '^[0-9]{1,5}$'
             THEN (item->>'size')::integer BETWEEN 1 AND 65535 ELSE FALSE END
    AND CASE WHEN NOT (item ? 'decimals') THEN TRUE
             WHEN jsonb_typeof(item->'decimals') = 'number'
                  AND item->>'decimals' ~ '^-?[0-9]{1,2}$'
             THEN (item->>'decimals')::integer BETWEEN -1 AND 8 ELSE FALSE END
    AND (NOT (item ? 'writable') OR jsonb_typeof(item->'writable') = 'boolean')
), TRUE) FROM areas)sql",
                                                    service::common::dbParams(body));
            if (areas.front()[0].value().value_or(std::string_view{}) != "t")
                service::common::fail(16004, "S7 寄存器配置无效", 400);
            co_return;
        }
        if (protocol != "Modbus")
            co_return;

	    const auto configRows = co_await c.db().query(R"sql(
	WITH cfg AS (SELECT ($1::jsonb)->'config' AS value)
	SELECT
	    (NOT (value ? 'byteOrder') OR value->>'byteOrder' IN
	        ('BIG_ENDIAN', 'LITTLE_ENDIAN', 'BIG_ENDIAN_BYTE_SWAP', 'LITTLE_ENDIAN_BYTE_SWAP')),
	    (NOT (value ? 'registers') OR jsonb_typeof(value->'registers') = 'array'),
	    (NOT (value ? 'packet') OR jsonb_typeof(value->'packet') = 'object'),
	    (NOT (value ? 'readInterval') OR
	     CASE WHEN jsonb_typeof(value->'readInterval') IN ('number', 'string')
	               AND value->>'readInterval' ~ '^[0-9]{1,5}$'
	          THEN (value->>'readInterval')::integer BETWEEN 1 AND 3600
	          ELSE FALSE END),
	    CASE WHEN NOT (value ? 'packet') OR jsonb_typeof(value->'packet') <> 'object'
	              OR NOT (value->'packet' ? 'mergeGap') THEN TRUE
	         WHEN jsonb_typeof(value->'packet'->'mergeGap') IN ('number', 'string')
	              AND value->'packet'->>'mergeGap' ~ '^[0-9]{1,5}$'
	         THEN (value->'packet'->>'mergeGap')::integer BETWEEN 0 AND 2000
	         ELSE FALSE END,
	    CASE WHEN NOT (value ? 'packet') OR jsonb_typeof(value->'packet') <> 'object'
	              OR NOT (value->'packet' ? 'maxQuantity') THEN TRUE
	         WHEN jsonb_typeof(value->'packet'->'maxQuantity') IN ('number', 'string')
	              AND value->'packet'->>'maxQuantity' ~ '^[0-9]{1,5}$'
	         THEN (value->'packet'->>'maxQuantity')::integer BETWEEN 1 AND 125
	         ELSE FALSE END,
	    value ? 'byteOrder',
	    value ? 'registers'
	FROM cfg)sql",
	                                                      service::common::dbParams(body));
	        const auto& config = configRows.front();
	        if (config[0].value().value_or(std::string_view{}) != "t" || (required && config[6].value().value_or(std::string_view{}) != "t"))
	            service::common::fail(16004, "Modbus 配置的 byteOrder 无效", 400);
	        if (config[1].value().value_or(std::string_view{}) != "t" || (required && config[7].value().value_or(std::string_view{}) != "t"))
	            service::common::fail(16004, "Modbus 配置的 registers 必须是数组", 400);
	        if (config[2].value().value_or(std::string_view{}) != "t")
	            service::common::fail(16004, "Modbus 配置的 packet 必须是对象", 400);
	        if (config[3].value().value_or(std::string_view{}) != "t")
	            service::common::fail(16004, "Modbus 配置的 readInterval 无效", 400);
	        if (config[4].value().value_or(std::string_view{}) != "t")
	            service::common::fail(16004, "Modbus 配置的 packet.mergeGap 无效", 400);
	        if (config[5].value().value_or(std::string_view{}) != "t")
	            service::common::fail(16004, "Modbus 配置的 packet.maxQuantity 无效", 400);
	        if (config[7].value().value_or(std::string_view{}) != "t")
	            co_return;
	
	        const auto registers = co_await c.db().query(R"sql(
	WITH cfg AS (SELECT ($1::jsonb)->'config' AS value),
registers AS (SELECT item FROM cfg, jsonb_array_elements(value->'registers') AS item)
SELECT
  COALESCE(bool_and(
    jsonb_typeof(item) = 'object'
    AND jsonb_typeof(item->'id') = 'string' AND COALESCE(item->>'id', '') <> ''
    AND jsonb_typeof(item->'name') = 'string' AND COALESCE(item->>'name', '') <> ''
    AND item->>'registerType' IN ('COIL', 'DISCRETE_INPUT', 'HOLDING_REGISTER', 'INPUT_REGISTER')
    AND item->>'dataType' IN ('BOOL', 'INT16', 'UINT16', 'INT32', 'UINT32', 'FLOAT32',
                              'INT64', 'UINT64', 'DOUBLE')
    AND CASE WHEN jsonb_typeof(item->'address') = 'number'
                  AND item->>'address' ~ '^[0-9]{1,5}$'
             THEN (item->>'address')::integer BETWEEN 0 AND 65535 ELSE FALSE END
    AND CASE WHEN jsonb_typeof(item->'quantity') = 'number'
                  AND item->>'quantity' ~ '^[1-4]$'
             THEN TRUE ELSE FALSE END
  ), TRUE),
  COALESCE(bool_and(
    CASE WHEN NOT (item ? 'byteOrder') THEN TRUE
         ELSE item->>'byteOrder' IN
             ('BIG_ENDIAN', 'LITTLE_ENDIAN', 'BIG_ENDIAN_BYTE_SWAP', 'LITTLE_ENDIAN_BYTE_SWAP') END
  ), TRUE),
  COALESCE(bool_and(
    CASE WHEN NOT (item ? 'scale') THEN TRUE
         WHEN jsonb_typeof(item->'scale') IN ('number', 'string')
              AND item->>'scale' ~ '^[+-]?([0-9]{1,18}(\.[0-9]{0,12})?|\.[0-9]{1,12})([eE][+-]?[0-9]{1,3})?$'
         THEN abs((item->>'scale')::numeric) <= 1000000000
         ELSE FALSE END
  ), TRUE),
  COALESCE(bool_and(
    CASE WHEN NOT (item ? 'decimals') THEN TRUE
         WHEN jsonb_typeof(item->'decimals') IN ('number', 'string')
              AND item->>'decimals' ~ '^-?[0-9]{1,2}$'
         THEN (item->>'decimals')::integer BETWEEN -1 AND 8
         ELSE FALSE END
  ), TRUE),
  COALESCE(bool_and(NOT (item ? 'writable') OR jsonb_typeof(item->'writable') = 'boolean'), TRUE)
FROM registers)sql",
                                                     service::common::dbParams(body));
        if (registers.front()[0].value().value_or(std::string_view{}) != "t")
            service::common::fail(16004, "Modbus 寄存器配置无效", 400);
        if (registers.front()[1].value().value_or(std::string_view{}) != "t")
            service::common::fail(16004, "Modbus 寄存器 byteOrder 无效", 400);
        if (registers.front()[2].value().value_or(std::string_view{}) != "t")
            service::common::fail(16004, "Modbus 寄存器 scale 无效", 400);
        if (registers.front()[3].value().value_or(std::string_view{}) != "t")
            service::common::fail(16004, "Modbus 寄存器 decimals 无效", 400);
        if (registers.front()[4].value().value_or(std::string_view{}) != "t")
            service::common::fail(16004, "Modbus 寄存器 writable 必须是布尔值", 400);
    }

    ruvia::Task<void> ensureNameAvailable(ruvia::Context& c, const std::string& name,
                                          std::optional<std::string> excludedId) {
        std::string sql =
            "SELECT 1 FROM protocol_config WHERE name = $1 AND deleted_at IS NULL";
        auto params = service::common::dbParams(name);
        if (excludedId) {
            params.emplace_back(*excludedId);
            sql += " AND id <> $2";
        }
        sql += " LIMIT 1";
        const auto rows = co_await c.db().query(sql, params);
        if (!rows.empty())
            service::common::fail(16005, "配置名称已存在", 409);
    }

    ruvia::Task<void> requireOwner(ruvia::Context& c, std::string_view ownerId) {
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
        if (rows.empty() || rows.front()[0].value().value_or(std::string_view{}) != "t")
            service::common::fail(16007, "只能修改或删除自己创建的协议配置", 403);
    }
};

inline ProtocolService& protocolService() { return ProtocolService::instance(); }

} // namespace service::protocol
