#pragma once
#include <string>
#include <memory_resource>
#include <ruvia/web/db/DbEntity.h>
#include <string_view>

namespace service::packet_log {
RUVIA_DB_ENTITY(DebugDeviceEntity, "device",
    RUVIA_DB_COLUMN(id, std::pmr::string, ruvia::DbColumnOptions{.dataType=ruvia::DbDataType::kUuid, .primaryKey=true}),
    RUVIA_DB_COLUMN(debug_enabled, bool, ruvia::DbColumnOptions{.dataType=ruvia::DbDataType::kBoolean}))

RUVIA_DB_ENTITY(DebugLinkEntity, "link",
    RUVIA_DB_COLUMN(id, std::pmr::string, ruvia::DbColumnOptions{.dataType=ruvia::DbDataType::kUuid, .primaryKey=true}),
    RUVIA_DB_COLUMN(debug_enabled, bool, ruvia::DbColumnOptions{.dataType=ruvia::DbDataType::kBoolean}))

RUVIA_DB_ENTITY(DebugHistoryEntity, "device_data",
    RUVIA_DB_COLUMN(id, std::pmr::string, ruvia::DbColumnOptions{.dataType=ruvia::DbDataType::kUuid, .primaryKey=true}),
    RUVIA_DB_COLUMN(device_id, std::pmr::string, ruvia::DbColumnOptions{.dataType=ruvia::DbDataType::kUuid}),
    RUVIA_DB_COLUMN(link_id, std::pmr::string, ruvia::DbColumnOptions{.dataType=ruvia::DbDataType::kUuid}),
    RUVIA_DB_COLUMN(data, std::pmr::string, ruvia::DbColumnOptions{.dataType=ruvia::DbDataType::kJsonb}),
    RUVIA_DB_COLUMN(raw_payload_hex, std::pmr::string, ruvia::DbColumnOptions{.dataType=ruvia::DbDataType::kJsonb}),
    RUVIA_DB_COLUMN(source, std::pmr::string, ruvia::DbColumnOptions{.dataType=ruvia::DbDataType::kText}),
    RUVIA_DB_COLUMN(report_time, std::pmr::string, ruvia::DbColumnOptions{.dataType=ruvia::DbDataType::kTimestampTz, .primaryKey=true}))

// 有界 Redis Stream 的动态报文字段无法由固定 Hash ORM 映射。
struct DebugPacketStream {
    static std::string key(std::string_view scope, std::string_view id) {
        return "iot:debug:packets:" + std::string(scope) + ':' + std::string(id);
    }
};
}
