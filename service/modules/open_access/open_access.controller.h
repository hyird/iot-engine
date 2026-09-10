#pragma once

#include <string>
#include <string_view>

#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/middleware/live.h"
#include "service/middleware/auth.h"
#include "service/middleware/permission.h"
#include "service/modules/command/command.service.h"
#include "service/modules/device/device.types.h"
#include "service/modules/open_access/open_access.service.h"
#include "service/utils/json.h"
#include "service/utils/text.h"

namespace service::access {

inline ruvia::HttpResponse jsonData(ruvia::Context& c, std::string_view data,
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

inline std::string routeId(ruvia::Context& c) {
    return std::string(c.req().param("id").value_or(""));
}

class AccessAdminController final : public ruvia::Controller<AccessAdminController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET_SSE("/device/options", devices);
    RUVIA_GET_SSE("/open-access-key", keys);
    RUVIA_POST("/open-access-key", createKey);
    RUVIA_POST("/open-access-key/:id/rotate", rotateKey);
    RUVIA_PUT("/open-access-key/:id", updateKey);
    RUVIA_DELETE("/open-access-key/:id", removeKey);
    RUVIA_GET_SSE("/open-webhook", webhooks);
    RUVIA_POST("/open-webhook", createWebhook);
    RUVIA_PUT("/open-webhook/:id", updateWebhook);
    RUVIA_DELETE("/open-webhook/:id", removeWebhook);
    RUVIA_GET_SSE("/open-access-log", logs);
    RUVIA_ROUTES_END

  private:
    ruvia::Task<void> devices(ruvia::Context& c) {
        co_await service::live::serve(c, "access", [this, &c]() { return devicesSnapshot(c); });
    }

    ruvia::Task<std::string> devicesSnapshot(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:open-access:query");
        co_return service::live::data(c, co_await accessService().deviceOptions(c));
    }

    ruvia::Task<void> keys(ruvia::Context& c) {
        co_await service::live::serve(c, "access", [this, &c]() { return keysSnapshot(c); });
    }

    ruvia::Task<std::string> keysSnapshot(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:open-access:query");
        co_return service::live::data(c, co_await accessService().listKeys(c));
    }

    ruvia::Task<ruvia::HttpResponse> createKey(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:open-access:add");
        const auto payload = co_await c.req().jsonValue();
        co_return jsonData(c, co_await accessService().createKey(c, payload), "创建成功");
    }

    ruvia::Task<ruvia::HttpResponse> updateKey(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:open-access:edit");
        const auto payload = co_await c.req().jsonValue();
        co_await accessService().updateKey(c, routeId(c), payload);
        co_return c.json(service::common::operation(c, "更新成功"));
    }

    ruvia::Task<ruvia::HttpResponse> rotateKey(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:open-access:edit");
        co_return jsonData(c, co_await accessService().rotateKey(c, routeId(c)), "轮换成功");
    }

    ruvia::Task<ruvia::HttpResponse> removeKey(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:open-access:delete");
        co_await accessService().removeKey(c, routeId(c));
        co_return c.json(service::common::operation(c, "删除成功"));
    }

    ruvia::Task<void> webhooks(ruvia::Context& c) {
        co_await service::live::serve(c, "access", [this, &c]() { return webhooksSnapshot(c); });
    }

    ruvia::Task<std::string> webhooksSnapshot(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:open-access:query");
        co_return service::live::data(c, co_await accessService().listWebhooks(c));
    }

    ruvia::Task<ruvia::HttpResponse> createWebhook(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:open-access:add");
        const auto payload = co_await c.req().jsonValue();
        co_return jsonData(c, co_await accessService().createWebhook(c, payload), "创建成功");
    }

    ruvia::Task<ruvia::HttpResponse> updateWebhook(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:open-access:edit");
        const auto payload = co_await c.req().jsonValue();
        co_await accessService().updateWebhook(c, routeId(c), payload);
        co_return c.json(service::common::operation(c, "更新成功"));
    }

    ruvia::Task<ruvia::HttpResponse> removeWebhook(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:open-access:delete");
        co_await accessService().removeWebhook(c, routeId(c));
        co_return c.json(service::common::operation(c, "删除成功"));
    }

    ruvia::Task<void> logs(ruvia::Context& c) {
        co_await service::live::serve(c, "access", [this, &c]() { return logsSnapshot(c); });
    }

    ruvia::Task<std::string> logsSnapshot(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:open-access:query");
        co_return service::live::data(c, co_await accessService().listLogs(c));
    }
};

class AccessController final : public ruvia::Controller<AccessController> {
  public:
    RUVIA_CONTROLLER_GROUP("/open-api/device")
    RUVIA_ROUTES_BEGIN
    RUVIA_GET_SSE("/list", devices);
    RUVIA_GET_SSE("/realtime", realtime);
    RUVIA_GET_SSE("/history", history);
    RUVIA_POST("/command", command);
    RUVIA_GET_SSE("/alert", alerts);
    RUVIA_ROUTES_END

  private:
    ruvia::Task<void> devices(ruvia::Context& c) {
        bool audited = false;
        co_await service::live::serve(c, "access", [this, &c, &audited]() { return devicesSnapshot(c, audited); }, [&c]() -> ruvia::Task<void> { (void)co_await accessService().authenticate(c, {}); });
    }

    ruvia::Task<std::string> devicesSnapshot(ruvia::Context& c, bool& audited) {
        const auto session = co_await accessService().authenticate(c, {});
        const auto data = co_await accessService().publicDevices(c, session);
        if (!audited) {
            co_await accessService().audit(c, "device-list", session, std::string_view{}, "{}", data);
            audited = true;
        }
        co_return service::live::data(c, data);
    }

    ruvia::Task<void> realtime(ruvia::Context& c) {
        bool audited = false;
        co_await service::live::serve(c, "access", [this, &c, &audited]() { return realtimeSnapshot(c, audited); }, [&c]() -> ruvia::Task<void> { (void)co_await accessService().authenticate(c, kScopeRealtime); });
    }

    ruvia::Task<std::string> realtimeSnapshot(ruvia::Context& c, bool& audited) {
        const auto session = co_await accessService().authenticate(c, kScopeRealtime);
        const auto id = service::utils::trim(c.req().query("deviceId").value_or(""));
        const auto data = co_await accessService().publicRealtime(c, session, id);
        if (!audited) {
            co_await accessService().audit(c, "realtime", session, id, "{}", data);
            audited = true;
        }
        co_return service::live::data(c, data);
    }

    ruvia::Task<void> history(ruvia::Context& c) {
        bool audited = false;
        co_await service::live::serve(c, "access", [this, &c, &audited]() { return historySnapshot(c, audited); }, [&c]() -> ruvia::Task<void> { (void)co_await accessService().authenticate(c, kScopeHistory); });
    }

    ruvia::Task<std::string> historySnapshot(ruvia::Context& c, bool& audited) {
        const auto session = co_await accessService().authenticate(c, kScopeHistory);
        const auto id = service::utils::trim(c.req().query("deviceId").value_or(""));
        const auto data = co_await accessService().publicHistory(c, session, id);
        if (!audited) {
            co_await accessService().audit(c, "history", session, id, "{}", data);
            audited = true;
        }
        co_return service::live::data(c, data);
    }

    ruvia::Task<ruvia::HttpResponse> command(ruvia::Context& c) {
        const auto session = co_await accessService().authenticate(c, kScopeCommand);
        const auto body = co_await c.req().json<service::device::DeviceCommandBody>();
        const auto id =
            body.get<"deviceId">() ? std::string(body.get<"deviceId">()->view()) : std::string{};
        service::common::requireUuid(19002, id, "设备 ID 无效");
        if (!session.allowsDevice(id))
            service::common::fail(19011, "AccessKey 无权控制该设备", 403);
        auto result = co_await service::command::commandService().createExternal(
            c, id, body, session.id);
        co_await accessService().audit(c, "command", session, id);
        co_return c.json(service::common::ok<service::device::DeviceCommandCreateResponse>(
            c, std::move(result)));
    }

    ruvia::Task<void> alerts(ruvia::Context& c) {
        bool audited = false;
        co_await service::live::serve(c, "access", [this, &c, &audited]() { return alertsSnapshot(c, audited); }, [&c]() -> ruvia::Task<void> { (void)co_await accessService().authenticate(c, kScopeAlert); });
    }

    ruvia::Task<std::string> alertsSnapshot(ruvia::Context& c, bool& audited) {
        const auto session = co_await accessService().authenticate(c, kScopeAlert);
        const auto data = co_await accessService().publicAlerts(c, session);
        if (!audited) {
            co_await accessService().audit(c, "alert", session, std::string_view{}, "{}", data);
            audited = true;
        }
        co_return service::live::data(c, data);
    }
};

} // namespace service::access
