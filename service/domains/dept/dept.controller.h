#pragma once

#include <optional>
#include <string>

#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/middleware/auth.h"
#include "service/middleware/permission.h"
#include "service/domains/dept/dept.schema.h"
#include "service/domains/dept/dept.service.h"

namespace service::dept {

class DeptController final : public ruvia::Controller<DeptController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api/departments", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/", list, DeptListQueryValidator);
    RUVIA_GET("/options", options);
    RUVIA_GET("/:id", detail, DeptIdParamsValidator);
    RUVIA_POST("/", create, CreateDeptValidator);
    RUVIA_PUT("/:id", update, DeptIdParamsValidator, UpdateDeptValidator);
    RUVIA_DELETE("/:id", remove, DeptIdParamsValidator);
    RUVIA_ROUTES_END

  private:
    static std::string id(ruvia::Context& c) {
        return std::string(c.req().valid<DeptIdParams>().id()->view());
    }

    ruvia::Task<ruvia::HttpResponse> list(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "system:dept:query");
        const auto& query = c.req().valid<DeptListQuery>();
        const auto keyword = query.keyword()
                                 ? std::optional<std::string>(std::string(query.keyword()->view()))
                                 : std::nullopt;
        const auto status = query.status()
                                ? std::optional<std::string>(std::string(query.status()->view()))
                                : std::nullopt;
        const auto parentId =
            query.parentId() ? std::optional<std::string>(std::string(query.parentId()->view()))
                             : std::nullopt;
        co_return c.json(service::common::ok<DeptPageResponse>(
            c, co_await deptService().list(c, static_cast<std::int64_t>(*query.page()),
                                           static_cast<std::int64_t>(*query.pageSize()), keyword,
                                           status, parentId)));
    }

    ruvia::Task<ruvia::HttpResponse> options(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "system:dept:query");
        co_return c.json(
            service::common::ok<DeptOptionsResponse>(c, co_await deptService().options(c)));
    }

    ruvia::Task<ruvia::HttpResponse> detail(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "system:dept:query");
        co_return c.json(
            service::common::ok<DeptDetailResponse>(c, co_await deptService().detail(c, id(c))));
    }

    ruvia::Task<ruvia::HttpResponse> create(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "system:dept:add");
        co_await deptService().create(c, c.req().valid<CreateDeptBody>());
        co_return c.json(service::common::operation(c, "创建成功"));
    }

    ruvia::Task<ruvia::HttpResponse> update(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "system:dept:edit");
        co_await deptService().update(c, id(c), c.req().valid<UpdateDeptBody>());
        co_return c.json(service::common::operation(c, "更新成功"));
    }

    ruvia::Task<ruvia::HttpResponse> remove(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "system:dept:delete");
        co_await deptService().remove(c, id(c));
        co_return c.json(service::common::operation(c, "删除成功"));
    }
};

} // namespace service::dept
