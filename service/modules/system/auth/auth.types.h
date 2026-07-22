#pragma once

#include <ruvia/web/Model.h>

#include "service/modules/system/role/role.types.h"

namespace service::auth {

struct LoginBody final {
    RUVIA_OPTIONAL_FIELD(username, ruvia::String);
    RUVIA_OPTIONAL_FIELD(password, ruvia::String);
    RUVIA_MODEL(LoginBody, username, password);
};

struct RefreshBody final {
    RUVIA_OPTIONAL_FIELD_NAME("refresh_token", refreshToken, ruvia::String);
    RUVIA_MODEL(RefreshBody, refreshToken);
};

struct AuthUserInfoDto final {
    RUVIA_OPTIONAL_FIELD(id, ruvia::String);
    RUVIA_OPTIONAL_FIELD(username, ruvia::String);
    RUVIA_OPTIONAL_FIELD(nickname, ruvia::String, RUVIA_OMIT_EMPTY);
    RUVIA_OPTIONAL_FIELD(status, ruvia::String);
    RUVIA_OPTIONAL_FIELD(roles, ruvia::Array<service::role::RoleOptionDto>);
    RUVIA_OPTIONAL_FIELD(permissions, ruvia::Array<ruvia::String>);
    RUVIA_MODEL(AuthUserInfoDto, id, username, nickname, status, roles, permissions);
};

struct LoginResultDto final {
    RUVIA_OPTIONAL_FIELD(token, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("refresh_token", refreshToken, ruvia::String);
    RUVIA_OPTIONAL_FIELD(user, AuthUserInfoDto);
    RUVIA_MODEL(LoginResultDto, token, refreshToken, user);
};

struct LoginResponse final {
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(message, ruvia::String);
    RUVIA_OPTIONAL_FIELD(data, LoginResultDto);
    RUVIA_MODEL(LoginResponse, code, message, data);
};

struct CurrentUserResponse final {
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(message, ruvia::String);
    RUVIA_OPTIONAL_FIELD(data, AuthUserInfoDto);
    RUVIA_MODEL(CurrentUserResponse, code, message, data);
};

} // namespace service::auth
