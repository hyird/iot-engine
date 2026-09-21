#pragma once

#include <ruvia/web/Controller.h>
#include <ruvia/web/ModelJson.h>

#include "service/common/http.h"
#include "service/middleware/auth.h"
#include "service/middleware/live.h"
#include "service/middleware/permission.h"
#include "service/modules/gb28181/gb28181.types.h"
#include "service/modules/gb28181/gb28181.service.h"

namespace service::gb28181 {
class Gb28181Controller final : public ruvia::Controller<Gb28181Controller> {
  public:
    RUVIA_CONTROLLER_GROUP("/v1/gb28181", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/health", health, service::middleware::PermissionMiddleware<"iot:gb28181:query">);
    RUVIA_GET("/config/sip", sipConfig, service::middleware::PermissionMiddleware<"iot:gb28181:query">);
    RUVIA_GET_SSE("/devices/events", devicesEvents, service::middleware::PermissionMiddleware<"iot:gb28181:query">);
    RUVIA_GET("/devices", devices, service::middleware::PermissionMiddleware<"iot:gb28181:query">);
    RUVIA_GET("/devices/:deviceId", device, service::middleware::PermissionMiddleware<"iot:gb28181:query">, ruvia::PathModel<GbDeviceParams>);
    RUVIA_GET("/streams", streams, service::middleware::PermissionMiddleware<"iot:gb28181:query">);
    RUVIA_GET("/streams/:streamId", stream, service::middleware::PermissionMiddleware<"iot:gb28181:query">, ruvia::PathModel<GbStreamParams>);
    RUVIA_GET("/streams/:streamId/recording", recording, service::middleware::PermissionMiddleware<"iot:gb28181:record">, ruvia::PathModel<GbStreamParams>);
    RUVIA_POST("/devices/:deviceId/catalog/query", catalog, service::middleware::PermissionMiddleware<"iot:gb28181:control">, ruvia::PathModel<GbDeviceParams>);
    RUVIA_PUT("/devices/:deviceId/name", renameDevice, service::middleware::PermissionMiddleware<"iot:gb28181:control">, ruvia::PathModel<GbDeviceParams>, ruvia::JsonBody<GbNameBody>);
    RUVIA_PUT("/devices/:deviceId/channels/:channelId/name", renameChannel, service::middleware::PermissionMiddleware<"iot:gb28181:control">, ruvia::PathModel<GbChannelParams>, ruvia::JsonBody<GbNameBody>);
    RUVIA_POST("/devices/:deviceId/mapping", mapDevice, service::middleware::PermissionMiddleware<"iot:gb28181:control">, ruvia::PathModel<GbDeviceParams>, ruvia::JsonBody<GbMappingBody>);
    RUVIA_DELETE("/devices/:deviceId/mapping", unmapDevice, service::middleware::PermissionMiddleware<"iot:gb28181:control">, ruvia::PathModel<GbDeviceParams>);
    RUVIA_POST("/devices/:deviceId/channels/:channelId/preview/start", startPreview, service::middleware::PermissionMiddleware<"iot:gb28181:control">, ruvia::PathModel<GbChannelParams>);
    RUVIA_POST("/previews/:sessionId/stop", stopPreview, service::middleware::PermissionMiddleware<"iot:gb28181:control">, ruvia::PathModel<GbSessionParams>);
    RUVIA_POST("/previews/:sessionId/heartbeat", heartbeatPreview, service::middleware::PermissionMiddleware<"iot:gb28181:control">, ruvia::PathModel<GbSessionParams>);
    RUVIA_POST("/devices/:deviceId/channels/:channelId/ptz/position/set", ptzPosition, service::middleware::PermissionMiddleware<"iot:gb28181:control">, ruvia::PathModel<GbChannelParams>, ruvia::JsonBody<GbPositionBody>);
    RUVIA_POST("/devices/:deviceId/channels/:channelId/ptz/:action", ptz, service::middleware::PermissionMiddleware<"iot:gb28181:control">, ruvia::PathModel<GbPtzParams>, ruvia::JsonBody<GbPtzBody>);
    RUVIA_POST("/devices/:deviceId/channels/:channelId/records/query", records, service::middleware::PermissionMiddleware<"iot:gb28181:record">, ruvia::PathModel<GbChannelParams>, ruvia::JsonBody<GbRecordBody>);
    RUVIA_POST("/devices/:deviceId/channels/:channelId/playback/start", startPlayback, service::middleware::PermissionMiddleware<"iot:gb28181:record">, ruvia::PathModel<GbChannelParams>, ruvia::JsonBody<GbRecordBody>);
    RUVIA_POST("/streams/:streamId/recording/start", startRecording, service::middleware::PermissionMiddleware<"iot:gb28181:record">, ruvia::PathModel<GbStreamParams>);
    RUVIA_POST("/streams/:streamId/recording/stop", stopRecording, service::middleware::PermissionMiddleware<"iot:gb28181:record">, ruvia::PathModel<GbStreamParams>);
    RUVIA_ROUTES_END

  private:
    ruvia::Task<ruvia::HttpResponse> health(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        auto result = co_await gb28181Service().health(request);
        co_return c.json(service::common::ok<GbHealthResponse>(request, std::move(result)));
    }

    ruvia::Task<ruvia::HttpResponse> sipConfig(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        auto result = co_await gb28181Service().sipConfig(request);
        co_return c.json(service::common::ok<GbSipConfigResponse>(request, std::move(result)));
    }

    ruvia::Task<ruvia::HttpResponse> devices(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        auto result = co_await gb28181Service().devices(request);
        co_return c.json(service::common::ok<GbDeviceListResponse>(request, std::move(result)));
    }

    ruvia::Task<void> devicesEvents(ruvia::Context& c) {
        co_await service::live::serveSnapshots(c, "gb28181", service::middleware::requireAuth(c).userId,
            [](service::middleware::RequestContext& request) -> ruvia::Task<std::string> {
                // 连接建立后权限仍可能被撤销，每次事件查询前重新校验。
                co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:query");
                auto result = co_await gb28181Service().devices(request);
                co_return service::live::json(service::common::ok<GbDeviceListResponse>(request, std::move(result)));
            },
            [&c] { (void)service::middleware::requireAuth(c); });
    }

    ruvia::Task<ruvia::HttpResponse> device(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto& params = c.req().validated<GbDeviceParams>();
        auto result = co_await gb28181Service().device(request, std::string(params.get<"deviceId">().view()));
        co_return c.json(service::common::ok<GbDeviceResponse>(request, std::move(result)));
    }

    ruvia::Task<ruvia::HttpResponse> streams(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        auto result = co_await gb28181Service().streams(request);
        co_return c.json(service::common::ok<GbStreamListResponse>(request, std::move(result)));
    }

    ruvia::Task<ruvia::HttpResponse> stream(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto& params = c.req().validated<GbStreamParams>();
        auto result = co_await gb28181Service().stream(request, std::string(params.get<"streamId">().view()));
        co_return c.json(service::common::ok<GbStreamResponse>(request, std::move(result)));
    }

    ruvia::Task<ruvia::HttpResponse> recording(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        GbActionDto result(ruvia::ModelOptions{.resource = request.arena()});
        result.set<"recording">(co_await gb28181Service().recording(request, std::string(c.req().validated<GbStreamParams>().get<"streamId">().view())));
        co_return c.json(service::common::ok<GbActionResponse>(request, std::move(result)));
    }

    ruvia::Task<ruvia::HttpResponse> catalog(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto& params = c.req().validated<GbDeviceParams>();
        co_await gb28181Service().queryCatalog(request, std::string(params.get<"deviceId">().view()));
        GbActionDto result(ruvia::ModelOptions{.resource = request.arena()});
        result.set<"sent">(true);
        result.set<"deviceId">(params.get<"deviceId">().view());
        co_return c.json(service::common::ok<GbActionResponse>(request, std::move(result)));
    }

    ruvia::Task<ruvia::HttpResponse> renameDevice(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await gb28181Service().renameDevice(request, std::string(c.req().validated<GbDeviceParams>().get<"deviceId">().view()), service::utils::trim(c.req().validated<GbNameBody>().get<"name">().view()));
        co_return c.json(service::common::operation(c, "操作成功"));
    }

    ruvia::Task<ruvia::HttpResponse> renameChannel(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto& params = c.req().validated<GbChannelParams>();
        co_await gb28181Service().renameChannel(request, std::string(params.get<"deviceId">().view()), std::string(params.get<"channelId">().view()), service::utils::trim(c.req().validated<GbNameBody>().get<"name">().view()));
        co_return c.json(service::common::operation(c, "操作成功"));
    }

    ruvia::Task<ruvia::HttpResponse> mapDevice(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const std::string deviceId(c.req().validated<GbDeviceParams>().get<"deviceId">().view());
        const auto mappedDeviceId = service::utils::trim(c.req().validated<GbMappingBody>().get<"mappedDeviceId">().view());
        co_await gb28181Service().mapDevice(request, deviceId, mappedDeviceId);
        GbActionDto result(ruvia::ModelOptions{.resource = request.arena()});
        result.set<"deviceId">(deviceId);
        result.set<"mappedDeviceId">(mappedDeviceId);
        co_return c.json(service::common::ok<GbActionResponse>(request, std::move(result)));
    }

    ruvia::Task<ruvia::HttpResponse> unmapDevice(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto& params = c.req().validated<GbDeviceParams>();
        co_await gb28181Service().mapDevice(request, std::string(params.get<"deviceId">().view()), {});
        GbActionDto result(ruvia::ModelOptions{.resource = request.arena()});
        result.set<"deviceId">(params.get<"deviceId">().view());
        result.set<"mappedDeviceId">("");
        co_return c.json(service::common::ok<GbActionResponse>(request, std::move(result)));
    }

    ruvia::Task<ruvia::HttpResponse> startPreview(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto& params = c.req().validated<GbChannelParams>();
        auto result = co_await gb28181Service().startPreview(request, std::string(params.get<"deviceId">().view()), std::string(params.get<"channelId">().view()));
        co_return c.json(service::common::ok<GbPreviewStartResponse>(request, std::move(result)));
    }

    ruvia::Task<ruvia::HttpResponse> stopPreview(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        auto result = co_await gb28181Service().stopPreview(request, std::string(c.req().validated<GbSessionParams>().get<"sessionId">().view()));
        co_return c.json(service::common::ok<GbPreviewStopResponse>(request, std::move(result)));
    }

    ruvia::Task<ruvia::HttpResponse> heartbeatPreview(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await gb28181Service().renewPreview(request, std::string(c.req().validated<GbSessionParams>().get<"sessionId">().view()));
        GbActionDto result(ruvia::ModelOptions{.resource = request.arena()});
        result.set<"sent">(true);
        result.set<"action">("heartbeat");
        co_return c.json(service::common::ok<GbActionResponse>(request, std::move(result)));
    }

    ruvia::Task<ruvia::HttpResponse> ptzPosition(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto& params = c.req().validated<GbChannelParams>();
        const auto& body = c.req().validated<GbPositionBody>();
        co_await gb28181Service().ptzPosition(request, std::string(params.get<"deviceId">().view()), std::string(params.get<"channelId">().view()), body.get<"pan">().value, body.get<"tilt">().value, body.get<"zoom">().value);
        GbActionDto result(ruvia::ModelOptions{.resource = request.arena()});
        result.set<"pan">(body.get<"pan">());
        result.set<"tilt">(body.get<"tilt">());
        result.set<"zoom">(body.get<"zoom">());
        result.set<"sent">(true);
        co_return c.json(service::common::ok<GbActionResponse>(request, std::move(result)));
    }

    ruvia::Task<ruvia::HttpResponse> ptz(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto& params = c.req().validated<GbPtzParams>();
        const auto speed = static_cast<std::uint8_t>(c.req().validated<GbPtzBody>().get<"speed">().value_or(ruvia::Int64{80}).value);
        co_await gb28181Service().ptz(request, std::string(params.get<"deviceId">().view()), std::string(params.get<"channelId">().view()), std::string(params.get<"action">().view()), speed);
        GbActionDto result(ruvia::ModelOptions{.resource = request.arena()});
        result.set<"deviceId">(params.get<"deviceId">().view());
        result.set<"channelId">(params.get<"channelId">().view());
        result.set<"action">(params.get<"action">().view());
        result.set<"speed">(speed);
        result.set<"sent">(true);
        co_return c.json(service::common::ok<GbActionResponse>(request, std::move(result)));
    }

    ruvia::Task<ruvia::HttpResponse> records(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto& params = c.req().validated<GbChannelParams>();
        const auto& body = c.req().validated<GbRecordBody>();
        co_await gb28181Service().queryRecords(request, std::string(params.get<"deviceId">().view()), std::string(params.get<"channelId">().view()), service::common::canonicalUtcTimestamp(service::utils::trim(body.get<"startTime">().view())), service::common::canonicalUtcTimestamp(service::utils::trim(body.get<"endTime">().view())));
        GbActionDto result(ruvia::ModelOptions{.resource = request.arena()});
        result.set<"sent">(true);
        result.set<"deviceId">(params.get<"deviceId">().view());
        result.set<"channelId">(params.get<"channelId">().view());
        co_return c.json(service::common::ok<GbActionResponse>(request, std::move(result)));
    }

    ruvia::Task<ruvia::HttpResponse> startPlayback(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto& params = c.req().validated<GbChannelParams>();
        const auto& body = c.req().validated<GbRecordBody>();
        auto result = co_await gb28181Service().startPlayback(request, std::string(params.get<"deviceId">().view()), std::string(params.get<"channelId">().view()), service::common::canonicalUtcTimestamp(service::utils::trim(body.get<"startTime">().view())), service::common::canonicalUtcTimestamp(service::utils::trim(body.get<"endTime">().view())));
        co_return c.json(service::common::ok<GbPreviewStartResponse>(request, std::move(result)));
    }

    ruvia::Task<ruvia::HttpResponse> startRecording(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await gb28181Service().startRecording(request, std::string(c.req().validated<GbStreamParams>().get<"streamId">().view()));
        GbActionDto result(ruvia::ModelOptions{.resource = request.arena()});
        result.set<"recording">(true);
        co_return c.json(service::common::ok<GbActionResponse>(request, std::move(result)));
    }

    ruvia::Task<ruvia::HttpResponse> stopRecording(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await gb28181Service().stopRecording(request, std::string(c.req().validated<GbStreamParams>().get<"streamId">().view()));
        GbActionDto result(ruvia::ModelOptions{.resource = request.arena()});
        result.set<"recording">(false);
        co_return c.json(service::common::ok<GbActionResponse>(request, std::move(result)));
    }
};
} // namespace service::gb28181
