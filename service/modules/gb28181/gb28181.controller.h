#pragma once

#include <string>

#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/middleware/live.h"
#include "service/modules/gb28181/gb28181.schema.h"
#include "service/modules/gb28181/gb28181.service.h"
#include "service/middleware/auth.h"
#include "service/middleware/permission.h"

namespace service::gb28181 {

class Gb28181Controller final : public ruvia::Controller<Gb28181Controller> {
public:
  RUVIA_CONTROLLER_GROUP("/v1/gb28181", service::middleware::AuthMiddleware)
  RUVIA_ROUTES_BEGIN
  RUVIA_GET_SSE("/health", health);
  RUVIA_GET_SSE("/config/sip", sipConfig);
  RUVIA_GET_SSE("/devices", devices);
  RUVIA_GET_SSE("/streams", streams);
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
  RUVIA_POST("/previews/:sessionId/heartbeat", heartbeatPreview);
  RUVIA_POST("/previews/:sessionId/stop", stopPreview);
  RUVIA_GET_SSE("/devices/:deviceId", device);
  RUVIA_GET_SSE("/streams/:streamId", stream);
  RUVIA_GET_SSE("/streams/:streamId/recording", recording);
  RUVIA_POST("/streams/:streamId/recording/start", startRecording);
  RUVIA_POST("/streams/:streamId/recording/stop", stopRecording);
  RUVIA_ROUTES_END

private:
  ruvia::Task<void> health(ruvia::Context& c) {
        co_await service::live::serve(c, "gb28181", [this, &c]() { return healthSnapshot(c); });
    }

  ruvia::Task<std::string> healthSnapshot(ruvia::Context& c) {
    co_await service::middleware::requirePermission(c, "iot:gb28181:query");
    co_return service::live::json(
        service::common::ok<GbHealthResponse>(
            c, co_await gb28181Service().health(c)));
  }

  ruvia::Task<void> sipConfig(ruvia::Context& c) {
        co_await service::live::serve(c, "gb28181", [this, &c]() { return sipConfigSnapshot(c); });
    }

  ruvia::Task<std::string> sipConfigSnapshot(ruvia::Context& c) {
    co_await service::middleware::requirePermission(c, "iot:gb28181:query");
    co_return service::live::json(service::common::ok<GbSipConfigResponse>(
        c, co_await gb28181Service().sipConfig(c)));
  }

  ruvia::Task<void> devices(ruvia::Context& c) {
        co_await service::live::serve(c, "gb28181", [this, &c]() { return devicesSnapshot(c); });
    }

  ruvia::Task<std::string> devicesSnapshot(ruvia::Context& c) {
    co_await service::middleware::requirePermission(c, "iot:gb28181:query");
    co_return service::live::json(service::common::ok<GbDeviceListResponse>(
        c, co_await gb28181Service().devices(c)));
  }

  ruvia::Task<void> device(ruvia::Context& c) {
        co_await service::live::serve(c, "gb28181", [this, &c]() { return deviceSnapshot(c); });
    }

  ruvia::Task<std::string> deviceSnapshot(ruvia::Context& c) {
    co_await service::middleware::requirePermission(c, "iot:gb28181:query");
    co_return service::live::json(service::common::ok<GbDeviceResponse>(
        c, co_await gb28181Service().device(
               c, requiredRoute(c, "deviceId", "设备编号不能为空"))));
  }

  ruvia::Task<void> streams(ruvia::Context& c) {
        co_await service::live::serve(c, "gb28181", [this, &c]() { return streamsSnapshot(c); });
    }

  ruvia::Task<std::string> streamsSnapshot(ruvia::Context& c) {
    co_await service::middleware::requirePermission(c, "iot:gb28181:query");
    co_return service::live::json(service::common::ok<GbStreamListResponse>(
        c, co_await gb28181Service().streams(c)));
  }

  ruvia::Task<void> stream(ruvia::Context& c) {
        co_await service::live::serve(c, "gb28181", [this, &c]() { return streamSnapshot(c); });
    }

  ruvia::Task<std::string> streamSnapshot(ruvia::Context& c) {
    co_await service::middleware::requirePermission(c, "iot:gb28181:query");
    co_return service::live::json(service::common::ok<GbStreamResponse>(
        c, co_await gb28181Service().stream(
               c, requiredRoute(c, "streamId", "流编号不能为空"))));
  }

  ruvia::Task<ruvia::HttpResponse> catalog(ruvia::Context &c) {
    co_await service::middleware::requirePermission(c, "iot:gb28181:control");
    const auto deviceId = requiredRoute(c, "deviceId", "设备编号不能为空");
    co_await gb28181Service().queryCatalog(c, deviceId);
    GbActionDto data(c);
    data.set<"sent">(true).set<"deviceId">(deviceId);
    co_return c.json(service::common::ok<GbActionResponse>(c, std::move(data)));
  }

  ruvia::Task<ruvia::HttpResponse> renameDevice(ruvia::Context &c) {
    co_await service::middleware::requirePermission(c, "iot:gb28181:control");
    const auto deviceId = requiredRoute(c, "deviceId", "设备编号不能为空");
    const auto name = requiredName(c.req().validated<GbNameBody>());
    co_await gb28181Service().renameDevice(c, deviceId, name);
    co_return c.json(service::common::operation(c, "摄像头名称已更新"));
  }

  ruvia::Task<ruvia::HttpResponse> renameChannel(ruvia::Context &c) {
    co_await service::middleware::requirePermission(c, "iot:gb28181:control");
    const auto deviceId = requiredRoute(c, "deviceId", "设备编号不能为空");
    const auto channelId = requiredRoute(c, "channelId", "通道编号不能为空");
    const auto name = requiredName(c.req().validated<GbNameBody>());
    co_await gb28181Service().renameChannel(c, deviceId, channelId, name);
    co_return c.json(service::common::operation(c, "通道名称已更新"));
  }

  ruvia::Task<ruvia::HttpResponse> mapDevice(ruvia::Context &c) {
    co_await service::middleware::requirePermission(c, "iot:gb28181:control");
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
    const auto deviceId = requiredRoute(c, "deviceId", "设备编号不能为空");
    co_await gb28181Service().mapDevice(c, deviceId, {});
    GbActionDto data(c);
    data.set<"deviceId">(deviceId).set<"mappedDeviceId">("");
    co_return c.json(service::common::ok<GbActionResponse>(c, std::move(data)));
  }

  ruvia::Task<ruvia::HttpResponse> startPreview(ruvia::Context &c) {
    co_await service::middleware::requirePermission(c, "iot:gb28181:control");
    const auto deviceId = requiredRoute(c, "deviceId", "设备编号不能为空");
    const auto channelId = requiredRoute(c, "channelId", "通道编号不能为空");
    co_return c.json(service::common::ok<GbPreviewStartResponse>(
        c, co_await gb28181Service().startPreview(c, deviceId, channelId)));
  }

  ruvia::Task<ruvia::HttpResponse> stopPreview(ruvia::Context &c) {
    co_await service::middleware::requirePermission(c, "iot:gb28181:control");
    const auto sessionId = requiredRoute(c, "sessionId", "会话编号不能为空");
    co_return c.json(service::common::ok<GbPreviewStopResponse>(
        c, co_await gb28181Service().stopPreview(c, sessionId)));
  }

  ruvia::Task<ruvia::HttpResponse> heartbeatPreview(ruvia::Context &c) {
    co_await service::middleware::requirePermission(c, "iot:gb28181:control");
    const auto sessionId = requiredRoute(c, "sessionId", "会话编号不能为空");
    co_await gb28181Service().renewPreview(c, sessionId);
    GbActionDto data(c);
    data.set<"sent">(true).set<"action">("heartbeat");
    co_return c.json(service::common::ok<GbActionResponse>(c, std::move(data)));
  }

  ruvia::Task<ruvia::HttpResponse> ptz(ruvia::Context &c) {
    co_await service::middleware::requirePermission(c, "iot:gb28181:control");
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
    const auto deviceId = requiredRoute(c, "deviceId", "设备编号不能为空");
    const auto channelId = requiredRoute(c, "channelId", "通道编号不能为空");
    const auto startTime =
        requiredUtcQuery(c, "start_time", "开始时间不能为空");
    const auto endTime = requiredUtcQuery(c, "end_time", "结束时间不能为空");
    co_return c.json(service::common::ok<GbPreviewStartResponse>(
        c, co_await gb28181Service().startPlayback(c, deviceId, channelId,
                                                   startTime, endTime)));
  }

  ruvia::Task<void> recording(ruvia::Context& c) {
        co_await service::live::serve(c, "gb28181", [this, &c]() { return recordingSnapshot(c); });
    }

  ruvia::Task<std::string> recordingSnapshot(ruvia::Context& c) {
    co_await service::middleware::requirePermission(c, "iot:gb28181:record");
    const auto streamId = requiredRoute(c, "streamId", "流编号不能为空");
    GbActionDto data(c);
    data.set<"recording">(co_await gb28181Service().recording(c, streamId));
    co_return service::live::json(service::common::ok<GbActionResponse>(c, std::move(data)));
  }

  ruvia::Task<ruvia::HttpResponse> startRecording(ruvia::Context &c) {
    co_await service::middleware::requirePermission(c, "iot:gb28181:record");
    const auto streamId = requiredRoute(c, "streamId", "流编号不能为空");
    co_await gb28181Service().startRecording(c, streamId);
    GbActionDto data(c);
    data.set<"recording">(true);
    co_return c.json(service::common::ok<GbActionResponse>(c, std::move(data)));
  }

  ruvia::Task<ruvia::HttpResponse> stopRecording(ruvia::Context &c) {
    co_await service::middleware::requirePermission(c, "iot:gb28181:record");
    const auto streamId = requiredRoute(c, "streamId", "流编号不能为空");
    co_await gb28181Service().stopRecording(c, streamId);
    GbActionDto data(c);
    data.set<"recording">(false);
    co_return c.json(service::common::ok<GbActionResponse>(c, std::move(data)));
  }
};

} // namespace service::gb28181
