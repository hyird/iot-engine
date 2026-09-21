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
#include "service/modules/device/device.types.h"
#include "service/modules/device/device.service.h"

namespace service::device {

class DeviceController final : public ruvia::Controller<DeviceController> {
  public:
    RUVIA_CONTROLLER_GROUP("/v1/device", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/commands", commandStatuses, service::middleware::PermissionMiddleware<"iot:device:command">, ruvia::QueryModel<DeviceCommandStatusesQuery>);
    RUVIA_GET("/commands/:id", commandStatus, service::middleware::PermissionMiddleware<"iot:device:command">, ruvia::PathModel<DeviceIdParams>);
    // 共享流按频道独立鉴权，不能把设备、分组及用户频道改成整体 AND 权限。
    RUVIA_GET_SSE("/events", deviceEvents, ruvia::QueryModel<DeviceEventsQuery>);
    RUVIA_GET("/:id/debug/packets", debugPackets, service::middleware::PermissionMiddleware<"iot:device:edit">, ruvia::PathModel<DeviceIdParams>);
    RUVIA_PUT("/:id/debug", setDebug, service::middleware::PermissionMiddleware<"iot:device:edit">, ruvia::PathModel<DeviceIdParams>, ruvia::JsonBody<DeviceDebugBody>);
    // 设备
    RUVIA_GET("/options", options, service::middleware::PermissionMiddleware<"iot:device:query">);
    RUVIA_GET("/realtime", realtime, service::middleware::PermissionMiddleware<"iot:device:query">);
    // 设备分组（统一收编到 /v1/device/groups）
    RUVIA_GET("/groups/tree-count", groupTreeCount, service::middleware::PermissionMiddleware<"iot:device-group:query">);
    RUVIA_GET("/groups/tree", groupTree, service::middleware::PermissionMiddleware<"iot:device-group:query">);
    RUVIA_GET("/groups/:id/shares", groupShares, service::middleware::PermissionMiddleware<"iot:device-group:share">, ruvia::PathModel<DeviceIdParams>);
    RUVIA_GET("/groups/:id/share-targets", groupShareTargets, service::middleware::PermissionMiddleware<"iot:device-group:share">, ruvia::PathModel<DeviceIdParams>);
    RUVIA_PUT("/groups/:id/shares", replaceGroupShares, service::middleware::PermissionMiddleware<"iot:device-group:share">, ruvia::PathModel<DeviceIdParams>, ruvia::JsonBody<ReplaceDeviceSharesBody>);
    RUVIA_GET("/groups/:id", groupDetail, service::middleware::PermissionMiddleware<"iot:device-group:query">, ruvia::PathModel<DeviceIdParams>);
    RUVIA_POST("/groups", groupCreate, service::middleware::PermissionMiddleware<"iot:device-group:add">, ruvia::JsonBody<CreateDeviceGroupBody>);
    RUVIA_PUT("/groups/:id", groupUpdate, service::middleware::PermissionMiddleware<"iot:device-group:edit">, ruvia::PathModel<DeviceIdParams>, ruvia::JsonBody<UpdateDeviceGroupBody>);
    RUVIA_DELETE("/groups/:id", groupRemove, service::middleware::PermissionMiddleware<"iot:device-group:delete">, ruvia::PathModel<DeviceIdParams>);
    RUVIA_GET("/:id/history", history, service::middleware::PermissionMiddleware<"iot:device:query">, ruvia::PathModel<DeviceIdParams>, ruvia::QueryModel<DeviceHistoryQuery>);
    RUVIA_GET("/:id/shares", shares, service::middleware::PermissionMiddleware<"iot:device:share">, ruvia::PathModel<DeviceIdParams>);
    RUVIA_GET("/:id/share-targets", shareTargets, service::middleware::PermissionMiddleware<"iot:device:share">, ruvia::PathModel<DeviceIdParams>);
    RUVIA_PUT("/:id/shares", replaceShares, service::middleware::PermissionMiddleware<"iot:device:share">, ruvia::PathModel<DeviceIdParams>, ruvia::JsonBody<ReplaceDeviceSharesBody>);
    // 设备（带参数的通配路由放在静态路由之后）
    RUVIA_GET("/:id", detail, service::middleware::PermissionMiddleware<"iot:device:query">, ruvia::PathModel<DeviceIdParams>);
    RUVIA_GET("/", list, service::middleware::PermissionMiddleware<"iot:device:query">);
    RUVIA_POST("/", create, service::middleware::PermissionMiddleware<"iot:device:add">, ruvia::JsonBody<CreateDeviceBody>);
    RUVIA_PUT("/:id", update, service::middleware::PermissionMiddleware<"iot:device:edit">, ruvia::PathModel<DeviceIdParams>, ruvia::JsonBody<UpdateDeviceBody>);
    RUVIA_DELETE("/:id", remove, service::middleware::PermissionMiddleware<"iot:device:delete">, ruvia::PathModel<DeviceIdParams>);
    RUVIA_ROUTES_END

  private:
    ruvia::Task<DeviceCommandStatusesResponse> queryCommandStatuses(service::middleware::RequestContext& request, std::string_view ids) {
        co_return service::common::ok<DeviceCommandStatusesResponse>(request, co_await deviceService().commandStatuses(request, ids));
    }

    ruvia::Task<ruvia::HttpResponse> commandStatuses(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_return c.json(co_await queryCommandStatuses(request, c.req().validated<DeviceCommandStatusesQuery>().get<"ids">().view()));
    }

    ruvia::Task<ruvia::HttpResponse> commandStatus(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_return c.json(service::common::ok<DeviceCommandStatusResponse>(request, co_await deviceService().commandStatus(request, id(c))));
    }

    ruvia::Task<ruvia::HttpResponse> debugPackets(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_return c.json(co_await queryDebugPackets(request, id(c)));
    }

    ruvia::Task<DeviceDebugPacketsResponse> queryDebugPackets(service::middleware::RequestContext& request, std::string_view deviceId) {
        co_return service::common::ok<DeviceDebugPacketsResponse>(request, co_await deviceService().debugPackets(request, deviceId));
    }

    ruvia::Task<ruvia::HttpResponse> setDebug(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await deviceService().setDebug(request, id(c), c.req().validated<DeviceDebugBody>().get<"enabled">());
        co_return c.json(service::common::operation(c, "调试设置已保存"));
    }

    static std::string id(ruvia::Context& c) {
        return std::string(c.req().validated<DeviceIdParams>().get<"id">().view());
    }

    // ---- 设备 ----
    ruvia::Task<ruvia::HttpResponse> list(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_return c.json(co_await queryList(c, request));
    }

    ruvia::Task<void> deviceEvents(ruvia::Context& c) {

        std::vector<service::live::SnapshotChannel> channels{
            { "devices", "device", [this, &c](service::middleware::RequestContext& request) -> ruvia::Task<std::string> {
                 co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:device:query");
                 co_return service::live::json(co_await queryList(c, request));
             } },
            { "groups", "device", [this, &c](service::middleware::RequestContext& request) -> ruvia::Task<std::string> {
                 co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:device-group:query");
                 co_return service::live::json(co_await queryGroupTreeCount(c, request));
             } },
            { "realtime", "device.realtime", [this, &c](service::middleware::RequestContext& request) -> ruvia::Task<std::string> {
                 co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:device:query");
                 co_return service::live::json(co_await queryRealtime(c, request));
             } }
        };
        const auto& debugDeviceId = c.req().validated<DeviceEventsQuery>().get<"debugDeviceId">();
        if (debugDeviceId) {
            channels.push_back({ "packets", "packet-debug", [this, deviceId = std::string(debugDeviceId->view())](service::middleware::RequestContext& request) -> ruvia::Task<std::string> {
                                    co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:device:edit");
                                    co_return service::live::json(co_await queryDebugPackets(request, deviceId));
                                } });
        }
        const auto& commandIds = c.req().validated<DeviceEventsQuery>().get<"commandIds">();
        if (commandIds) {
            channels.push_back({ "commands", "command", [this, ids = std::string(commandIds->view())](service::middleware::RequestContext& request) -> ruvia::Task<std::string> {
                                    co_return service::live::json(co_await queryCommandStatuses(request, ids));
                                } });
        }
        const std::string debugId = debugDeviceId ? std::string(debugDeviceId->view()) : std::string();
        co_await service::live::serveSnapshotChannels(
            c,
            service::middleware::requireAuth(c).userId,
            std::move(channels),
            [&c] { (void)service::middleware::requireAuth(c); },
            [&c, debugId]() -> ruvia::Task<void> {
                if (!debugId.empty()) {
                    co_await service::debug_idle::touch(c.redis(), "device", debugId);
                }
                co_return;
            });
    }

    ruvia::Task<DevicePageResponse> queryList(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_return service::common::ok<DevicePageResponse>(request, co_await deviceService().list(request));
    }

    ruvia::Task<ruvia::HttpResponse> realtime(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_return c.json(co_await queryRealtime(c, request));
    }

    ruvia::Task<DeviceRealtimeResponse> queryRealtime(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_return service::common::ok<DeviceRealtimeResponse>(request, co_await deviceService().realtime(request));
    }

    ruvia::Task<ruvia::HttpResponse> options(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_return c.json(service::common::ok<DeviceOptionsResponse>(request, co_await deviceService().options(request)));
    }

    ruvia::Task<ruvia::HttpResponse> detail(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_return c.json(service::common::ok<DeviceDetailResponse>(request, co_await deviceService().detail(request, id(c))));
    }

    ruvia::Task<ruvia::HttpResponse> history(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await historySnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> historySnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_return service::live::data(c, co_await deviceService().history(request, id(c), c.req().validated<DeviceHistoryQuery>()));
    }

    ruvia::Task<ruvia::HttpResponse> create(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await deviceService().create(request, c.req().validated<CreateDeviceBody>());
        co_return c.json(service::common::operation(c, "创建成功"));
    }

    ruvia::Task<ruvia::HttpResponse> update(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await deviceService().update(request, id(c), c.req().validated<UpdateDeviceBody>());
        co_return c.json(service::common::operation(c, "更新成功"));
    }

    ruvia::Task<ruvia::HttpResponse> remove(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await deviceService().remove(request, id(c));
        co_return c.json(service::common::operation(c, "删除成功"));
    }

    ruvia::Task<ruvia::HttpResponse> shares(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_return c.json(service::common::ok<DeviceSharesResponse>(request, co_await deviceShareService().list(request, id(c))));
    }

    ruvia::Task<ruvia::HttpResponse> shareTargets(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_return c.json(service::common::ok<DeviceShareTargetsResponse>(request, co_await deviceShareService().targets(request, id(c))));
    }

    ruvia::Task<ruvia::HttpResponse> replaceShares(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await deviceShareService().replace(request, id(c), c.req().validated<ReplaceDeviceSharesBody>());
        co_return c.json(service::common::operation(c, "设备分享已更新"));
    }

    // ---- 设备分组 ----
    ruvia::Task<ruvia::HttpResponse> groupTree(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_return c.json(service::common::ok<DeviceGroupListResponse>(request, co_await deviceService().listGroups(request, false)));
    }

    ruvia::Task<ruvia::HttpResponse> groupTreeCount(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_return c.json(co_await queryGroupTreeCount(c, request));
    }

    ruvia::Task<DeviceGroupListResponse> queryGroupTreeCount(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_return service::common::ok<DeviceGroupListResponse>(request, co_await deviceService().listGroups(request, true));
    }

    ruvia::Task<ruvia::HttpResponse> groupShares(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_return c.json(service::common::ok<DeviceSharesResponse>(request, co_await deviceShareService().listGroup(request, id(c))));
    }

    ruvia::Task<ruvia::HttpResponse> groupShareTargets(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_return c.json(service::common::ok<DeviceShareTargetsResponse>(request, co_await deviceShareService().groupTargets(request, id(c))));
    }

    ruvia::Task<ruvia::HttpResponse> replaceGroupShares(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await deviceShareService().replaceGroup(request, id(c), c.req().validated<ReplaceDeviceSharesBody>());
        co_return c.json(service::common::operation(c, "设备分组分享已更新"));
    }

    ruvia::Task<ruvia::HttpResponse> groupDetail(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_return c.json(service::common::ok<DeviceGroupDetailResponse>(request, co_await deviceService().groupDetail(request, id(c))));
    }

    ruvia::Task<ruvia::HttpResponse> groupCreate(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await deviceService().createGroup(request, c.req().validated<CreateDeviceGroupBody>());
        co_return c.json(service::common::operation(c, "创建成功"));
    }

    ruvia::Task<ruvia::HttpResponse> groupUpdate(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await deviceService().updateGroup(request, id(c), c.req().validated<UpdateDeviceGroupBody>());
        co_return c.json(service::common::operation(c, "更新成功"));
    }

    ruvia::Task<ruvia::HttpResponse> groupRemove(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await deviceService().removeGroup(request, id(c));
        co_return c.json(service::common::operation(c, "删除成功"));
    }
};

} // namespace service::device
