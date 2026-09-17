#pragma once
#include <ruvia/web/Controller.h>
#include <ruvia/web/ModelJson.h>

#include "service/common/http.h"
#include "service/middleware/auth.h"
#include "service/middleware/live.h"
#include "service/middleware/permission.h"
#include "service/modules/gb28181/gb28181.schema.h"
#include "service/modules/gb28181/gb28181.service.h"

namespace service::gb28181 {
class Gb28181Controller final : public ruvia::Controller<Gb28181Controller> {
  public:
    RUVIA_CONTROLLER_GROUP("/v1/gb28181", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/health", health);
    RUVIA_GET("/config/sip", sipConfig);
    RUVIA_GET_SSE("/devices/events", devicesEvents);
    RUVIA_GET("/devices", devices);
    RUVIA_GET("/devices/:deviceId", device, GbDeviceValidator);
    RUVIA_GET("/streams", streams);
    RUVIA_GET("/streams/:streamId", stream, GbStreamValidator);
    RUVIA_GET("/streams/:streamId/recording", recording, GbStreamValidator);
    RUVIA_POST("/devices/:deviceId/catalog/query", catalog, GbDeviceValidator);
    RUVIA_PUT("/devices/:deviceId/name", renameDevice, GbDeviceValidator);
    RUVIA_PUT("/devices/:deviceId/channels/:channelId/name", renameChannel, GbChannelValidator);
    RUVIA_POST("/devices/:deviceId/mapping", mapDevice, GbDeviceValidator);
    RUVIA_DELETE("/devices/:deviceId/mapping", unmapDevice, GbDeviceValidator);
    RUVIA_POST("/devices/:deviceId/channels/:channelId/preview/start", startPreview, GbChannelValidator);
    RUVIA_POST("/previews/:sessionId/stop", stopPreview, GbSessionValidator);
    RUVIA_POST("/previews/:sessionId/heartbeat", heartbeatPreview, GbSessionValidator);
    RUVIA_POST("/devices/:deviceId/channels/:channelId/ptz/position/set", ptzPosition, GbChannelValidator);
    RUVIA_POST("/devices/:deviceId/channels/:channelId/ptz/:action", ptz, GbPtzRouteValidator);
    RUVIA_POST("/devices/:deviceId/channels/:channelId/records/query", records, GbChannelValidator);
    RUVIA_POST("/devices/:deviceId/channels/:channelId/playback/start", startPlayback, GbChannelValidator);
    RUVIA_POST("/streams/:streamId/recording/start", startRecording, GbStreamValidator);
    RUVIA_POST("/streams/:streamId/recording/stop", stopRecording, GbStreamValidator);
    RUVIA_ROUTES_END
  private:
    ruvia::Task<ruvia::HttpResponse> health(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await healthSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> healthSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:query");
        auto result = co_await gb28181Service().health(request);
        co_return service::live::data(c, std::string(ruvia::toJson(result)));
    }

    ruvia::Task<ruvia::HttpResponse> sipConfig(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await sipConfigSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> sipConfigSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:query");
        auto result = co_await gb28181Service().sipConfig(request);
        co_return service::live::data(c, std::string(ruvia::toJson(result)));
    }

    ruvia::Task<ruvia::HttpResponse> devices(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await devicesSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> devicesSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:query");
        auto result = co_await gb28181Service().devices(request);
        co_return service::live::data(c, std::string(ruvia::toJson(result)));
    }

    ruvia::Task<void> devicesEvents(ruvia::Context& c) {
        co_await service::live::serveSnapshots(c, "gb28181", service::middleware::requireAuth(c).userId, [this, &c](service::middleware::RequestContext& request) {
            return devicesSnapshot(c, request);
        },
                                               [&c] {
                                                   (void)service::middleware::requireAuth(c);
                                               });
    }

    ruvia::Task<ruvia::HttpResponse> device(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await deviceSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> deviceSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:query");
        const GbDeviceInput body{ std::string(c.req().validated<GbRouteParams>().get<"deviceId">()->view()) };
        auto result = co_await gb28181Service().device(request, body.deviceId);
        co_return service::live::data(c, std::string(ruvia::toJson(result)));
    }

    ruvia::Task<ruvia::HttpResponse> streams(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await streamsSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> streamsSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:query");
        auto result = co_await gb28181Service().streams(request);
        co_return service::live::data(c, std::string(ruvia::toJson(result)));
    }

    ruvia::Task<ruvia::HttpResponse> stream(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await streamSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> streamSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:query");
        const GbStreamInput body{ std::string(c.req().validated<GbRouteParams>().get<"streamId">()->view()) };
        auto result = co_await gb28181Service().stream(request, body.streamId);
        co_return service::live::data(c, std::string(ruvia::toJson(result)));
    }

    ruvia::Task<ruvia::HttpResponse> recording(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await recordingSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> recordingSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:record");
        const GbStreamInput body{ std::string(c.req().validated<GbRouteParams>().get<"streamId">()->view()) };
        GbActionDto result(ruvia::ModelOptions{ .resource = request.arena() });
        result.set<"recording">(co_await gb28181Service().recording(request, body.streamId));
        co_return service::live::data(c, std::string(ruvia::toJson(result)));
    }

    ruvia::Task<ruvia::HttpResponse> catalog(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:control");
        const GbDeviceInput body{ std::string(c.req().validated<GbRouteParams>().get<"deviceId">()->view()) };
        co_await gb28181Service().queryCatalog(request, body.deviceId);
        GbActionDto result(ruvia::ModelOptions{ .resource = request.arena() });
        result.set<"sent">(true);
        result.set<"deviceId">(body.deviceId);
        const auto payload = service::live::data(c, std::string(ruvia::toJson(result)));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<ruvia::HttpResponse> renameDevice(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:control");
        const auto json = co_await c.req().jsonValue();
        const auto body = GbDeviceNameValidator::parse(json, std::string(c.req().validated<GbRouteParams>().get<"deviceId">()->view()));
        co_await gb28181Service().renameDevice(request, body.deviceId, body.name);
        co_return c.json(service::common::operation(c, "操作成功"));
    }

    ruvia::Task<ruvia::HttpResponse> renameChannel(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:control");
        const auto json = co_await c.req().jsonValue();
        const auto body = GbChannelNameValidator::parse(json, std::string(c.req().validated<GbRouteParams>().get<"deviceId">()->view()), std::string(c.req().validated<GbRouteParams>().get<"channelId">()->view()));
        co_await gb28181Service().renameChannel(request, body.deviceId, body.channelId, body.name);
        co_return c.json(service::common::operation(c, "操作成功"));
    }

    ruvia::Task<ruvia::HttpResponse> mapDevice(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:control");
        const auto json = co_await c.req().jsonValue();
        const auto body = GbMappingValidator::parse(json, std::string(c.req().validated<GbRouteParams>().get<"deviceId">()->view()));
        co_await gb28181Service().mapDevice(request, body.deviceId, body.mapped_device_id);
        GbActionDto result(ruvia::ModelOptions{ .resource = request.arena() });
        result.set<"deviceId">(body.deviceId);
        result.set<"mappedDeviceId">(body.mapped_device_id);
        const auto payload = service::live::data(c, std::string(ruvia::toJson(result)));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<ruvia::HttpResponse> unmapDevice(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:control");
        const GbDeviceInput body{ std::string(c.req().validated<GbRouteParams>().get<"deviceId">()->view()) };
        co_await gb28181Service().mapDevice(request, body.deviceId, {});
        GbActionDto result(ruvia::ModelOptions{ .resource = request.arena() });
        result.set<"deviceId">(body.deviceId);
        result.set<"mappedDeviceId">("");
        const auto payload = service::live::data(c, std::string(ruvia::toJson(result)));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<ruvia::HttpResponse> startPreview(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:control");
        const GbChannelInput body{ std::string(c.req().validated<GbRouteParams>().get<"deviceId">()->view()), std::string(c.req().validated<GbRouteParams>().get<"channelId">()->view()) };
        auto result = co_await gb28181Service().startPreview(request, body.deviceId, body.channelId);
        const auto payload = service::live::data(c, std::string(ruvia::toJson(result)));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<ruvia::HttpResponse> stopPreview(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:control");
        const GbSessionInput body{ std::string(c.req().validated<GbRouteParams>().get<"sessionId">()->view()) };
        auto result = co_await gb28181Service().stopPreview(request, body.sessionId);
        const auto payload = service::live::data(c, std::string(ruvia::toJson(result)));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<ruvia::HttpResponse> heartbeatPreview(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:control");
        const GbSessionInput body{ std::string(c.req().validated<GbRouteParams>().get<"sessionId">()->view()) };
        co_await gb28181Service().renewPreview(request, body.sessionId);
        GbActionDto result(ruvia::ModelOptions{ .resource = request.arena() });
        result.set<"sent">(true);
        result.set<"action">("heartbeat");
        const auto payload = service::live::data(c, std::string(ruvia::toJson(result)));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<ruvia::HttpResponse> ptzPosition(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:control");
        const auto json = co_await c.req().jsonValue();
        const auto body = GbPositionValidator::parse(json, std::string(c.req().validated<GbRouteParams>().get<"deviceId">()->view()), std::string(c.req().validated<GbRouteParams>().get<"channelId">()->view()));
        co_await gb28181Service().ptzPosition(request, body.deviceId, body.channelId, body.pan, body.tilt, body.zoom);
        GbActionDto result(ruvia::ModelOptions{ .resource = request.arena() });
        result.set<"pan">(body.pan);
        result.set<"tilt">(body.tilt);
        result.set<"zoom">(body.zoom);
        result.set<"sent">(true);
        const auto payload = service::live::data(c, std::string(ruvia::toJson(result)));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<ruvia::HttpResponse> ptz(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:control");
        const auto json = co_await c.req().jsonValue();
        const auto body = GbPtzValidator::parse(json, std::string(c.req().validated<GbRouteParams>().get<"deviceId">()->view()), std::string(c.req().validated<GbRouteParams>().get<"channelId">()->view()), std::string(c.req().validated<GbRouteParams>().get<"action">()->view()));
        co_await gb28181Service().ptz(request, body.deviceId, body.channelId, body.action, body.speed);
        GbActionDto result(ruvia::ModelOptions{ .resource = request.arena() });
        result.set<"deviceId">(body.deviceId);
        result.set<"channelId">(body.channelId);
        result.set<"action">(body.action);
        result.set<"speed">(body.speed);
        result.set<"sent">(true);
        const auto payload = service::live::data(c, std::string(ruvia::toJson(result)));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<ruvia::HttpResponse> records(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:record");
        const auto json = co_await c.req().jsonValue();
        const auto body = GbRecordValidator::parse(json, std::string(c.req().validated<GbRouteParams>().get<"deviceId">()->view()), std::string(c.req().validated<GbRouteParams>().get<"channelId">()->view()));
        co_await gb28181Service().queryRecords(request, body.deviceId, body.channelId, body.start_time, body.end_time);
        GbActionDto result(ruvia::ModelOptions{ .resource = request.arena() });
        result.set<"sent">(true);
        result.set<"deviceId">(body.deviceId);
        result.set<"channelId">(body.channelId);
        const auto payload = service::live::data(c, std::string(ruvia::toJson(result)));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<ruvia::HttpResponse> startPlayback(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:record");
        const auto json = co_await c.req().jsonValue();
        const auto body = GbRecordValidator::parse(json, std::string(c.req().validated<GbRouteParams>().get<"deviceId">()->view()), std::string(c.req().validated<GbRouteParams>().get<"channelId">()->view()));
        auto result = co_await gb28181Service().startPlayback(request, body.deviceId, body.channelId, body.start_time, body.end_time);
        const auto payload = service::live::data(c, std::string(ruvia::toJson(result)));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<ruvia::HttpResponse> startRecording(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:record");
        const GbStreamInput body{ std::string(c.req().validated<GbRouteParams>().get<"streamId">()->view()) };
        co_await gb28181Service().startRecording(request, body.streamId);
        GbActionDto result(ruvia::ModelOptions{ .resource = request.arena() });
        result.set<"recording">(true);
        const auto payload = service::live::data(c, std::string(ruvia::toJson(result)));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<ruvia::HttpResponse> stopRecording(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:record");
        const GbStreamInput body{ std::string(c.req().validated<GbRouteParams>().get<"streamId">()->view()) };
        co_await gb28181Service().stopRecording(request, body.streamId);
        GbActionDto result(ruvia::ModelOptions{ .resource = request.arena() });
        result.set<"recording">(false);
        const auto payload = service::live::data(c, std::string(ruvia::toJson(result)));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }
};
} // namespace service::gb28181
