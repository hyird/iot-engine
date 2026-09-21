#pragma once

#include <string_view>

#include <ruvia/web/Controller.h>
#include <ruvia/web/FixedString.h>

#include "service/common/http.h"
#include "service/middleware/auth.h"
#include "service/modules/system/auth/auth.service.h"

namespace service::middleware {

inline ruvia::Task<void> requirePermission(ruvia::Context& c, std::string_view permission) {
    const auto principal = requireAuth(c);
    co_await service::auth::AuthService::requirePermission(c, principal.userId, permission);
}

template <ruvia::FixedString Permission>
class PermissionMiddleware final : public ruvia::Middleware {
  public:
    ruvia::Task<void> handle(ruvia::Context& c, ruvia::Next& next) {
        co_await requirePermission(c, Permission.view());
        co_await next();
    }
};

} // namespace service::middleware
