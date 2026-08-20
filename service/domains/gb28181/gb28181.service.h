#pragma once

#include <string>
#include <string_view>
#include <utility>

#include <ruvia/web/Context.h>

#include "service/common/http.h"
#include "service/common/timestamp.h"
#include "service/common/uuid.h"
#include "service/domains/gb28181/gb28181.types.h"
#include "service/features/gb28181/runtime.h"

namespace service::gb28181 {

class Gb28181Service final {
public:
  [[nodiscard]] GbHealthDto health(ruvia::Context &c) const {
    const auto &instance = runtime();
    const auto ports = instance.mediaPorts();
    GbMediaPortsDto portsDto(c);
    portsDto.set<"http">(ports.http)
        .set<"https">(ports.https)
        .set<"rtsp">(ports.rtsp)
        .set<"rtsps">(ports.rtsps)
        .set<"rtmp">(ports.rtmp)
        .set<"rtmps">(ports.rtmps)
        .set<"rtc">(ports.rtc)
        .set<"srt">(ports.srt);
    const auto capabilities = instance.mediaCapabilities();
    GbMediaCapabilitiesDto capabilitiesDto(c);
    capabilitiesDto.set<"faac">(capabilities.faac)
        .set<"ffmpeg">(capabilities.ffmpeg)
        .set<"hls">(capabilities.hls)
        .set<"mp4">(capabilities.mp4)
        .set<"rtpProxy">(capabilities.rtpProxy)
        .set<"srt">(capabilities.srt)
        .set<"sctp">(capabilities.sctp)
        .set<"webRtc">(capabilities.webRtc)
        .set<"x264">(capabilities.x264)
        .set<"videoStack">(capabilities.videoStack)
        .set<"tls">(capabilities.tls)
        .set<"recording">(capabilities.recording);
    GbHealthDto result(c);
    result
        .set<"status">(instance.started()
                           ? "ok"
                           : (instance.enabled() ? "error" : "disabled"))
        .set<"service">("iot-engine-gb28181")
        .set<"enabled">(instance.enabled())
        .set<"started">(instance.started())
        .set<"error">(instance.lastError())
        .set<"mediaPorts">(std::move(portsDto))
        .set<"mediaCapabilities">(std::move(capabilitiesDto));
    return result;
  }

  [[nodiscard]] GbSipConfigDto sipConfig(ruvia::Context &c) const {
    const auto &config = runtime().config().sip;
    GbSipConfigDto result(c);
    result.set<"domain">(config.domain)
        .set<"id">(config.id)
        .set<"host">(config.host)
        .set<"publicIp">(config.publicIp)
        .set<"port">(config.port)
        .set<"transport">(config.transport);
    return result;
  }

  ruvia::Task<GbDeviceListDto> devices(ruvia::Context &c) const {
    const auto values = co_await runtime().devices(c);
    ruvia::BoxedArray<GbDeviceDto> items(c.resource());
    for (const auto &value : values)
      items.emplace(deviceDto(c, value));
    GbDeviceListDto result(c);
    result.set<"items">(std::move(items));
    co_return result;
  }

  ruvia::Task<GbDeviceDto> device(ruvia::Context &c, std::string id) const {
    const auto value = co_await runtime().device(c, std::move(id));
    if (!value)
      service::common::fail(10003, "设备不存在", 404);
    co_return deviceDto(c, *value);
  }

  ruvia::Task<GbStreamListDto> streams(ruvia::Context &c) const {
    const auto values = co_await runtime().streams(c);
    ruvia::BoxedArray<GbStreamDto> items(c.resource());
    for (const auto &value : values)
      items.emplace(streamDto(c, value));
    GbStreamListDto result(c);
    result.set<"items">(std::move(items));
    co_return result;
  }

  ruvia::Task<GbStreamDto> stream(ruvia::Context &c, std::string id) const {
    const auto value = co_await runtime().stream(c, std::move(id));
    if (!value)
      service::common::fail(10003, "流不存在", 404);
    co_return streamDto(c, *value);
  }

  ruvia::Task<void> queryCatalog(ruvia::Context &c,
                                 std::string deviceId) const {
    if (!co_await runtime().queryCatalog(c, std::move(deviceId)))
      service::common::fail(10003, "设备不在线", 404);
  }

  ruvia::Task<void> renameDevice(ruvia::Context &c, std::string deviceId,
                                 std::string name) const {
    if (!co_await runtime().renameDevice(c, std::move(deviceId),
                                         std::move(name)))
      service::common::fail(10003, "GB28181 设备不存在", 404);
  }

  ruvia::Task<void> renameChannel(ruvia::Context &c, std::string deviceId,
                                  std::string channelId,
                                  std::string name) const {
    if (!co_await runtime().renameChannel(
            c, std::move(deviceId), std::move(channelId), std::move(name)))
      service::common::fail(10003, "GB28181 设备或通道不存在", 404);
  }

  ruvia::Task<void> queryRecords(ruvia::Context &c, std::string deviceId,
                                 std::string channelId, std::string startTime,
                                 std::string endTime) const {
    if (!co_await runtime().queryRecords(
            c, std::move(deviceId), std::move(channelId), std::move(startTime),
            std::move(endTime)))
      service::common::fail(10003, "设备或通道不可用", 404);
  }

  ruvia::Task<void> ptz(ruvia::Context &c, std::string deviceId,
                        std::string channelId, std::string action,
                        std::uint8_t speed) const {
    if (!co_await runtime().ptz(c, std::move(deviceId), std::move(channelId),
                                std::move(action), speed))
      service::common::fail(10003, "设备或通道不可用", 404);
  }

  ruvia::Task<void> ptzPosition(ruvia::Context &c, std::string deviceId,
                                std::string channelId, double pan, double tilt,
                                double zoom) const {
    if (!co_await runtime().ptzPosition(c, std::move(deviceId),
                                        std::move(channelId), pan, tilt, zoom))
      service::common::fail(10003, "设备或通道不可用", 404);
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
    if (!co_await runtime().mapDevice(c, std::move(deviceId),
                                      std::move(mappedDeviceId)))
      service::common::fail(10003, "GB28181 设备不存在", 404);
  }

  ruvia::Task<bool> recording(ruvia::Context &c, std::string streamId) const {
    co_return co_await runtime().recording(c, std::move(streamId));
  }

  ruvia::Task<void> startRecording(ruvia::Context &c,
                                   std::string streamId) const {
    if (!co_await runtime().startRecording(c, std::move(streamId)))
      service::common::fail(10003, "流不存在或录像功能未启用", 404);
  }

  ruvia::Task<void> stopRecording(ruvia::Context &c,
                                  std::string streamId) const {
    if (!co_await runtime().stopRecording(c, std::move(streamId)))
      service::common::fail(10003, "流不存在或未在录像", 404);
  }

  [[nodiscard]] static GbPreviewStartDto
  previewStart(ruvia::Context &c, const SipServer::PreviewStartResult &value) {
    GbPlayUrlsDto urls(c);
    urls.set<"httpFlv">(value.playUrls.httpFlv)
        .set<"wsFlv">(value.playUrls.wsFlv)
        .set<"httpTs">(value.playUrls.httpTs)
        .set<"hls">(value.playUrls.hls)
        .set<"webrtc">(value.playUrls.webRtc)
        .set<"rtsp">(value.playUrls.rtsp)
        .set<"rtmp">(value.playUrls.rtmp);
    GbPreviewStartDto result(c);
    result.set<"sent">(true)
        .set<"sessionId">(value.sessionId)
        .set<"deviceId">(value.deviceId)
        .set<"channelId">(value.channelId)
        .set<"streamId">(value.streamId)
        .set<"ssrc">(value.ssrc)
        .set<"rtpPort">(value.rtpPort)
        .set<"leaseTimeoutSeconds">(value.leaseTimeoutSeconds)
        .set<"playUrls">(std::move(urls));
    return result;
  }

  [[nodiscard]] static GbPreviewStopDto
  previewStop(ruvia::Context &c, const SipServer::PreviewStopResult &value) {
    GbPreviewStopDto result(c);
    result.set<"stopped">(true)
        .set<"sessionId">(value.sessionId)
        .set<"streamId">(value.streamId)
        .set<"byeSent">(value.byeSent)
        .set<"rtpServerClosed">(value.rtpServerClosed);
    return result;
  }

private:
  [[nodiscard]] static std::pair<std::string, std::string>
  splitRemoteAddress(std::string_view address) {
    if (address.starts_with('[')) {
      const auto end = address.find(']');
      if (end != std::string_view::npos) {
        auto port = end + 1 < address.size() && address[end + 1] == ':'
                        ? std::string(address.substr(end + 2))
                        : std::string{};
        return {std::string(address.substr(1, end - 1)), std::move(port)};
      }
    }
    const auto colon = address.rfind(':');
    if (colon == std::string_view::npos || address.find(':') != colon)
      return {std::string(address), {}};
    return {std::string(address.substr(0, colon)),
            std::string(address.substr(colon + 1))};
  }

  [[nodiscard]] static GbDeviceDto deviceDto(ruvia::Context &c,
                                             const Device &value) {
    const auto [remoteIp, remotePort] = splitRemoteAddress(value.remoteAddress);
    ruvia::BoxedArray<GbChannelDto> channels(c.resource());
    for (const auto &channel : value.channels) {
      auto &dto = channels.emplace(c);
      dto.set<"id">(channel.id)
          .set<"name">(channel.displayName())
          .set<"reportedName">(channel.name)
          .set<"customName">(channel.customName)
          .set<"manufacturer">(channel.manufacturer)
          .set<"online">(channel.online)
          .set<"ptzType">(channel.ptzType)
          .set<"ptzCapable">(channel.ptzType > 0);
    }
    ruvia::BoxedArray<GbRecordDto> records(c.resource());
    for (const auto &record : value.records) {
      auto &dto = records.emplace(c);
      dto.set<"deviceId">(record.deviceId)
          .set<"name">(record.name)
          .set<"filePath">(record.filePath)
          .set<"address">(record.address)
          .set<"startTime">(record.startTime)
          .set<"endTime">(record.endTime)
          .set<"type">(record.type)
          .set<"recorderId">(record.recorderId);
    }
    GbDeviceDto result(c);
    result.set<"id">(value.id)
        .set<"name">(value.displayName())
        .set<"reportedName">(value.name)
        .set<"customName">(value.customName)
        .set<"manufacturer">(value.manufacturer)
        .set<"remoteAddress">(value.remoteAddress)
        .set<"remoteIp">(remoteIp)
        .set<"remotePort">(remotePort)
        .set<"registrationSource">(value.registrationSource)
        .set<"mappedDeviceId">(value.mappedDeviceId)
        .set<"lastSeenAt">(service::common::utcTimestamp(value.lastSeen))
        .set<"online">(value.online)
        .set<"channels">(std::move(channels))
        .set<"records">(std::move(records));
    return result;
  }

  [[nodiscard]] static GbStreamDto streamDto(ruvia::Context &c,
                                             const StreamStatus &value) {
    GbStreamDto result(c);
    result
        .set<"id">(
            StreamRegistry::identity(value.app, value.stream, value.schema))
        .set<"app">(value.app)
        .set<"stream">(value.stream)
        .set<"schema">(value.schema)
        .set<"online">(value.online)
        .set<"readerCount">(value.readerCount);
    return result;
  }
};

inline Gb28181Service &gb28181Service() {
  static Gb28181Service value;
  return value;
}

} // namespace service::gb28181
