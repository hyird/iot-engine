#pragma once

#include <ruvia/web/Model.h>

#include "service/modules/system/role/role.types.h"

namespace service::auth {

RUVIA_REQUEST_MODEL(LoginBody, RUVIA_FIELD(username, ruvia::String),
                    RUVIA_FIELD(password, ruvia::String));

RUVIA_REQUEST_MODEL(RefreshBody, RUVIA_FIELD_NAME("refresh_token", refreshToken, ruvia::String));

RUVIA_RESPONSE_MODEL(AuthUserInfoDto, RUVIA_FIELD(id, ruvia::String),
                     RUVIA_FIELD(username, ruvia::String),
                     RUVIA_FIELD(nickname, ruvia::String, RUVIA_OMIT_EMPTY),
                     RUVIA_FIELD(status, ruvia::String),
                     RUVIA_FIELD(roles, ruvia::Array<service::role::RoleOptionDto>),
                     RUVIA_FIELD(permissions, ruvia::Array<ruvia::String>));

RUVIA_RESPONSE_MODEL(LoginResultDto, RUVIA_FIELD(token, ruvia::String),
                     RUVIA_FIELD_NAME("refresh_token", refreshToken, ruvia::String),
                     RUVIA_FIELD(user, AuthUserInfoDto));

RUVIA_RESPONSE_MODEL(LoginResponse, RUVIA_FIELD(code, ruvia::Int64),
                     RUVIA_FIELD(message, ruvia::String), RUVIA_FIELD(data, LoginResultDto));

RUVIA_RESPONSE_MODEL(CurrentUserResponse, RUVIA_FIELD(code, ruvia::Int64),
                     RUVIA_FIELD(message, ruvia::String), RUVIA_FIELD(data, AuthUserInfoDto));

} // namespace service::auth
