#pragma once

#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/middleware/auth.h"
#include "service/middleware/permission.h"
#include "service/modules/system/user/user.schema.h"
#include "service/modules/system/user/user.service.h"

namespace service::user {

class UserController final : public ruvia::Controller<UserController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api/users", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/", list, UserListQueryValidator);
    RUVIA_GET("/options", options, UserOptionsQueryValidator);
    RUVIA_GET("/:id", detail, UserIdParamsValidator);
    RUVIA_POST("/", create, CreateUserValidator);
    RUVIA_PUT("/:id", update, UserIdParamsValidator, UpdateUserValidator);
    RUVIA_DELETE("/:id", remove, UserIdParamsValidator);
    RUVIA_ROUTES_END

  private:
    static std::int64_t positiveId(ruvia::Context& c) {
        return static_cast<std::int64_t>(*c.req().valid<UserIdParams>().id());
    }

    ruvia::Task<ruvia::HttpResponse> list(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "system:user:query");
        const auto& query = c.req().valid<UserListQuery>();
        const auto page = static_cast<std::int64_t>(*query.page());
        const auto pageSize = static_cast<std::int64_t>(*query.pageSize());
        const auto keyword = query.keyword()
                                 ? std::optional<std::string>(std::string(query.keyword()->view()))
                                 : std::nullopt;
        const auto status = query.status()
                                ? std::optional<std::string>(std::string(query.status()->view()))
                                : std::nullopt;
        co_return c.json(service::common::ok<UserPageResponse>(
            c, co_await userService().list(c, page, pageSize, keyword, status)));
    }

    ruvia::Task<ruvia::HttpResponse> options(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "system:user:query");
        const auto& query = c.req().valid<UserOptionsQuery>();
        const auto keyword = query.keyword()
                                 ? std::optional<std::string>(std::string(query.keyword()->view()))
                                 : std::nullopt;
        co_return c.json(service::common::ok<UserOptionsResponse>(
            c, co_await userService().options(c, keyword)));
    }

    ruvia::Task<ruvia::HttpResponse> detail(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "system:user:query");
        co_return c.json(service::common::ok<UserDetailResponse>(
            c, co_await userService().detail(c, positiveId(c))));
    }

    ruvia::Task<ruvia::HttpResponse> create(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "system:user:add");
        co_await userService().create(c, c.req().valid<CreateUserBody>());
        co_return c.json(service::common::operation(c, "创建成功"));
    }

    ruvia::Task<ruvia::HttpResponse> update(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "system:user:edit");
        co_await userService().update(c, positiveId(c), c.req().valid<UpdateUserBody>());
        co_return c.json(service::common::operation(c, "更新成功"));
    }

    ruvia::Task<ruvia::HttpResponse> remove(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "system:user:delete");
        const auto principal = service::middleware::requireAuth(c);
        co_await userService().remove(c, positiveId(c), principal.userId);
        co_return c.json(service::common::operation(c, "删除成功"));
    }
};

} // namespace service::user
