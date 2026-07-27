#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>

#include "service/features/gb28181/device/Device.h"
#include "service/features/gb28181/media/MediaTypes.h"
#include "service/features/gb28181/media/StreamRegistry.h"
#include "service/features/gb28181/sip/SipServer.h"

namespace service::gb28181 {

inline std::string jsonEscape(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 8);
    static constexpr char digits[] = "0123456789abcdef";
    for (const auto ch : value) {
        switch (ch) {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\b':
            result += "\\b";
            break;
        case '\f':
            result += "\\f";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20U) {
                result += "\\u00";
                result.push_back(digits[(static_cast<unsigned char>(ch) >> 4U) & 0x0FU]);
                result.push_back(digits[static_cast<unsigned char>(ch) & 0x0FU]);
            } else {
                result.push_back(ch);
            }
        }
    }
    return result;
}

inline std::string jsonQuoted(std::string_view value) {
    return "\"" + jsonEscape(value) + "\"";
}

inline std::pair<std::string, std::string> splitRemoteAddress(std::string_view address) {
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
    return {std::string(address.substr(0, colon)), std::string(address.substr(colon + 1))};
}

inline std::string channelJson(const Channel& channel) {
    return "{\"id\":" + jsonQuoted(channel.id) + ",\"name\":" + jsonQuoted(channel.name) +
           ",\"manufacturer\":" + jsonQuoted(channel.manufacturer) +
           ",\"online\":" + (channel.online ? "true" : "false") +
           ",\"ptz_type\":" + std::to_string(channel.ptzType) +
           ",\"ptz_capable\":" + (channel.ptzType > 0 ? "true" : "false") + "}";
}

inline std::string recordJson(const RecordItem& record) {
    return "{\"device_id\":" + jsonQuoted(record.deviceId) +
           ",\"name\":" + jsonQuoted(record.name) +
           ",\"file_path\":" + jsonQuoted(record.filePath) +
           ",\"address\":" + jsonQuoted(record.address) +
           ",\"start_time\":" + jsonQuoted(record.startTime) +
           ",\"end_time\":" + jsonQuoted(record.endTime) +
           ",\"type\":" + jsonQuoted(record.type) +
           ",\"recorder_id\":" + jsonQuoted(record.recorderId) + "}";
}

inline std::string deviceJson(const Device& device) {
    const auto [remoteIp, remotePort] = splitRemoteAddress(device.remoteAddress);
    std::string channels{"["};
    for (std::size_t index = 0; index < device.channels.size(); ++index) {
        if (index)
            channels.push_back(',');
        channels += channelJson(device.channels[index]);
    }
    channels.push_back(']');

    std::string records{"["};
    for (std::size_t index = 0; index < device.records.size(); ++index) {
        if (index)
            records.push_back(',');
        records += recordJson(device.records[index]);
    }
    records.push_back(']');

    return "{\"id\":" + jsonQuoted(device.id) + ",\"name\":" + jsonQuoted(device.name) +
           ",\"manufacturer\":" + jsonQuoted(device.manufacturer) +
           ",\"remote_address\":" + jsonQuoted(device.remoteAddress) +
           ",\"remote_ip\":" + jsonQuoted(remoteIp) +
           ",\"remote_port\":" + jsonQuoted(remotePort) +
           ",\"registration_source\":" + jsonQuoted(device.registrationSource) +
           ",\"online\":" + (device.online ? "true" : "false") +
           ",\"channels\":" + channels + ",\"records\":" + records + "}";
}

inline std::string streamJson(const StreamStatus& stream) {
    return "{\"app\":" + jsonQuoted(stream.app) + ",\"stream\":" + jsonQuoted(stream.stream) +
           ",\"schema\":" + jsonQuoted(stream.schema) +
           ",\"online\":" + (stream.online ? "true" : "false") +
           ",\"reader_count\":" + std::to_string(stream.readerCount) + "}";
}

inline std::string playUrlsJson(const PlayUrls& urls) {
    return "{\"http_flv\":" + jsonQuoted(urls.httpFlv) +
           ",\"ws_flv\":" + jsonQuoted(urls.wsFlv) +
           ",\"http_ts\":" + jsonQuoted(urls.httpTs) + ",\"hls\":" + jsonQuoted(urls.hls) +
           ",\"webrtc\":" + jsonQuoted(urls.webRtc) + ",\"rtsp\":" + jsonQuoted(urls.rtsp) +
           "}";
}

inline std::string previewJson(const SipServer::PreviewStartResult& result) {
    return "{\"sent\":true,\"session_id\":" + jsonQuoted(result.sessionId) +
           ",\"device_id\":" + jsonQuoted(result.deviceId) +
           ",\"channel_id\":" + jsonQuoted(result.channelId) +
           ",\"stream_id\":" + jsonQuoted(result.streamId) +
           ",\"ssrc\":" + jsonQuoted(result.ssrc) +
           ",\"rtp_port\":" + std::to_string(result.rtpPort) +
           ",\"play_urls\":" + playUrlsJson(result.playUrls) + "}";
}

inline std::string previewStopJson(const SipServer::PreviewStopResult& result) {
    return "{\"stopped\":true,\"session_id\":" + jsonQuoted(result.sessionId) +
           ",\"stream_id\":" + jsonQuoted(result.streamId) +
           ",\"bye_sent\":" + (result.byeSent ? "true" : "false") +
           ",\"rtp_server_closed\":" + (result.rtpServerClosed ? "true" : "false") + "}";
}

inline std::string jsonNumber(double value) {
    std::ostringstream output;
    output.precision(15);
    output << value;
    return output.str();
}

} // namespace service::gb28181
