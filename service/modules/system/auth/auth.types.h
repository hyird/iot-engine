#pragma once

#include <string>

#include <ruvia/web/Validation.h>

#include "service/modules/system/role/role.types.h"

namespace service::auth {

struct JwtPayload {
    std::string userId;
    std::string username;
};

RUVIA_REQUEST_MODEL(LoginBody, RUVIA_REQUIRED_FIELD(username, ruvia::String, RUVIA_MIN(1, "用户名不能为空")), RUVIA_REQUIRED_FIELD(password, ruvia::String, RUVIA_MIN(1, "密码不能为空")));

RUVIA_REQUEST_MODEL(RefreshBody, RUVIA_REQUIRED_FIELD_NAME("refresh_token", refreshToken, ruvia::String, RUVIA_MIN(1, "刷新令牌不能为空")));

RUVIA_RESPONSE_MODEL(AuthUserInfoDto, RUVIA_OPTIONAL_FIELD(id, ruvia::String), RUVIA_OPTIONAL_FIELD(username, ruvia::String), RUVIA_OPTIONAL_FIELD(nickname, ruvia::String, RUVIA_OMIT_EMPTY), RUVIA_OPTIONAL_FIELD(status, ruvia::String), RUVIA_OPTIONAL_FIELD(roles, ruvia::Array<service::role::RoleOptionDto>), RUVIA_OPTIONAL_FIELD(permissions, ruvia::Array<ruvia::String>));

RUVIA_RESPONSE_MODEL(LoginResultDto, RUVIA_OPTIONAL_FIELD(token, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("refresh_token", refreshToken, ruvia::String), RUVIA_OPTIONAL_FIELD(user, AuthUserInfoDto));

RUVIA_RESPONSE_MODEL(LoginResponse, RUVIA_OPTIONAL_FIELD(code, ruvia::Int64), RUVIA_OPTIONAL_FIELD(message, ruvia::String), RUVIA_OPTIONAL_FIELD(data, LoginResultDto));

RUVIA_RESPONSE_MODEL(CurrentUserResponse, RUVIA_OPTIONAL_FIELD(code, ruvia::Int64), RUVIA_OPTIONAL_FIELD(message, ruvia::String), RUVIA_OPTIONAL_FIELD(data, AuthUserInfoDto));

} // namespace service::auth
