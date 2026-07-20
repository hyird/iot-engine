#pragma once

#include <ruvia/web/Controller.h>

#include "service/modules/system/auth/auth.types.h"

namespace service::auth {

class LoginValidator final : public ruvia::Middleware<LoginValidator> {
  public:
    RUVIA_VALIDATE_JSON(LoginBody,
                        RUVIA_RULE(username, RUVIA_REQUIRED("用户名不能为空"),
                                   RUVIA_MIN(1, "用户名不能为空")),
                        RUVIA_RULE(password, RUVIA_REQUIRED("密码不能为空"),
                                   RUVIA_MIN(1, "密码不能为空")))
};

class RefreshValidator final : public ruvia::Middleware<RefreshValidator> {
  public:
    RUVIA_VALIDATE_JSON(RefreshBody, RUVIA_RULE_NAME("refresh_token", refreshToken,
                                                     RUVIA_REQUIRED("刷新令牌不能为空"),
                                                     RUVIA_MIN(1, "刷新令牌不能为空")))
};

} // namespace service::auth
