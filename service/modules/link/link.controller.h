#pragma once

#include <optional>
#include <string>

#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/middleware/auth.h"
#include "service/middleware/live.h"
#include "service/middleware/permission.h"
#include "service/modules/link/link.schema.h"
#include "service/modules/link/link.service.h"

namespace service::link {

class LinkController final : public ruvia::Controller<LinkController> {
  public:
    RUVIA_CONTROLLER_GROUP("/v1/link", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET_SSE("/events", linkEvents, LinkListQueryValidator, LinkEventsQueryValidator);
    RUVIA_GET("/:id/debug/packets", debugPackets, LinkIdParamsValidator);
    RUVIA_PUT("/:id/debug", setDebug, LinkIdParamsValidator, LinkDebugValidator);
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
    ruvia::Task<ruvia::HttpResponse> debugPackets(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await debugPacketsSnapshot(c, request, id(c));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> debugPacketsSnapshot(ruvia::Context& c, service::middleware::RequestContext& request, std::string_view linkId) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:link:edit");
        co_return service::live::json(service::common::ok<LinkDebugPacketsResponse>(request, co_await linkService().debugPackets(request, linkId)));
    }

    ruvia::Task<ruvia::HttpResponse> setDebug(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:link:edit");
        co_await linkService().setDebug(request, id(c), *c.req().validated<LinkDebugBody>().get<"enabled">());
        co_return c.json(service::common::operation(c, "调试设置已保存"));
    }

    static std::string id(ruvia::Context& c) {
        return std::string(c.req().validated<LinkIdParams>().get<"id">()->view());
    }

    static std::optional<std::string> text(const std::optional<ruvia::String>& value) {
        return value ? std::optional<std::string>(std::string(value->view())) : std::nullopt;
    }

    ruvia::Task<ruvia::HttpResponse> list(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await listSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<void> linkEvents(ruvia::Context& c) {
        service::middleware::RequestContext access(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(access, access.userId, "iot:link:query");
        std::vector<service::live::SnapshotChannel> channels;
        channels.push_back({ "links", "link", [this, &c](service::middleware::RequestContext& request) {
                                return listSnapshot(c, request);
                            } });
        if (const auto& value = c.req().validated<LinkEventsQuery>().get<"debugLinkId">()) {
            channels.push_back({ "packets", "packet-debug", [this, &c, linkId = std::string(value->view())](service::middleware::RequestContext& request) {
                                    return debugPacketsSnapshot(c, request, linkId);
                                } });
        }
        co_await service::live::serveSnapshotChannels(c, access.userId, std::move(channels), [&c] {
            (void)service::middleware::requireAuth(c);
        });
    }

    ruvia::Task<std::string> listSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:link:query");
        const auto& query = c.req().validated<LinkListQuery>();
        co_return service::live::json(service::common::ok<LinkPageResponse>(request, co_await linkService().list(request, static_cast<std::int64_t>(*query.get<"page">()), static_cast<std::int64_t>(*query.get<"pageSize">()), text(query.get<"keyword">()), text(query.get<"mode">()), text(query.get<"protocol">()), text(query.get<"status">()))));
    }

    ruvia::Task<ruvia::HttpResponse> options(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await optionsSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> optionsSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:link:query");
        co_return service::live::json(
            service::common::ok<LinkOptionsResponse>(request, co_await linkService().options(request))
        );
    }

    ruvia::Task<ruvia::HttpResponse> enums(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await enumsSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> enumsSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:link:query");
        co_return service::live::json(service::common::ok<LinkEnumsResponse>(request, linkService().enums(request)));
    }

    ruvia::Task<ruvia::HttpResponse> publicIp(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await publicIpSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> publicIpSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:link:query");
        PublicIpDto result(ruvia::ModelOptions{ .resource = request.arena() });
        result.set<"ip">(co_await linkService().publicIp(request));
        co_return service::live::json(service::common::ok<PublicIpResponse>(request, std::move(result)));
    }

    ruvia::Task<ruvia::HttpResponse> detail(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await detailSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> detailSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:link:query");
        co_return service::live::json(
            service::common::ok<LinkDetailResponse>(request, co_await linkService().detail(request, id(c)))
        );
    }

    ruvia::Task<ruvia::HttpResponse> create(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:link:add");
        co_await linkService().create(request, c.req().validated<SaveLinkBody>());
        co_return c.json(service::common::operation(c, "创建成功"));
    }

    ruvia::Task<ruvia::HttpResponse> update(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:link:edit");
        co_await linkService().update(request, id(c), c.req().validated<SaveLinkBody>());
        co_return c.json(service::common::operation(c, "更新成功"));
    }

    ruvia::Task<ruvia::HttpResponse> remove(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:link:delete");
        co_await linkService().remove(request, id(c));
        co_return c.json(service::common::operation(c, "删除成功"));
    }
};

} // namespace service::link
