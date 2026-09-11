#pragma once

#include <memory_resource>
#include <optional>
#include <string>

#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/middleware/live.h"
#include "service/modules/vpn/vpn.schema.h"
#include "service/modules/vpn/vpn.service.h"
#include "service/middleware/auth.h"
#include "service/middleware/permission.h"
#include "service/utils/json.h"

namespace service::vpn {

inline ruvia::HttpResponse vpnJson(ruvia::Context& c, std::string_view data,
                                   std::string_view message = "ok") {
    std::pmr::string body(c.allocator<char>());
    body.append("{\"code\":0,\"message\":");
    body.append(service::utils::jsonQuoted(message));
    body.append(",\"data\":");
    body.append(data);
    body.push_back('}');
    auto response = c.body(std::move(body));
    response.header("Content-Type", "application/json; charset=UTF-8");
    return response;
}

inline std::string vpnId(ruvia::Context& c) {
    return std::string(c.req().validated<VpnIdParams>().get<"id">()->view());
}

class VpnController final : public ruvia::Controller<VpnController> {
  public:
    RUVIA_CONTROLLER_GROUP("/v1/vpn", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET_SSE("/networks", listNetworks, VpnListValidator);
    RUVIA_GET_SSE("/networks/:id", network, VpnIdValidator);
    RUVIA_POST("/networks", createNetwork);
    RUVIA_PATCH("/networks/:id", updateNetwork, VpnIdValidator);
    RUVIA_DELETE("/networks/:id", removeNetwork, VpnIdValidator);
    RUVIA_GET_SSE("/routes", routes);
    RUVIA_POST("/routes", createRoute);
    RUVIA_PATCH("/routes/:id", updateRoute, VpnIdValidator);
    RUVIA_DELETE("/routes/:id", removeRoute, VpnIdValidator);
    RUVIA_GET_SSE("/peers", peers);
    RUVIA_POST("/peers", createPeer);
    RUVIA_POST("/peers/:id/revoke", revokePeer, VpnIdValidator);
    RUVIA_POST("/peers/:id/sync", syncPeer, VpnIdValidator);
    RUVIA_POST("/peers/:id/rotate-key", rotatePeerKey, VpnIdValidator);
    RUVIA_GET_SSE("/desktop/devices", desktopDevices);
    RUVIA_POST("/desktop/peers", desktopCreatePeer);
    RUVIA_PATCH("/desktop/peers/:id", desktopUpdatePeer, VpnIdValidator);
    RUVIA_GET_SSE("/desktop/peers/:id/config", desktopPeerConfig, VpnIdValidator);
    RUVIA_DELETE("/desktop/peers/:id", desktopDeletePeer, VpnIdValidator);
    RUVIA_POST("/enrollments", createEnrollment);
    RUVIA_GET_SSE("/sessions", sessions);
    RUVIA_GET_SSE("/diagnostics", diagnostics);
    RUVIA_ROUTES_END

  private:
    ruvia::Task<void> listNetworks(ruvia::Context& c) {
        co_await service::live::serve(c, "vpn", [this, &c]() { return listNetworksSnapshot(c); });
    }

    ruvia::Task<std::string> listNetworksSnapshot(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:vpn:query");
        const auto& query = c.req().validated<VpnListQuery>();
        const auto keyword = query.get<"keyword">()
                                 ? std::optional<std::string>(std::string(query.get<"keyword">()->view()))
                                 : std::nullopt;
        const auto status = query.get<"status">()
                                ? std::optional<std::string>(std::string(query.get<"status">()->view()))
                                : std::nullopt;
        co_return service::live::data(c, co_await vpnService().networks(
                                 c, *query.get<"page">(), *query.get<"pageSize">(), keyword, status));
    }

    ruvia::Task<void> network(ruvia::Context& c) {
        co_await service::live::serve(c, "vpn", [this, &c]() { return networkSnapshot(c); });
    }

    ruvia::Task<std::string> networkSnapshot(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:vpn:query");
        co_return service::live::data(c, co_await vpnService().network(c, vpnId(c)));
    }

    ruvia::Task<ruvia::HttpResponse> createNetwork(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:vpn:add");
        co_return vpnJson(c, "{\"id\":" +
                              service::utils::jsonQuoted(co_await vpnService().createNetwork(
                                  c, co_await c.req().jsonValue())) + "}", "创建成功");
    }

    ruvia::Task<ruvia::HttpResponse> updateNetwork(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:vpn:edit");
        co_await vpnService().updateNetwork(c, vpnId(c), co_await c.req().jsonValue());
        co_return c.json(service::common::operation(c, "更新成功"));
    }

    ruvia::Task<ruvia::HttpResponse> removeNetwork(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:vpn:delete");
        co_await vpnService().removeNetwork(c, vpnId(c));
        co_return c.json(service::common::operation(c, "删除成功"));
    }

    ruvia::Task<void> routes(ruvia::Context& c) {
        co_await service::live::serve(c, "vpn", [this, &c]() { return routesSnapshot(c); });
    }

    ruvia::Task<std::string> routesSnapshot(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:vpn:query");
        const auto networkId = c.req().query("networkId");
        const auto edgeNodeId = c.req().query("edgeNodeId");
        co_return service::live::data(c, co_await vpnService().routes(
                                 c, networkId ? std::optional<std::string>(std::string(*networkId))
                                              : std::nullopt,
                                 edgeNodeId ? std::optional<std::string>(std::string(*edgeNodeId))
                                            : std::nullopt));
    }

    ruvia::Task<ruvia::HttpResponse> createRoute(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:vpn:add");
        co_return vpnJson(c, "{\"id\":" +
                              service::utils::jsonQuoted(co_await vpnService().createRoute(
                                  c, co_await c.req().jsonValue())) + "}", "创建成功");
    }

    ruvia::Task<ruvia::HttpResponse> updateRoute(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:vpn:edit");
        co_await vpnService().updateRoute(c, vpnId(c), co_await c.req().jsonValue());
        co_return c.json(service::common::operation(c, "更新成功"));
    }

    ruvia::Task<ruvia::HttpResponse> removeRoute(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:vpn:delete");
        co_await vpnService().removeRoute(c, vpnId(c));
        co_return c.json(service::common::operation(c, "删除成功"));
    }

    ruvia::Task<void> peers(ruvia::Context& c) {
        co_await service::live::serve(c, "vpn", [this, &c]() { return peersSnapshot(c); });
    }

    ruvia::Task<std::string> peersSnapshot(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:vpn:query");
        const auto networkId = c.req().query("networkId");
        const auto edgeNodeId = c.req().query("edgeNodeId");
        co_return service::live::data(c, co_await vpnService().peers(
                                 c, networkId ? std::optional<std::string>(std::string(*networkId))
                                              : std::nullopt,
                                 edgeNodeId ? std::optional<std::string>(std::string(*edgeNodeId))
                                            : std::nullopt));
    }

    ruvia::Task<ruvia::HttpResponse> createPeer(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:vpn:add");
        co_return vpnJson(c, "{\"id\":" +
                              service::utils::jsonQuoted(co_await vpnService().createPeer(
                                  c, co_await c.req().jsonValue())) + "}", "创建成功");
    }

    ruvia::Task<ruvia::HttpResponse> revokePeer(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:vpn:revoke");
        co_await vpnService().revokePeer(c, vpnId(c));
        co_return c.json(service::common::operation(c, "Peer 已撤销"));
    }

    ruvia::Task<ruvia::HttpResponse> syncPeer(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:vpn:edit");
        co_await vpnService().syncPeer(c, vpnId(c));
        co_return c.json(service::common::operation(c, "VPN 配置已重新下发"));
    }

    ruvia::Task<ruvia::HttpResponse> rotatePeerKey(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:vpn:edit");
        co_await vpnService().rotatePeerKey(c, vpnId(c), co_await c.req().jsonValue());
        co_return c.json(service::common::operation(c, "Peer 公钥已轮换"));
    }

    ruvia::Task<ruvia::HttpResponse> createEnrollment(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:vpn:enroll");
        co_await service::middleware::requirePermission(c, "iot:edge:query");
        co_return vpnJson(c, co_await vpnService().createEnrollment(
                                 c, co_await c.req().jsonValue()), "Enrollment 已创建");
    }

    ruvia::Task<void> desktopDevices(ruvia::Context& c) {
        co_await service::live::serve(c, "vpn", [this, &c]() -> ruvia::Task<std::string> {
            co_await requireDesktopPermissions(c);
            co_return service::live::data(c, co_await vpnService().desktopDevices(c));
        });
    }

    ruvia::Task<ruvia::HttpResponse> desktopCreatePeer(ruvia::Context& c) {
        co_await requireDesktopPermissions(c);
        co_return vpnJson(c, co_await vpnService().desktopCreatePeer(c, co_await c.req().jsonValue()));
    }
    ruvia::Task<ruvia::HttpResponse> desktopUpdatePeer(ruvia::Context& c) {
        co_await requireDesktopPermissions(c);
        co_return vpnJson(c, co_await vpnService().desktopUpdatePeer(c, vpnId(c), co_await c.req().jsonValue()));
    }
    ruvia::Task<void> desktopPeerConfig(ruvia::Context& c) {
        co_await service::live::serve(c, "vpn", [this, &c]() -> ruvia::Task<std::string> {
            co_await requireDesktopPermissions(c);
            co_return service::live::data(c, co_await vpnService().desktopPeerConfig(c, vpnId(c)));
        });
    }
    ruvia::Task<ruvia::HttpResponse> desktopDeletePeer(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:vpn:enroll");
        co_await vpnService().desktopDeletePeer(c, vpnId(c));
        co_return c.json(service::common::operation(c, "VPN 配置已撤销"));
    }

    static ruvia::Task<void> requireDesktopPermissions(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:vpn:query");
        co_await service::middleware::requirePermission(c, "iot:vpn:enroll");
        co_await service::middleware::requirePermission(c, "iot:edge:query");
    }

    ruvia::Task<void> sessions(ruvia::Context& c) {
        co_await service::live::serve(c, "vpn", [this, &c]() { return sessionsSnapshot(c); });
    }

    ruvia::Task<std::string> sessionsSnapshot(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:vpn:query");
        co_return service::live::data(c, co_await vpnService().sessions(c));
    }

    ruvia::Task<void> diagnostics(ruvia::Context& c) {
        co_await service::live::serve(c, "vpn", [this, &c]() { return diagnosticsSnapshot(c); });
    }

    ruvia::Task<std::string> diagnosticsSnapshot(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:vpn:diagnose");
        co_return service::live::data(c, co_await vpnService().diagnostics(c));
    }
};

class VpnClientController final : public ruvia::Controller<VpnClientController> {
  public:
    RUVIA_CONTROLLER_GROUP("/v1/vpn/client")
    RUVIA_ROUTES_BEGIN
    RUVIA_POST("/enroll", enroll);
    RUVIA_ROUTES_END

  private:
    ruvia::Task<ruvia::HttpResponse> enroll(ruvia::Context& c) {
        co_return vpnJson(c, co_await vpnService().enrollClient(
                                 c, co_await c.req().jsonValue()), "Enrollment 成功");
    }
};

} // namespace service::vpn
