#pragma once

#include <cstdint>
#include <memory_resource>
#include <string>
#include <ruvia/web/db/DbEntity.h>
#include <ruvia/web/db/DbProjection.h>

namespace service::role {

RUVIA_DB_ENTITY(RoleEntity, "sys_role",
                RUVIA_DB_COLUMN(id, std::pmr::string,
                                ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid,
                                                       .primaryKey = true}),
                RUVIA_DB_COLUMN(name, std::pmr::string, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 50}), RUVIA_DB_COLUMN(code, std::pmr::string, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 50}),
                RUVIA_DB_COLUMN(description, std::pmr::string,
                                ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .nullable = true, .length = 255}),
                RUVIA_DB_COLUMN(status, std::pmr::string, ruvia::DbColumnOptions{.enumName = ruvia::FixedString{"status_enum"}, .defaultExpression = ruvia::FixedString{"'enabled'"}}),
                RUVIA_DB_COLUMN(permissions, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kJsonb, .defaultExpression = ruvia::FixedString{"'[]'::jsonb"}}),
    RUVIA_DB_COLUMN(created_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}),
    RUVIA_DB_COLUMN(updated_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}),
    RUVIA_DB_COLUMN(deleted_at, std::pmr::string,
                                ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz,
                                                       .nullable = true}))

RUVIA_DB_PROJECTION(RolePermission, RUVIA_DB_COLUMN(permission, std::pmr::string))

} // namespace service::role
