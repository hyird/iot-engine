#pragma once

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
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 255}),
    RUVIA_DB_COLUMN(manufacturer, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 255}),
    RUVIA_DB_COLUMN(online, bool,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kBoolean}),
    RUVIA_DB_COLUMN(ptz_type, std::int32_t,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kInteger}),
    RUVIA_DB_COLUMN(updated_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz}),
    RUVIA_DB_COLUMN(custom_name, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .nullable = true, .length = 255}))

RUVIA_DB_ENTITY(Gb28181DeviceEntity, "gb28181_device",
    RUVIA_DB_COLUMN(id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .primaryKey = true, .length = 128}),
    RUVIA_DB_COLUMN(name, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 255}),
    RUVIA_DB_COLUMN(manufacturer, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 255}),
    RUVIA_DB_COLUMN(remote_address, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 255}),
    RUVIA_DB_COLUMN(registration_source, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 32}),
    RUVIA_DB_COLUMN(online, bool,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kBoolean}),
    RUVIA_DB_COLUMN(last_seen_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz}),
    RUVIA_DB_COLUMN(mapped_device_id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid, .nullable = true}),
    RUVIA_DB_COLUMN(created_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz}),
    RUVIA_DB_COLUMN(updated_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz}),
    RUVIA_DB_COLUMN(custom_name, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .nullable = true, .length = 255}),
    RUVIA_DB_COLUMN(projection_cursor, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kNumeric, .precision = 40, .scale = 0}))

RUVIA_DB_ENTITY(Gb28181RecordEntity, "gb28181_record",
    RUVIA_DB_COLUMN(device_id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .primaryKey = true, .length = 128}),
    RUVIA_DB_COLUMN(channel_id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .primaryKey = true, .length = 128}),
    RUVIA_DB_COLUMN(name, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 255}),
    RUVIA_DB_COLUMN(file_path, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kText, .primaryKey = true}),
    RUVIA_DB_COLUMN(address, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kText}),
    RUVIA_DB_COLUMN(start_time, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .primaryKey = true}),
    RUVIA_DB_COLUMN(end_time, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .primaryKey = true}),
    RUVIA_DB_COLUMN(record_type, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 64}),
    RUVIA_DB_COLUMN(recorder_id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 128}),
    RUVIA_DB_COLUMN(created_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz}))

RUVIA_DB_ENTITY(Gb28181StreamEntity, "gb28181_stream",
    RUVIA_DB_COLUMN(app, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .primaryKey = true, .length = 128}),
    RUVIA_DB_COLUMN(stream, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .primaryKey = true, .length = 255}),
    RUVIA_DB_COLUMN(schema, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .primaryKey = true, .length = 32}),
    RUVIA_DB_COLUMN(online, bool,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kBoolean}),
    RUVIA_DB_COLUMN(reader_count, std::int32_t,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kInteger}),
    RUVIA_DB_COLUMN(updated_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz}),
    RUVIA_DB_COLUMN(projection_cursor, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kNumeric, .precision = 40, .scale = 0}))

} // namespace service::gb28181::persistence
