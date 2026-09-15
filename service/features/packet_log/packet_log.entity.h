#pragma once
#include <string>
#include <memory_resource>
#include <ruvia/web/db/DbEntity.h>
#include <string_view>
#include <ruvia/web/redis/RedisEntity.h>

namespace service::packet_log {
RUVIA_DB_ENTITY(DebugHistoryEntity, "device_data",
    RUVIA_DB_COLUMN(id, std::pmr::string, ruvia::DbColumnOptions{.dataType=ruvia::DbDataType::kUuid, .primaryKey=true}),
    RUVIA_DB_COLUMN(device_id, std::pmr::string, ruvia::DbColumnOptions{.dataType=ruvia::DbDataType::kUuid}),
    RUVIA_DB_COLUMN(link_id, std::pmr::string, ruvia::DbColumnOptions{.dataType=ruvia::DbDataType::kUuid}),
    RUVIA_DB_COLUMN(data, std::pmr::string, ruvia::DbColumnOptions{.dataType=ruvia::DbDataType::kJsonb}),
    RUVIA_DB_COLUMN(raw_payload_hex, std::pmr::string, ruvia::DbColumnOptions{.dataType=ruvia::DbDataType::kJsonb}),
    RUVIA_DB_COLUMN(source, std::pmr::string, ruvia::DbColumnOptions{.dataType=ruvia::DbDataType::kText}),
    RUVIA_DB_COLUMN(report_time, std::pmr::string, ruvia::DbColumnOptions{.dataType=ruvia::DbDataType::kTimestampTz, .primaryKey=true}))

// 同一报文 Hash 由设备和链路 ZSET 引用。公开 ORM 不支持跨 Hash/ZSET 的
// 原子状态推进、双索引裁剪及引用回收，因此这些操作由 service 中的 Lua 完成。
RUVIA_REDIS_ENTITY(DebugPacketHash, "iot:debug:v2:packet",
    RUVIA_REDIS_COLUMN(event_id, ruvia::String, ruvia::RedisColumnOptions{.primaryKey=true}),
    RUVIA_REDIS_COLUMN(link_id, ruvia::String),
    RUVIA_REDIS_COLUMN(device_id, ruvia::String),
    RUVIA_REDIS_COLUMN(direction, ruvia::String),
    RUVIA_REDIS_COLUMN(source, ruvia::String),
    RUVIA_REDIS_COLUMN(address, ruvia::String),
    RUVIA_REDIS_COLUMN(payload_hex, ruvia::String),
    RUVIA_REDIS_COLUMN(time_ms, ruvia::String),
    RUVIA_REDIS_COLUMN(offset, ruvia::String),
    RUVIA_REDIS_COLUMN(transport_status, ruvia::String),
    RUVIA_REDIS_COLUMN(response_status, ruvia::String),
    RUVIA_REDIS_COLUMN(parse_status, ruvia::String),
    RUVIA_REDIS_COLUMN(storage_status, ruvia::String),
    RUVIA_REDIS_COLUMN(reason, ruvia::String),
    RUVIA_REDIS_COLUMN(history_id, ruvia::String),
    RUVIA_REDIS_COLUMN(parsed_json, ruvia::String),
    RUVIA_REDIS_COLUMN(reply_to_packet_id, ruvia::String),
    RUVIA_REDIS_COLUMN(revision, ruvia::String),
    RUVIA_REDIS_COLUMN(created_ms, ruvia::String),
    RUVIA_REDIS_COLUMN(link_index, ruvia::String),
    RUVIA_REDIS_COLUMN(device_index, ruvia::String));

struct DebugPacketStorage {
    static std::string key(std::string_view scope, std::string_view id) {
        return "iot:debug:v2:" + std::string(scope) + ':' + std::string(id);
    }
    static constexpr std::string_view packetPrefix = "iot:debug:v2:packet:";
};
}
