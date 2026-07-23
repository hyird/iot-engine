#pragma once

#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/middleware/auth.h"
#include "service/middleware/permission.h"
#include "service/modules/northbridge/command/device_command.service.h"
#include "service/modules/northbridge/device/device.schema.h"
#include "service/modules/northbridge/device/device.share.service.h"
#include "service/modules/northbridge/device/device.service.h"

namespace service::device {

class DeviceController final : public ruvia::Controller<DeviceController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api/device", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    // 设备
    RUVIA_GET("/options", options);
    RUVIA_GET("/realtime", realtime);
    RUVIA_GET("/commands/:id", commandStatus, DeviceIdParamsValidator);
    // 设备分组（统一收编到 /api/device/groups）
    RUVIA_GET("/groups/tree-count", groupTreeCount);
    RUVIA_GET("/groups/tree", groupTree);
    RUVIA_GET("/groups/:id/shares", groupShares, DeviceIdParamsValidator);
    RUVIA_GET("/groups/:id/share-targets", groupShareTargets, DeviceIdParamsValidator);
    RUVIA_PUT("/groups/:id/shares", replaceGroupShares, DeviceIdParamsValidator,
              ReplaceDeviceSharesValidator);
    RUVIA_GET("/groups/:id", groupDetail, DeviceIdParamsValidator);
    RUVIA_POST("/groups", groupCreate, CreateDeviceGroupValidator);
    RUVIA_PUT("/groups/:id", groupUpdate, DeviceIdParamsValidator, UpdateDeviceGroupValidator);
    RUVIA_DELETE("/groups/:id", groupRemove, DeviceIdParamsValidator);
    RUVIA_GET("/:id/history", history, DeviceIdParamsValidator);
    RUVIA_GET("/:id/shares", shares, DeviceIdParamsValidator);
    RUVIA_GET("/:id/share-targets", shareTargets, DeviceIdParamsValidator);
    RUVIA_PUT("/:id/shares", replaceShares, DeviceIdParamsValidator, ReplaceDeviceSharesValidator);
    RUVIA_POST("/:id/commands", command, DeviceIdParamsValidator, DeviceCommandValidator);
    // 设备（带参数的通配路由放在静态路由之后）
    RUVIA_GET("/:id", detail, DeviceIdParamsValidator);
    RUVIA_GET("/", list);
    RUVIA_POST("/", create, CreateDeviceValidator);
    RUVIA_PUT("/:id", update, DeviceIdParamsValidator, UpdateDeviceValidator);
    RUVIA_DELETE("/:id", remove, DeviceIdParamsValidator);
    RUVIA_ROUTES_END

  private:
    static std::string id(ruvia::Context& c) {
        return std::string(c.req().valid<DeviceIdParams>().id()->view());
    }

    static ruvia::HttpResponse jsonData(ruvia::Context& c, std::string_view data) {
        std::pmr::string body(c.allocator<char>());
        body.append("{\"code\":0,\"message\":\"ok\",\"data\":");
        body.append(data);
        body.push_back('}');
        auto response = c.body(std::move(body));
        response.header("Content-Type", "application/json; charset=UTF-8");
        return response;
    }

    // ---- 设备 ----
    ruvia::Task<ruvia::HttpResponse> list(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device:query");
        co_return c.json(
            service::common::ok<DevicePageResponse>(c, co_await deviceService().list(c)));
    }
    ruvia::Task<ruvia::HttpResponse> realtime(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device:query");
        co_return c.json(
            service::common::ok<DeviceRealtimeResponse>(c, co_await deviceService().realtime(c)));
    }
    ruvia::Task<ruvia::HttpResponse> options(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device:query");
        co_return c.json(
            service::common::ok<DeviceOptionsResponse>(c, co_await deviceService().options(c)));
    }
    ruvia::Task<ruvia::HttpResponse> detail(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device:query");
        co_return c.json(service::common::ok<DeviceDetailResponse>(
            c, co_await deviceService().detail(c, id(c))));
    }
    ruvia::Task<ruvia::HttpResponse> history(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device:query");
        co_return jsonData(c, co_await deviceService().history(c, id(c)));
    }
    ruvia::Task<ruvia::HttpResponse> create(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device:add");
        co_await deviceService().create(c, c.req().valid<SaveDeviceBody>());
        co_return c.json(service::common::operation(c, "创建成功"));
    }
    ruvia::Task<ruvia::HttpResponse> update(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device:edit");
        co_await deviceService().update(c, id(c), c.req().valid<SaveDeviceBody>());
        co_return c.json(service::common::operation(c, "更新成功"));
    }
    ruvia::Task<ruvia::HttpResponse> remove(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device:delete");
        co_await deviceService().remove(c, id(c));
        co_return c.json(service::common::operation(c, "删除成功"));
    }

    ruvia::Task<ruvia::HttpResponse> command(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device:command");
        co_return c.json(service::common::ok<DeviceCommandCreateResponse>(
            c, co_await service::northbridge::command::deviceCommandService().create(
                   c, id(c), c.req().valid<DeviceCommandBody>())));
    }

    ruvia::Task<ruvia::HttpResponse> commandStatus(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device:command");
        co_return c.json(service::common::ok<DeviceCommandStatusResponse>(
            c, co_await service::northbridge::command::deviceCommandService().status(c, id(c))));
    }

    ruvia::Task<ruvia::HttpResponse> shares(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device:share");
        co_return c.json(service::common::ok<DeviceSharesResponse>(
            c, co_await deviceShareService().list(c, id(c))));
    }

    ruvia::Task<ruvia::HttpResponse> shareTargets(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device:share");
        co_return c.json(service::common::ok<DeviceShareTargetsResponse>(
            c, co_await deviceShareService().targets(c, id(c))));
    }

    ruvia::Task<ruvia::HttpResponse> replaceShares(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device:share");
        co_await deviceShareService().replace(c, id(c), c.req().valid<ReplaceDeviceSharesBody>());
        co_return c.json(service::common::operation(c, "设备分享已更新"));
    }

    // ---- 设备分组 ----
    ruvia::Task<ruvia::HttpResponse> groupTree(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device-group:query");
        co_return c.json(service::common::ok<DeviceGroupListResponse>(
            c, co_await deviceService().listGroups(c, false)));
    }
    ruvia::Task<ruvia::HttpResponse> groupTreeCount(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device-group:query");
        co_return c.json(service::common::ok<DeviceGroupListResponse>(
            c, co_await deviceService().listGroups(c, true)));
    }
    ruvia::Task<ruvia::HttpResponse> groupShares(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device-group:share");
        co_return c.json(service::common::ok<DeviceSharesResponse>(
            c, co_await deviceShareService().listGroup(c, id(c))));
    }
    ruvia::Task<ruvia::HttpResponse> groupShareTargets(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device-group:share");
        co_return c.json(service::common::ok<DeviceShareTargetsResponse>(
            c, co_await deviceShareService().groupTargets(c, id(c))));
    }
    ruvia::Task<ruvia::HttpResponse> replaceGroupShares(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device-group:share");
        co_await deviceShareService().replaceGroup(c, id(c),
                                                   c.req().valid<ReplaceDeviceSharesBody>());
        co_return c.json(service::common::operation(c, "设备分组分享已更新"));
    }
    ruvia::Task<ruvia::HttpResponse> groupDetail(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device-group:query");
        co_return c.json(service::common::ok<DeviceGroupDetailResponse>(
            c, co_await deviceService().groupDetail(c, id(c))));
    }
    ruvia::Task<ruvia::HttpResponse> groupCreate(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device-group:add");
        co_await deviceService().createGroup(c, c.req().valid<SaveDeviceGroupBody>());
        co_return c.json(service::common::operation(c, "创建成功"));
    }
    ruvia::Task<ruvia::HttpResponse> groupUpdate(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device-group:edit");
        co_await deviceService().updateGroup(c, id(c), c.req().valid<SaveDeviceGroupBody>());
        co_return c.json(service::common::operation(c, "更新成功"));
    }
    ruvia::Task<ruvia::HttpResponse> groupRemove(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device-group:delete");
        co_await deviceService().removeGroup(c, id(c));
        co_return c.json(service::common::operation(c, "删除成功"));
    }
};

} // namespace service::device
