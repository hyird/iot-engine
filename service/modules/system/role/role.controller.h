#pragma once

#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/middleware/auth.h"
#include "service/middleware/permission.h"
#include "service/modules/system/role/role.service.h"

namespace service::role {

class RoleController final : public ruvia::Controller<RoleController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api/roles", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/options", options);
    RUVIA_ROUTES_END

  private:
    ruvia::Task<ruvia::HttpResponse> options(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "system:user:query");
        co_return c.json(service::common::ok<RoleOptionsResponse>(c, co_await listRoleOptions(c)));
    }
};

} // namespace service::role
