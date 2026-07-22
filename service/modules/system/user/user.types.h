#pragma once

#include <string_view>

#include <ruvia/web/Model.h>

#include "service/modules/system/role/role.types.h"

namespace service::user {

inline bool isPhoneNumber(const ruvia::String& value) {
    const auto text = value.view();
    if (text.empty())
        return true;
    if (text.size() < 7 || text.size() > 20)
        return false;
    for (const char ch : text) {
        if ((ch < '0' || ch > '9') && ch != '+' && ch != '-' && ch != ' ')
            return false;
    }
    return true;
}

struct CreateUserBody final {
    RUVIA_OPTIONAL_FIELD(username, ruvia::String);
    RUVIA_OPTIONAL_FIELD(password, ruvia::String);
    RUVIA_OPTIONAL_FIELD(nickname, ruvia::String);
    RUVIA_OPTIONAL_FIELD(phone, ruvia::String);
    RUVIA_OPTIONAL_FIELD(email, ruvia::String);
    RUVIA_OPTIONAL_FIELD(status, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("department_id", departmentId, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("role_ids", roleIds, ruvia::Array<ruvia::String>);
    RUVIA_MODEL(CreateUserBody, username, password, nickname, phone, email, status, departmentId,
                roleIds);
};

struct UpdateUserBody final {
    RUVIA_OPTIONAL_FIELD(nickname, ruvia::String);
    RUVIA_OPTIONAL_FIELD(phone, ruvia::String);
    RUVIA_OPTIONAL_FIELD(email, ruvia::String);
    RUVIA_OPTIONAL_FIELD(status, ruvia::String);
    RUVIA_OPTIONAL_FIELD(password, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("department_id", departmentId, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("role_ids", roleIds, ruvia::Array<ruvia::String>);
    RUVIA_MODEL(UpdateUserBody, nickname, phone, email, status, password, departmentId, roleIds);
};

struct UserListQuery final {
    RUVIA_OPTIONAL_FIELD(page, ruvia::Int64, RUVIA_DEFAULT(1));
    RUVIA_OPTIONAL_FIELD_NAME("pageSize", pageSize, ruvia::Int64, RUVIA_DEFAULT(10));
    RUVIA_OPTIONAL_FIELD(keyword, ruvia::String);
    RUVIA_OPTIONAL_FIELD(status, ruvia::String);
    RUVIA_MODEL(UserListQuery, page, pageSize, keyword, status);
};

struct UserOptionsQuery final {
    RUVIA_OPTIONAL_FIELD(keyword, ruvia::String);
    RUVIA_MODEL(UserOptionsQuery, keyword);
};

struct UserIdParams final {
    RUVIA_OPTIONAL_FIELD(id, ruvia::String);
    RUVIA_MODEL(UserIdParams, id);
};

struct UserOptionDto final {
    RUVIA_OPTIONAL_FIELD(id, ruvia::String);
    RUVIA_OPTIONAL_FIELD(username, ruvia::String);
    RUVIA_OPTIONAL_FIELD(nickname, ruvia::String, RUVIA_OMIT_EMPTY);
    RUVIA_MODEL(UserOptionDto, id, username, nickname);
};

struct UserItemDto final {
    RUVIA_OPTIONAL_FIELD(id, ruvia::String);
    RUVIA_OPTIONAL_FIELD(username, ruvia::String);
    RUVIA_OPTIONAL_FIELD(nickname, ruvia::String, RUVIA_OMIT_EMPTY);
    RUVIA_OPTIONAL_FIELD(phone, ruvia::String, RUVIA_OMIT_EMPTY);
    RUVIA_OPTIONAL_FIELD(email, ruvia::String, RUVIA_OMIT_EMPTY);
    RUVIA_OPTIONAL_FIELD(status, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("department_id", departmentId, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("department_name", departmentName, ruvia::String,
                              RUVIA_OMIT_EMPTY);
    RUVIA_OPTIONAL_FIELD(roles, ruvia::List<service::role::RoleOptionDto>);
    RUVIA_OPTIONAL_FIELD_NAME("created_at", createdAt, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("updated_at", updatedAt, ruvia::String);
    RUVIA_MODEL(UserItemDto, id, username, nickname, phone, email, status, departmentId,
                departmentName, roles, createdAt, updatedAt);
};

struct UserPageDataDto final {
    RUVIA_OPTIONAL_FIELD(list, ruvia::List<UserItemDto>);
    RUVIA_OPTIONAL_FIELD(total, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(page, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD_NAME("pageSize", pageSize, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD_NAME("totalPages", totalPages, ruvia::Int64);
    RUVIA_MODEL(UserPageDataDto, list, total, page, pageSize, totalPages);
};

struct UserPageResponse final {
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(message, ruvia::String);
    RUVIA_OPTIONAL_FIELD(data, UserPageDataDto);
    RUVIA_MODEL(UserPageResponse, code, message, data);
};

struct UserDetailResponse final {
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(message, ruvia::String);
    RUVIA_OPTIONAL_FIELD(data, UserItemDto);
    RUVIA_MODEL(UserDetailResponse, code, message, data);
};

struct UserOptionsResponse final {
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(message, ruvia::String);
    RUVIA_OPTIONAL_FIELD(data, ruvia::List<UserOptionDto>);
    RUVIA_MODEL(UserOptionsResponse, code, message, data);
};

} // namespace service::user
