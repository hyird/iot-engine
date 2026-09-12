#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <ruvia/web/Context.h>
#include <ruvia/web/ModelJson.h>

#include "service/common/http.h"
#include "service/common/uuid.h"
#include "service/modules/gb28181/gb28181.types.h"
#include "service/middleware/rpc.h"
#include "service/utils/json.h"

namespace service::gb28181 {

namespace detail {

class RequestJson final {
public:
  RequestJson() { body_.push_back('{'); }

  void text(std::string_view name, std::string_view value) {
    fieldName(name);
    body_ += service::utils::jsonQuoted(value);
  }

  void integer(std::string_view name, std::int64_t value) {
    fieldName(name);
    body_ += std::to_string(value);
  }

  void number(std::string_view name, double value) {
    fieldName(name);
    body_ += std::to_string(value);
  }

  [[nodiscard]] std::string finish() && {
    body_.push_back('}');
    return std::move(body_);
  }

private:
  void fieldName(std::string_view name) {
    if (!first_)
      body_.push_back(',');
    first_ = false;
    body_ += service::utils::jsonQuoted(name);
    body_.push_back(':');
  }

  std::string body_;
  bool first_{true};
};

template <typename Response>
Response parseResponse(ruvia::Context &c, std::string_view body) {
  auto parsed =
      ruvia::fromJson<Response>(body, {.resource = c.arena()});
  if (!parsed || !parsed->template get<"data">())
    service::common::fail(10004, "GB28181 RPC 响应无效", 502);
  return std::move(*parsed);
}

GbMediaPortsDto mediaPorts(ruvia::Context &c,
                           const rpc_wire::MediaPorts &value) {
  GbMediaPortsDto result(ruvia::ModelOptions{.resource = c.arena()});
  if (const auto &field = value.get<"http">()) result.set<"http">(*field);
  if (const auto &field = value.get<"https">()) result.set<"https">(*field);
  if (const auto &field = value.get<"rtsp">()) result.set<"rtsp">(*field);
  if (const auto &field = value.get<"rtsps">()) result.set<"rtsps">(*field);
  if (const auto &field = value.get<"rtmp">()) result.set<"rtmp">(*field);
  if (const auto &field = value.get<"rtmps">()) result.set<"rtmps">(*field);
  if (const auto &field = value.get<"rtc">()) result.set<"rtc">(*field);
  if (const auto &field = value.get<"srt">()) result.set<"srt">(*field);
  return result;
}

GbMediaCapabilitiesDto mediaCapabilities(
    ruvia::Context &c, const rpc_wire::MediaCapabilities &value) {
  GbMediaCapabilitiesDto result(ruvia::ModelOptions{.resource = c.arena()});
  if (const auto &field = value.get<"faac">()) result.set<"faac">(*field);
  if (const auto &field = value.get<"ffmpeg">())
    result.set<"ffmpeg">(*field);
  if (const auto &field = value.get<"hls">()) result.set<"hls">(*field);
  if (const auto &field = value.get<"mp4">()) result.set<"mp4">(*field);
  if (const auto &field = value.get<"rtpProxy">())
    result.set<"rtpProxy">(*field);
  if (const auto &field = value.get<"srt">()) result.set<"srt">(*field);
  if (const auto &field = value.get<"sctp">()) result.set<"sctp">(*field);
  if (const auto &field = value.get<"webRtc">())
    result.set<"webRtc">(*field);
  if (const auto &field = value.get<"x264">()) result.set<"x264">(*field);
  if (const auto &field = value.get<"videoStack">())
    result.set<"videoStack">(*field);
  if (const auto &field = value.get<"tls">()) result.set<"tls">(*field);
  if (const auto &field = value.get<"recording">())
    result.set<"recording">(*field);
  return result;
}

GbHealthDto health(ruvia::Context &c, const rpc_wire::Health &value) {
  GbHealthDto result(ruvia::ModelOptions{.resource = c.arena()});
  if (const auto &field = value.get<"status">())
    result.set<"status">(field->view());
  if (const auto &field = value.get<"service">())
    result.set<"service">(field->view());
  if (const auto &field = value.get<"enabled">())
    result.set<"enabled">(*field);
  if (const auto &field = value.get<"started">())
    result.set<"started">(*field);
  if (const auto &field = value.get<"error">())
    result.set<"error">(field->view());
  if (const auto &field = value.get<"mediaPorts">())
    result.set<"mediaPorts">(mediaPorts(c, *field));
  if (const auto &field = value.get<"mediaCapabilities">())
    result.set<"mediaCapabilities">(mediaCapabilities(c, *field));
  return result;
}

GbSipConfigDto sipConfig(ruvia::Context &c, const rpc_wire::SipConfig &value) {
  GbSipConfigDto result(ruvia::ModelOptions{.resource = c.arena()});
  if (const auto &field = value.get<"domain">())
    result.set<"domain">(field->view());
  if (const auto &field = value.get<"id">()) result.set<"id">(field->view());
  if (const auto &field = value.get<"host">())
    result.set<"host">(field->view());
  if (const auto &field = value.get<"publicIp">())
    result.set<"publicIp">(field->view());
  if (const auto &field = value.get<"port">()) result.set<"port">(*field);
  if (const auto &field = value.get<"transport">())
    result.set<"transport">(field->view());
  return result;
}

GbChannelDto channel(ruvia::Context &c, const rpc_wire::Channel &value) {
  GbChannelDto result(ruvia::ModelOptions{.resource = c.arena()});
  if (const auto &field = value.get<"id">()) result.set<"id">(field->view());
  if (const auto &field = value.get<"name">())
    result.set<"name">(field->view());
  if (const auto &field = value.get<"reportedName">())
    result.set<"reportedName">(field->view());
  if (const auto &field = value.get<"customName">())
    result.set<"customName">(field->view());
  if (const auto &field = value.get<"manufacturer">())
    result.set<"manufacturer">(field->view());
  if (const auto &field = value.get<"online">())
    result.set<"online">(*field);
  if (const auto &field = value.get<"ptzType">())
    result.set<"ptzType">(*field);
  if (const auto &field = value.get<"ptzCapable">())
    result.set<"ptzCapable">(*field);
  return result;
}

GbRecordDto record(ruvia::Context &c, const rpc_wire::Record &value) {
  GbRecordDto result(ruvia::ModelOptions{.resource = c.arena()});
  if (const auto &field = value.get<"deviceId">())
    result.set<"deviceId">(field->view());
  if (const auto &field = value.get<"name">())
    result.set<"name">(field->view());
  if (const auto &field = value.get<"filePath">())
    result.set<"filePath">(field->view());
  if (const auto &field = value.get<"address">())
    result.set<"address">(field->view());
  if (const auto &field = value.get<"startTime">())
    result.set<"startTime">(field->view());
  if (const auto &field = value.get<"endTime">())
    result.set<"endTime">(field->view());
  if (const auto &field = value.get<"type">())
    result.set<"type">(field->view());
  if (const auto &field = value.get<"recorderId">())
    result.set<"recorderId">(field->view());
  return result;
}

GbDeviceDto device(ruvia::Context &c, const rpc_wire::Device &value) {
  GbDeviceDto result(ruvia::ModelOptions{.resource = c.arena()});
  if (const auto &field = value.get<"id">()) result.set<"id">(field->view());
  if (const auto &field = value.get<"name">())
    result.set<"name">(field->view());
  if (const auto &field = value.get<"reportedName">())
    result.set<"reportedName">(field->view());
  if (const auto &field = value.get<"customName">())
    result.set<"customName">(field->view());
  if (const auto &field = value.get<"manufacturer">())
    result.set<"manufacturer">(field->view());
  if (const auto &field = value.get<"remoteAddress">())
    result.set<"remoteAddress">(field->view());
  if (const auto &field = value.get<"remoteIp">())
    result.set<"remoteIp">(field->view());
  if (const auto &field = value.get<"remotePort">())
    result.set<"remotePort">(field->view());
  if (const auto &field = value.get<"registrationSource">())
    result.set<"registrationSource">(field->view());
  if (const auto &field = value.get<"mappedDeviceId">())
    result.set<"mappedDeviceId">(field->view());
  if (const auto &field = value.get<"lastSeenAt">())
    result.set<"lastSeenAt">(field->view());
  if (const auto &field = value.get<"online">())
    result.set<"online">(*field);

  if (const auto &source = value.get<"channels">()) {
    ruvia::BoxedArray<GbChannelDto> channels(
        ruvia::ModelOptions{.resource = c.arena()});
    for (const auto &item : *source)
      channels.emplace(channel(c, item));
    result.set<"channels">(std::move(channels));
  }
  if (const auto &source = value.get<"records">()) {
    ruvia::BoxedArray<GbRecordDto> records(
        ruvia::ModelOptions{.resource = c.arena()});
    for (const auto &item : *source)
      records.emplace(record(c, item));
    result.set<"records">(std::move(records));
  }
  return result;
}

GbStreamDto stream(ruvia::Context &c, const rpc_wire::Stream &value) {
  GbStreamDto result(ruvia::ModelOptions{.resource = c.arena()});
  if (const auto &field = value.get<"id">()) result.set<"id">(field->view());
  if (const auto &field = value.get<"app">())
    result.set<"app">(field->view());
  if (const auto &field = value.get<"stream">())
    result.set<"stream">(field->view());
  if (const auto &field = value.get<"schema">())
    result.set<"schema">(field->view());
  if (const auto &field = value.get<"online">())
    result.set<"online">(*field);
  if (const auto &field = value.get<"readerCount">())
    result.set<"readerCount">(*field);
  return result;
}

GbPlayUrlsDto playUrls(ruvia::Context &c, const rpc_wire::PlayUrls &value) {
  GbPlayUrlsDto result(ruvia::ModelOptions{.resource = c.arena()});
  if (const auto &field = value.get<"httpFlv">())
    result.set<"httpFlv">(field->view());
  if (const auto &field = value.get<"wsFlv">())
    result.set<"wsFlv">(field->view());
  if (const auto &field = value.get<"httpTs">())
    result.set<"httpTs">(field->view());
  if (const auto &field = value.get<"hls">())
    result.set<"hls">(field->view());
  if (const auto &field = value.get<"webrtc">())
    result.set<"webrtc">(field->view());
  if (const auto &field = value.get<"rtsp">())
    result.set<"rtsp">(field->view());
  if (const auto &field = value.get<"rtmp">())
    result.set<"rtmp">(field->view());
  return result;
}

GbPreviewStartDto previewStart(ruvia::Context &c,
                               const rpc_wire::PreviewStart &value) {
  GbPreviewStartDto result(ruvia::ModelOptions{.resource = c.arena()});
  if (const auto &field = value.get<"sent">()) result.set<"sent">(*field);
  if (const auto &field = value.get<"sessionId">())
    result.set<"sessionId">(field->view());
  if (const auto &field = value.get<"deviceId">())
    result.set<"deviceId">(field->view());
  if (const auto &field = value.get<"channelId">())
    result.set<"channelId">(field->view());
  if (const auto &field = value.get<"streamId">())
    result.set<"streamId">(field->view());
  if (const auto &field = value.get<"ssrc">())
    result.set<"ssrc">(field->view());
  if (const auto &field = value.get<"rtpPort">())
    result.set<"rtpPort">(*field);
  if (const auto &field = value.get<"leaseTimeoutSeconds">())
    result.set<"leaseTimeoutSeconds">(*field);
  if (const auto &field = value.get<"playUrls">())
    result.set<"playUrls">(playUrls(c, *field));
  return result;
}

GbPreviewStopDto previewStop(ruvia::Context &c,
                             const rpc_wire::PreviewStop &value) {
  GbPreviewStopDto result(ruvia::ModelOptions{.resource = c.arena()});
  if (const auto &field = value.get<"stopped">())
    result.set<"stopped">(*field);
  if (const auto &field = value.get<"sessionId">())
    result.set<"sessionId">(field->view());
  if (const auto &field = value.get<"streamId">())
    result.set<"streamId">(field->view());
  if (const auto &field = value.get<"byeSent">())
    result.set<"byeSent">(*field);
  if (const auto &field = value.get<"rtpServerClosed">())
    result.set<"rtpServerClosed">(*field);
  return result;
}

GbActionDto action(ruvia::Context &c, const rpc_wire::Action &value) {
  GbActionDto result(ruvia::ModelOptions{.resource = c.arena()});
  if (const auto &field = value.get<"registered">())
    result.set<"registered">(*field);
  if (const auto &field = value.get<"sent">()) result.set<"sent">(*field);
  if (const auto &field = value.get<"deviceId">())
    result.set<"deviceId">(field->view());
  if (const auto &field = value.get<"channelId">())
    result.set<"channelId">(field->view());
  if (const auto &field = value.get<"action">())
    result.set<"action">(field->view());
  if (const auto &field = value.get<"mappedDeviceId">())
    result.set<"mappedDeviceId">(field->view());
  if (const auto &field = value.get<"speed">()) result.set<"speed">(*field);
  if (const auto &field = value.get<"pan">()) result.set<"pan">(*field);
  if (const auto &field = value.get<"tilt">()) result.set<"tilt">(*field);
  if (const auto &field = value.get<"zoom">()) result.set<"zoom">(*field);
  if (const auto &field = value.get<"recording">())
    result.set<"recording">(*field);
  return result;
}

} // namespace detail

class Gb28181Service final {
public:
  [[nodiscard]] ruvia::Task<GbHealthDto> health(ruvia::Context &c) const {
    const auto raw = co_await call(c, "health", "{}");
    auto response = detail::parseResponse<rpc_wire::HealthResponse>(c, raw);
    co_return detail::health(c, *response.get<"data">());
  }

  [[nodiscard]] ruvia::Task<GbSipConfigDto>
  sipConfig(ruvia::Context &c) const {
    const auto raw = co_await call(c, "config.sip", "{}");
    auto response = detail::parseResponse<rpc_wire::SipConfigResponse>(c, raw);
    co_return detail::sipConfig(c, *response.get<"data">());
  }

  [[nodiscard]] ruvia::Task<GbDeviceListDto>
  devices(ruvia::Context &c) const {
    const auto raw = co_await call(c, "devices", "{}");
    auto response = detail::parseResponse<rpc_wire::DeviceListResponse>(c, raw);
    const auto &source = *response.get<"data">();
    ruvia::BoxedArray<GbDeviceDto> items(
        ruvia::ModelOptions{.resource = c.arena()});
    if (const auto &values = source.get<"items">()) {
      for (const auto &item : *values)
        items.emplace(detail::device(c, item));
    }
    GbDeviceListDto result(ruvia::ModelOptions{.resource = c.arena()});
    result.set<"items">(std::move(items));
    co_return result;
  }

  [[nodiscard]] ruvia::Task<GbDeviceDto>
  device(ruvia::Context &c, std::string id) const {
    detail::RequestJson payload;
    payload.text("device_id", id);
    const auto raw = co_await call(c, "device", std::move(payload).finish());
    auto response = detail::parseResponse<rpc_wire::DeviceResponse>(c, raw);
    co_return detail::device(c, *response.get<"data">());
  }

  [[nodiscard]] ruvia::Task<GbStreamListDto>
  streams(ruvia::Context &c) const {
    const auto raw = co_await call(c, "streams", "{}");
    auto response = detail::parseResponse<rpc_wire::StreamListResponse>(c, raw);
    const auto &source = *response.get<"data">();
    ruvia::BoxedArray<GbStreamDto> items(
        ruvia::ModelOptions{.resource = c.arena()});
    if (const auto &values = source.get<"items">()) {
      for (const auto &item : *values)
        items.emplace(detail::stream(c, item));
    }
    GbStreamListDto result(ruvia::ModelOptions{.resource = c.arena()});
    result.set<"items">(std::move(items));
    co_return result;
  }

  [[nodiscard]] ruvia::Task<GbStreamDto>
  stream(ruvia::Context &c, std::string id) const {
    detail::RequestJson payload;
    payload.text("stream_id", id);
    const auto raw = co_await call(c, "stream", std::move(payload).finish());
    auto response = detail::parseResponse<rpc_wire::StreamResponse>(c, raw);
    co_return detail::stream(c, *response.get<"data">());
  }

  ruvia::Task<void> queryCatalog(ruvia::Context &c,
                                 std::string deviceId) const {
    detail::RequestJson payload;
    payload.text("device_id", deviceId);
    (void)co_await call(c, "catalog", std::move(payload).finish());
    co_return;
  }

  ruvia::Task<void> renameDevice(ruvia::Context &c, std::string deviceId,
                                 std::string name) const {
    detail::RequestJson payload;
    payload.text("device_id", deviceId);
    payload.text("name", name);
    (void)co_await call(c, "rename_device", std::move(payload).finish());
    co_return;
  }

  ruvia::Task<void> renameChannel(ruvia::Context &c, std::string deviceId,
                                  std::string channelId,
                                  std::string name) const {
    detail::RequestJson payload;
    payload.text("device_id", deviceId);
    payload.text("channel_id", channelId);
    payload.text("name", name);
    (void)co_await call(c, "rename_channel", std::move(payload).finish());
    co_return;
  }

  ruvia::Task<void> mapDevice(ruvia::Context &c, std::string deviceId,
                              std::string mappedDeviceId) const {
    if (!mappedDeviceId.empty()) {
      if (!service::common::isUuid(mappedDeviceId))
        service::common::fail(10001, "mapped_device_id 必须是 UUID", 400);
      const auto target = co_await c.db().query(
          "SELECT 1 FROM device WHERE id = $1::uuid AND deleted_at IS NULL",
          service::common::dbParams(mappedDeviceId));
      if (target.empty())
        service::common::fail(10003, "映射目标设备不存在", 404);
    }
    detail::RequestJson payload;
    payload.text("device_id", deviceId);
    if (!mappedDeviceId.empty())
      payload.text("mapped_device_id", mappedDeviceId);
    (void)co_await call(c, mappedDeviceId.empty() ? "unmap" : "map",
                        std::move(payload).finish());
    co_return;
  }

  ruvia::Task<void> queryRecords(ruvia::Context &c, std::string deviceId,
                                 std::string channelId, std::string startTime,
                                 std::string endTime) const {
    detail::RequestJson payload;
    payload.text("device_id", deviceId);
    payload.text("channel_id", channelId);
    payload.text("start_time", startTime);
    payload.text("end_time", endTime);
    (void)co_await call(c, "records", std::move(payload).finish());
    co_return;
  }

  ruvia::Task<void> ptz(ruvia::Context &c, std::string deviceId,
                        std::string channelId, std::string action,
                        std::uint8_t speed) const {
    detail::RequestJson payload;
    payload.text("device_id", deviceId);
    payload.text("channel_id", channelId);
    payload.text("action", action);
    payload.integer("speed", speed);
    (void)co_await call(c, "ptz", std::move(payload).finish());
    co_return;
  }

  ruvia::Task<void> ptzPosition(ruvia::Context &c, std::string deviceId,
                                std::string channelId, double pan, double tilt,
                                double zoom) const {
    detail::RequestJson payload;
    payload.text("device_id", deviceId);
    payload.text("channel_id", channelId);
    payload.number("pan", pan);
    payload.number("tilt", tilt);
    payload.number("zoom", zoom);
    (void)co_await call(c, "ptz.position", std::move(payload).finish());
    co_return;
  }

  ruvia::Task<GbPreviewStartDto>
  startPreview(ruvia::Context &c, std::string deviceId,
               std::string channelId) const {
    detail::RequestJson payload;
    payload.text("device_id", deviceId);
    payload.text("channel_id", channelId);
    const auto raw =
        co_await call(c, "preview.start", std::move(payload).finish());
    auto response =
        detail::parseResponse<rpc_wire::PreviewStartResponse>(c, raw);
    co_return detail::previewStart(c, *response.get<"data">());
  }

  ruvia::Task<GbPreviewStopDto>
  stopPreview(ruvia::Context &c, std::string sessionId) const {
    detail::RequestJson payload;
    payload.text("session_id", sessionId);
    const auto raw =
        co_await call(c, "preview.stop", std::move(payload).finish());
    auto response =
        detail::parseResponse<rpc_wire::PreviewStopResponse>(c, raw);
    co_return detail::previewStop(c, *response.get<"data">());
  }

  ruvia::Task<void> renewPreview(ruvia::Context &c,
                                 std::string sessionId) const {
    detail::RequestJson payload;
    payload.text("session_id", sessionId);
    (void)co_await call(c, "preview.heartbeat", std::move(payload).finish());
    co_return;
  }

  ruvia::Task<GbPreviewStartDto>
  startPlayback(ruvia::Context &c, std::string deviceId,
                std::string channelId, std::string startTime,
                std::string endTime) const {
    detail::RequestJson payload;
    payload.text("device_id", deviceId);
    payload.text("channel_id", channelId);
    payload.text("start_time", startTime);
    payload.text("end_time", endTime);
    const auto raw =
        co_await call(c, "playback.start", std::move(payload).finish());
    auto response =
        detail::parseResponse<rpc_wire::PreviewStartResponse>(c, raw);
    co_return detail::previewStart(c, *response.get<"data">());
  }

  ruvia::Task<bool> recording(ruvia::Context &c, std::string streamId) const {
    detail::RequestJson payload;
    payload.text("stream_id", streamId);
    const auto raw =
        co_await call(c, "recording", std::move(payload).finish());
    auto response = detail::parseResponse<rpc_wire::ActionResponse>(c, raw);
    const auto &value = response.get<"data">()->get<"recording">();
    co_return value && static_cast<bool>(*value);
  }

  ruvia::Task<void> startRecording(ruvia::Context &c,
                                   std::string streamId) const {
    detail::RequestJson payload;
    payload.text("stream_id", streamId);
    (void)co_await call(c, "recording.start", std::move(payload).finish());
    co_return;
  }

  ruvia::Task<void> stopRecording(ruvia::Context &c,
                                  std::string streamId) const {
    detail::RequestJson payload;
    payload.text("stream_id", streamId);
    (void)co_await call(c, "recording.stop", std::move(payload).finish());
    co_return;
  }

private:
  static ruvia::Task<std::string> call(ruvia::Context &c,
                                       std::string_view operation,
                                       std::string payload) {
    co_return co_await service::rpc::call(c, "gb28181", operation,
                                          std::move(payload));
  }
};

inline Gb28181Service &gb28181Service() {
  static Gb28181Service value;
  return value;
}

} // namespace service::gb28181
