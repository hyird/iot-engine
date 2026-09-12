#pragma once

#include <optional>
#include <string>

#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/middleware/live.h"
#include "service/middleware/auth.h"
#include "service/middleware/permission.h"
#include "service/modules/link/link.schema.h"
#include "service/modules/link/link.service.h"

namespace service::link {

class LinkController final : public ruvia::Controller<LinkController> {
  public:
    RUVIA_CONTROLLER_GROUP("/v1/link", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET_SSE("/", list, LinkListQueryValidator);
    RUVIA_GET_SSE("/options", options);
    RUVIA_GET_SSE("/enums", enums);
    RUVIA_GET_SSE("/public-ip", publicIp);
    RUVIA_GET_SSE("/:id", detail, LinkIdParamsValidator);
    RUVIA_POST("/", create, SaveLinkValidator);
    RUVIA_PUT("/:id", update, LinkIdParamsValidator, SaveLinkValidator);
    RUVIA_DELETE("/:id", remove, LinkIdParamsValidator);
    RUVIA_ROUTES_END

  private:
    static std::string id(ruvia::Context& c) {
        return std::string(c.req().validated<LinkIdParams>().get<"id">()->view());
    }

    static std::optional<std::string> text(const std::optional<ruvia::String>& value) {
        return value ? std::optional<std::string>(std::string(value->view())) : std::nullopt;
    }

    ruvia::Task<void> list(ruvia::Context& c) {
        co_await service::live::serve(c, "link", [this, &c]() { return listSnapshot(c); });
    }

    ruvia::Task<std::string> listSnapshot(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:link:query");
        const auto& query = c.req().validated<LinkListQuery>();
        co_return service::live::json(service::common::ok<LinkPageResponse>(
            c, co_await linkService().list(c, static_cast<std::int64_t>(*query.get<"page">()),
                                           static_cast<std::int64_t>(*query.get<"pageSize">()),
                                           text(query.get<"keyword">()), text(query.get<"mode">()),
                                           text(query.get<"protocol">()), text(query.get<"status">()))));
    }

    ruvia::Task<void> options(ruvia::Context& c) {
        co_await service::live::serve(c, "link", [this, &c]() { return optionsSnapshot(c); });
    }

    ruvia::Task<std::string> optionsSnapshot(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:link:query");
        co_return service::live::json(
            service::common::ok<LinkOptionsResponse>(c, co_await linkService().options(c)));
    }

    ruvia::Task<void> enums(ruvia::Context& c) {
        co_await service::live::serve(c, "link", [this, &c]() { return enumsSnapshot(c); });
    }

    ruvia::Task<std::string> enumsSnapshot(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:link:query");
        co_return service::live::json(service::common::ok<LinkEnumsResponse>(c, linkService().enums(c)));
    }

    ruvia::Task<void> publicIp(ruvia::Context& c) {
        co_await service::live::serve(c, "link", [this, &c]() { return publicIpSnapshot(c); });
    }

    ruvia::Task<std::string> publicIpSnapshot(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:link:query");
        PublicIpDto result(ruvia::ModelOptions{.resource = c.arena()});
        result.set<"ip">(co_await linkService().publicIp(c));
        co_return service::live::json(service::common::ok<PublicIpResponse>(c, std::move(result)));
    }

    ruvia::Task<void> detail(ruvia::Context& c) {
        co_await service::live::serve(c, "link", [this, &c]() { return detailSnapshot(c); });
    }

    ruvia::Task<std::string> detailSnapshot(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:link:query");
        co_return service::live::json(
            service::common::ok<LinkDetailResponse>(c, co_await linkService().detail(c, id(c))));
    }

    ruvia::Task<ruvia::HttpResponse> create(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:link:add");
        co_await linkService().create(c, c.req().validated<SaveLinkBody>());
        co_return c.json(service::common::operation(c, "创建成功"));
    }

    ruvia::Task<ruvia::HttpResponse> update(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:link:edit");
        co_await linkService().update(c, id(c), c.req().validated<SaveLinkBody>());
        co_return c.json(service::common::operation(c, "更新成功"));
    }

    ruvia::Task<ruvia::HttpResponse> remove(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:link:delete");
        co_await linkService().remove(c, id(c));
        co_return c.json(service::common::operation(c, "删除成功"));
    }
};

} // namespace service::link
