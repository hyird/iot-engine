#pragma once

#include <charconv>
#include <span>
#include <vector>
#include <ruvia/web/redis/RedisTypes.h>
#include "service/common/message.h"

#include <cstdint>
#include <memory_resource>
#include <string>
#include <ruvia/web/db/DbEntity.h>

namespace service::gb28181::persistence {

RUVIA_DB_ENTITY(Gb28181ChannelEntity, "gb28181_channel",
    RUVIA_DB_COLUMN(device_id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .primaryKey = true, .length = 128}),
    RUVIA_DB_COLUMN(id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .primaryKey = true, .length = 128}),
    RUVIA_DB_COLUMN(name, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 255, .defaultExpression = ruvia::FixedString{"''::character varying"}}),
    RUVIA_DB_COLUMN(manufacturer, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 255, .defaultExpression = ruvia::FixedString{"''::character varying"}}),
    RUVIA_DB_COLUMN(online, bool,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kBoolean, .defaultExpression = ruvia::FixedString{"false"}}),
    RUVIA_DB_COLUMN(ptz_type, std::int32_t,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kInteger, .defaultExpression = ruvia::FixedString{"'-1'::integer"}}),
    RUVIA_DB_COLUMN(updated_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}),
    RUVIA_DB_COLUMN(custom_name, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .nullable = true, .length = 255}))

RUVIA_DB_ENTITY(Gb28181DeviceEntity, "gb28181_device",
    RUVIA_DB_COLUMN(id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .primaryKey = true, .length = 128}),
    RUVIA_DB_COLUMN(name, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 255, .defaultExpression = ruvia::FixedString{"''::character varying"}}),
    RUVIA_DB_COLUMN(manufacturer, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 255, .defaultExpression = ruvia::FixedString{"''::character varying"}}),
    RUVIA_DB_COLUMN(remote_address, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 255, .defaultExpression = ruvia::FixedString{"''::character varying"}}),
    RUVIA_DB_COLUMN(registration_source, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 32, .defaultExpression = ruvia::FixedString{"'sip'::character varying"}}),
    RUVIA_DB_COLUMN(online, bool,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kBoolean, .defaultExpression = ruvia::FixedString{"false"}}),
    RUVIA_DB_COLUMN(last_seen_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz}),
    RUVIA_DB_COLUMN(mapped_device_id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid, .nullable = true}),
    RUVIA_DB_COLUMN(created_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}),
    RUVIA_DB_COLUMN(updated_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}),
    RUVIA_DB_COLUMN(custom_name, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .nullable = true, .length = 255}),
    RUVIA_DB_COLUMN(projection_cursor, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kNumeric, .precision = 40, .scale = 0, .defaultExpression = ruvia::FixedString{"0"}}))

RUVIA_DB_ENTITY(Gb28181RecordEntity, "gb28181_record",
    RUVIA_DB_COLUMN(device_id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .primaryKey = true, .length = 128}),
    RUVIA_DB_COLUMN(channel_id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .primaryKey = true, .length = 128}),
    RUVIA_DB_COLUMN(name, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 255, .defaultExpression = ruvia::FixedString{"''::character varying"}}),
    RUVIA_DB_COLUMN(file_path, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kText, .primaryKey = true, .defaultExpression = ruvia::FixedString{"''::text"}}),
    RUVIA_DB_COLUMN(address, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kText, .defaultExpression = ruvia::FixedString{"''::text"}}),
    RUVIA_DB_COLUMN(start_time, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .primaryKey = true}),
    RUVIA_DB_COLUMN(end_time, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .primaryKey = true}),
    RUVIA_DB_COLUMN(record_type, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 64, .defaultExpression = ruvia::FixedString{"''::character varying"}}),
    RUVIA_DB_COLUMN(recorder_id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 128, .defaultExpression = ruvia::FixedString{"''::character varying"}}),
    RUVIA_DB_COLUMN(created_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}))

RUVIA_DB_ENTITY(Gb28181StreamEntity, "gb28181_stream",
    RUVIA_DB_COLUMN(app, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .primaryKey = true, .length = 128}),
    RUVIA_DB_COLUMN(stream, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .primaryKey = true, .length = 255}),
    RUVIA_DB_COLUMN(schema, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .primaryKey = true, .length = 32}),
    RUVIA_DB_COLUMN(online, bool,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kBoolean, .defaultExpression = ruvia::FixedString{"false"}}),
    RUVIA_DB_COLUMN(reader_count, std::int32_t,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kInteger, .defaultExpression = ruvia::FixedString{"0"}}),
    RUVIA_DB_COLUMN(updated_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}),
    RUVIA_DB_COLUMN(projection_cursor, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kNumeric, .precision = 40, .scale = 0, .defaultExpression = ruvia::FixedString{"0"}}))

} // namespace service::gb28181::persistence

namespace service::gb28181 {

// Fixed Redis String stores status and payload separated by the first newline.
// Redis ORM cannot preserve this existing scalar key/value format.
struct ControlResultRecord final {
    std::string status;
    std::string payload;

    static ControlResultRecord decode(std::string_view value) {
        const auto separator = value.find('\n');
        if (separator == std::string_view::npos) {
            return {"ok", std::string(value)};
        }
        return {std::string(value.substr(0, separator)), std::string(value.substr(separator + 1))};
    }

    std::string encode() const { return status + "\n" + payload; }
};

// 固定 Hash 使用既有配置键；Ruvia Redis ORM 总会附加 ruvia:orm: 前缀及实体 ID，
// 无法映射该原键。此记录集中定义实际发布与读取的字段，不改变已有投影格式。
struct ConfigProjectionRecord final {
    bool enabled{};
    std::string domain{};
    std::string id{};
    std::string host{};
    std::string publicIp{};
    std::int64_t port{};
    std::string transport{};
    std::int64_t registrationTimeoutSeconds{};
    std::int64_t commandTimeoutSeconds{};
    std::int64_t inviteTimeoutSeconds{};
    std::int64_t viewerLeaseTimeoutSeconds{};

    static bool enabledValue(std::string_view value) {
        return value == "t" || value == "true" || value == "1";
    }

    std::vector<service::message::StreamField> encode() const {
        return {
            {"enabled", enabled ? "1" : "0"},
            {"domain", domain},
            {"id", id},
            {"host", host},
            {"public_ip", publicIp},
            {"port", std::to_string(port)},
            {"transport", transport},
            {"registration_timeout_seconds", std::to_string(registrationTimeoutSeconds)},
            {"command_timeout_seconds", std::to_string(commandTimeoutSeconds)},
            {"invite_timeout_seconds", std::to_string(inviteTimeoutSeconds)},
            {"viewer_lease_timeout_seconds", std::to_string(viewerLeaseTimeoutSeconds)},
        };
    }

    static ConfigProjectionRecord decode(std::span<const ruvia::RedisKeyValue> fields) {
        ConfigProjectionRecord record;
        for (const auto& field : fields) {
            if (field.key() == "enabled") record.enabled = enabledValue(field.value());
            else if (field.key() == "domain") record.domain = std::string(field.value());
            else if (field.key() == "id") record.id = std::string(field.value());
            else if (field.key() == "host") record.host = std::string(field.value());
            else if (field.key() == "public_ip") record.publicIp = std::string(field.value());
            else if (field.key() == "port") record.port = integer(field.value());
            else if (field.key() == "transport") record.transport = std::string(field.value());
            else if (field.key() == "registration_timeout_seconds") record.registrationTimeoutSeconds = integer(field.value());
            else if (field.key() == "command_timeout_seconds") record.commandTimeoutSeconds = integer(field.value());
            else if (field.key() == "invite_timeout_seconds") record.inviteTimeoutSeconds = integer(field.value());
            else if (field.key() == "viewer_lease_timeout_seconds") record.viewerLeaseTimeoutSeconds = integer(field.value());
        }
        return record;
    }

  private:
    static std::int64_t integer(std::string_view value) {
        std::int64_t result{};
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
        return error == std::errc{} && end == value.data() + value.size() ? result : 0;
    }
};

} // namespace service::gb28181
