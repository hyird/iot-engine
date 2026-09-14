#pragma once

#include <cstdint>
#include <memory_resource>
#include <string>

#include <ruvia/web/db/DbEntity.h>

namespace service::protocol {

RUVIA_DB_ENTITY(
    ProtocolConfigEntity, "protocol_config",
    RUVIA_DB_COLUMN(id, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid,
                                           .primaryKey = true}),
    RUVIA_DB_COLUMN(protocol, std::pmr::string), RUVIA_DB_COLUMN(name, std::pmr::string),
    RUVIA_DB_COLUMN(enabled, bool),
    RUVIA_DB_COLUMN(config, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kJsonb}),
    RUVIA_DB_COLUMN(remark, std::pmr::string,
                    ruvia::DbColumnOptions{.nullable = true}),
    RUVIA_DB_COLUMN(created_by, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid}),
    RUVIA_DB_COLUMN(created_at, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz}),
    RUVIA_DB_COLUMN(updated_at, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz}),
    RUVIA_DB_COLUMN(deleted_at, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz,
                                           .nullable = true}))


} // namespace service::protocol

namespace service::protocol::entities {

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
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kText}),
    RUVIA_DB_COLUMN(protocol_params, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kJsonb}),
    RUVIA_DB_COLUMN(remark, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kText, .nullable = true}),
    RUVIA_DB_COLUMN(created_by, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid}),
    RUVIA_DB_COLUMN(created_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz}),
    RUVIA_DB_COLUMN(updated_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz}),
    RUVIA_DB_COLUMN(deleted_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .nullable = true}),
    RUVIA_DB_COLUMN(protocol_address, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kText}))

} // namespace service::protocol::entities
