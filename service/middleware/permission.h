#pragma once

#include <string_view>

#include <ruvia/web/db/Db.h>

#include "service/common/http.h"
#include "service/middleware/auth.h"
#include "service/modules/system/auth/auth.service.h"

namespace service::middleware {

inline ruvia::Task<void> requirePermission(ruvia::Context& c, std::string_view permission) {
    const auto principal = requireAuth(c);
    co_await service::auth::AuthService::requirePermission(c, principal.userId, permission);
}

} // namespace service::middleware
