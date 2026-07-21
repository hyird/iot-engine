#pragma once

#include <optional>
#include <string>

#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/middleware/auth.h"
#include "service/middleware/permission.h"
#include "service/modules/system/role/role.schema.h"
#include "service/modules/system/role/role.service.h"

namespace service::role {

class RoleController final : public ruvia::Controller<RoleController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api/roles", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/", list, RoleListQueryValidator);
    RUVIA_GET("/options", options);
    RUVIA_GET("/:id", detail, RoleIdParamsValidator);
    RUVIA_POST("/", create, CreateRoleValidator);
    RUVIA_PUT("/:id", update, RoleIdParamsValidator, UpdateRoleValidator);
    RUVIA_DELETE("/:id", remove, RoleIdParamsValidator);
    RUVIA_ROUTES_END

  private:
    static std::string id(ruvia::Context& c) {
        return std::string(c.req().valid<RoleIdParams>().id()->view());
    }

    ruvia::Task<ruvia::HttpResponse> list(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "system:role:query");
        const auto& query = c.req().valid<RoleListQuery>();
        const auto keyword = query.keyword()
                                 ? std::optional<std::string>(std::string(query.keyword()->view()))
                                 : std::nullopt;
        const auto status = query.status()
                                ? std::optional<std::string>(std::string(query.status()->view()))
                                : std::nullopt;
        co_return c.json(service::common::ok<RolePageResponse>(
            c, co_await roleService().list(c, static_cast<std::int64_t>(*query.page()),
                                           static_cast<std::int64_t>(*query.pageSize()), keyword,
                                           status)));
    }

    ruvia::Task<ruvia::HttpResponse> options(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "system:user:query");
        co_return c.json(
            service::common::ok<RoleOptionsResponse>(c, co_await roleService().options(c)));
    }

    ruvia::Task<ruvia::HttpResponse> detail(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "system:role:query");
        co_return c.json(
            service::common::ok<RoleDetailResponse>(c, co_await roleService().detail(c, id(c))));
    }

    ruvia::Task<ruvia::HttpResponse> create(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "system:role:add");
        co_await roleService().create(c, c.req().valid<CreateRoleBody>());
        co_return c.json(service::common::operation(c, "创建成功"));
    }

    ruvia::Task<ruvia::HttpResponse> update(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "system:role:edit");
        co_await roleService().update(c, id(c), c.req().valid<UpdateRoleBody>());
        co_return c.json(service::common::operation(c, "更新成功"));
    }

    ruvia::Task<ruvia::HttpResponse> remove(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "system:role:delete");
        co_await roleService().remove(c, id(c));
        co_return c.json(service::common::operation(c, "删除成功"));
    }
};

} // namespace service::role
