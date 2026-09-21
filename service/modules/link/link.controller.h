#pragma once

#include <optional>
#include <string>

#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/middleware/auth.h"
#include "service/middleware/live.h"
#include "service/middleware/permission.h"
#include "service/middleware/validation.h"
#include "service/modules/link/link.types.h"
#include "service/modules/link/link.service.h"

namespace service::link {

class LinkController final : public ruvia::Controller<LinkController> {
  public:
    RUVIA_CONTROLLER_GROUP("/v1/link", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET_SSE("/events", linkEvents, service::middleware::PermissionMiddleware<"iot:link:query">, ruvia::QueryModel<LinkListQuery>, ruvia::QueryModel<LinkEventsQuery>);
    RUVIA_GET("/:id/debug/packets", debugPackets, service::middleware::PermissionMiddleware<"iot:link:edit">, ruvia::PathModel<LinkIdParams>);
    RUVIA_PUT("/:id/debug", setDebug, service::middleware::PermissionMiddleware<"iot:link:edit">, ruvia::PathModel<LinkIdParams>, ruvia::JsonBody<LinkDebugBody>);
    RUVIA_GET("/", list, service::middleware::PermissionMiddleware<"iot:link:query">, ruvia::QueryModel<LinkListQuery>);
    RUVIA_GET("/options", options, service::middleware::PermissionMiddleware<"iot:link:query">);
    RUVIA_GET("/enums", enums, service::middleware::PermissionMiddleware<"iot:link:query">);
    RUVIA_GET("/public-ip", publicIp, service::middleware::PermissionMiddleware<"iot:link:query">);
    RUVIA_GET("/:id", detail, service::middleware::PermissionMiddleware<"iot:link:query">, ruvia::PathModel<LinkIdParams>);
    RUVIA_POST("/", create, service::middleware::PermissionMiddleware<"iot:link:add">, service::middleware::ValidationErrorCodeMiddleware<10001, "name", 15002, "too_small">, ruvia::JsonBody<SaveLinkBody>);
    RUVIA_PUT("/:id", update, service::middleware::PermissionMiddleware<"iot:link:edit">, ruvia::PathModel<LinkIdParams>, service::middleware::ValidationErrorCodeMiddleware<10001, "name", 15002, "too_small">, ruvia::JsonBody<SaveLinkBody>);
    RUVIA_DELETE("/:id", remove, service::middleware::PermissionMiddleware<"iot:link:delete">, ruvia::PathModel<LinkIdParams>);
    RUVIA_ROUTES_END

  private:
    ruvia::Task<ruvia::HttpResponse> debugPackets(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_return c.json(co_await queryDebugPackets(c, request, id(c)));
    }

    ruvia::Task<LinkDebugPacketsResponse> queryDebugPackets(ruvia::Context& c, service::middleware::RequestContext& request, std::string_view linkId) {
        co_return service::common::ok<LinkDebugPacketsResponse>(request, co_await linkService().debugPackets(request, linkId));
    }

    ruvia::Task<ruvia::HttpResponse> setDebug(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await linkService().setDebug(request, id(c), c.req().validated<LinkDebugBody>().get<"enabled">());
        co_return c.json(service::common::operation(c, "调试设置已保存"));
    }

    static std::string id(ruvia::Context& c) {
        return std::string(c.req().validated<LinkIdParams>().get<"id">().view());
    }

    static std::optional<std::string> text(const std::optional<ruvia::String>& value) {
        return value ? std::optional<std::string>(std::string(value->view())) : std::nullopt;
    }

    ruvia::Task<ruvia::HttpResponse> list(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_return c.json(co_await queryList(c, request));
    }

    ruvia::Task<void> linkEvents(ruvia::Context& c) {

        service::middleware::RequestContext access(c, service::middleware::requireAuth(c).userId);
        std::vector<service::live::SnapshotChannel> channels;
        channels.push_back({ "links", "link", [this, &c](service::middleware::RequestContext& request) -> ruvia::Task<std::string> {
                                co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:link:query");
                                co_return service::live::json(co_await queryList(c, request));
                            } });
        if (const auto& value = c.req().validated<LinkEventsQuery>().get<"debugLinkId">()) {
            channels.push_back({ "packets", "packet-debug", [this, &c, linkId = std::string(value->view())](service::middleware::RequestContext& request) -> ruvia::Task<std::string> {
                                    co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:link:edit");
                                    co_return service::live::json(co_await queryDebugPackets(c, request, linkId));
                                } });
        }
        co_await service::live::serveSnapshotChannels(c, access.userId, std::move(channels), [&c] {
            (void)service::middleware::requireAuth(c);
        });
    }

    ruvia::Task<LinkPageResponse> queryList(ruvia::Context& c, service::middleware::RequestContext& request) {
        const auto& query = c.req().validated<LinkListQuery>();
        co_return service::common::ok<LinkPageResponse>(request, co_await linkService().list(request, static_cast<std::int64_t>(*query.get<"page">()), static_cast<std::int64_t>(*query.get<"pageSize">()), text(query.get<"keyword">()), text(query.get<"mode">()), text(query.get<"protocol">()), text(query.get<"status">())));
    }

    ruvia::Task<ruvia::HttpResponse> options(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_return c.json(service::common::ok<LinkOptionsResponse>(request, co_await linkService().options(request)));
    }

    ruvia::Task<ruvia::HttpResponse> enums(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_return c.json(service::common::ok<LinkEnumsResponse>(request, linkService().enums(request)));
    }

    ruvia::Task<ruvia::HttpResponse> publicIp(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        PublicIpDto result(ruvia::ModelOptions{ .resource = request.arena() });
        result.set<"ip">(co_await linkService().publicIp(request));
        co_return c.json(service::common::ok<PublicIpResponse>(request, std::move(result)));
    }

    ruvia::Task<ruvia::HttpResponse> detail(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_return c.json(service::common::ok<LinkDetailResponse>(request, co_await linkService().detail(request, id(c))));
    }

    ruvia::Task<ruvia::HttpResponse> create(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await linkService().create(request, c.req().validated<SaveLinkBody>());
        co_return c.json(service::common::operation(c, "创建成功"));
    }

    ruvia::Task<ruvia::HttpResponse> update(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await linkService().update(request, id(c), c.req().validated<SaveLinkBody>());
        co_return c.json(service::common::operation(c, "更新成功"));
    }

    ruvia::Task<ruvia::HttpResponse> remove(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await linkService().remove(request, id(c));
        co_return c.json(service::common::operation(c, "删除成功"));
    }
};

} // namespace service::link
