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
    RUVIA_GET("/", list, service::middleware::PermissionMiddleware<"system:role:query">, ruvia::QueryModel<RoleListQuery>);
    RUVIA_GET("/options", options, service::middleware::PermissionMiddleware<"system:user:query">);
    RUVIA_GET("/:id", detail, service::middleware::PermissionMiddleware<"system:role:query">, ruvia::PathModel<RoleIdParams>);
    RUVIA_POST("/", create, service::middleware::PermissionMiddleware<"system:role:add">, ruvia::JsonBody<CreateRoleBody>);
    RUVIA_PUT("/:id", update, service::middleware::PermissionMiddleware<"system:role:edit">, ruvia::PathModel<RoleIdParams>, ruvia::JsonBody<UpdateRoleBody>);
    RUVIA_DELETE("/:id", remove, service::middleware::PermissionMiddleware<"system:role:delete">, ruvia::PathModel<RoleIdParams>);
    RUVIA_ROUTES_END

  private:
    static std::string id(ruvia::Context& c) {
        return std::string(c.req().validated<RoleIdParams>().get<"id">().view());
    }

    ruvia::Task<ruvia::HttpResponse> list(ruvia::Context& c) {
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

    ruvia::Task<ruvia::HttpResponse> options(ruvia::Context& c) {
        co_return c.json(service::common::ok<RoleOptionsResponse>(c, co_await roleService().options(c)));
    }

    ruvia::Task<ruvia::HttpResponse> detail(ruvia::Context& c) {
        co_return c.json(service::common::ok<RoleDetailResponse>(c, co_await roleService().detail(c, id(c))));
    }

    ruvia::Task<ruvia::HttpResponse> create(ruvia::Context& c) {
        co_await roleService().create(c, c.req().validated<CreateRoleBody>());
        co_return c.json(service::common::operation(c, "创建成功"));
    }

    ruvia::Task<ruvia::HttpResponse> update(ruvia::Context& c) {
        co_await roleService().update(c, id(c), c.req().validated<UpdateRoleBody>());
        co_return c.json(service::common::operation(c, "更新成功"));
    }

    ruvia::Task<ruvia::HttpResponse> remove(ruvia::Context& c) {
        co_await roleService().remove(c, id(c));
        co_return c.json(service::common::operation(c, "删除成功"));
    }
};

} // namespace service::role
