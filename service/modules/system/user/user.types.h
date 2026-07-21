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

RUVIA_REQUEST_MODEL(CreateUserBody, RUVIA_FIELD(username, ruvia::String),
                    RUVIA_FIELD(password, ruvia::String), RUVIA_FIELD(nickname, ruvia::String),
                    RUVIA_FIELD(phone, ruvia::String), RUVIA_FIELD(email, ruvia::String),
                    RUVIA_FIELD(status, ruvia::String),
                    RUVIA_FIELD_NAME("role_ids", roleIds, ruvia::Array<ruvia::String>));

RUVIA_REQUEST_MODEL(UpdateUserBody, RUVIA_FIELD(nickname, ruvia::String),
                    RUVIA_FIELD(phone, ruvia::String), RUVIA_FIELD(email, ruvia::String),
                    RUVIA_FIELD(status, ruvia::String), RUVIA_FIELD(password, ruvia::String),
                    RUVIA_FIELD_NAME("role_ids", roleIds, ruvia::Array<ruvia::String>));

RUVIA_REQUEST_MODEL(UserListQuery, RUVIA_FIELD(page, ruvia::Int64, RUVIA_DEFAULT(1)),
                    RUVIA_FIELD_NAME("pageSize", pageSize, ruvia::Int64, RUVIA_DEFAULT(10)),
                    RUVIA_FIELD(keyword, ruvia::String), RUVIA_FIELD(status, ruvia::String));

RUVIA_REQUEST_MODEL(UserOptionsQuery, RUVIA_FIELD(keyword, ruvia::String));

RUVIA_REQUEST_MODEL(UserIdParams, RUVIA_FIELD(id, ruvia::String));

RUVIA_RESPONSE_MODEL(UserOptionDto, RUVIA_FIELD(id, ruvia::String),
                     RUVIA_FIELD(username, ruvia::String),
                     RUVIA_FIELD(nickname, ruvia::String, RUVIA_OMIT_EMPTY));

RUVIA_RESPONSE_MODEL(UserItemDto, RUVIA_FIELD(id, ruvia::String),
                     RUVIA_FIELD(username, ruvia::String),
                     RUVIA_FIELD(nickname, ruvia::String, RUVIA_OMIT_EMPTY),
                     RUVIA_FIELD(phone, ruvia::String, RUVIA_OMIT_EMPTY),
                     RUVIA_FIELD(email, ruvia::String, RUVIA_OMIT_EMPTY),
                     RUVIA_FIELD(status, ruvia::String),
                     RUVIA_FIELD(roles, ruvia::List<service::role::RoleOptionDto>),
                     RUVIA_FIELD_NAME("created_at", createdAt, ruvia::String),
                     RUVIA_FIELD_NAME("updated_at", updatedAt, ruvia::String));

RUVIA_RESPONSE_MODEL(UserPageDataDto, RUVIA_FIELD(list, ruvia::List<UserItemDto>),
                     RUVIA_FIELD(total, ruvia::Int64), RUVIA_FIELD(page, ruvia::Int64),
                     RUVIA_FIELD_NAME("pageSize", pageSize, ruvia::Int64),
                     RUVIA_FIELD_NAME("totalPages", totalPages, ruvia::Int64));

RUVIA_RESPONSE_MODEL(UserPageResponse, RUVIA_FIELD(code, ruvia::Int64),
                     RUVIA_FIELD(message, ruvia::String), RUVIA_FIELD(data, UserPageDataDto));

RUVIA_RESPONSE_MODEL(UserDetailResponse, RUVIA_FIELD(code, ruvia::Int64),
                     RUVIA_FIELD(message, ruvia::String), RUVIA_FIELD(data, UserItemDto));

RUVIA_RESPONSE_MODEL(UserOptionsResponse, RUVIA_FIELD(code, ruvia::Int64),
                     RUVIA_FIELD(message, ruvia::String),
                     RUVIA_FIELD(data, ruvia::List<UserOptionDto>));

} // namespace service::user
