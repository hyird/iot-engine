#pragma once

#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/middleware/auth.h"
#include "service/middleware/permission.h"
#include "service/modules/system/user/user.types.h"
#include "service/modules/system/user/user.service.h"

namespace service::user {

class UserController final : public ruvia::Controller<UserController> {
  public:
    RUVIA_CONTROLLER_GROUP("/v1/users", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/", list, ruvia::QueryModel<UserListQuery>);
    RUVIA_GET("/options", options, ruvia::QueryModel<UserOptionsQuery>);
    RUVIA_GET("/:id", detail, ruvia::PathModel<UserIdParams>);
    RUVIA_POST("/", create, ruvia::JsonBody<CreateUserBody>);
    RUVIA_PUT("/:id", update, ruvia::PathModel<UserIdParams>, ruvia::JsonBody<UpdateUserBody>);
    RUVIA_DELETE("/:id", remove, ruvia::PathModel<UserIdParams>);
    RUVIA_ROUTES_END

  private:
    static std::string id(ruvia::Context& c) {
        return std::string(c.req().validated<UserIdParams>().get<"id">().view());
    }

    ruvia::Task<> list(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "system:user:query");
        const auto& query = c.req().validated<UserListQuery>();
        const auto page = static_cast<std::int64_t>(*query.get<"page">());
        const auto pageSize = static_cast<std::int64_t>(*query.get<"pageSize">());
        const auto keyword = query.get<"keyword">()
                                 ? std::optional<std::string>(std::string(query.get<"keyword">()->view()))
                                 : std::nullopt;
        const auto status = query.get<"status">()
                                ? std::optional<std::string>(std::string(query.get<"status">()->view()))
                                : std::nullopt;
        co_return c.json(service::common::ok<UserPageResponse>(
            c, co_await userService().list(c, page, pageSize, keyword, status)));
    }

    ruvia::Task<> options(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "system:user:query");
        const auto& query = c.req().validated<UserOptionsQuery>();
        const auto keyword = query.get<"keyword">()
                                 ? std::optional<std::string>(std::string(query.get<"keyword">()->view()))
                                 : std::nullopt;
        co_return c.json(service::common::ok<UserOptionsResponse>(
            c, co_await userService().options(c, keyword)));
    }

    ruvia::Task<> detail(ruvia::Context& c) {

        co_await service::middleware::requirePermission(c, "system:user:query");
        co_return c.json(service::common::ok<UserDetailResponse>(c, co_await userService().detail(c, id(c))));
    }

    ruvia::Task<> create(ruvia::Context& c) {

        co_await service::middleware::requirePermission(c, "system:user:add");
        co_await userService().create(c, c.req().validated<CreateUserBody>());
        co_return c.json(service::common::operation(c, "创建成功"));
    }

    ruvia::Task<> update(ruvia::Context& c) {

        co_await service::middleware::requirePermission(c, "system:user:edit");
        co_await userService().update(c, id(c), c.req().validated<UpdateUserBody>());
        co_return c.json(service::common::operation(c, "更新成功"));
    }

    ruvia::Task<> remove(ruvia::Context& c) {

        co_await service::middleware::requirePermission(c, "system:user:delete");
        const auto principal = service::middleware::requireAuth(c);
        co_await userService().remove(c, id(c), principal.userId);
        co_return c.json(service::common::operation(c, "删除成功"));
    }
};

} // namespace service::user
