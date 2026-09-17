#pragma once

#include <chrono>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/middleware/auth.h"
#include "service/middleware/live.h"
#include "service/middleware/permission.h"
#include "service/modules/device/device.schema.h"
#include "service/modules/device/device.service.h"

namespace service::device {

class DeviceController final : public ruvia::Controller<DeviceController> {
  public:
    RUVIA_CONTROLLER_GROUP("/v1/device", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/commands", commandStatuses, DeviceCommandStatusesValidator);
    RUVIA_GET("/commands/:id", commandStatus, DeviceIdParamsValidator);
    RUVIA_GET_SSE("/events", deviceEvents, DeviceEventsValidator);
    RUVIA_GET("/:id/debug/packets", debugPackets, DeviceIdParamsValidator);
    RUVIA_PUT("/:id/debug", setDebug, DeviceIdParamsValidator, DeviceDebugValidator);
    // 设备
    RUVIA_GET("/options", options);
    RUVIA_GET("/realtime", realtime);
    // 设备分组（统一收编到 /v1/device/groups）
    RUVIA_GET("/groups/tree-count", groupTreeCount);
    RUVIA_GET("/groups/tree", groupTree);
    RUVIA_GET("/groups/:id/shares", groupShares, DeviceIdParamsValidator);
    RUVIA_GET("/groups/:id/share-targets", groupShareTargets, DeviceIdParamsValidator);
    RUVIA_PUT("/groups/:id/shares", replaceGroupShares, DeviceIdParamsValidator, ReplaceDeviceSharesValidator);
    RUVIA_GET("/groups/:id", groupDetail, DeviceIdParamsValidator);
    RUVIA_POST("/groups", groupCreate, CreateDeviceGroupValidator);
    RUVIA_PUT("/groups/:id", groupUpdate, DeviceIdParamsValidator, UpdateDeviceGroupValidator);
    RUVIA_DELETE("/groups/:id", groupRemove, DeviceIdParamsValidator);
    RUVIA_GET("/:id/history", history, DeviceIdParamsValidator, DeviceHistoryValidator);
    RUVIA_GET("/:id/shares", shares, DeviceIdParamsValidator);
    RUVIA_GET("/:id/share-targets", shareTargets, DeviceIdParamsValidator);
    RUVIA_PUT("/:id/shares", replaceShares, DeviceIdParamsValidator, ReplaceDeviceSharesValidator);
    // 设备（带参数的通配路由放在静态路由之后）
    RUVIA_GET("/:id", detail, DeviceIdParamsValidator);
    RUVIA_GET("/", list);
    RUVIA_POST("/", create, CreateDeviceValidator);
    RUVIA_PUT("/:id", update, DeviceIdParamsValidator, UpdateDeviceValidator);
    RUVIA_DELETE("/:id", remove, DeviceIdParamsValidator);
    RUVIA_ROUTES_END

  private:
    ruvia::Task<std::string> commandStatusesSnapshot(service::middleware::RequestContext& request, std::string_view ids) {
        co_return service::live::json(service::common::ok<DeviceCommandStatusesResponse>(request, co_await deviceService().commandStatuses(request, ids)));
    }

    ruvia::Task<ruvia::HttpResponse> commandStatuses(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await commandStatusesSnapshot(request, c.req().validated<DeviceCommandStatusesQuery>().get<"ids">().view());
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<ruvia::HttpResponse> commandStatus(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_return c.json(service::common::ok<DeviceCommandStatusResponse>(request, co_await deviceService().commandStatus(request, id(c))));
    }

    ruvia::Task<ruvia::HttpResponse> debugPackets(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await debugPacketsSnapshot(request, id(c));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> debugPacketsSnapshot(service::middleware::RequestContext& request, std::string_view deviceId) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:device:edit");
        co_return service::live::json(service::common::ok<DeviceDebugPacketsResponse>(request, co_await deviceService().debugPackets(request, deviceId)));
    }

    ruvia::Task<ruvia::HttpResponse> setDebug(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:device:edit");
        co_await deviceService().setDebug(request, id(c), *c.req().validated<DeviceDebugBody>().get<"enabled">());
        co_return c.json(service::common::operation(c, "调试设置已保存"));
    }

    static std::string id(ruvia::Context& c) {
        return std::string(c.req().validated<DeviceIdParams>().get<"id">()->view());
    }

    // ---- 设备 ----
    ruvia::Task<ruvia::HttpResponse> list(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await listSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<void> deviceEvents(ruvia::Context& c) {
        std::vector<service::live::SnapshotChannel> channels{
            { "devices", "device", [this, &c](service::middleware::RequestContext& request) {
                 return listSnapshot(c, request);
             } },
            { "groups", "device", [this, &c](service::middleware::RequestContext& request) {
                 return groupTreeCountSnapshot(c, request);
             } },
            { "realtime", "device.realtime", [this, &c](service::middleware::RequestContext& request) {
                 return realtimeSnapshot(c, request);
             } }
        };
        const auto& debugDeviceId = c.req().validated<DeviceEventsQuery>().get<"debugDeviceId">();
        if (debugDeviceId) {
            channels.push_back({ "packets", "packet-debug", [this, deviceId = std::string(debugDeviceId->view())](service::middleware::RequestContext& request) {
                                    return debugPacketsSnapshot(request, deviceId);
                                } });
        }
        const auto& commandIds = c.req().validated<DeviceEventsQuery>().get<"commandIds">();
        if (commandIds) {
            channels.push_back({ "commands", "command", [this, ids = std::string(commandIds->view())](service::middleware::RequestContext& request) {
                                    return commandStatusesSnapshot(request, ids);
                                } });
        }
        co_await service::live::serveSnapshotChannels(c, service::middleware::requireAuth(c).userId, std::move(channels), [&c] {
            (void)service::middleware::requireAuth(c);
        });
    }

    ruvia::Task<std::string> listSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:device:query");
        co_return service::live::json(
            service::common::ok<DevicePageResponse>(request, co_await deviceService().list(request))
        );
    }

    ruvia::Task<ruvia::HttpResponse> realtime(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await realtimeSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> realtimeSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:device:query");
        co_return service::live::json(
            service::common::ok<DeviceRealtimeResponse>(request, co_await deviceService().realtime(request))
        );
    }

    ruvia::Task<ruvia::HttpResponse> options(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await optionsSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> optionsSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:device:query");
        co_return service::live::json(
            service::common::ok<DeviceOptionsResponse>(request, co_await deviceService().options(request))
        );
    }

    ruvia::Task<ruvia::HttpResponse> detail(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await detailSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> detailSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:device:query");
        co_return service::live::json(service::common::ok<DeviceDetailResponse>(request, co_await deviceService().detail(request, id(c))));
    }

    ruvia::Task<ruvia::HttpResponse> history(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await historySnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> historySnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:device:query");
        co_return service::live::data(c, co_await deviceService().history(request, id(c), c.req().validated<DeviceHistoryQuery>()));
    }

    ruvia::Task<ruvia::HttpResponse> create(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:device:add");
        co_await deviceService().create(request, c.req().validated<SaveDeviceBody>());
        co_return c.json(service::common::operation(c, "创建成功"));
    }

    ruvia::Task<ruvia::HttpResponse> update(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:device:edit");
        co_await deviceService().update(request, id(c), c.req().validated<SaveDeviceBody>());
        co_return c.json(service::common::operation(c, "更新成功"));
    }

    ruvia::Task<ruvia::HttpResponse> remove(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:device:delete");
        co_await deviceService().remove(request, id(c));
        co_return c.json(service::common::operation(c, "删除成功"));
    }

    ruvia::Task<ruvia::HttpResponse> shares(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await sharesSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> sharesSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:device:share");
        co_return service::live::json(service::common::ok<DeviceSharesResponse>(request, co_await deviceShareService().list(request, id(c))));
    }

    ruvia::Task<ruvia::HttpResponse> shareTargets(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await shareTargetsSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> shareTargetsSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:device:share");
        co_return service::live::json(service::common::ok<DeviceShareTargetsResponse>(request, co_await deviceShareService().targets(request, id(c))));
    }

    ruvia::Task<ruvia::HttpResponse> replaceShares(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:device:share");
        co_await deviceShareService().replace(request, id(c), c.req().validated<ReplaceDeviceSharesBody>());
        co_return c.json(service::common::operation(c, "设备分享已更新"));
    }

    // ---- 设备分组 ----
    ruvia::Task<ruvia::HttpResponse> groupTree(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await groupTreeSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> groupTreeSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:device-group:query");
        co_return service::live::json(service::common::ok<DeviceGroupListResponse>(request, co_await deviceService().listGroups(request, false)));
    }

    ruvia::Task<ruvia::HttpResponse> groupTreeCount(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await groupTreeCountSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> groupTreeCountSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:device-group:query");
        co_return service::live::json(service::common::ok<DeviceGroupListResponse>(request, co_await deviceService().listGroups(request, true)));
    }

    ruvia::Task<ruvia::HttpResponse> groupShares(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await groupSharesSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> groupSharesSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:device-group:share");
        co_return service::live::json(service::common::ok<DeviceSharesResponse>(request, co_await deviceShareService().listGroup(request, id(c))));
    }

    ruvia::Task<ruvia::HttpResponse> groupShareTargets(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await groupShareTargetsSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> groupShareTargetsSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:device-group:share");
        co_return service::live::json(service::common::ok<DeviceShareTargetsResponse>(request, co_await deviceShareService().groupTargets(request, id(c))));
    }

    ruvia::Task<ruvia::HttpResponse> replaceGroupShares(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:device-group:share");
        co_await deviceShareService().replaceGroup(request, id(c), c.req().validated<ReplaceDeviceSharesBody>());
        co_return c.json(service::common::operation(c, "设备分组分享已更新"));
    }

    ruvia::Task<ruvia::HttpResponse> groupDetail(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await groupDetailSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> groupDetailSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:device-group:query");
        co_return service::live::json(service::common::ok<DeviceGroupDetailResponse>(request, co_await deviceService().groupDetail(request, id(c))));
    }

    ruvia::Task<ruvia::HttpResponse> groupCreate(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:device-group:add");
        co_await deviceService().createGroup(request, c.req().validated<SaveDeviceGroupBody>());
        co_return c.json(service::common::operation(c, "创建成功"));
    }

    ruvia::Task<ruvia::HttpResponse> groupUpdate(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:device-group:edit");
        co_await deviceService().updateGroup(request, id(c), c.req().validated<SaveDeviceGroupBody>());
        co_return c.json(service::common::operation(c, "更新成功"));
    }

    ruvia::Task<ruvia::HttpResponse> groupRemove(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:device-group:delete");
        co_await deviceService().removeGroup(request, id(c));
        co_return c.json(service::common::operation(c, "删除成功"));
    }
};

} // namespace service::device
