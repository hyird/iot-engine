#pragma once

#include <chrono>
#include <memory_resource>
#include <string>
#include <string_view>

#include <ruvia/web/Controller.h>
#include <ruvia/web/ConnInfo.h>

#include "service/common/http.h"
#include "service/domains/gb28181/gb28181.schema.h"
#include "service/domains/gb28181/gb28181.service.h"
#include "service/middleware/auth.h"
#include "service/middleware/permission.h"

namespace service::gb28181 {

inline ruvia::HttpResponse gbJson(ruvia::Context& c, std::string_view data,
                                  std::string_view message = "ok") {
    std::pmr::string body(c.allocator<char>());
    body.append("{\"code\":0,\"message\":");
    body.append(jsonQuoted(message));
    body.append(",\"data\":");
    body.append(data);
    body.push_back('}');
    auto response = c.body(std::move(body));
    response.header("Content-Type", "application/json; charset=UTF-8");
    return response;
}

inline ruvia::HttpResponse zlmJson(ruvia::Context& c, std::string_view fields = {}) {
    std::pmr::string body(c.allocator<char>());
    body.append("{\"code\":0");
    if (!fields.empty()) {
        body.push_back(',');
        body.append(fields);
    }
    body.push_back('}');
    auto response = c.body(std::move(body));
    response.header("Content-Type", "application/json; charset=UTF-8");
    return response;
}

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
        co_return gbJson(c, gb28181Service().health());
    }

    ruvia::Task<ruvia::HttpResponse> sipConfig(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:gb28181:query");
        co_return gbJson(c, gb28181Service().sipConfig());
    }

    ruvia::Task<ruvia::HttpResponse> devices(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:gb28181:query");
        co_return gbJson(c, gb28181Service().devices());
    }

    ruvia::Task<ruvia::HttpResponse> device(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:gb28181:query");
        co_return gbJson(
            c, gb28181Service().device(requiredRoute(c, "deviceId", "设备编号不能为空")));
    }

    ruvia::Task<ruvia::HttpResponse> streams(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:gb28181:query");
        co_return gbJson(c, gb28181Service().streams());
    }

    ruvia::Task<ruvia::HttpResponse> stream(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:gb28181:query");
        co_return gbJson(c,
                         gb28181Service().stream(requiredRoute(c, "streamId", "流编号不能为空")));
    }

    ruvia::Task<ruvia::HttpResponse> mockRegister(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:gb28181:control");
        auto deviceId =
            std::string(c.req().query("device_id").value_or("34020000001320000001"));
        if (deviceId.empty() || deviceId.size() > 128)
            service::common::fail(10001, "设备编号无效", 400);
        const auto remote = std::string(ruvia::getConnInfo(c).remote().address());
        gb28181Service().mockRegister(deviceId, remote);
        co_return gbJson(c, "{\"registered\":true,\"device_id\":" + jsonQuoted(deviceId) + "}",
                         "模拟注册成功");
    }

    ruvia::Task<ruvia::HttpResponse> catalog(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:gb28181:control");
        const auto deviceId = requiredRoute(c, "deviceId", "设备编号不能为空");
        gb28181Service().queryCatalog(deviceId);
        co_return gbJson(c, "{\"sent\":true,\"device_id\":" + jsonQuoted(deviceId) + "}");
    }

    ruvia::Task<ruvia::HttpResponse> startPreview(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:gb28181:control");
        const auto deviceId = requiredRoute(c, "deviceId", "设备编号不能为空");
        const auto channelId = requiredRoute(c, "channelId", "通道编号不能为空");
        auto result = co_await Runtime::wait(c, runtime().startPreview(deviceId, channelId),
                                             std::chrono::seconds(15));
        if (!result)
            service::common::fail(10003, "设备或通道不可用", 404);
        co_return gbJson(c, previewJson(*result));
    }

    ruvia::Task<ruvia::HttpResponse> stopPreview(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:gb28181:control");
        const auto sessionId = requiredRoute(c, "sessionId", "会话编号不能为空");
        auto result = co_await Runtime::wait(c, runtime().stopPreview(sessionId),
                                             std::chrono::seconds(15));
        if (!result)
            service::common::fail(10003, "预览会话不存在", 404);
        co_return gbJson(c, previewStopJson(*result));
    }

    ruvia::Task<ruvia::HttpResponse> ptz(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:gb28181:control");
        const auto deviceId = requiredRoute(c, "deviceId", "设备编号不能为空");
        const auto channelId = requiredRoute(c, "channelId", "通道编号不能为空");
        const auto action = requiredRoute(c, "action", "云台动作不能为空");
        requirePtzAction(action);
        const auto speed = ptzSpeed(c);
        gb28181Service().ptz(deviceId, channelId, action, speed);
        co_return gbJson(c, "{\"sent\":true,\"device_id\":" + jsonQuoted(deviceId) +
                                ",\"channel_id\":" + jsonQuoted(channelId) +
                                ",\"action\":" + jsonQuoted(action) +
                                ",\"speed\":" + std::to_string(speed) + "}");
    }

    ruvia::Task<ruvia::HttpResponse> ptzPosition(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:gb28181:control");
        const auto deviceId = requiredRoute(c, "deviceId", "设备编号不能为空");
        const auto channelId = requiredRoute(c, "channelId", "通道编号不能为空");
        const auto pan = finiteQuery(c, "pan", 0.0, 360.0);
        const auto tilt = finiteQuery(c, "tilt", -30.0, 90.0);
        const auto zoom = finiteQuery(c, "zoom", 1.0, 1000.0);
        gb28181Service().ptzPosition(deviceId, channelId, pan, tilt, zoom);
        co_return gbJson(c, "{\"sent\":true,\"pan\":" + jsonNumber(pan) +
                                ",\"tilt\":" + jsonNumber(tilt) +
                                ",\"zoom\":" + jsonNumber(zoom) + "}");
    }

    ruvia::Task<ruvia::HttpResponse> records(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:gb28181:record");
        const auto deviceId = requiredRoute(c, "deviceId", "设备编号不能为空");
        const auto channelId = requiredRoute(c, "channelId", "通道编号不能为空");
        const auto startTime = requiredQuery(c, "start_time", "开始时间不能为空");
        const auto endTime = requiredQuery(c, "end_time", "结束时间不能为空");
        gb28181Service().queryRecords(deviceId, channelId, startTime, endTime);
        co_return gbJson(c, "{\"sent\":true,\"device_id\":" + jsonQuoted(deviceId) +
                                ",\"channel_id\":" + jsonQuoted(channelId) + "}");
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
        co_return gbJson(c, previewJson(*result));
    }
};

class Gb28181HookController final : public ruvia::Controller<Gb28181HookController> {
  public:
    RUVIA_CONTROLLER_GROUP("/v1/gb28181/zlm/hook")
    RUVIA_ROUTES_BEGIN
    RUVIA_POST("/on_stream_changed", streamChanged);
    RUVIA_POST("/on_stream_none_reader", streamNoneReader);
    RUVIA_POST("/on_rtp_server_timeout", rtpServerTimeout);
    RUVIA_POST("/on_send_rtp_stopped", sendRtpStopped);
    RUVIA_ROUTES_END

  private:
    ruvia::Task<ruvia::HttpResponse> streamChanged(ruvia::Context& c) {
        const auto payload = co_await c.req().json();
        gb28181Service().streamChanged(payload);
        co_return zlmJson(c);
    }

    ruvia::Task<ruvia::HttpResponse> streamNoneReader(ruvia::Context& c) {
        const auto payload = co_await c.req().json();
        gb28181Service().authorizeHook(payload);
        const auto app = Gb28181Service::stringField(payload, "app");
        const auto stream = Gb28181Service::stringField(payload, "stream");
        gb28181Service().streamNoneReader(payload);
        bool closed = false;
        if (!stream.empty()) {
            auto stopped = co_await Runtime::wait(c, runtime().stopPreviewByStream(stream),
                                                  std::chrono::seconds(15));
            closed = stopped.has_value();
            if (!closed && app == "rtp" && Gb28181Service::isManagedStream(stream))
                closed = co_await Runtime::wait(c, runtime().forceCloseRtp(stream),
                                                std::chrono::seconds(15));
        }
        co_return zlmJson(c, std::string("\"close\":") + (closed ? "true" : "false"));
    }

    ruvia::Task<ruvia::HttpResponse> rtpServerTimeout(ruvia::Context& c) {
        const auto payload = co_await c.req().json();
        gb28181Service().authorizeHook(payload);
        auto stream = Gb28181Service::stringField(payload, "stream_id");
        if (stream.empty())
            stream = Gb28181Service::stringField(payload, "stream");
        bool closed = false;
        if (!stream.empty()) {
            auto stopped = co_await Runtime::wait(c, runtime().stopPreviewByStream(stream),
                                                  std::chrono::seconds(15));
            closed = stopped.has_value();
            if (!closed && Gb28181Service::isManagedStream(stream))
                closed = co_await Runtime::wait(c, runtime().forceCloseRtp(stream),
                                                std::chrono::seconds(15));
        }
        co_return zlmJson(c, std::string("\"closed\":") + (closed ? "true" : "false"));
    }

    ruvia::Task<ruvia::HttpResponse> sendRtpStopped(ruvia::Context& c) {
        const auto payload = co_await c.req().json();
        gb28181Service().authorizeHook(payload);
        co_return zlmJson(c);
    }
};

} // namespace service::gb28181
