#pragma once

#include <string>
#include <string_view>

#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/middleware/auth.h"
#include "service/middleware/permission.h"
#include "service/modules/northbridge/command/device_command.service.h"
#include "service/modules/northbridge/device/device.types.h"
#include "service/modules/northbridge/open/open_access.service.h"

namespace service::open_access {

inline ruvia::HttpResponse jsonData(ruvia::Context& c, std::string_view data,
                                    std::string_view message = "ok") {
    std::pmr::string body(c.allocator<char>());
    body.append("{\"code\":0,\"message\":");
    body.append(jsonQuoted(message));
    body.append(",\"data\":");
    body.append(data);
    body.push_back('}');
    auto response = c.body(std::move(body));
    response.header("Content-Type", "application/json; charset=UTF-8");
    return response;
}

inline std::string routeId(ruvia::Context& c) {
    return std::string(c.req().param("id").value_or(""));
}

class OpenAccessAdminController final : public ruvia::Controller<OpenAccessAdminController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api/open-access", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/devices", devices);
    RUVIA_GET("/keys", keys);
    RUVIA_POST("/keys", createKey);
    RUVIA_POST("/keys/:id/rotate", rotateKey);
    RUVIA_PUT("/keys/:id", updateKey);
    RUVIA_DELETE("/keys/:id", removeKey);
    RUVIA_GET("/webhooks", webhooks);
    RUVIA_POST("/webhooks", createWebhook);
    RUVIA_PUT("/webhooks/:id", updateWebhook);
    RUVIA_DELETE("/webhooks/:id", removeWebhook);
    RUVIA_GET("/logs", logs);
    RUVIA_ROUTES_END

  private:
    ruvia::Task<ruvia::HttpResponse> devices(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:open-access:query");
        co_return jsonData(c, co_await openAccessService().deviceOptions(c));
    }

    ruvia::Task<ruvia::HttpResponse> keys(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:open-access:query");
        co_return jsonData(c, co_await openAccessService().listKeys(c));
    }

    ruvia::Task<ruvia::HttpResponse> createKey(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:open-access:add");
        const auto payload = co_await c.req().json();
        co_return jsonData(c, co_await openAccessService().createKey(c, payload), "创建成功");
    }

    ruvia::Task<ruvia::HttpResponse> updateKey(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:open-access:edit");
        const auto payload = co_await c.req().json();
        co_await openAccessService().updateKey(c, routeId(c), payload);
        co_return c.json(service::common::operation(c, "更新成功"));
    }

    ruvia::Task<ruvia::HttpResponse> rotateKey(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:open-access:edit");
        co_return jsonData(c, co_await openAccessService().rotateKey(c, routeId(c)), "轮换成功");
    }

    ruvia::Task<ruvia::HttpResponse> removeKey(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:open-access:delete");
        co_await openAccessService().removeKey(c, routeId(c));
        co_return c.json(service::common::operation(c, "删除成功"));
    }

    ruvia::Task<ruvia::HttpResponse> webhooks(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:open-access:query");
        co_return jsonData(c, co_await openAccessService().listWebhooks(c));
    }

    ruvia::Task<ruvia::HttpResponse> createWebhook(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:open-access:add");
        const auto payload = co_await c.req().json();
        co_return jsonData(c, co_await openAccessService().createWebhook(c, payload), "创建成功");
    }

    ruvia::Task<ruvia::HttpResponse> updateWebhook(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:open-access:edit");
        const auto payload = co_await c.req().json();
        co_await openAccessService().updateWebhook(c, routeId(c), payload);
        co_return c.json(service::common::operation(c, "更新成功"));
    }

    ruvia::Task<ruvia::HttpResponse> removeWebhook(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:open-access:delete");
        co_await openAccessService().removeWebhook(c, routeId(c));
        co_return c.json(service::common::operation(c, "删除成功"));
    }

    ruvia::Task<ruvia::HttpResponse> logs(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:open-access:query");
        co_return jsonData(c, co_await openAccessService().listLogs(c));
    }
};

class OpenApiController final : public ruvia::Controller<OpenApiController> {
  public:
    RUVIA_CONTROLLER_GROUP("/open-api")
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/devices", devices);
    RUVIA_GET("/devices/:id/realtime", realtime);
    RUVIA_GET("/devices/:id/history", history);
    RUVIA_POST("/devices/:id/commands", command);
    RUVIA_GET("/alerts", alerts);
    RUVIA_ROUTES_END

  private:
    static ruvia::Task<void> logSafe(ruvia::Context& c, std::string_view action,
                                     const AccessSession& session, std::string_view deviceId = {},
                                     std::string_view requestPayload = "{}",
                                     std::string_view responsePayload = "{}") {
        try {
            co_await openAccessService().writeLog(
                c, "pull", action, "success", session.id, {}, {}, c.req().method(), c.req().path(),
                clientIp(c), 200, deviceId, {}, {}, requestPayload, responsePayload);
        } catch (...) {
        }
    }

    ruvia::Task<ruvia::HttpResponse> devices(ruvia::Context& c) {
        const auto session = co_await openAccessService().authenticate(c, {});
        const auto data = co_await openAccessService().publicDevices(c, session);
        co_await logSafe(c, "device-list", session, {}, "{}", "{}");
        co_return jsonData(c, data);
    }

    ruvia::Task<ruvia::HttpResponse> realtime(ruvia::Context& c) {
        const auto session = co_await openAccessService().authenticate(c, kScopeRealtime);
        const auto id = routeId(c);
        const auto data = co_await openAccessService().publicRealtime(c, session, id);
        co_await logSafe(c, "realtime", session, id);
        co_return jsonData(c, data);
    }

    ruvia::Task<ruvia::HttpResponse> history(ruvia::Context& c) {
        const auto session = co_await openAccessService().authenticate(c, kScopeHistory);
        const auto id = routeId(c);
        const auto data = co_await openAccessService().publicHistory(c, session, id);
        co_await logSafe(c, "history", session, id);
        co_return jsonData(c, data);
    }

    ruvia::Task<ruvia::HttpResponse> command(ruvia::Context& c) {
        const auto session = co_await openAccessService().authenticate(c, kScopeCommand);
        const auto id = routeId(c);
        if (!session.allowsDevice(id))
            service::common::fail(19011, "AccessKey 无权控制该设备", 403);
        requireUuid(id, "设备 ID 无效");
        const auto body = co_await c.req().json<service::device::DeviceCommandBody>();
        auto result = co_await service::northbridge::command::deviceCommandService().createExternal(
            c, id, body, session.id);
        co_await logSafe(c, "command", session, id);
        co_return c.json(service::common::ok<service::device::DeviceCommandCreateResponse>(
            c, std::move(result)));
    }

    ruvia::Task<ruvia::HttpResponse> alerts(ruvia::Context& c) {
        const auto session = co_await openAccessService().authenticate(c, kScopeAlert);
        const auto data = co_await openAccessService().publicAlerts(c, session);
        co_await logSafe(c, "alert", session);
        co_return jsonData(c, data);
    }
};

} // namespace service::open_access
