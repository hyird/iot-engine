#pragma once

#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/utils/jwt.h"

namespace service::middleware {

inline service::core::JwtPayload requireAuth(ruvia::Context& c) {
    const auto token = ruvia::jwtBearerToken(c.req().header("Authorization").value_or(""));
    if (!token)
        service::common::fail(service::common::kUnauthorizedErrorCode, "未登录", 401);
    try {
        return service::utils::verifyAccessToken(c, *token);
    } catch (const service::utils::JwtExpiredError&) {
        service::common::fail(service::common::kTokenExpiredErrorCode, "Token 已过期", 401);
    } catch (...) {
        service::common::fail(service::common::kTokenInvalidErrorCode, "Token 无效", 401);
    }
}

class AuthMiddleware final : public ruvia::Middleware<AuthMiddleware> {
  public:
    ruvia::Task<void> handle(ruvia::Context& c, ruvia::Next& next) {
        (void)requireAuth(c);
        co_await next();
    }
};

} // namespace service::middleware
