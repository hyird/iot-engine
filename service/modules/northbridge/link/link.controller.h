#pragma once

#include <optional>
#include <string>

#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/middleware/auth.h"
#include "service/middleware/permission.h"
#include "service/modules/northbridge/link/link.schema.h"
#include "service/modules/northbridge/link/link.service.h"

namespace service::link {

class LinkController final : public ruvia::Controller<LinkController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api/link", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/", list, LinkListQueryValidator);
    RUVIA_GET("/options", options);
    RUVIA_GET("/enums", enums);
    RUVIA_GET("/public-ip", publicIp);
    RUVIA_GET("/:id", detail, LinkIdParamsValidator);
    RUVIA_POST("/", create, SaveLinkValidator);
    RUVIA_PUT("/:id", update, LinkIdParamsValidator, SaveLinkValidator);
    RUVIA_DELETE("/:id", remove, LinkIdParamsValidator);
    RUVIA_ROUTES_END

  private:
    static std::string id(ruvia::Context& c) {
        return std::string(c.req().valid<LinkIdParams>().id()->view());
    }

    static std::optional<std::string> text(const std::optional<ruvia::String>& value) {
        return value ? std::optional<std::string>(std::string(value->view())) : std::nullopt;
    }

    ruvia::Task<ruvia::HttpResponse> list(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:link:query");
        const auto& query = c.req().valid<LinkListQuery>();
        co_return c.json(service::common::ok<LinkPageResponse>(
            c, co_await linkService().list(c, static_cast<std::int64_t>(*query.page()),
                                           static_cast<std::int64_t>(*query.pageSize()),
                                           text(query.keyword()), text(query.mode()),
                                           text(query.protocol()), text(query.status()))));
    }

    ruvia::Task<ruvia::HttpResponse> options(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:link:query");
        co_return c.json(
            service::common::ok<LinkOptionsResponse>(c, co_await linkService().options(c)));
    }

    ruvia::Task<ruvia::HttpResponse> enums(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:link:query");
        co_return c.json(service::common::ok<LinkEnumsResponse>(c, linkService().enums(c)));
    }

    ruvia::Task<ruvia::HttpResponse> publicIp(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:link:query");
        PublicIpDto result(c);
        result.ip(co_await linkService().publicIp(c));
        co_return c.json(service::common::ok<PublicIpResponse>(c, std::move(result)));
    }

    ruvia::Task<ruvia::HttpResponse> detail(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:link:query");
        co_return c.json(
            service::common::ok<LinkDetailResponse>(c, co_await linkService().detail(c, id(c))));
    }

    ruvia::Task<ruvia::HttpResponse> create(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:link:add");
        co_await linkService().create(c, c.req().valid<SaveLinkBody>());
        co_return c.json(service::common::operation(c, "创建成功"));
    }

    ruvia::Task<ruvia::HttpResponse> update(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:link:edit");
        co_await linkService().update(c, id(c), c.req().valid<SaveLinkBody>());
        co_return c.json(service::common::operation(c, "更新成功"));
    }

    ruvia::Task<ruvia::HttpResponse> remove(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:link:delete");
        co_await linkService().remove(c, id(c));
        co_return c.json(service::common::operation(c, "删除成功"));
    }
};

} // namespace service::link
