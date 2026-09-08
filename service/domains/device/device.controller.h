#pragma once

#include <chrono>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/features/live/query.h"
#include "service/features/telemetry/latest.h"
#include "service/middleware/auth.h"
#include "service/middleware/permission.h"
#include "service/features/command/service.h"
#include "service/domains/device/device.schema.h"
#include "service/domains/device/device.service.h"

namespace service::device {

class DeviceController final : public ruvia::Controller<DeviceController> {
  public:
    RUVIA_CONTROLLER_GROUP("/v1/device", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    // 设备
    RUVIA_GET_SSE("/options", options);
    RUVIA_GET_SSE("/realtime", realtime);
    RUVIA_GET_SSE("/commands/:id", commandStatus, DeviceIdParamsValidator);
    RUVIA_GET_SSE("/commands", commandStatuses);
    // 设备分组（统一收编到 /v1/device/groups）
    RUVIA_GET_SSE("/groups/tree-count", groupTreeCount);
    RUVIA_GET_SSE("/groups/tree", groupTree);
    RUVIA_GET_SSE("/groups/:id/shares", groupShares, DeviceIdParamsValidator);
    RUVIA_GET_SSE("/groups/:id/share-targets", groupShareTargets, DeviceIdParamsValidator);
    RUVIA_PUT("/groups/:id/shares", replaceGroupShares, DeviceIdParamsValidator,
              ReplaceDeviceSharesValidator);
    RUVIA_GET_SSE("/groups/:id", groupDetail, DeviceIdParamsValidator);
    RUVIA_POST("/groups", groupCreate, CreateDeviceGroupValidator);
    RUVIA_PUT("/groups/:id", groupUpdate, DeviceIdParamsValidator, UpdateDeviceGroupValidator);
    RUVIA_DELETE("/groups/:id", groupRemove, DeviceIdParamsValidator);
    RUVIA_GET_SSE("/:id/history", history, DeviceIdParamsValidator);
    RUVIA_GET_SSE("/:id/shares", shares, DeviceIdParamsValidator);
    RUVIA_GET_SSE("/:id/share-targets", shareTargets, DeviceIdParamsValidator);
    RUVIA_PUT("/:id/shares", replaceShares, DeviceIdParamsValidator, ReplaceDeviceSharesValidator);
    RUVIA_POST("/:id/commands", command, DeviceIdParamsValidator, DeviceCommandValidator);
    // 设备（带参数的通配路由放在静态路由之后）
    RUVIA_GET_SSE("/:id", detail, DeviceIdParamsValidator);
    RUVIA_GET_SSE("/", list);
    RUVIA_POST("/", create, CreateDeviceValidator);
    RUVIA_PUT("/:id", update, DeviceIdParamsValidator, UpdateDeviceValidator);
    RUVIA_DELETE("/:id", remove, DeviceIdParamsValidator);
    RUVIA_ROUTES_END

  private:
    static std::string id(ruvia::Context& c) {
        return std::string(c.req().validated<DeviceIdParams>().get<"id">()->view());
    }

    // ---- 设备 ----
    ruvia::Task<void> list(ruvia::Context& c) {
        co_await service::live::serve(c, "device", [this, &c]() { return listSnapshot(c); });
    }

    ruvia::Task<std::string> listSnapshot(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device:query");
        co_return service::live::json(
            service::common::ok<DevicePageResponse>(c, co_await deviceService().list(c)));
    }
    ruvia::Task<void> realtime(ruvia::Context& c) {
        co_await service::live::serve(c, "device", [this, &c]() { return realtimeSnapshot(c); });
    }

    ruvia::Task<std::string> realtimeSnapshot(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device:query");
        co_return service::live::json(
            service::common::ok<DeviceRealtimeResponse>(c, co_await deviceService().realtime(c)));
    }

    ruvia::Task<void> options(ruvia::Context& c) {
        co_await service::live::serve(c, "device", [this, &c]() { return optionsSnapshot(c); });
    }

    ruvia::Task<std::string> optionsSnapshot(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device:query");
        co_return service::live::json(
            service::common::ok<DeviceOptionsResponse>(c, co_await deviceService().options(c)));
    }
    ruvia::Task<void> detail(ruvia::Context& c) {
        co_await service::live::serve(c, "device", [this, &c]() { return detailSnapshot(c); });
    }

    ruvia::Task<std::string> detailSnapshot(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device:query");
        co_return service::live::json(service::common::ok<DeviceDetailResponse>(
            c, co_await deviceService().detail(c, id(c))));
    }
    ruvia::Task<void> history(ruvia::Context& c) {
        co_await service::live::serve(c, "device", [this, &c]() { return historySnapshot(c); });
    }

    ruvia::Task<std::string> historySnapshot(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device:query");
        co_return service::live::data(c, co_await deviceService().history(c, id(c)));
    }
    ruvia::Task<ruvia::HttpResponse> create(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device:add");
        co_await deviceService().create(c, c.req().validated<SaveDeviceBody>());
        co_return c.json(service::common::operation(c, "创建成功"));
    }
    ruvia::Task<ruvia::HttpResponse> update(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device:edit");
        co_await deviceService().update(c, id(c), c.req().validated<SaveDeviceBody>());
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
            c, co_await service::command::commandService().create(
                   c, id(c), c.req().validated<DeviceCommandBody>())));
    }

    ruvia::Task<void> commandStatus(ruvia::Context& c) {
        co_await service::live::serve(c, "device", [this, &c]() { return commandStatusSnapshot(c); });
    }

    ruvia::Task<std::string> commandStatusSnapshot(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device:command");
        co_return service::live::json(service::common::ok<DeviceCommandStatusResponse>(
            c, co_await service::command::commandService().status(c, id(c))));
    }

    ruvia::Task<void> commandStatuses(ruvia::Context& c) {
        co_await service::live::serve(c, "device", [this, &c]() { return commandStatusesSnapshot(c); });
    }
    ruvia::Task<std::string> commandStatusesSnapshot(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device:command");
        co_return service::live::json(service::common::ok<DeviceCommandStatusesResponse>(
            c, co_await service::command::commandService().statuses(c)));
    }

    ruvia::Task<void> shares(ruvia::Context& c) {
        co_await service::live::serve(c, "device", [this, &c]() { return sharesSnapshot(c); });
    }

    ruvia::Task<std::string> sharesSnapshot(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device:share");
        co_return service::live::json(service::common::ok<DeviceSharesResponse>(
            c, co_await deviceShareService().list(c, id(c))));
    }

    ruvia::Task<void> shareTargets(ruvia::Context& c) {
        co_await service::live::serve(c, "device", [this, &c]() { return shareTargetsSnapshot(c); });
    }

    ruvia::Task<std::string> shareTargetsSnapshot(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device:share");
        co_return service::live::json(service::common::ok<DeviceShareTargetsResponse>(
            c, co_await deviceShareService().targets(c, id(c))));
    }

    ruvia::Task<ruvia::HttpResponse> replaceShares(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device:share");
        co_await deviceShareService().replace(c, id(c), c.req().validated<ReplaceDeviceSharesBody>());
        co_return c.json(service::common::operation(c, "设备分享已更新"));
    }

    // ---- 设备分组 ----
    ruvia::Task<void> groupTree(ruvia::Context& c) {
        co_await service::live::serve(c, "device", [this, &c]() { return groupTreeSnapshot(c); });
    }

    ruvia::Task<std::string> groupTreeSnapshot(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device-group:query");
        co_return service::live::json(service::common::ok<DeviceGroupListResponse>(
            c, co_await deviceService().listGroups(c, false)));
    }
    ruvia::Task<void> groupTreeCount(ruvia::Context& c) {
        co_await service::live::serve(c, "device", [this, &c]() { return groupTreeCountSnapshot(c); });
    }

    ruvia::Task<std::string> groupTreeCountSnapshot(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device-group:query");
        co_return service::live::json(service::common::ok<DeviceGroupListResponse>(
            c, co_await deviceService().listGroups(c, true)));
    }
    ruvia::Task<void> groupShares(ruvia::Context& c) {
        co_await service::live::serve(c, "device", [this, &c]() { return groupSharesSnapshot(c); });
    }

    ruvia::Task<std::string> groupSharesSnapshot(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device-group:share");
        co_return service::live::json(service::common::ok<DeviceSharesResponse>(
            c, co_await deviceShareService().listGroup(c, id(c))));
    }
    ruvia::Task<void> groupShareTargets(ruvia::Context& c) {
        co_await service::live::serve(c, "device", [this, &c]() { return groupShareTargetsSnapshot(c); });
    }

    ruvia::Task<std::string> groupShareTargetsSnapshot(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device-group:share");
        co_return service::live::json(service::common::ok<DeviceShareTargetsResponse>(
            c, co_await deviceShareService().groupTargets(c, id(c))));
    }
    ruvia::Task<ruvia::HttpResponse> replaceGroupShares(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device-group:share");
        co_await deviceShareService().replaceGroup(c, id(c),
                                                   c.req().validated<ReplaceDeviceSharesBody>());
        co_return c.json(service::common::operation(c, "设备分组分享已更新"));
    }
    ruvia::Task<void> groupDetail(ruvia::Context& c) {
        co_await service::live::serve(c, "device", [this, &c]() { return groupDetailSnapshot(c); });
    }

    ruvia::Task<std::string> groupDetailSnapshot(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device-group:query");
        co_return service::live::json(service::common::ok<DeviceGroupDetailResponse>(
            c, co_await deviceService().groupDetail(c, id(c))));
    }
    ruvia::Task<ruvia::HttpResponse> groupCreate(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device-group:add");
        co_await deviceService().createGroup(c, c.req().validated<SaveDeviceGroupBody>());
        co_return c.json(service::common::operation(c, "创建成功"));
    }
    ruvia::Task<ruvia::HttpResponse> groupUpdate(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device-group:edit");
        co_await deviceService().updateGroup(c, id(c), c.req().validated<SaveDeviceGroupBody>());
        co_return c.json(service::common::operation(c, "更新成功"));
    }
    ruvia::Task<ruvia::HttpResponse> groupRemove(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device-group:delete");
        co_await deviceService().removeGroup(c, id(c));
        co_return c.json(service::common::operation(c, "删除成功"));
    }
};

} // namespace service::device
