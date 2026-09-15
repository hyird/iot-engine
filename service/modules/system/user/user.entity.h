#pragma once

#include <cstdint>
#include <memory_resource>
#include <string>
#include <ruvia/web/db/DbEntity.h>
#include <ruvia/web/db/DbProjection.h>

namespace service::user {

RUVIA_DB_ENTITY(
    UserEntity, "sys_user",
    RUVIA_DB_COLUMN(id, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid,
                                           .primaryKey = true}),
    RUVIA_DB_COLUMN(username, std::pmr::string, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 50}), RUVIA_DB_COLUMN(password_hash, std::pmr::string, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 255}),
    RUVIA_DB_COLUMN(nickname, std::pmr::string, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .nullable = true, .length = 100}),
    RUVIA_DB_COLUMN(phone, std::pmr::string, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .nullable = true, .length = 20}),
    RUVIA_DB_COLUMN(email, std::pmr::string, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .nullable = true, .length = 100}),
    RUVIA_DB_COLUMN(status, std::pmr::string, ruvia::DbColumnOptions{.enumName = ruvia::FixedString{"status_enum"}, .defaultExpression = ruvia::FixedString{"'enabled'"}}),
    RUVIA_DB_COLUMN(department_id, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid, .nullable = true}),
    RUVIA_DB_COLUMN(created_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}),
    RUVIA_DB_COLUMN(updated_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}),
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

RUVIA_DB_PROJECTION(UserDetails,
    RUVIA_DB_COLUMN(id, std::pmr::string),
    RUVIA_DB_COLUMN(username, std::pmr::string),
    RUVIA_DB_COLUMN(nickname, std::pmr::string),
    RUVIA_DB_COLUMN(phone, std::pmr::string),
    RUVIA_DB_COLUMN(email, std::pmr::string),
    RUVIA_DB_COLUMN(status, std::pmr::string),
    RUVIA_DB_COLUMN(department_id, std::pmr::string),
    RUVIA_DB_COLUMN(department_name, std::pmr::string),
    RUVIA_DB_COLUMN(created_at, std::pmr::string),
    RUVIA_DB_COLUMN(updated_at, std::pmr::string))

} // namespace service::user
