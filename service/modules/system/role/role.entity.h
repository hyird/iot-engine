#pragma once

#include <cstdint>
#include <memory_resource>
#include <string>
#include <ruvia/web/db/DbEntity.h>

namespace service::role {

RUVIA_DB_ENTITY(RoleEntity, "sys_role",
                RUVIA_DB_COLUMN(id, std::pmr::string,
                                ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid,
                                                       .primaryKey = true}),
                RUVIA_DB_COLUMN(name, std::pmr::string), RUVIA_DB_COLUMN(code, std::pmr::string),
                RUVIA_DB_COLUMN(description, std::pmr::string,
                                ruvia::DbColumnOptions{.nullable = true}),
                RUVIA_DB_COLUMN(status, std::pmr::string),
                RUVIA_DB_COLUMN(deleted_at, std::pmr::string,
                                ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz,
                                                       .nullable = true}))

} // namespace service::role
