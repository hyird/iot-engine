#pragma once
#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/middleware/auth.h"
#include "service/middleware/live.h"
#include "service/middleware/permission.h"
#include "service/modules/vpn/vpn.schema.h"
#include "service/modules/vpn/vpn.service.h"
#include "service/utils/json.h"

namespace service::vpn {
class VpnController final : public ruvia::Controller<VpnController> {
  public:
    RUVIA_CONTROLLER_GROUP("/v1/vpn", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/networks", listNetworks, VpnListValidator);
    RUVIA_GET("/networks/:id", network, VpnIdValidator);
    RUVIA_POST("/networks", createNetwork);
    RUVIA_PATCH("/networks/:id", updateNetwork, VpnIdValidator);
    RUVIA_DELETE("/networks/:id", removeNetwork, VpnIdValidator);
    RUVIA_GET("/routes", routes, VpnFilterValidator);
    RUVIA_POST("/routes", createRoute);
    RUVIA_PATCH("/routes/:id", updateRoute, VpnIdValidator);
    RUVIA_DELETE("/routes/:id", removeRoute, VpnIdValidator);
    RUVIA_GET("/peers", peers, VpnFilterValidator);
    RUVIA_POST("/peers", createPeer);
    RUVIA_POST("/peers/:id/revoke", revokePeer, VpnIdValidator);
    RUVIA_POST("/peers/:id/sync", syncPeer, VpnIdValidator);
    RUVIA_POST("/peers/:id/rotate-key", rotatePeerKey, VpnIdValidator);
    RUVIA_GET_SSE("/desktop/devices/events", desktopDevicesEvents);
    RUVIA_GET("/desktop/devices", desktopDevices);
    RUVIA_POST("/desktop/peers", desktopCreatePeer);
    RUVIA_PATCH("/desktop/peers/:id", desktopUpdatePeer, VpnIdValidator);
    RUVIA_GET_SSE("/desktop/peers/:id/config/events", desktopPeerConfigEvents, VpnIdValidator);
    RUVIA_GET("/desktop/peers/:id/config", desktopPeerConfig, VpnIdValidator);
    RUVIA_DELETE("/desktop/peers/:id", desktopDeletePeer, VpnIdValidator);
    RUVIA_POST("/enrollments", createEnrollment);
    RUVIA_GET("/sessions", sessions);
    RUVIA_GET("/diagnostics", diagnostics);
    RUVIA_ROUTES_END
  private:
    ruvia::Task<ruvia::HttpResponse> listNetworks(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await listNetworksSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> listNetworksSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:vpn:query");
        const auto& body = c.req().validated<VpnListQuery>();
        const auto keyword = body.get<"keyword">() ? std::optional<std::string>(body.get<"keyword">()->view()) : std::nullopt;
        const auto status = body.get<"status">() ? std::optional<std::string>(body.get<"status">()->view()) : std::nullopt;
        co_return service::live::data(c, co_await vpnService().networks(request, *body.get<"page">(), *body.get<"pageSize">(), keyword, status));
    }

    ruvia::Task<ruvia::HttpResponse> network(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await networkSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> networkSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:vpn:query");
        const auto& body = c.req().validated<VpnIdParams>();
        const auto id = body.get<"id">()->view();
        co_return service::live::data(c, co_await vpnService().network(request, id));
    }

    ruvia::Task<ruvia::HttpResponse> createNetwork(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:vpn:add");
        const auto body = VpnPayloadValidator::parseNetworkInput(co_await c.req().jsonValue());
        const auto id = co_await vpnService().createNetwork(request, body);
        const auto payload = service::live::data(c, "{\"id\":" + service::utils::jsonQuoted(id) + "}");
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<ruvia::HttpResponse> updateNetwork(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:vpn:edit");
        const auto body = VpnPayloadValidator::parseNetworkInput(co_await c.req().jsonValue());
        const auto id = c.req().validated<VpnIdParams>().get<"id">()->view();
        co_await vpnService().updateNetwork(request, id, body);
        const auto payload = service::live::data(c, "null");
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<ruvia::HttpResponse> removeNetwork(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:vpn:delete");
        const auto& body = c.req().validated<VpnIdParams>();
        const auto id = body.get<"id">()->view();
        co_await vpnService().removeNetwork(request, id);
        const auto payload = service::live::data(c, "null");
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<ruvia::HttpResponse> routes(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await routesSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> routesSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:vpn:query");
        const auto& query = c.req().validated<VpnFilterQuery>();
        const auto networkId = query.get<"networkId">() ? std::optional<std::string>(query.get<"networkId">()->view()) : std::nullopt;
        const auto edgeNodeId = query.get<"edgeNodeId">() ? std::optional<std::string>(query.get<"edgeNodeId">()->view()) : std::nullopt;
        co_return service::live::data(c, co_await vpnService().routes(request, networkId, edgeNodeId));
    }

    ruvia::Task<ruvia::HttpResponse> createRoute(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:vpn:add");
        const auto body = VpnPayloadValidator::parseRouteInput(co_await c.req().jsonValue());
        const auto id = co_await vpnService().createRoute(request, body);
        const auto payload = service::live::data(c, "{\"id\":" + service::utils::jsonQuoted(id) + "}");
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<ruvia::HttpResponse> updateRoute(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:vpn:edit");
        const auto body = VpnPayloadValidator::parseRoutePatch(co_await c.req().jsonValue());
        const auto id = c.req().validated<VpnIdParams>().get<"id">()->view();
        co_await vpnService().updateRoute(request, id, body);
        const auto payload = service::live::data(c, "null");
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<ruvia::HttpResponse> removeRoute(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:vpn:delete");
        const auto& body = c.req().validated<VpnIdParams>();
        const auto id = body.get<"id">()->view();
        co_await vpnService().removeRoute(request, id);
        const auto payload = service::live::data(c, "null");
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<ruvia::HttpResponse> peers(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await peersSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> peersSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:vpn:query");
        const auto& query = c.req().validated<VpnFilterQuery>();
        const auto networkId = query.get<"networkId">() ? std::optional<std::string>(query.get<"networkId">()->view()) : std::nullopt;
        const auto edgeNodeId = query.get<"edgeNodeId">() ? std::optional<std::string>(query.get<"edgeNodeId">()->view()) : std::nullopt;
        co_return service::live::data(c, co_await vpnService().peers(request, networkId, edgeNodeId));
    }

    ruvia::Task<ruvia::HttpResponse> createPeer(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:vpn:add");
        const auto body = VpnPayloadValidator::parsePeerInput(co_await c.req().jsonValue());
        const auto id = co_await vpnService().createPeer(request, body);
        const auto payload = service::live::data(c, "{\"id\":" + service::utils::jsonQuoted(id) + "}");
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<ruvia::HttpResponse> revokePeer(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:vpn:revoke");
        const auto& body = c.req().validated<VpnIdParams>();
        const auto id = body.get<"id">()->view();
        co_await vpnService().revokePeer(request, id);
        const auto payload = service::live::data(c, "null");
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<ruvia::HttpResponse> syncPeer(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:vpn:edit");
        const auto& body = c.req().validated<VpnIdParams>();
        const auto id = body.get<"id">()->view();
        co_await vpnService().syncPeer(request, id);
        const auto payload = service::live::data(c, "null");
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<ruvia::HttpResponse> rotatePeerKey(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:vpn:edit");
        const auto body = VpnPayloadValidator::parsePeerKeyInput(co_await c.req().jsonValue());
        const auto id = c.req().validated<VpnIdParams>().get<"id">()->view();
        co_await vpnService().rotatePeerKey(request, id, body);
        const auto payload = service::live::data(c, "null");
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<ruvia::HttpResponse> desktopDevices(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await desktopDevicesSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> desktopDevicesSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:vpn:query");
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:vpn:enroll");
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:edge:query");
        co_return service::live::data(c, co_await vpnService().desktopDevices(request));
    }

    ruvia::Task<void> desktopDevicesEvents(ruvia::Context& c) {
        co_await service::live::serveSnapshots(c, "vpn", service::middleware::requireAuth(c).userId, [this, &c](service::middleware::RequestContext& request) {
            return desktopDevicesSnapshot(c, request);
        },
                                               [&c] {
                                                   (void)service::middleware::requireAuth(c);
                                               });
    }

    ruvia::Task<ruvia::HttpResponse> desktopCreatePeer(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:vpn:query");
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:vpn:enroll");
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:edge:query");
        const auto body = VpnPayloadValidator::parseDesktopPeerInput(co_await c.req().jsonValue());
        const auto payload = service::live::data(c, co_await vpnService().desktopCreatePeer(request, body));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<ruvia::HttpResponse> desktopUpdatePeer(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:vpn:query");
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:vpn:enroll");
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:edge:query");
        const auto body = VpnPayloadValidator::parseDesktopSelectionInput(co_await c.req().jsonValue());
        const auto id = c.req().validated<VpnIdParams>().get<"id">()->view();
        const auto payload = service::live::data(c, co_await vpnService().desktopUpdatePeer(request, id, body));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<ruvia::HttpResponse> desktopPeerConfig(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await desktopPeerConfigSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> desktopPeerConfigSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:vpn:query");
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:vpn:enroll");
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:edge:query");
        const auto& body = c.req().validated<VpnIdParams>();
        const auto id = body.get<"id">()->view();
        co_return service::live::data(c, co_await vpnService().desktopPeerConfig(request, id));
    }

    ruvia::Task<void> desktopPeerConfigEvents(ruvia::Context& c) {
        co_await service::live::serveSnapshots(c, "vpn", service::middleware::requireAuth(c).userId, [this, &c](service::middleware::RequestContext& request) {
            return desktopPeerConfigSnapshot(c, request);
        },
                                               [&c] {
                                                   (void)service::middleware::requireAuth(c);
                                               });
    }

    ruvia::Task<ruvia::HttpResponse> desktopDeletePeer(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:vpn:enroll");
        const auto& body = c.req().validated<VpnIdParams>();
        const auto id = body.get<"id">()->view();
        co_await vpnService().desktopDeletePeer(request, id);
        const auto payload = service::live::data(c, "null");
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<ruvia::HttpResponse> createEnrollment(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:vpn:enroll");
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:edge:query");
        const auto body = VpnPayloadValidator::parseEnrollmentInput(co_await c.req().jsonValue());
        const auto payload = service::live::data(c, co_await vpnService().createEnrollment(request, body));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<ruvia::HttpResponse> sessions(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await sessionsSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> sessionsSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:vpn:query");
        co_return service::live::data(c, co_await vpnService().sessions(request));
    }

    ruvia::Task<ruvia::HttpResponse> diagnostics(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await diagnosticsSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> diagnosticsSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:vpn:diagnose");
        co_return service::live::data(c, co_await vpnService().diagnostics(request));
    }

    ruvia::Task<void> diagnosticsEvents(ruvia::Context& c) {
        co_await service::live::serveSnapshots(c, "vpn", service::middleware::requireAuth(c).userId, [this, &c](service::middleware::RequestContext& request) {
            return diagnosticsSnapshot(c, request);
        },
                                               [&c] {
                                                   (void)service::middleware::requireAuth(c);
                                               });
    }
};

class VpnClientController final : public ruvia::Controller<VpnClientController> {
  public:
    RUVIA_CONTROLLER_GROUP("/v1/vpn/client")
    RUVIA_ROUTES_BEGIN
    RUVIA_POST("/enroll", enrollClient);
    RUVIA_ROUTES_END
  private:
    ruvia::Task<ruvia::HttpResponse> enrollClient(ruvia::Context& c) {
        service::middleware::RequestContext request(c, std::string{});
        const auto body = VpnPayloadValidator::parseClientEnrollmentInput(co_await c.req().jsonValue());
        const auto payload = service::live::data(c, co_await vpnService().enrollClient(request, body));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }
};
} // namespace service::vpn
