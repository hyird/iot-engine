#pragma once

#include <cstdint>
#include <memory_resource>
#include <string>
#include <ruvia/web/db/DbEntity.h>

namespace service::user {

RUVIA_DB_ENTITY(
    UserEntity, "sys_user",
    RUVIA_DB_COLUMN(id, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid,
                                           .primaryKey = true}),
    RUVIA_DB_COLUMN(username, std::pmr::string), RUVIA_DB_COLUMN(password_hash, std::pmr::string),
    RUVIA_DB_COLUMN(nickname, std::pmr::string, ruvia::DbColumnOptions{.nullable = true}),
    RUVIA_DB_COLUMN(phone, std::pmr::string, ruvia::DbColumnOptions{.nullable = true}),
    RUVIA_DB_COLUMN(email, std::pmr::string, ruvia::DbColumnOptions{.nullable = true}),
    RUVIA_DB_COLUMN(status, std::pmr::string),
    RUVIA_DB_COLUMN(department_id, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid, .nullable = true}),
    RUVIA_DB_COLUMN(deleted_at, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz,
                                           .nullable = true}))

RUVIA_DB_ENTITY(UserRoleEntity, "sys_user_role",
                RUVIA_DB_COLUMN(id, std::pmr::string,
                                ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid,
                                                       .primaryKey = true}),
                RUVIA_DB_COLUMN(user_id, std::pmr::string,
                                ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid}),
                RUVIA_DB_COLUMN(role_id, std::pmr::string,
                                ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid}))

} // namespace service::user
