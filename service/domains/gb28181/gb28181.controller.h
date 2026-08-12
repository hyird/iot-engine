#pragma once

#include <string>

#include <ruvia/web/Controller.h>

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
  RUVIA_GET("/streams", streams);
  RUVIA_PUT("/devices/:deviceId/name", renameDevice, GbNameValidator);
  RUVIA_PUT("/devices/:deviceId/channels/:channelId/name", renameChannel,
            GbNameValidator);
  RUVIA_POST("/devices/:deviceId/catalog/query", catalog);
  RUVIA_POST("/devices/:deviceId/mapping", mapDevice);
  RUVIA_DELETE("/devices/:deviceId/mapping", unmapDevice);
  RUVIA_POST("/devices/:deviceId/channels/:channelId/preview/start",
             startPreview);
  RUVIA_POST("/devices/:deviceId/channels/:channelId/ptz/position/set",
             ptzPosition);
  RUVIA_POST("/devices/:deviceId/channels/:channelId/ptz/:action", ptz);
  RUVIA_POST("/devices/:deviceId/channels/:channelId/records/query", records);
  RUVIA_POST("/devices/:deviceId/channels/:channelId/playback/start",
             startPlayback);
  RUVIA_POST("/previews/:sessionId/stop", stopPreview);
  RUVIA_GET("/devices/:deviceId", device);
  RUVIA_GET("/streams/:streamId", stream);
  RUVIA_GET("/streams/:streamId/recording", recording);
  RUVIA_POST("/streams/:streamId/recording/start", startRecording);
  RUVIA_POST("/streams/:streamId/recording/stop", stopRecording);
  RUVIA_ROUTES_END

private:
  ruvia::Task<ruvia::HttpResponse> health(ruvia::Context &c) {
    co_await service::middleware::requirePermission(c, "iot:gb28181:query");
    co_return c.json(
        service::common::ok<GbHealthResponse>(c, gb28181Service().health(c)));
  }

  ruvia::Task<ruvia::HttpResponse> sipConfig(ruvia::Context &c) {
    co_await service::middleware::requirePermission(c, "iot:gb28181:query");
    requireEnabled();
    co_return c.json(service::common::ok<GbSipConfigResponse>(
        c, gb28181Service().sipConfig(c)));
  }

  ruvia::Task<ruvia::HttpResponse> devices(ruvia::Context &c) {
    co_await service::middleware::requirePermission(c, "iot:gb28181:query");
    requireEnabled();
    co_return c.json(service::common::ok<GbDeviceListResponse>(
        c, co_await gb28181Service().devices(c)));
  }

  ruvia::Task<ruvia::HttpResponse> device(ruvia::Context &c) {
    co_await service::middleware::requirePermission(c, "iot:gb28181:query");
    requireEnabled();
    co_return c.json(service::common::ok<GbDeviceResponse>(
        c, co_await gb28181Service().device(
               c, requiredRoute(c, "deviceId", "设备编号不能为空"))));
  }

  ruvia::Task<ruvia::HttpResponse> streams(ruvia::Context &c) {
    co_await service::middleware::requirePermission(c, "iot:gb28181:query");
    requireEnabled();
    co_return c.json(service::common::ok<GbStreamListResponse>(
        c, co_await gb28181Service().streams(c)));
  }

  ruvia::Task<ruvia::HttpResponse> stream(ruvia::Context &c) {
    co_await service::middleware::requirePermission(c, "iot:gb28181:query");
    requireEnabled();
    co_return c.json(service::common::ok<GbStreamResponse>(
        c, co_await gb28181Service().stream(
               c, requiredRoute(c, "streamId", "流编号不能为空"))));
  }

  ruvia::Task<ruvia::HttpResponse> catalog(ruvia::Context &c) {
    co_await service::middleware::requirePermission(c, "iot:gb28181:control");
    requireEnabled();
    const auto deviceId = requiredRoute(c, "deviceId", "设备编号不能为空");
    co_await gb28181Service().queryCatalog(c, deviceId);
    GbActionDto data(c);
    data.set<"sent">(true).set<"deviceId">(deviceId);
    co_return c.json(service::common::ok<GbActionResponse>(c, std::move(data)));
  }

  ruvia::Task<ruvia::HttpResponse> renameDevice(ruvia::Context &c) {
    co_await service::middleware::requirePermission(c, "iot:gb28181:control");
    requireEnabled();
    const auto deviceId = requiredRoute(c, "deviceId", "设备编号不能为空");
    const auto name = requiredName(c.req().validated<GbNameBody>());
    co_await gb28181Service().renameDevice(c, deviceId, name);
    co_return c.json(service::common::operation(c, "摄像头名称已更新"));
  }

  ruvia::Task<ruvia::HttpResponse> renameChannel(ruvia::Context &c) {
    co_await service::middleware::requirePermission(c, "iot:gb28181:control");
    requireEnabled();
    const auto deviceId = requiredRoute(c, "deviceId", "设备编号不能为空");
    const auto channelId = requiredRoute(c, "channelId", "通道编号不能为空");
    const auto name = requiredName(c.req().validated<GbNameBody>());
    co_await gb28181Service().renameChannel(c, deviceId, channelId, name);
    co_return c.json(service::common::operation(c, "通道名称已更新"));
  }

  ruvia::Task<ruvia::HttpResponse> mapDevice(ruvia::Context &c) {
    co_await service::middleware::requirePermission(c, "iot:gb28181:control");
    requireEnabled();
    const auto deviceId = requiredRoute(c, "deviceId", "设备编号不能为空");
    const auto mappedDeviceId =
        requiredQuery(c, "mapped_device_id", "映射设备编号不能为空");
    co_await gb28181Service().mapDevice(c, deviceId, mappedDeviceId);
    GbActionDto data(c);
    data.set<"deviceId">(deviceId).set<"mappedDeviceId">(mappedDeviceId);
    co_return c.json(service::common::ok<GbActionResponse>(c, std::move(data)));
  }

  ruvia::Task<ruvia::HttpResponse> unmapDevice(ruvia::Context &c) {
    co_await service::middleware::requirePermission(c, "iot:gb28181:control");
    requireEnabled();
    const auto deviceId = requiredRoute(c, "deviceId", "设备编号不能为空");
    co_await gb28181Service().mapDevice(c, deviceId, {});
    GbActionDto data(c);
    data.set<"deviceId">(deviceId).set<"mappedDeviceId">("");
    co_return c.json(service::common::ok<GbActionResponse>(c, std::move(data)));
  }

  ruvia::Task<ruvia::HttpResponse> startPreview(ruvia::Context &c) {
    co_await service::middleware::requirePermission(c, "iot:gb28181:control");
    requireEnabled();
    const auto deviceId = requiredRoute(c, "deviceId", "设备编号不能为空");
    const auto channelId = requiredRoute(c, "channelId", "通道编号不能为空");
    auto result = co_await runtime().startPreview(c, deviceId, channelId);
    if (!result)
      service::common::fail(10003, "设备或通道不可用", 404);
    co_return c.json(service::common::ok<GbPreviewStartResponse>(
        c, Gb28181Service::previewStart(c, *result)));
  }

  ruvia::Task<ruvia::HttpResponse> stopPreview(ruvia::Context &c) {
    co_await service::middleware::requirePermission(c, "iot:gb28181:control");
    requireEnabled();
    const auto sessionId = requiredRoute(c, "sessionId", "会话编号不能为空");
    auto result = co_await runtime().stopPreview(c, sessionId);
    if (!result)
      service::common::fail(10003, "预览会话不存在", 404);
    co_return c.json(service::common::ok<GbPreviewStopResponse>(
        c, Gb28181Service::previewStop(c, *result)));
  }

  ruvia::Task<ruvia::HttpResponse> ptz(ruvia::Context &c) {
    co_await service::middleware::requirePermission(c, "iot:gb28181:control");
    requireEnabled();
    const auto deviceId = requiredRoute(c, "deviceId", "设备编号不能为空");
    const auto channelId = requiredRoute(c, "channelId", "通道编号不能为空");
    const auto action = requiredRoute(c, "action", "云台动作不能为空");
    requirePtzAction(action);
    const auto speed = ptzSpeed(c);
    co_await gb28181Service().ptz(c, deviceId, channelId, action, speed);
    GbActionDto data(c);
    data.set<"sent">(true)
        .set<"deviceId">(deviceId)
        .set<"channelId">(channelId)
        .set<"action">(action)
        .set<"speed">(speed);
    co_return c.json(service::common::ok<GbActionResponse>(c, std::move(data)));
  }

  ruvia::Task<ruvia::HttpResponse> ptzPosition(ruvia::Context &c) {
    co_await service::middleware::requirePermission(c, "iot:gb28181:control");
    requireEnabled();
    const auto deviceId = requiredRoute(c, "deviceId", "设备编号不能为空");
    const auto channelId = requiredRoute(c, "channelId", "通道编号不能为空");
    const auto pan = finiteQuery(c, "pan", 0.0, 360.0);
    const auto tilt = finiteQuery(c, "tilt", -30.0, 90.0);
    const auto zoom = finiteQuery(c, "zoom", 1.0, 1000.0);
    co_await gb28181Service().ptzPosition(c, deviceId, channelId, pan, tilt,
                                          zoom);
    GbActionDto data(c);
    data.set<"sent">(true).set<"pan">(pan).set<"tilt">(tilt).set<"zoom">(zoom);
    co_return c.json(service::common::ok<GbActionResponse>(c, std::move(data)));
  }

  ruvia::Task<ruvia::HttpResponse> records(ruvia::Context &c) {
    co_await service::middleware::requirePermission(c, "iot:gb28181:record");
    requireEnabled();
    const auto deviceId = requiredRoute(c, "deviceId", "设备编号不能为空");
    const auto channelId = requiredRoute(c, "channelId", "通道编号不能为空");
    const auto startTime =
        requiredUtcQuery(c, "start_time", "开始时间不能为空");
    const auto endTime = requiredUtcQuery(c, "end_time", "结束时间不能为空");
    co_await gb28181Service().queryRecords(c, deviceId, channelId, startTime,
                                           endTime);
    GbActionDto data(c);
    data.set<"sent">(true).set<"deviceId">(deviceId).set<"channelId">(
        channelId);
    co_return c.json(service::common::ok<GbActionResponse>(c, std::move(data)));
  }

  ruvia::Task<ruvia::HttpResponse> startPlayback(ruvia::Context &c) {
    co_await service::middleware::requirePermission(c, "iot:gb28181:record");
    requireEnabled();
    const auto deviceId = requiredRoute(c, "deviceId", "设备编号不能为空");
    const auto channelId = requiredRoute(c, "channelId", "通道编号不能为空");
    const auto startTime =
        requiredUtcQuery(c, "start_time", "开始时间不能为空");
    const auto endTime = requiredUtcQuery(c, "end_time", "结束时间不能为空");
    auto result = co_await runtime().startPlayback(c, deviceId, channelId,
                                                   startTime, endTime);
    if (!result)
      service::common::fail(10003, "设备或通道不可用", 404);
    co_return c.json(service::common::ok<GbPreviewStartResponse>(
        c, Gb28181Service::previewStart(c, *result)));
  }

  ruvia::Task<ruvia::HttpResponse> recording(ruvia::Context &c) {
    co_await service::middleware::requirePermission(c, "iot:gb28181:record");
    requireEnabled();
    const auto streamId = requiredRoute(c, "streamId", "流编号不能为空");
    GbActionDto data(c);
    data.set<"recording">(co_await gb28181Service().recording(c, streamId));
    co_return c.json(service::common::ok<GbActionResponse>(c, std::move(data)));
  }

  ruvia::Task<ruvia::HttpResponse> startRecording(ruvia::Context &c) {
    co_await service::middleware::requirePermission(c, "iot:gb28181:record");
    requireEnabled();
    const auto streamId = requiredRoute(c, "streamId", "流编号不能为空");
    co_await gb28181Service().startRecording(c, streamId);
    GbActionDto data(c);
    data.set<"recording">(true);
    co_return c.json(service::common::ok<GbActionResponse>(c, std::move(data)));
  }

  ruvia::Task<ruvia::HttpResponse> stopRecording(ruvia::Context &c) {
    co_await service::middleware::requirePermission(c, "iot:gb28181:record");
    requireEnabled();
    const auto streamId = requiredRoute(c, "streamId", "流编号不能为空");
    co_await gb28181Service().stopRecording(c, streamId);
    GbActionDto data(c);
    data.set<"recording">(false);
    co_return c.json(service::common::ok<GbActionResponse>(c, std::move(data)));
  }

  static void requireEnabled() {
    if (!runtime().enabled())
      service::common::fail(10004, "GB28181 功能未启用", 404);
  }
};

} // namespace service::gb28181
