#pragma once

#include <optional>
#include <string>

#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/middleware/auth.h"
#include "service/middleware/permission.h"
#include "service/modules/system/role/role.types.h"
#include "service/modules/system/role/role.service.h"

namespace service::role {

class RoleController final : public ruvia::Controller<RoleController> {
  public:
    RUVIA_CONTROLLER_GROUP("/v1/roles", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/", list, ruvia::QueryModel<RoleListQuery>);
    RUVIA_GET("/options", options);
    RUVIA_GET("/:id", detail, ruvia::PathModel<RoleIdParams>);
    RUVIA_POST("/", create, ruvia::JsonBody<CreateRoleBody>);
    RUVIA_PUT("/:id", update, ruvia::PathModel<RoleIdParams>, ruvia::JsonBody<UpdateRoleBody>);
    RUVIA_DELETE("/:id", remove, ruvia::PathModel<RoleIdParams>);
    RUVIA_ROUTES_END

  private:
    static std::string id(ruvia::Context& c) {
        return std::string(c.req().validated<RoleIdParams>().get<"id">().view());
    }

    ruvia::Task<> list(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "system:role:query");
        const auto& query = c.req().validated<RoleListQuery>();
        const auto keyword = query.get<"keyword">()
                                 ? std::optional<std::string>(std::string(query.get<"keyword">()->view()))
                                 : std::nullopt;
        const auto status = query.get<"status">()
                                ? std::optional<std::string>(std::string(query.get<"status">()->view()))
                                : std::nullopt;
        co_return c.json(service::common::ok<RolePageResponse>(
            c, co_await roleService().list(c, static_cast<std::int64_t>(*query.get<"page">()),
                                           static_cast<std::int64_t>(*query.get<"pageSize">()), keyword,
                                           status)));
    }

    ruvia::Task<> options(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "system:user:query");
        co_return c.json(service::common::ok<RoleOptionsResponse>(c, co_await roleService().options(c)));
    }

    ruvia::Task<> detail(ruvia::Context& c) {

        co_await service::middleware::requirePermission(c, "system:role:query");
        co_return c.json(service::common::ok<RoleDetailResponse>(c, co_await roleService().detail(c, id(c))));
    }

    ruvia::Task<> create(ruvia::Context& c) {

        co_await service::middleware::requirePermission(c, "system:role:add");
        co_await roleService().create(c, c.req().validated<CreateRoleBody>());
        co_return c.json(service::common::operation(c, "创建成功"));
    }

    ruvia::Task<> update(ruvia::Context& c) {

        co_await service::middleware::requirePermission(c, "system:role:edit");
        co_await roleService().update(c, id(c), c.req().validated<UpdateRoleBody>());
        co_return c.json(service::common::operation(c, "更新成功"));
    }

    ruvia::Task<> remove(ruvia::Context& c) {

        co_await service::middleware::requirePermission(c, "system:role:delete");
        co_await roleService().remove(c, id(c));
        co_return c.json(service::common::operation(c, "删除成功"));
    }
};

} // namespace service::role
