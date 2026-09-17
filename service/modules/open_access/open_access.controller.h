#pragma once

#include <string>
#include <string_view>

#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/middleware/live.h"
#include "service/middleware/permission.h"
#include "service/modules/command/command.service.h"
#include "service/modules/device/device.types.h"
#include "service/modules/open_access/open_access.schema.h"
#include "service/modules/open_access/open_access.service.h"
#include "service/utils/json.h"
#include "service/utils/text.h"

namespace service::access {

inline ruvia::HttpResponse jsonData(ruvia::Context& c, std::string_view data, std::string_view message = "ok") {
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

class AccessAdminController final : public ruvia::Controller<AccessAdminController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/device/options", devices);
    RUVIA_GET("/open-access-key", keys);
    RUVIA_GET("/open-webhook", webhooks, WebhookQueryValidator);
    RUVIA_GET("/open-access-log", logs, AccessLogValidator);
    RUVIA_POST("/open-access-key", createKey);
    RUVIA_PUT("/open-access-key/:id", updateKey, AccessIdValidator);
    RUVIA_POST("/open-access-key/:id/rotate", rotateKey, AccessIdValidator);
    RUVIA_DELETE("/open-access-key/:id", removeKey, AccessIdValidator);
    RUVIA_POST("/open-webhook", createWebhook);
    RUVIA_PUT("/open-webhook/:id", updateWebhook, AccessIdValidator);
    RUVIA_DELETE("/open-webhook/:id", removeWebhook, AccessIdValidator);
    RUVIA_ROUTES_END
  private:
    static std::string id(ruvia::Context& c) {
        return std::string(c.req().validated<AccessIdParams>().get<"id">()->view());
    }

    ruvia::Task<ruvia::HttpResponse> devices(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await devicesSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> devicesSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:open-access:query");
        co_return service::live::data(c, co_await accessService().deviceOptions(request));
    }

    ruvia::Task<ruvia::HttpResponse> keys(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await keysSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> keysSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:open-access:query");
        co_return service::live::data(c, co_await accessService().listKeys(request));
    }

    ruvia::Task<ruvia::HttpResponse> webhooks(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await webhooksSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> webhooksSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:open-access:query");
        WebhookQuery filters;
        if (const auto& value = c.req().validated<WebhookQueryParams>().get<"accessKeyId">()) {
            filters.accessKeyId = std::string(value->view());
        }
        co_return service::live::data(c, co_await accessService().listWebhooks(request, filters));
    }

    ruvia::Task<ruvia::HttpResponse> logs(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await logsSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> logsSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:open-access:query");
        co_return service::live::data(c, co_await accessService().listLogs(request, AccessLogValidator::filters(c.req().validated<AccessLogParams>())));
    }

    ruvia::Task<ruvia::HttpResponse> createKey(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:open-access:add");
        const auto json = co_await c.req().jsonValue();
        if (!json.isObject()) {
            service::common::fail(19002, "请求体必须是对象", 400);
        }
        const auto body = AccessPayloadValidator::keyInput(json, true);
        co_return jsonData(c, co_await accessService().createKey(request, body));
    }

    ruvia::Task<ruvia::HttpResponse> updateKey(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:open-access:edit");
        const auto json = co_await c.req().jsonValue();
        if (!json.isObject()) {
            service::common::fail(19002, "请求体必须是对象", 400);
        }
        const auto body = AccessPayloadValidator::keyInput(json, false);
        co_await accessService().updateKey(request, id(c), body);
        co_return c.json(service::common::operation(c, "操作成功"));
    }

    ruvia::Task<ruvia::HttpResponse> rotateKey(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:open-access:edit");
        co_return jsonData(c, co_await accessService().rotateKey(request, id(c)));
    }

    ruvia::Task<ruvia::HttpResponse> removeKey(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:open-access:delete");
        co_await accessService().removeKey(request, id(c));
        co_return c.json(service::common::operation(c, "操作成功"));
    }

    ruvia::Task<ruvia::HttpResponse> createWebhook(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:open-access:add");
        const auto json = co_await c.req().jsonValue();
        if (!json.isObject()) {
            service::common::fail(19002, "请求体必须是对象", 400);
        }
        const auto body = AccessPayloadValidator::webhookInput(json, true);
        co_return jsonData(c, co_await accessService().createWebhook(request, body));
    }

    ruvia::Task<ruvia::HttpResponse> updateWebhook(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:open-access:edit");
        const auto json = co_await c.req().jsonValue();
        if (!json.isObject()) {
            service::common::fail(19002, "请求体必须是对象", 400);
        }
        const auto body = AccessPayloadValidator::webhookInput(json, false);
        co_await accessService().updateWebhook(request, id(c), body);
        co_return c.json(service::common::operation(c, "操作成功"));
    }

    ruvia::Task<ruvia::HttpResponse> removeWebhook(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:open-access:delete");
        co_await accessService().removeWebhook(request, id(c));
        co_return c.json(service::common::operation(c, "操作成功"));
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
    template <typename Query>
    static ruvia::Task<void> servePublicQuery(ruvia::Context& context, std::string_view topic, Query query, std::function<ruvia::Task<void>()> authorize = {}, std::chrono::milliseconds coalesceDelay = {}) {
        using namespace std::chrono_literals;
        if (context.req().header("Accept").value_or("").find("application/json") != std::string_view::npos) {
            if (authorize) {
                co_await authorize();
            }
            const auto snapshot = co_await query();
            context.header("Content-Type", "application/json");
            context.header("Cache-Control", "no-store");
            context.header("Vary", "Accept");
            context.header("X-Snapshot-Topic", topic);
            context.header("X-Snapshot-Coalesce-Ms", std::to_string(coalesceDelay.count()));
            context.respond(context.body(std::string_view(snapshot)));
            co_return;
        }
        if (context.req().header("Accept").value_or("").find("text/event-stream") ==
            std::string_view::npos) {
            service::common::fail(10002, "This query requires text/event-stream", 406);
        }
        auto subscription = service::live::bus().subscribe(context.worker(), topic);
        auto snapshot = co_await query();
        context.header("X-Accel-Buffering", "no");
        context.header("Cache-Control", "no-store");
        auto stream = context.streamSse();
        std::uint64_t revision = 1;
        auto id = std::to_string(revision);
        co_await stream.write({ .data = snapshot, .event = "snapshot", .id = id, .retry = 1s });
        // Bound request-arena retention. Reconnection creates a new authorized
        // snapshot; event IDs are connection-local, never misleading replay cursors.
        const auto expires = std::chrono::steady_clock::now() + (coalesceDelay.count() > 0 ? 1min : 5min);
        while (!stream.aborted() && std::chrono::steady_clock::now() < expires) {
            const auto notification =
                co_await subscription->receiver.receiveFor(15s, context.stopToken());
            if (stream.aborted() || context.stopToken().stopRequested()) {
                co_return;
            }
            if (notification.hasValue() && coalesceDelay.count() > 0) {
                (void)co_await ruvia::sleepFor(context.worker(), coalesceDelay, context.stopToken());
                if (stream.aborted() || context.stopToken().stopRequested()) {
                    co_return;
                }
            }
            std::string error;
            std::string next;
            try {
                // Heartbeats only check token expiration. Permission changes publish
                // an auth event, which reexecutes the authorized query immediately.
                if (authorize) {
                    co_await authorize();
                } else {
                    (void)service::middleware::requireAuth(context);
                }
                if (notification.hasValue()) {
                    next = co_await query();
                }
            } catch (const ruvia::HttpError& failure) {
                const auto info = failure.info();
                error = service::live::json(service::common::error(context, service::common::errorCode(info.code(), info.status().value()), info.message()));
            } catch (const std::exception&) {
                error = "{\"code\":10004,\"message\":\"Subscription interrupted\"}";
            }
            if (!error.empty()) {
                co_await stream.write({ .data = error, .event = "error" });
                co_return;
            }
            if (notification.hasValue() && next != snapshot) {
                snapshot = std::move(next);
                id = std::to_string(++revision);
                co_await stream.write({ .data = snapshot, .event = "snapshot", .id = id });
            } else {
                co_await stream.write({ .data = "{}", .event = "heartbeat" });
            }
        }
    }

    ruvia::Task<void> devices(ruvia::Context& c) {
        bool audited = false;
        co_await servePublicQuery(c, "access", [this, &c, &audited]() {
            return devicesSnapshot(c, audited);
        },
                                  [&c]() -> ruvia::Task<void> {
                                      (void)co_await accessService().authenticate(c, {});
                                  });
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
        co_await servePublicQuery(c, "access", [this, &c, &audited]() {
            return realtimeSnapshot(c, audited);
        },
                                  [&c]() -> ruvia::Task<void> {
                                      (void)co_await accessService().authenticate(c, kScopeRealtime);
                                  });
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
        co_await servePublicQuery(c, "access", [this, &c, &audited]() {
            return historySnapshot(c, audited);
        },
                                  [&c]() -> ruvia::Task<void> {
                                      (void)co_await accessService().authenticate(c, kScopeHistory);
                                  });
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
        if (!session.allowsDevice(id)) {
            service::common::fail(19011, "AccessKey 无权控制该设备", 403);
        }
        auto result = co_await service::command::commandService().createExternal(
            c,
            id,
            body,
            session.id
        );
        co_await accessService().audit(c, "command", session, id);
        co_return c.json(service::common::ok<service::device::DeviceCommandCreateResponse>(c, std::move(result)));
    }

    ruvia::Task<void> alerts(ruvia::Context& c) {
        bool audited = false;
        co_await servePublicQuery(c, "access", [this, &c, &audited]() {
            return alertsSnapshot(c, audited);
        },
                                  [&c]() -> ruvia::Task<void> {
                                      (void)co_await accessService().authenticate(c, kScopeAlert);
                                  });
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
