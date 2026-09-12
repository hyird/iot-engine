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
                                           .nullable = true}),
    RUVIA_DB_COLUMN(revision, std::int64_t))

RUVIA_DB_ENTITY(
    ProtocolRevisionEntity, "protocol_revision",
    RUVIA_DB_COLUMN(id, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid,
                                           .primaryKey = true}),
    RUVIA_DB_COLUMN(revision, std::int64_t,
                    ruvia::DbColumnOptions{.primaryKey = true}),
    RUVIA_DB_COLUMN(protocol, std::pmr::string), RUVIA_DB_COLUMN(name, std::pmr::string),
    RUVIA_DB_COLUMN(config, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kJsonb}),
    RUVIA_DB_COLUMN(remark, std::pmr::string,
                    ruvia::DbColumnOptions{.nullable = true}),
    RUVIA_DB_COLUMN(created_by, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid}),
    RUVIA_DB_COLUMN(created_at, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz}),
    RUVIA_DB_COLUMN(origin, std::pmr::string))

} // namespace service::protocol
