#pragma once
#include <ruvia/web/Controller.h>
#include "service/utils/json.h"
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
    RUVIA_GET("/health", health);
    RUVIA_GET("/config/sip", sipConfig);
    RUVIA_GET_SSE("/devices/events", devicesEvents);
    RUVIA_GET("/devices", devices);
    RUVIA_GET("/devices/:deviceId", device, ruvia::PathModel<GbDeviceParams>);
    RUVIA_GET("/streams", streams);
    RUVIA_GET("/streams/:streamId", stream, ruvia::PathModel<GbStreamParams>);
    RUVIA_GET("/streams/:streamId/recording", recording, ruvia::PathModel<GbStreamParams>);
    RUVIA_POST("/devices/:deviceId/catalog/query", catalog, ruvia::PathModel<GbDeviceParams>);
    RUVIA_PUT("/devices/:deviceId/name", renameDevice, ruvia::PathModel<GbDeviceParams>);
    RUVIA_PUT("/devices/:deviceId/channels/:channelId/name", renameChannel, ruvia::PathModel<GbChannelParams>);
    RUVIA_POST("/devices/:deviceId/mapping", mapDevice, ruvia::PathModel<GbDeviceParams>);
    RUVIA_DELETE("/devices/:deviceId/mapping", unmapDevice, ruvia::PathModel<GbDeviceParams>);
    RUVIA_POST("/devices/:deviceId/channels/:channelId/preview/start", startPreview, ruvia::PathModel<GbChannelParams>);
    RUVIA_POST("/previews/:sessionId/stop", stopPreview, ruvia::PathModel<GbSessionParams>);
    RUVIA_POST("/previews/:sessionId/heartbeat", heartbeatPreview, ruvia::PathModel<GbSessionParams>);
    RUVIA_POST("/devices/:deviceId/channels/:channelId/ptz/position/set", ptzPosition, ruvia::PathModel<GbChannelParams>);
    RUVIA_POST("/devices/:deviceId/channels/:channelId/ptz/:action", ptz, ruvia::PathModel<GbPtzParams>);
    RUVIA_POST("/devices/:deviceId/channels/:channelId/records/query", records, ruvia::PathModel<GbChannelParams>);
    RUVIA_POST("/devices/:deviceId/channels/:channelId/playback/start", startPlayback, ruvia::PathModel<GbChannelParams>);
    RUVIA_POST("/streams/:streamId/recording/start", startRecording, ruvia::PathModel<GbStreamParams>);
    RUVIA_POST("/streams/:streamId/recording/stop", stopRecording, ruvia::PathModel<GbStreamParams>);
    RUVIA_ROUTES_END
  private:
    ruvia::Task<> health(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:query");
        auto result = co_await gb28181Service().health(request);
        co_return c.json(service::common::ok<GbHealthResponse>(request, std::move(result)));
    }

    ruvia::Task<> sipConfig(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:query");
        auto result = co_await gb28181Service().sipConfig(request);
        co_return c.json(service::common::ok<GbSipConfigResponse>(request, std::move(result)));
    }

    ruvia::Task<> devices(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_return c.json(co_await queryDevices(c, request));
    }

    ruvia::Task<GbDeviceListResponse> queryDevices(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:query");
        auto result = co_await gb28181Service().devices(request);
        co_return service::common::ok<GbDeviceListResponse>(request, std::move(result));
    }

    ruvia::Task<void> devicesEvents(ruvia::Context& c) {
        co_await service::live::serveSnapshots(c, "gb28181", service::middleware::requireAuth(c).userId, [this, &c](service::middleware::RequestContext& request) -> ruvia::Task<std::string> {
            co_return service::live::json(co_await queryDevices(c, request));
        },
                                               [&c] {
                                                   (void)service::middleware::requireAuth(c);
                                               });
    }

    ruvia::Task<> device(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:query");
        const GbDeviceInput body{ std::string(c.req().validated<GbDeviceParams>().get<"deviceId">().view()) };
        auto result = co_await gb28181Service().device(request, body.deviceId);
        co_return c.json(service::common::ok<GbDeviceResponse>(request, std::move(result)));
    }

    ruvia::Task<> streams(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:query");
        auto result = co_await gb28181Service().streams(request);
        co_return c.json(service::common::ok<GbStreamListResponse>(request, std::move(result)));
    }

    ruvia::Task<> stream(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:query");
        const GbStreamInput body{ std::string(c.req().validated<GbStreamParams>().get<"streamId">().view()) };
        auto result = co_await gb28181Service().stream(request, body.streamId);
        co_return c.json(service::common::ok<GbStreamResponse>(request, std::move(result)));
    }

    ruvia::Task<> recording(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:record");
        const GbStreamInput body{ std::string(c.req().validated<GbStreamParams>().get<"streamId">().view()) };
        GbActionDto result(ruvia::ModelOptions{ .resource = request.arena() });
        result.set<"recording">(co_await gb28181Service().recording(request, body.streamId));
        co_return c.json(service::common::ok<GbActionResponse>(request, std::move(result)));
    }

    ruvia::Task<> catalog(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:control");
        const GbDeviceInput body{ std::string(c.req().validated<GbDeviceParams>().get<"deviceId">().view()) };
        co_await gb28181Service().queryCatalog(request, body.deviceId);
        GbActionDto result(ruvia::ModelOptions{ .resource = request.arena() });
        result.set<"sent">(true);
        result.set<"deviceId">(body.deviceId);
        co_return c.json(service::common::ok<GbActionResponse>(request, std::move(result)));
    }

    ruvia::Task<> renameDevice(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:control");
        if (!service::utils::isJsonContentType(c.req().header("Content-Type").value_or(std::string_view{}))) {
            throw ruvia::HttpError({ .status = ruvia::http_status::kUnsupportedMediaType,
                .code = "unsupported_media_type", .message = "request body must be application/json" });
        }
        const auto rawBody = co_await c.req().text();
        auto parsedJson = ruvia::JsonValue::parse(rawBody, { .resource = c.arena() });
        if (!parsedJson) {
            throw ruvia::HttpError({ .status = ruvia::http_status::kBadRequest, .message = "invalid json body" });
        }
        const auto json = std::move(*parsedJson);
        const auto body = GbDeviceNameInput::parse(json, std::string(c.req().validated<GbDeviceParams>().get<"deviceId">().view()));
        co_await gb28181Service().renameDevice(request, body.deviceId, body.name);
        co_return c.json(service::common::operation(c, "操作成功"));
    }

    ruvia::Task<> renameChannel(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:control");
        if (!service::utils::isJsonContentType(c.req().header("Content-Type").value_or(std::string_view{}))) {
            throw ruvia::HttpError({ .status = ruvia::http_status::kUnsupportedMediaType,
                .code = "unsupported_media_type", .message = "request body must be application/json" });
        }
        const auto rawBody = co_await c.req().text();
        auto parsedJson = ruvia::JsonValue::parse(rawBody, { .resource = c.arena() });
        if (!parsedJson) {
            throw ruvia::HttpError({ .status = ruvia::http_status::kBadRequest, .message = "invalid json body" });
        }
        const auto json = std::move(*parsedJson);
        const auto body = GbChannelNameInput::parse(json, std::string(c.req().validated<GbChannelParams>().get<"deviceId">().view()), std::string(c.req().validated<GbChannelParams>().get<"channelId">().view()));
        co_await gb28181Service().renameChannel(request, body.deviceId, body.channelId, body.name);
        co_return c.json(service::common::operation(c, "操作成功"));
    }

    ruvia::Task<> mapDevice(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:control");
        if (!service::utils::isJsonContentType(c.req().header("Content-Type").value_or(std::string_view{}))) {
            throw ruvia::HttpError({ .status = ruvia::http_status::kUnsupportedMediaType,
                .code = "unsupported_media_type", .message = "request body must be application/json" });
        }
        const auto rawBody = co_await c.req().text();
        auto parsedJson = ruvia::JsonValue::parse(rawBody, { .resource = c.arena() });
        if (!parsedJson) {
            throw ruvia::HttpError({ .status = ruvia::http_status::kBadRequest, .message = "invalid json body" });
        }
        const auto json = std::move(*parsedJson);
        const auto body = GbMappingInput::parse(json, std::string(c.req().validated<GbDeviceParams>().get<"deviceId">().view()));
        co_await gb28181Service().mapDevice(request, body.deviceId, body.mapped_device_id);
        GbActionDto result(ruvia::ModelOptions{ .resource = request.arena() });
        result.set<"deviceId">(body.deviceId);
        result.set<"mappedDeviceId">(body.mapped_device_id);
        co_return c.json(service::common::ok<GbActionResponse>(request, std::move(result)));
    }

    ruvia::Task<> unmapDevice(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:control");
        const GbDeviceInput body{ std::string(c.req().validated<GbDeviceParams>().get<"deviceId">().view()) };
        co_await gb28181Service().mapDevice(request, body.deviceId, {});
        GbActionDto result(ruvia::ModelOptions{ .resource = request.arena() });
        result.set<"deviceId">(body.deviceId);
        result.set<"mappedDeviceId">("");
        co_return c.json(service::common::ok<GbActionResponse>(request, std::move(result)));
    }

    ruvia::Task<> startPreview(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:control");
        const GbChannelInput body{ std::string(c.req().validated<GbChannelParams>().get<"deviceId">().view()), std::string(c.req().validated<GbChannelParams>().get<"channelId">().view()) };
        auto result = co_await gb28181Service().startPreview(request, body.deviceId, body.channelId);
        co_return c.json(service::common::ok<GbPreviewStartResponse>(request, std::move(result)));
    }

    ruvia::Task<> stopPreview(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:control");
        const GbSessionInput body{ std::string(c.req().validated<GbSessionParams>().get<"sessionId">().view()) };
        auto result = co_await gb28181Service().stopPreview(request, body.sessionId);
        co_return c.json(service::common::ok<GbPreviewStopResponse>(request, std::move(result)));
    }

    ruvia::Task<> heartbeatPreview(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:control");
        const GbSessionInput body{ std::string(c.req().validated<GbSessionParams>().get<"sessionId">().view()) };
        co_await gb28181Service().renewPreview(request, body.sessionId);
        GbActionDto result(ruvia::ModelOptions{ .resource = request.arena() });
        result.set<"sent">(true);
        result.set<"action">("heartbeat");
        co_return c.json(service::common::ok<GbActionResponse>(request, std::move(result)));
    }

    ruvia::Task<> ptzPosition(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:control");
        if (!service::utils::isJsonContentType(c.req().header("Content-Type").value_or(std::string_view{}))) {
            throw ruvia::HttpError({ .status = ruvia::http_status::kUnsupportedMediaType,
                .code = "unsupported_media_type", .message = "request body must be application/json" });
        }
        const auto rawBody = co_await c.req().text();
        auto parsedJson = ruvia::JsonValue::parse(rawBody, { .resource = c.arena() });
        if (!parsedJson) {
            throw ruvia::HttpError({ .status = ruvia::http_status::kBadRequest, .message = "invalid json body" });
        }
        const auto json = std::move(*parsedJson);
        const auto body = GbPositionInput::parse(json, std::string(c.req().validated<GbChannelParams>().get<"deviceId">().view()), std::string(c.req().validated<GbChannelParams>().get<"channelId">().view()));
        co_await gb28181Service().ptzPosition(request, body.deviceId, body.channelId, body.pan, body.tilt, body.zoom);
        GbActionDto result(ruvia::ModelOptions{ .resource = request.arena() });
        result.set<"pan">(body.pan);
        result.set<"tilt">(body.tilt);
        result.set<"zoom">(body.zoom);
        result.set<"sent">(true);
        co_return c.json(service::common::ok<GbActionResponse>(request, std::move(result)));
    }

    ruvia::Task<> ptz(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:control");
        if (!service::utils::isJsonContentType(c.req().header("Content-Type").value_or(std::string_view{}))) {
            throw ruvia::HttpError({ .status = ruvia::http_status::kUnsupportedMediaType,
                .code = "unsupported_media_type", .message = "request body must be application/json" });
        }
        const auto rawBody = co_await c.req().text();
        auto parsedJson = ruvia::JsonValue::parse(rawBody, { .resource = c.arena() });
        if (!parsedJson) {
            throw ruvia::HttpError({ .status = ruvia::http_status::kBadRequest, .message = "invalid json body" });
        }
        const auto json = std::move(*parsedJson);
        const auto body = GbPtzInput::parse(json, std::string(c.req().validated<GbPtzParams>().get<"deviceId">().view()), std::string(c.req().validated<GbPtzParams>().get<"channelId">().view()), std::string(c.req().validated<GbPtzParams>().get<"action">().view()));
        co_await gb28181Service().ptz(request, body.deviceId, body.channelId, body.action, body.speed);
        GbActionDto result(ruvia::ModelOptions{ .resource = request.arena() });
        result.set<"deviceId">(body.deviceId);
        result.set<"channelId">(body.channelId);
        result.set<"action">(body.action);
        result.set<"speed">(body.speed);
        result.set<"sent">(true);
        co_return c.json(service::common::ok<GbActionResponse>(request, std::move(result)));
    }

    ruvia::Task<> records(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:record");
        if (!service::utils::isJsonContentType(c.req().header("Content-Type").value_or(std::string_view{}))) {
            throw ruvia::HttpError({ .status = ruvia::http_status::kUnsupportedMediaType,
                .code = "unsupported_media_type", .message = "request body must be application/json" });
        }
        const auto rawBody = co_await c.req().text();
        auto parsedJson = ruvia::JsonValue::parse(rawBody, { .resource = c.arena() });
        if (!parsedJson) {
            throw ruvia::HttpError({ .status = ruvia::http_status::kBadRequest, .message = "invalid json body" });
        }
        const auto json = std::move(*parsedJson);
        const auto body = GbRecordInput::parse(json, std::string(c.req().validated<GbChannelParams>().get<"deviceId">().view()), std::string(c.req().validated<GbChannelParams>().get<"channelId">().view()));
        co_await gb28181Service().queryRecords(request, body.deviceId, body.channelId, body.start_time, body.end_time);
        GbActionDto result(ruvia::ModelOptions{ .resource = request.arena() });
        result.set<"sent">(true);
        result.set<"deviceId">(body.deviceId);
        result.set<"channelId">(body.channelId);
        co_return c.json(service::common::ok<GbActionResponse>(request, std::move(result)));
    }

    ruvia::Task<> startPlayback(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:record");
        if (!service::utils::isJsonContentType(c.req().header("Content-Type").value_or(std::string_view{}))) {
            throw ruvia::HttpError({ .status = ruvia::http_status::kUnsupportedMediaType,
                .code = "unsupported_media_type", .message = "request body must be application/json" });
        }
        const auto rawBody = co_await c.req().text();
        auto parsedJson = ruvia::JsonValue::parse(rawBody, { .resource = c.arena() });
        if (!parsedJson) {
            throw ruvia::HttpError({ .status = ruvia::http_status::kBadRequest, .message = "invalid json body" });
        }
        const auto json = std::move(*parsedJson);
        const auto body = GbRecordInput::parse(json, std::string(c.req().validated<GbChannelParams>().get<"deviceId">().view()), std::string(c.req().validated<GbChannelParams>().get<"channelId">().view()));
        auto result = co_await gb28181Service().startPlayback(request, body.deviceId, body.channelId, body.start_time, body.end_time);
        co_return c.json(service::common::ok<GbPreviewStartResponse>(request, std::move(result)));
    }

    ruvia::Task<> startRecording(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:record");
        const GbStreamInput body{ std::string(c.req().validated<GbStreamParams>().get<"streamId">().view()) };
        co_await gb28181Service().startRecording(request, body.streamId);
        GbActionDto result(ruvia::ModelOptions{ .resource = request.arena() });
        result.set<"recording">(true);
        co_return c.json(service::common::ok<GbActionResponse>(request, std::move(result)));
    }

    ruvia::Task<> stopRecording(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:gb28181:record");
        const GbStreamInput body{ std::string(c.req().validated<GbStreamParams>().get<"streamId">().view()) };
        co_await gb28181Service().stopRecording(request, body.streamId);
        GbActionDto result(ruvia::ModelOptions{ .resource = request.arena() });
        result.set<"recording">(false);
        co_return c.json(service::common::ok<GbActionResponse>(request, std::move(result)));
    }
};
} // namespace service::gb28181
