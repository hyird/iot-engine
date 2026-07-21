#pragma once

#include <string>
#include <string_view>

#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/middleware/auth.h"
#include "service/middleware/permission.h"
#include "service/modules/northbridge/device/device.schema.h"
#include "service/modules/northbridge/device/device.service.h"

namespace service::device {

class DeviceController final : public ruvia::Controller<DeviceController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api/device", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    // 设备
    RUVIA_GET("/options", options);
    RUVIA_GET("/realtime", realtime);
    // 设备分组（统一收编到 /api/device/groups）
    RUVIA_GET("/groups/tree-count", groupTreeCount);
    RUVIA_GET("/groups/tree", groupTree);
    RUVIA_GET("/groups/:id", groupDetail, DeviceIdParamsValidator);
    RUVIA_POST("/groups", groupCreate, CreateDeviceGroupValidator);
    RUVIA_PUT("/groups/:id", groupUpdate, DeviceIdParamsValidator, UpdateDeviceGroupValidator);
    RUVIA_DELETE("/groups/:id", groupRemove, DeviceIdParamsValidator);
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
        co_return c.json(
            service::common::ok<DeviceDetailResponse>(c, co_await deviceService().detail(c, id(c))));
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
