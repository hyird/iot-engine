#pragma once

#include <future>
#include <string>
#include <string_view>
#include <utility>

#include <ruvia/web/Context.h>

#include "service/common/http.h"
#include "service/domains/gb28181/gb28181.types.h"
#include "service/features/gb28181/runtime.h"

namespace service::gb28181 {

class Gb28181Service final {
  public:
    [[nodiscard]] GbHealthDto health(ruvia::Context& c) const {
        const auto& instance = runtime();
        GbHealthDto result(c);
        result.status(instance.started() ? "ok"
                                         : (instance.enabled() ? "error" : "disabled"))
            .service("iot-engine-gb28181")
            .enabled(instance.enabled())
            .started(instance.started())
            .error(instance.lastError());
        return result;
    }

    [[nodiscard]] GbSipConfigDto sipConfig(ruvia::Context& c) const {
        const auto& config = runtime().config().sip;
        GbSipConfigDto result(c);
        result.domain(config.domain)
            .id(config.id)
            .host(config.host)
            .publicIp(config.publicIp)
            .port(config.port)
            .transport(config.transport);
        return result;
    }

    [[nodiscard]] GbDeviceListDto devices(ruvia::Context& c) const {
        const auto values = runtime().devices();
        ruvia::List<GbDeviceDto> items(c.resource());
        for (const auto& value : values)
            items.emplace(deviceDto(c, value));
        GbDeviceListDto result(c);
        result.items(std::move(items));
        return result;
    }

    [[nodiscard]] GbDeviceDto device(ruvia::Context& c, std::string_view id) const {
        const auto value = runtime().device(id);
        if (!value)
            service::common::fail(10003, "设备不存在", 404);
        return deviceDto(c, *value);
    }

    [[nodiscard]] GbStreamListDto streams(ruvia::Context& c) const {
        const auto values = runtime().streams();
        ruvia::List<GbStreamDto> items(c.resource());
        for (const auto& value : values)
            items.emplace(streamDto(c, value));
        GbStreamListDto result(c);
        result.items(std::move(items));
        return result;
    }

    [[nodiscard]] GbStreamDto stream(ruvia::Context& c, std::string_view id) const {
        const auto value = runtime().stream(id);
        if (!value)
            service::common::fail(10003, "流不存在", 404);
        return streamDto(c, *value);
    }

    void mockRegister(std::string deviceId, std::string remoteAddress) const {
        runtime().mockRegister(std::move(deviceId), std::move(remoteAddress));
    }

    ruvia::Task<void> queryCatalog(ruvia::Context& c, std::string deviceId) const {
        if (!co_await Runtime::wait(c, runtime().queryCatalog(std::move(deviceId))))
            service::common::fail(10003, "设备不在线", 404);
    }

    ruvia::Task<void> queryRecords(ruvia::Context& c, std::string deviceId,
                                   std::string channelId, std::string startTime,
                                   std::string endTime) const {
        if (!co_await Runtime::wait(
                c, runtime().queryRecords(std::move(deviceId), std::move(channelId),
                                          std::move(startTime), std::move(endTime))))
            service::common::fail(10003, "设备或通道不可用", 404);
    }

    ruvia::Task<void> ptz(ruvia::Context& c, std::string deviceId,
                           std::string channelId, std::string action,
                           std::uint8_t speed) const {
        if (!co_await Runtime::wait(
                c, runtime().ptz(std::move(deviceId), std::move(channelId),
                                  std::move(action), speed)))
            service::common::fail(10003, "设备或通道不可用", 404);
    }

    ruvia::Task<void> ptzPosition(ruvia::Context& c, std::string deviceId,
                                  std::string channelId, double pan, double tilt,
                                  double zoom) const {
        if (!co_await Runtime::wait(
                c, runtime().ptzPosition(std::move(deviceId), std::move(channelId), pan,
                                          tilt, zoom)))
            service::common::fail(10003, "设备或通道不可用", 404);
    }

    [[nodiscard]] static GbPreviewStartDto previewStart(
        ruvia::Context& c, const SipServer::PreviewStartResult& value) {
        GbPlayUrlsDto urls(c);
        urls.httpFlv(value.playUrls.httpFlv)
            .wsFlv(value.playUrls.wsFlv)
            .httpTs(value.playUrls.httpTs)
            .hls(value.playUrls.hls)
            .webrtc(value.playUrls.webRtc)
            .rtsp(value.playUrls.rtsp);
        GbPreviewStartDto result(c);
        result.sent(true)
            .sessionId(value.sessionId)
            .deviceId(value.deviceId)
            .channelId(value.channelId)
            .streamId(value.streamId)
            .ssrc(value.ssrc)
            .rtpPort(value.rtpPort)
            .playUrls(std::move(urls));
        return result;
    }

    [[nodiscard]] static GbPreviewStopDto previewStop(
        ruvia::Context& c, const SipServer::PreviewStopResult& value) {
        GbPreviewStopDto result(c);
        result.stopped(true)
            .sessionId(value.sessionId)
            .streamId(value.streamId)
            .byeSent(value.byeSent)
            .rtpServerClosed(value.rtpServerClosed);
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

    [[nodiscard]] static GbDeviceDto deviceDto(ruvia::Context& c, const Device& value) {
        const auto [remoteIp, remotePort] = splitRemoteAddress(value.remoteAddress);
        ruvia::List<GbChannelDto> channels(c.resource());
        for (const auto& channel : value.channels) {
            auto& dto = channels.emplace(c);
            dto.id(channel.id)
                .name(channel.name)
                .manufacturer(channel.manufacturer)
                .online(channel.online)
                .ptzType(channel.ptzType)
                .ptzCapable(channel.ptzType > 0);
        }
        ruvia::List<GbRecordDto> records(c.resource());
        for (const auto& record : value.records) {
            auto& dto = records.emplace(c);
            dto.deviceId(record.deviceId)
                .name(record.name)
                .filePath(record.filePath)
                .address(record.address)
                .startTime(record.startTime)
                .endTime(record.endTime)
                .type(record.type)
                .recorderId(record.recorderId);
        }
        GbDeviceDto result(c);
        result.id(value.id)
            .name(value.name)
            .manufacturer(value.manufacturer)
            .remoteAddress(value.remoteAddress)
            .remoteIp(remoteIp)
            .remotePort(remotePort)
            .registrationSource(value.registrationSource)
            .online(value.online)
            .channels(std::move(channels))
            .records(std::move(records));
        return result;
    }

    [[nodiscard]] static GbStreamDto streamDto(ruvia::Context& c,
                                                const StreamStatus& value) {
        GbStreamDto result(c);
        result.app(value.app)
            .stream(value.stream)
            .schema(value.schema)
            .online(value.online)
            .readerCount(value.readerCount);
        return result;
    }
};

inline Gb28181Service& gb28181Service() {
    static Gb28181Service value;
    return value;
}

} // namespace service::gb28181
