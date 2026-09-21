#pragma once
#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/middleware/auth.h"
#include "service/middleware/live.h"
#include "service/middleware/permission.h"
#include "service/middleware/validation.h"
#include "service/modules/vpn/vpn.types.h"
#include "service/modules/vpn/vpn.service.h"
#include "service/utils/json.h"

namespace service::vpn {
class VpnController final : public ruvia::Controller<VpnController> {
  public:
    RUVIA_CONTROLLER_GROUP("/v1/vpn", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/networks", listNetworks, service::middleware::PermissionMiddleware<"iot:vpn:query">, ruvia::QueryModel<VpnListQuery>);
    RUVIA_GET("/networks/:id", network, service::middleware::PermissionMiddleware<"iot:vpn:query">, ruvia::PathModel<VpnIdParams>);
    RUVIA_POST("/networks", createNetwork, service::middleware::PermissionMiddleware<"iot:vpn:add">, service::middleware::ValidationErrorCodeMiddleware<21001>, ruvia::JsonBody<VpnNetworkBody>);
    RUVIA_PATCH("/networks/:id", updateNetwork, service::middleware::PermissionMiddleware<"iot:vpn:edit">, ruvia::PathModel<VpnIdParams>, service::middleware::ValidationErrorCodeMiddleware<21001>, ruvia::JsonBody<VpnNetworkBody>);
    RUVIA_DELETE("/networks/:id", removeNetwork, service::middleware::PermissionMiddleware<"iot:vpn:delete">, ruvia::PathModel<VpnIdParams>);
    RUVIA_GET("/routes", routes, service::middleware::PermissionMiddleware<"iot:vpn:query">, ruvia::QueryModel<VpnFilterQuery>);
    RUVIA_POST("/routes", createRoute, service::middleware::PermissionMiddleware<"iot:vpn:add">, service::middleware::ValidationErrorCodeMiddleware<21001>, ruvia::JsonBody<VpnRouteBody>);
    RUVIA_PATCH("/routes/:id", updateRoute, service::middleware::PermissionMiddleware<"iot:vpn:edit">, ruvia::PathModel<VpnIdParams>, service::middleware::ValidationErrorCodeMiddleware<21001>, ruvia::JsonBody<VpnRoutePatchBody>);
    RUVIA_DELETE("/routes/:id", removeRoute, service::middleware::PermissionMiddleware<"iot:vpn:delete">, ruvia::PathModel<VpnIdParams>);
    RUVIA_GET("/peers", peers, service::middleware::PermissionMiddleware<"iot:vpn:query">, ruvia::QueryModel<VpnFilterQuery>);
    RUVIA_POST("/peers", createPeer, service::middleware::PermissionMiddleware<"iot:vpn:add">, service::middleware::ValidationErrorCodeMiddleware<21001>, ruvia::JsonBody<VpnPeerBody>);
    RUVIA_POST("/peers/:id/revoke", revokePeer, service::middleware::PermissionMiddleware<"iot:vpn:revoke">, ruvia::PathModel<VpnIdParams>);
    RUVIA_POST("/peers/:id/sync", syncPeer, service::middleware::PermissionMiddleware<"iot:vpn:edit">, ruvia::PathModel<VpnIdParams>);
    RUVIA_POST("/peers/:id/rotate-key", rotatePeerKey, service::middleware::PermissionMiddleware<"iot:vpn:edit">, ruvia::PathModel<VpnIdParams>, service::middleware::ValidationErrorCodeMiddleware<21001>, ruvia::JsonBody<VpnPeerKeyBody>);
    RUVIA_GET_SSE("/desktop/devices/events", desktopDevicesEvents, service::middleware::PermissionMiddleware<"iot:vpn:query">, service::middleware::PermissionMiddleware<"iot:vpn:enroll">, service::middleware::PermissionMiddleware<"iot:edge:query">);
    RUVIA_GET("/desktop/devices", desktopDevices, service::middleware::PermissionMiddleware<"iot:vpn:query">, service::middleware::PermissionMiddleware<"iot:vpn:enroll">, service::middleware::PermissionMiddleware<"iot:edge:query">);
    RUVIA_POST("/desktop/peers", desktopCreatePeer, service::middleware::PermissionMiddleware<"iot:vpn:query">, service::middleware::PermissionMiddleware<"iot:vpn:enroll">, service::middleware::PermissionMiddleware<"iot:edge:query">, service::middleware::ValidationErrorCodeMiddleware<21001>, ruvia::JsonBody<VpnDesktopPeerBody>);
    RUVIA_PATCH("/desktop/peers/:id", desktopUpdatePeer, service::middleware::PermissionMiddleware<"iot:vpn:query">, service::middleware::PermissionMiddleware<"iot:vpn:enroll">, service::middleware::PermissionMiddleware<"iot:edge:query">, ruvia::PathModel<VpnIdParams>, service::middleware::ValidationErrorCodeMiddleware<21001>, ruvia::JsonBody<VpnDesktopSelectionBody>);
    RUVIA_GET_SSE("/desktop/peers/:id/config/events", desktopPeerConfigEvents, service::middleware::PermissionMiddleware<"iot:vpn:query">, service::middleware::PermissionMiddleware<"iot:vpn:enroll">, service::middleware::PermissionMiddleware<"iot:edge:query">, ruvia::PathModel<VpnIdParams>);
    RUVIA_GET("/desktop/peers/:id/config", desktopPeerConfig, service::middleware::PermissionMiddleware<"iot:vpn:query">, service::middleware::PermissionMiddleware<"iot:vpn:enroll">, service::middleware::PermissionMiddleware<"iot:edge:query">, ruvia::PathModel<VpnIdParams>);
    RUVIA_DELETE("/desktop/peers/:id", desktopDeletePeer, service::middleware::PermissionMiddleware<"iot:vpn:enroll">, ruvia::PathModel<VpnIdParams>);
    RUVIA_POST("/enrollments", createEnrollment, service::middleware::PermissionMiddleware<"iot:vpn:enroll">, service::middleware::PermissionMiddleware<"iot:edge:query">, service::middleware::ValidationErrorCodeMiddleware<21001>, ruvia::JsonBody<VpnEnrollmentBody>);
    RUVIA_GET("/sessions", sessions, service::middleware::PermissionMiddleware<"iot:vpn:query">);
    RUVIA_GET("/diagnostics", diagnostics, service::middleware::PermissionMiddleware<"iot:vpn:diagnose">);
    RUVIA_ROUTES_END

  private:
    ruvia::Task<ruvia::HttpResponse> listNetworks(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto& body = c.req().validated<VpnListQuery>();
        const auto keyword = body.get<"keyword">() ? std::optional<std::string>(body.get<"keyword">()->view()) : std::nullopt;
        const auto status = body.get<"status">() ? std::optional<std::string>(body.get<"status">()->view()) : std::nullopt;
        const auto payload = service::live::data(c, co_await vpnService().networks(request, *body.get<"page">(), *body.get<"pageSize">(), keyword, status));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }
    ruvia::Task<ruvia::HttpResponse> network(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = service::live::data(c, co_await vpnService().network(request, c.req().validated<VpnIdParams>().get<"id">().view()));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }
    ruvia::Task<ruvia::HttpResponse> createNetwork(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto& body = c.req().validated<VpnNetworkBody>();
        const auto id = co_await vpnService().createNetwork(request, body);
        const auto payload = service::live::data(c, "{\"id\":" + service::utils::jsonQuoted(id) + "}");
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }
    ruvia::Task<ruvia::HttpResponse> updateNetwork(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto& body = c.req().validated<VpnNetworkBody>();
        co_await vpnService().updateNetwork(request, c.req().validated<VpnIdParams>().get<"id">().view(), body);
        const auto payload = service::live::data(c, "null");
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }
    ruvia::Task<ruvia::HttpResponse> removeNetwork(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await vpnService().removeNetwork(request, c.req().validated<VpnIdParams>().get<"id">().view());
        const auto payload = service::live::data(c, "null");
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }
    ruvia::Task<ruvia::HttpResponse> routes(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto& query = c.req().validated<VpnFilterQuery>();
        const auto networkId = query.get<"networkId">() ? std::optional<std::string>(query.get<"networkId">()->view()) : std::nullopt;
        const auto edgeNodeId = query.get<"edgeNodeId">() ? std::optional<std::string>(query.get<"edgeNodeId">()->view()) : std::nullopt;
        const auto payload = service::live::data(c, co_await vpnService().routes(request, networkId, edgeNodeId));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }
    ruvia::Task<ruvia::HttpResponse> createRoute(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto& body = c.req().validated<VpnRouteBody>();
        const auto id = co_await vpnService().createRoute(request, body);
        const auto payload = service::live::data(c, "{\"id\":" + service::utils::jsonQuoted(id) + "}");
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }
    ruvia::Task<ruvia::HttpResponse> updateRoute(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto& body = c.req().validated<VpnRoutePatchBody>();
        co_await vpnService().updateRoute(request, c.req().validated<VpnIdParams>().get<"id">().view(), body);
        const auto payload = service::live::data(c, "null");
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }
    ruvia::Task<ruvia::HttpResponse> removeRoute(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await vpnService().removeRoute(request, c.req().validated<VpnIdParams>().get<"id">().view());
        const auto payload = service::live::data(c, "null");
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }
    ruvia::Task<ruvia::HttpResponse> peers(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto& query = c.req().validated<VpnFilterQuery>();
        const auto networkId = query.get<"networkId">() ? std::optional<std::string>(query.get<"networkId">()->view()) : std::nullopt;
        const auto edgeNodeId = query.get<"edgeNodeId">() ? std::optional<std::string>(query.get<"edgeNodeId">()->view()) : std::nullopt;
        const auto payload = service::live::data(c, co_await vpnService().peers(request, networkId, edgeNodeId));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }
    ruvia::Task<ruvia::HttpResponse> createPeer(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto& body = c.req().validated<VpnPeerBody>();
        const auto id = co_await vpnService().createPeer(request, body);
        const auto payload = service::live::data(c, "{\"id\":" + service::utils::jsonQuoted(id) + "}");
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }
    ruvia::Task<ruvia::HttpResponse> revokePeer(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await vpnService().revokePeer(request, c.req().validated<VpnIdParams>().get<"id">().view());
        const auto payload = service::live::data(c, "null");
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }
    ruvia::Task<ruvia::HttpResponse> syncPeer(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await vpnService().syncPeer(request, c.req().validated<VpnIdParams>().get<"id">().view());
        const auto payload = service::live::data(c, "null");
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }
    ruvia::Task<ruvia::HttpResponse> rotatePeerKey(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto& body = c.req().validated<VpnPeerKeyBody>();
        co_await vpnService().rotatePeerKey(request, c.req().validated<VpnIdParams>().get<"id">().view(), body);
        const auto payload = service::live::data(c, "null");
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }
    ruvia::Task<ruvia::HttpResponse> desktopDevices(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = service::live::data(c, co_await vpnService().desktopDevices(request));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }
    ruvia::Task<void> desktopDevicesEvents(ruvia::Context& c) {
        co_await service::live::serveSnapshots(c, "vpn", service::middleware::requireAuth(c).userId, [&c](service::middleware::RequestContext& request) -> ruvia::Task<std::string> {
            co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:vpn:query");
            co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:vpn:enroll");
            co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:edge:query");
            co_return service::live::data(c, co_await vpnService().desktopDevices(request));
        }, [&c] { (void)service::middleware::requireAuth(c); });
    }
    ruvia::Task<ruvia::HttpResponse> desktopCreatePeer(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto& body = c.req().validated<VpnDesktopPeerBody>();
        const auto payload = service::live::data(c, co_await vpnService().desktopCreatePeer(request, body));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }
    ruvia::Task<ruvia::HttpResponse> desktopUpdatePeer(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto& body = c.req().validated<VpnDesktopSelectionBody>();
        const auto payload = service::live::data(c, co_await vpnService().desktopUpdatePeer(request, c.req().validated<VpnIdParams>().get<"id">().view(), body));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }
    ruvia::Task<ruvia::HttpResponse> desktopPeerConfig(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = service::live::data(c, co_await vpnService().desktopPeerConfig(request, c.req().validated<VpnIdParams>().get<"id">().view()));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }
    ruvia::Task<void> desktopPeerConfigEvents(ruvia::Context& c) {
        co_await service::live::serveSnapshots(c, "vpn", service::middleware::requireAuth(c).userId, [&c](service::middleware::RequestContext& request) -> ruvia::Task<std::string> {
            co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:vpn:query");
            co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:vpn:enroll");
            co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:edge:query");
            co_return service::live::data(c, co_await vpnService().desktopPeerConfig(request, c.req().validated<VpnIdParams>().get<"id">().view()));
        }, [&c] { (void)service::middleware::requireAuth(c); });
    }
    ruvia::Task<ruvia::HttpResponse> desktopDeletePeer(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await vpnService().desktopDeletePeer(request, c.req().validated<VpnIdParams>().get<"id">().view());
        const auto payload = service::live::data(c, "null");
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }
    ruvia::Task<ruvia::HttpResponse> createEnrollment(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto& body = c.req().validated<VpnEnrollmentBody>();
        const auto payload = service::live::data(c, co_await vpnService().createEnrollment(request, body));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }
    ruvia::Task<ruvia::HttpResponse> sessions(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = service::live::data(c, co_await vpnService().sessions(request));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }
    ruvia::Task<ruvia::HttpResponse> diagnostics(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = service::live::data(c, co_await vpnService().diagnostics(request));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }
};

class VpnClientController final : public ruvia::Controller<VpnClientController> {
  public:
    RUVIA_CONTROLLER_GROUP("/v1/vpn/client")
    RUVIA_ROUTES_BEGIN
    RUVIA_POST("/enroll", enrollClient, service::middleware::ValidationErrorCodeMiddleware<21001>, ruvia::JsonBody<VpnClientEnrollmentBody>);
    RUVIA_ROUTES_END
  private:
    ruvia::Task<ruvia::HttpResponse> enrollClient(ruvia::Context& c) {
        service::middleware::RequestContext request(c, std::string{});
        const auto& body = c.req().validated<VpnClientEnrollmentBody>();
        const auto payload = service::live::data(c, co_await vpnService().enrollClient(request, body));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }
};
} // namespace service::vpn
