#pragma once

#include <chrono>
#include <string>

#include <ruvia/web/Controller.h>
#include <ruvia/web/ConnInfo.h>

#include "service/common/http.h"
#include "service/domains/gb28181/gb28181.schema.h"
#include "service/domains/gb28181/gb28181.service.h"
#include "service/middleware/auth.h"
#include "service/middleware/permission.h"

namespace service::gb28181 {

class Gb28181Controller final : public ruvia::Controller<Gb28181Controller> {
  public:
    RUVIA_CONTROLLER_GROUP("/v1/gb28181", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/health", health);
    RUVIA_GET("/config/sip", sipConfig);
    RUVIA_GET("/devices", devices);
    RUVIA_POST("/devices/mock-register", mockRegister);
    RUVIA_GET("/streams", streams);
    RUVIA_POST("/devices/:deviceId/catalog/query", catalog);
    RUVIA_POST("/devices/:deviceId/channels/:channelId/preview/start", startPreview);
    RUVIA_POST("/devices/:deviceId/channels/:channelId/ptz/position/set", ptzPosition);
    RUVIA_POST("/devices/:deviceId/channels/:channelId/ptz/:action", ptz);
    RUVIA_POST("/devices/:deviceId/channels/:channelId/records/query", records);
    RUVIA_POST("/devices/:deviceId/channels/:channelId/playback/start", startPlayback);
    RUVIA_POST("/previews/:sessionId/stop", stopPreview);
    RUVIA_GET("/devices/:deviceId", device);
    RUVIA_GET("/streams/:streamId", stream);
    RUVIA_ROUTES_END

  private:
    ruvia::Task<ruvia::HttpResponse> health(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:gb28181:query");
        co_return c.json(
            service::common::ok<GbHealthResponse>(c, gb28181Service().health(c)));
    }

    ruvia::Task<ruvia::HttpResponse> sipConfig(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:gb28181:query");
        co_return c.json(
            service::common::ok<GbSipConfigResponse>(c, gb28181Service().sipConfig(c)));
    }

    ruvia::Task<ruvia::HttpResponse> devices(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:gb28181:query");
        co_return c.json(
            service::common::ok<GbDeviceListResponse>(c, gb28181Service().devices(c)));
    }

    ruvia::Task<ruvia::HttpResponse> device(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:gb28181:query");
        co_return c.json(service::common::ok<GbDeviceResponse>(
            c, gb28181Service().device(
                   c, requiredRoute(c, "deviceId", "设备编号不能为空"))));
    }

    ruvia::Task<ruvia::HttpResponse> streams(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:gb28181:query");
        co_return c.json(
            service::common::ok<GbStreamListResponse>(c, gb28181Service().streams(c)));
    }

    ruvia::Task<ruvia::HttpResponse> stream(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:gb28181:query");
        co_return c.json(service::common::ok<GbStreamResponse>(
            c, gb28181Service().stream(
                   c, requiredRoute(c, "streamId", "流编号不能为空"))));
    }

    ruvia::Task<ruvia::HttpResponse> mockRegister(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:gb28181:control");
        auto deviceId =
            std::string(c.req().query("device_id").value_or("34020000001320000001"));
        if (deviceId.empty() || deviceId.size() > 128)
            service::common::fail(10001, "设备编号无效", 400);
        const auto remote = std::string(ruvia::getConnInfo(c).remote().address());
        gb28181Service().mockRegister(deviceId, remote);
        GbActionDto data(c);
        data.registered(true).deviceId(deviceId);
        auto response = service::common::ok<GbActionResponse>(c, std::move(data));
        response.message("模拟注册成功");
        co_return c.json(response);
    }

    ruvia::Task<ruvia::HttpResponse> catalog(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:gb28181:control");
        const auto deviceId = requiredRoute(c, "deviceId", "设备编号不能为空");
        co_await gb28181Service().queryCatalog(c, deviceId);
        GbActionDto data(c);
        data.sent(true).deviceId(deviceId);
        co_return c.json(service::common::ok<GbActionResponse>(c, std::move(data)));
    }

    ruvia::Task<ruvia::HttpResponse> startPreview(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:gb28181:control");
        const auto deviceId = requiredRoute(c, "deviceId", "设备编号不能为空");
        const auto channelId = requiredRoute(c, "channelId", "通道编号不能为空");
        auto result = co_await Runtime::wait(c, runtime().startPreview(deviceId, channelId),
                                             std::chrono::seconds(15));
        if (!result)
            service::common::fail(10003, "设备或通道不可用", 404);
        co_return c.json(service::common::ok<GbPreviewStartResponse>(
            c, Gb28181Service::previewStart(c, *result)));
    }

    ruvia::Task<ruvia::HttpResponse> stopPreview(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:gb28181:control");
        const auto sessionId = requiredRoute(c, "sessionId", "会话编号不能为空");
        auto result = co_await Runtime::wait(c, runtime().stopPreview(sessionId),
                                             std::chrono::seconds(15));
        if (!result)
            service::common::fail(10003, "预览会话不存在", 404);
        co_return c.json(service::common::ok<GbPreviewStopResponse>(
            c, Gb28181Service::previewStop(c, *result)));
    }

    ruvia::Task<ruvia::HttpResponse> ptz(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:gb28181:control");
        const auto deviceId = requiredRoute(c, "deviceId", "设备编号不能为空");
        const auto channelId = requiredRoute(c, "channelId", "通道编号不能为空");
        const auto action = requiredRoute(c, "action", "云台动作不能为空");
        requirePtzAction(action);
        const auto speed = ptzSpeed(c);
        co_await gb28181Service().ptz(c, deviceId, channelId, action, speed);
        GbActionDto data(c);
        data.sent(true)
            .deviceId(deviceId)
            .channelId(channelId)
            .action(action)
            .speed(speed);
        co_return c.json(service::common::ok<GbActionResponse>(c, std::move(data)));
    }

    ruvia::Task<ruvia::HttpResponse> ptzPosition(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:gb28181:control");
        const auto deviceId = requiredRoute(c, "deviceId", "设备编号不能为空");
        const auto channelId = requiredRoute(c, "channelId", "通道编号不能为空");
        const auto pan = finiteQuery(c, "pan", 0.0, 360.0);
        const auto tilt = finiteQuery(c, "tilt", -30.0, 90.0);
        const auto zoom = finiteQuery(c, "zoom", 1.0, 1000.0);
        co_await gb28181Service().ptzPosition(c, deviceId, channelId, pan, tilt, zoom);
        GbActionDto data(c);
        data.sent(true).pan(pan).tilt(tilt).zoom(zoom);
        co_return c.json(service::common::ok<GbActionResponse>(c, std::move(data)));
    }

    ruvia::Task<ruvia::HttpResponse> records(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:gb28181:record");
        const auto deviceId = requiredRoute(c, "deviceId", "设备编号不能为空");
        const auto channelId = requiredRoute(c, "channelId", "通道编号不能为空");
        const auto startTime = requiredQuery(c, "start_time", "开始时间不能为空");
        const auto endTime = requiredQuery(c, "end_time", "结束时间不能为空");
        co_await gb28181Service().queryRecords(c, deviceId, channelId, startTime, endTime);
        GbActionDto data(c);
        data.sent(true).deviceId(deviceId).channelId(channelId);
        co_return c.json(service::common::ok<GbActionResponse>(c, std::move(data)));
    }

    ruvia::Task<ruvia::HttpResponse> startPlayback(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:gb28181:record");
        const auto deviceId = requiredRoute(c, "deviceId", "设备编号不能为空");
        const auto channelId = requiredRoute(c, "channelId", "通道编号不能为空");
        const auto startTime = requiredQuery(c, "start_time", "开始时间不能为空");
        const auto endTime = requiredQuery(c, "end_time", "结束时间不能为空");
        auto result = co_await Runtime::wait(
            c, runtime().startPlayback(deviceId, channelId, startTime, endTime),
            std::chrono::seconds(15));
        if (!result)
            service::common::fail(10003, "设备或通道不可用", 404);
        co_return c.json(service::common::ok<GbPreviewStartResponse>(
            c, Gb28181Service::previewStart(c, *result)));
    }
};

} // namespace service::gb28181
