#pragma once

#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <set>
#include <map>
#include "service/common/message.h"
#include "service/utils/json.h"
#include <stdexcept>
#include <ruvia/web/redis/RedisTypes.h>
#include <ruvia/web/db/DbEntity.h>

namespace service::access::persistence {

RUVIA_DB_ENTITY(DeviceEntity, "device",
    RUVIA_DB_COLUMN(id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid, .primaryKey = true}),
    RUVIA_DB_COLUMN(name, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 100}),
    RUVIA_DB_COLUMN(link_id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid}),
    RUVIA_DB_COLUMN(protocol_config_id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid}),
    RUVIA_DB_COLUMN(group_id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid, .nullable = true}),
    RUVIA_DB_COLUMN(status, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kText, .enumName = ruvia::FixedString{"status_enum"}, .defaultExpression = ruvia::FixedString{"'enabled'::status_enum"}}),
    RUVIA_DB_COLUMN(protocol_params, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kJsonb, .defaultExpression = ruvia::FixedString{"'{}'::jsonb"}}),
    RUVIA_DB_COLUMN(remark, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kText, .nullable = true}),
    RUVIA_DB_COLUMN(created_by, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid}),
    RUVIA_DB_COLUMN(created_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}),
    RUVIA_DB_COLUMN(updated_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}),
    RUVIA_DB_COLUMN(deleted_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .nullable = true}),
    RUVIA_DB_COLUMN(protocol_address, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kText}))

RUVIA_DB_ENTITY(OpenAccessKeyEntity, "open_access_key",
    RUVIA_DB_COLUMN(id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid, .primaryKey = true}),
    RUVIA_DB_COLUMN(name, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 64}),
    RUVIA_DB_COLUMN(access_key_prefix, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 16}),
    RUVIA_DB_COLUMN(access_key_hash, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 64}),
    RUVIA_DB_COLUMN(status, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kText, .enumName = ruvia::FixedString{"status_enum"}, .defaultExpression = ruvia::FixedString{"'enabled'::status_enum"}}),
    RUVIA_DB_COLUMN(scopes, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kJsonb, .defaultExpression = ruvia::FixedString{"'[]'::jsonb"}}),
    RUVIA_DB_COLUMN(expires_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .nullable = true}),
    RUVIA_DB_COLUMN(last_used_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .nullable = true}),
    RUVIA_DB_COLUMN(last_used_ip, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .nullable = true, .length = 64}),
    RUVIA_DB_COLUMN(remark, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .nullable = true, .length = 200}),
    RUVIA_DB_COLUMN(created_by, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid}),
    RUVIA_DB_COLUMN(created_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}),
    RUVIA_DB_COLUMN(updated_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}),
    RUVIA_DB_COLUMN(deleted_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .nullable = true}))

RUVIA_DB_ENTITY(OpenAccessKeyDeviceEntity, "open_access_key_device",
    RUVIA_DB_COLUMN(access_key_id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid, .primaryKey = true}),
    RUVIA_DB_COLUMN(device_id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid, .primaryKey = true}),
    RUVIA_DB_COLUMN(created_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}))

RUVIA_DB_ENTITY(OpenAccessLogEntity, "open_access_log",
    RUVIA_DB_COLUMN(id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid, .primaryKey = true}),
    RUVIA_DB_COLUMN(access_key_id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid, .nullable = true}),
    RUVIA_DB_COLUMN(webhook_id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid, .nullable = true}),
    RUVIA_DB_COLUMN(direction, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 20}),
    RUVIA_DB_COLUMN(action, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 50}),
    RUVIA_DB_COLUMN(event_type, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .nullable = true, .length = 100}),
    RUVIA_DB_COLUMN(status, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 20}),
    RUVIA_DB_COLUMN(http_method, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .nullable = true, .length = 10}),
    RUVIA_DB_COLUMN(target, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kText, .nullable = true}),
    RUVIA_DB_COLUMN(request_ip, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .nullable = true, .length = 64}),
    RUVIA_DB_COLUMN(http_status, std::int32_t,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kInteger, .nullable = true}),
    RUVIA_DB_COLUMN(device_id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid, .nullable = true}),
    RUVIA_DB_COLUMN(device_code, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .nullable = true, .length = 100}),
    RUVIA_DB_COLUMN(message, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kText, .nullable = true}),
    RUVIA_DB_COLUMN(request_payload, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kJsonb, .defaultExpression = ruvia::FixedString{"'{}'::jsonb"}}),
    RUVIA_DB_COLUMN(response_payload, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kJsonb, .defaultExpression = ruvia::FixedString{"'{}'::jsonb"}}),
    RUVIA_DB_COLUMN(created_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}))

RUVIA_DB_ENTITY(OpenWebhookEntity, "open_webhook",
    RUVIA_DB_COLUMN(id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid, .primaryKey = true}),
    RUVIA_DB_COLUMN(access_key_id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid}),
    RUVIA_DB_COLUMN(name, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 64}),
    RUVIA_DB_COLUMN(url, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kText}),
    RUVIA_DB_COLUMN(status, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kText, .enumName = ruvia::FixedString{"status_enum"}, .defaultExpression = ruvia::FixedString{"'enabled'::status_enum"}}),
    RUVIA_DB_COLUMN(secret, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .nullable = true, .length = 255}),
    RUVIA_DB_COLUMN(headers, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kJsonb, .defaultExpression = ruvia::FixedString{"'{}'::jsonb"}}),
    RUVIA_DB_COLUMN(event_types, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kJsonb, .defaultExpression = ruvia::FixedString{"'[\"device.data.reported\"]'::jsonb"}}),
    RUVIA_DB_COLUMN(timeout_seconds, std::int32_t,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kInteger, .defaultExpression = ruvia::FixedString{"5"}}),
    RUVIA_DB_COLUMN(last_triggered_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .nullable = true}),
    RUVIA_DB_COLUMN(last_success_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .nullable = true}),
    RUVIA_DB_COLUMN(last_failure_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .nullable = true}),
    RUVIA_DB_COLUMN(last_http_status, std::int32_t,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kInteger, .nullable = true}),
    RUVIA_DB_COLUMN(last_error, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kText, .nullable = true}),
    RUVIA_DB_COLUMN(created_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}),
    RUVIA_DB_COLUMN(updated_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}),
    RUVIA_DB_COLUMN(deleted_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .nullable = true}),
    RUVIA_DB_COLUMN(skip_tls_verify, bool,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kBoolean, .defaultExpression = ruvia::FixedString{"false"}}))

} // namespace service::access::persistence

namespace service::access::webhook {

// Existing Hash stores completed target IDs as dynamic fields, with value 1.
// Ruvia Redis ORM cannot map this key format and dynamic fields; the result message
// and progress must be written atomically by the same Lua operation.
struct DeliveryProgressRecord final {
    std::set<std::string, std::less<>> completedTargets;

    static std::string key(std::string_view eventType, std::string_view eventId) {
        return "iot:open-access:delivery-progress:" + std::string(eventType) + ":" + std::string(eventId);
    }

    static DeliveryProgressRecord decode(const ruvia::RedisValue& reply) {
        if (reply.kind() != ruvia::RedisValue::Kind::kArray) {
            throw std::runtime_error("read webhook delivery progress: expected array");
        }
        DeliveryProgressRecord record;
        for (const auto& value : reply.array()) {
            if (value.kind() != ruvia::RedisValue::Kind::kString) {
                throw std::runtime_error("webhook delivery progress contains a non-string target");
            }
            record.completedTargets.emplace(value.string());
        }
        return record;
    }
};

} // namespace service::access::webhook

namespace service::access::webhook {

// Existing telemetry Hash uses dynamic point fields and JSON values. Redis ORM
// cannot map this fixed key format and heterogeneous fields. Preserve missing
// configuration, null values, and malformed-field handling when decoding.
struct LatestValuesRecord final {
    struct Point final {
        std::int64_t sort{ 0 };
        std::int64_t observedAt{ 0 };
        std::string id;
        std::string name;
        std::string value{ "null" };
        std::string unit;
        std::string dataType;
        std::string encode;
    };

    std::set<std::string, std::less<>> configured;
    std::map<std::string, Point, std::less<>> latest;
    bool hasConfigured{};

    static LatestValuesRecord decode(const ruvia::RedisValue& reply) {
        LatestValuesRecord record;
        auto& configured = record.configured;
        auto& latest = record.latest;
        auto& hasConfigured = record.hasConfigured;
        if (reply.kind() == ruvia::RedisValue::Kind::kArray) {
            const auto& entries = reply.array();
            for (std::size_t index = 0; index + 1 < entries.size(); index += 2) {
                if (entries[index].kind() != ruvia::RedisValue::Kind::kString ||
                    entries[index + 1].kind() != ruvia::RedisValue::Kind::kString) {
                    continue;
                }
                const auto field = entries[index].string();
                const auto raw = entries[index + 1].string();
                if (field == "_element_ids") {
                    hasConfigured = true;
                    const auto parsed = ruvia::JsonValue::parse(raw);
                    if (parsed && parsed->isObject()) {
                        (void)parsed->forEachField(
                            [&](std::string_view name, const ruvia::JsonValue&) {
                                if (!name.empty()) {
                                    configured.emplace(name);
                                }
                                return true;
                            }
                        );
                    }
                    continue;
                }
                if (field.empty() || field.front() == '_') {
                    continue;
                }
                const auto parsed = ruvia::JsonValue::parse(raw);
                if (!parsed || !parsed->isObject()) {
                    continue;
                }
                Point point;
                point.id.assign(field);
                if (const auto value = parsed->get<ruvia::String>("id")) {
                    point.id.assign(value->view());
                }
                point.name = point.id;
                if (const auto value = parsed->get<ruvia::String>("name")) {
                    point.name.assign(value->view());
                }
                if (const auto value = parsed->get<ruvia::String>("unit")) {
                    point.unit.assign(value->view());
                }
                if (const auto value = parsed->get<ruvia::String>("dataType")) {
                    point.dataType.assign(value->view());
                }
                if (const auto value = parsed->get<ruvia::String>("encode")) {
                    point.encode.assign(value->view());
                }
                if (const auto value = parsed->get<ruvia::Int64>("sort")) {
                    point.sort = static_cast<std::int64_t>(*value);
                }
                if (const auto value = parsed->get<ruvia::Int64>("observedAt")) {
                    point.observedAt = static_cast<std::int64_t>(*value);
                }
                if (const auto value = parsed->get<ruvia::JsonValue>("value")) {
                    point.value = telemetry::latest::canonicalPointJson(
                        value->view(),
                        point.dataType
                    );
                }
                latest.insert_or_assign(point.id, std::move(point));
            }
        }
        return record;
    }
};

} // namespace service::access::webhook
