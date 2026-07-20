#pragma once

#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/middleware/auth.h"
#include "service/modules/system/auth/auth.schema.h"
#include "service/modules/system/auth/auth.service.h"

namespace service::auth {

class AuthController final : public ruvia::Controller<AuthController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api/auth")
    RUVIA_ROUTES_BEGIN
    RUVIA_POST("/login", login, LoginValidator);
    RUVIA_POST("/refresh", refresh, RefreshValidator);
    RUVIA_POST("/logout", logout);
    RUVIA_GET("/me", me, service::middleware::AuthMiddleware);
    RUVIA_ROUTES_END

  private:
    ruvia::Task<ruvia::HttpResponse> login(ruvia::Context& c) {
        co_return c.json(service::common::ok<LoginResponse>(
            c, co_await authService().login(c, c.req().valid<LoginBody>())));
    }

    ruvia::Task<ruvia::HttpResponse> refresh(ruvia::Context& c) {
        co_return c.json(service::common::ok<LoginResponse>(
            c, co_await authService().refresh(c, c.req().valid<RefreshBody>())));
    }

    ruvia::Task<ruvia::HttpResponse> logout(ruvia::Context& c) {
        co_return c.json(service::common::operation(c, "退出成功"));
    }

    ruvia::Task<ruvia::HttpResponse> me(ruvia::Context& c) {
        const auto principal = service::middleware::requireAuth(c);
        co_return c.json(service::common::ok<CurrentUserResponse>(
            c, co_await authService().current(c, principal.userId)));
    }
};

} // namespace service::auth
