#pragma once

#include <cstdint>
#include <memory_resource>
#include <string>
#include <ruvia/web/db/DbEntity.h>
#include <ruvia/web/db/DbProjection.h>

namespace service::dept {

RUVIA_DB_ENTITY(
    DeptEntity, "sys_department",
    RUVIA_DB_COLUMN(id, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid,
                                           .primaryKey = true}),
    RUVIA_DB_COLUMN(name, std::pmr::string, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 100}),
    RUVIA_DB_COLUMN(code, std::pmr::string, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .nullable = true, .length = 50}),
    RUVIA_DB_COLUMN(parent_id, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid, .nullable = true}),
    RUVIA_DB_COLUMN(leader_id, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid, .nullable = true}),
    RUVIA_DB_COLUMN(sort_order, std::int64_t, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kInteger, .defaultExpression = ruvia::FixedString{"0"}}), RUVIA_DB_COLUMN(status, std::pmr::string, ruvia::DbColumnOptions{.enumName = ruvia::FixedString{"status_enum"}, .defaultExpression = ruvia::FixedString{"'enabled'"}}),
    RUVIA_DB_COLUMN(created_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}),
    RUVIA_DB_COLUMN(updated_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}),
    RUVIA_DB_COLUMN(deleted_at, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz,
                                           .nullable = true}))

RUVIA_DB_PROJECTION(DepartmentDetails,
    RUVIA_DB_COLUMN(id, std::pmr::string),
    RUVIA_DB_COLUMN(name, std::pmr::string),
    RUVIA_DB_COLUMN(code, std::pmr::string),
    RUVIA_DB_COLUMN(parent_id, std::pmr::string),
    RUVIA_DB_COLUMN(parent_name, std::pmr::string),
    RUVIA_DB_COLUMN(leader_id, std::pmr::string),
    RUVIA_DB_COLUMN(leader_name, std::pmr::string),
    RUVIA_DB_COLUMN(sort_order, std::int64_t),
    RUVIA_DB_COLUMN(status, std::pmr::string),
    RUVIA_DB_COLUMN(created_at, std::pmr::string),
    RUVIA_DB_COLUMN(updated_at, std::pmr::string))

} // namespace service::dept
