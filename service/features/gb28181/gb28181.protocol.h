#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include <ruvia/web/Model.h>
#include <ruvia/web/ModelJson.h>

#include "service/common/uuid.h"
#include "service/utils/json.h"

namespace service::gb28181::control_protocol {

namespace stream {

// Control streams are scoped to a process incarnation.  This prevents two
// processes with the same worker index from consuming each other's commands.
inline constexpr std::string_view kProjection{ "iot:gb28181:projection" };
inline constexpr std::string_view kProjectionGroup{ "iot-engine:gb28181-projector" };
inline constexpr std::string_view kControlPrefix{ "iot:gb28181:control:worker:" };
inline constexpr std::string_view kControlGroup{ "iot-engine:gb28181-control" };
inline constexpr std::string_view kConfigKey{ "iot:state:gb28181:config" };

// New control entries always carry request_id, owner_key, owner_token,
// operation, payload, reply_stream, result_key, deadline_ms and cancel_key.
// The owner token is a process/session fence; worker index only selects the
// already owning Collector stream and never partitions a device.

inline std::string control(
    std::size_t workerIndex,
    std::string_view instance = service::runtime::instanceId()
) {
    return std::string(kControlPrefix) + std::string(instance) + ":" +
        std::to_string(workerIndex);
}

inline std::string reply(std::string_view requestId) {
    return "iot:gb28181:reply:" + std::string(requestId);
}

inline std::string result(std::string_view requestId) {
    return "iot:gb28181:result:" + std::string(requestId);
}

inline std::string cancel(std::string_view requestId) {
    return "iot:gb28181:cancel:" + std::string(requestId);
}

inline std::string owner(std::string_view deviceId) {
    return "iot:gb28181:owner:" + std::string(deviceId);
}

inline std::string sessionOwner(std::string_view sessionId) {
    return "iot:gb28181:session-owner:" + std::string(sessionId);
}

inline std::string projectionDone(std::string_view projectionId) {
    return "iot:gb28181:projection:done:" + std::string(projectionId);
}

// Owner tokens are process-incarnation fences.  The suffix is the Collector
// index and is used only to select the already owning control stream; the
// process UUID and the session suffix prevent a restarted or stale connection
// from replaying a command for a new owner.
inline std::optional<std::size_t> ownerIndex(std::string_view token) {
    constexpr std::string_view marker{ ":collector:" };
    const auto markerPosition = token.rfind(marker);
    if (markerPosition == std::string_view::npos) {
        return std::nullopt;
    }
    auto indexText = token.substr(markerPosition + marker.size());
    if (const auto separator = indexText.find(':');
        separator != std::string_view::npos) {
        indexText = indexText.substr(0, separator);
    }
    if (indexText.empty()) {
        return std::nullopt;
    }
    std::size_t index{};
    const auto [end, error] =
        std::from_chars(indexText.data(), indexText.data() + indexText.size(), index);
    if (error != std::errc{} || end != indexText.data() + indexText.size()) {
        return std::nullopt;
    }
    return index;
}

inline std::optional<std::string_view> ownerInstance(std::string_view token) {
    constexpr std::string_view marker{ ":collector:" };
    const auto markerPosition = token.rfind(marker);
    if (markerPosition == std::string_view::npos || markerPosition == 0) {
        return std::nullopt;
    }
    const auto instance = token.substr(0, markerPosition);
    if (instance.empty()) {
        return std::nullopt;
    }
    return instance;
}

inline bool completeOwnerToken(std::string_view token) {
    const auto instance = ownerInstance(token);
    if (!instance || !service::common::isUuid(*instance)) {
        return false;
    }
    constexpr std::string_view marker{ ":collector:" };
    const auto markerPosition = token.rfind(marker);
    auto suffix = token.substr(markerPosition + marker.size());
    const auto indexSeparator = suffix.find(':');
    if (indexSeparator == std::string_view::npos) {
        return false;
    }
    suffix = suffix.substr(indexSeparator + 1);
    const auto generationSeparator = suffix.find(':');
    if (generationSeparator == std::string_view::npos) {
        return false;
    }
    const auto kind = suffix.substr(0, generationSeparator);
    if (kind != "session" && kind != "stream") {
        return false;
    }
    const auto generationText = suffix.substr(generationSeparator + 1);
    if (generationText.empty()) {
        return false;
    }
    std::uint64_t generation{};
    const auto [end, error] = std::from_chars(
        generationText.data(),
        generationText.data() + generationText.size(),
        generation
    );
    return error == std::errc{} &&
        end == generationText.data() + generationText.size() &&
        generation != 0;
}

} // namespace stream

// The northbound module and the GB28181 feature communicate with a small,
// deliberately boring JSON contract.  These models are feature-owned: the
// module keeps its HTTP DTOs independent and only parses the wire shape.
RUVIA_REQUEST_MODEL(
    ControlRequest,
    RUVIA_OPTIONAL_FIELD_NAME("device_id", deviceId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("channel_id", channelId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("stream_id", streamId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("session_id", sessionId, ruvia::String),
    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("mapped_device_id", mappedDeviceId, ruvia::String),
    RUVIA_OPTIONAL_FIELD(action, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("start_time", startTime, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("end_time", endTime, ruvia::String),
    RUVIA_OPTIONAL_FIELD(speed, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(pan, ruvia::Double),
    RUVIA_OPTIONAL_FIELD(tilt, ruvia::Double),
    RUVIA_OPTIONAL_FIELD(zoom, ruvia::Double)
);

RUVIA_RESPONSE_MODEL(
    MediaPortsJson,
    RUVIA_OPTIONAL_FIELD(http, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(https, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(rtsp, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(rtsps, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(rtmp, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(rtmps, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(rtc, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(srt, ruvia::Int64)
);

RUVIA_RESPONSE_MODEL(
    MediaCapabilitiesJson,
    RUVIA_OPTIONAL_FIELD(faac, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD(ffmpeg, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD(hls, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD(mp4, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("rtp_proxy", rtpProxy, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD(srt, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD(sctp, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("web_rtc", webRtc, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD(x264, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("video_stack", videoStack, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD(tls, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD(recording, ruvia::Bool)
);

RUVIA_RESPONSE_MODEL(
    HealthJson,
    RUVIA_OPTIONAL_FIELD(status, ruvia::String),
    RUVIA_OPTIONAL_FIELD(service, ruvia::String),
    RUVIA_OPTIONAL_FIELD(enabled, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD(started, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD(error, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("media_ports", mediaPorts, MediaPortsJson),
    RUVIA_OPTIONAL_FIELD_NAME("media_capabilities", mediaCapabilities, MediaCapabilitiesJson)
);

RUVIA_RESPONSE_MODEL(
    SipConfigJson,
    RUVIA_OPTIONAL_FIELD(domain, ruvia::String),
    RUVIA_OPTIONAL_FIELD(id, ruvia::String),
    RUVIA_OPTIONAL_FIELD(host, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("public_ip", publicIp, ruvia::String),
    RUVIA_OPTIONAL_FIELD(port, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(transport, ruvia::String)
);

RUVIA_RESPONSE_MODEL(
    ChannelJson,
    RUVIA_OPTIONAL_FIELD(id, ruvia::String),
    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("reported_name", reportedName, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("custom_name", customName, ruvia::String),
    RUVIA_OPTIONAL_FIELD(manufacturer, ruvia::String),
    RUVIA_OPTIONAL_FIELD(online, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("ptz_type", ptzType, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("ptz_capable", ptzCapable, ruvia::Bool)
);

RUVIA_RESPONSE_MODEL(
    RecordJson,
    RUVIA_OPTIONAL_FIELD_NAME("device_id", deviceId, ruvia::String),
    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("file_path", filePath, ruvia::String),
    RUVIA_OPTIONAL_FIELD(address, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("start_time", startTime, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("end_time", endTime, ruvia::String),
    RUVIA_OPTIONAL_FIELD(type, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("recorder_id", recorderId, ruvia::String)
);

RUVIA_RESPONSE_MODEL(
    DeviceJson,
    RUVIA_OPTIONAL_FIELD(id, ruvia::String),
    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("reported_name", reportedName, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("custom_name", customName, ruvia::String),
    RUVIA_OPTIONAL_FIELD(manufacturer, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("remote_address", remoteAddress, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("remote_ip", remoteIp, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("remote_port", remotePort, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("registration_source", registrationSource, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("mapped_device_id", mappedDeviceId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("last_seen_at", lastSeenAt, ruvia::String),
    RUVIA_OPTIONAL_FIELD(online, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD(channels, ruvia::BoxedArray<ChannelJson>),
    RUVIA_OPTIONAL_FIELD(records, ruvia::BoxedArray<RecordJson>)
);

RUVIA_RESPONSE_MODEL(DeviceListJson, RUVIA_OPTIONAL_FIELD(items, ruvia::BoxedArray<DeviceJson>));

RUVIA_RESPONSE_MODEL(
    StreamJson,
    RUVIA_OPTIONAL_FIELD(id, ruvia::String),
    RUVIA_OPTIONAL_FIELD(app, ruvia::String),
    RUVIA_OPTIONAL_FIELD(stream, ruvia::String),
    RUVIA_OPTIONAL_FIELD(schema, ruvia::String),
    RUVIA_OPTIONAL_FIELD(online, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("reader_count", readerCount, ruvia::Int64)
);

RUVIA_RESPONSE_MODEL(StreamListJson, RUVIA_OPTIONAL_FIELD(items, ruvia::BoxedArray<StreamJson>));

RUVIA_RESPONSE_MODEL(
    PlayUrlsJson,
    RUVIA_OPTIONAL_FIELD_NAME("http_flv", httpFlv, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("ws_flv", wsFlv, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("http_ts", httpTs, ruvia::String),
    RUVIA_OPTIONAL_FIELD(hls, ruvia::String),
    RUVIA_OPTIONAL_FIELD(webrtc, ruvia::String),
    RUVIA_OPTIONAL_FIELD(rtsp, ruvia::String),
    RUVIA_OPTIONAL_FIELD(rtmp, ruvia::String)
);

RUVIA_RESPONSE_MODEL(
    PreviewStartJson,
    RUVIA_OPTIONAL_FIELD(sent, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("session_id", sessionId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("device_id", deviceId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("channel_id", channelId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("stream_id", streamId, ruvia::String),
    RUVIA_OPTIONAL_FIELD(ssrc, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("rtp_port", rtpPort, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("lease_timeout_seconds", leaseTimeoutSeconds, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("play_urls", playUrls, PlayUrlsJson)
);

RUVIA_RESPONSE_MODEL(
    PreviewStopJson,
    RUVIA_OPTIONAL_FIELD(stopped, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("session_id", sessionId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("stream_id", streamId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("bye_sent", byeSent, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("rtp_server_closed", rtpServerClosed, ruvia::Bool)
);

RUVIA_RESPONSE_MODEL(
    ActionJson,
    RUVIA_OPTIONAL_FIELD(registered, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD(sent, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("device_id", deviceId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("channel_id", channelId, ruvia::String),
    RUVIA_OPTIONAL_FIELD(action, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("mapped_device_id", mappedDeviceId, ruvia::String),
    RUVIA_OPTIONAL_FIELD(speed, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(pan, ruvia::Double),
    RUVIA_OPTIONAL_FIELD(tilt, ruvia::Double),
    RUVIA_OPTIONAL_FIELD(zoom, ruvia::Double),
    RUVIA_OPTIONAL_FIELD(recording, ruvia::Bool)
);

inline std::optional<ControlRequest> parseRequest(
    std::string_view body,
    std::pmr::memory_resource* resource
) {
    return ruvia::fromJson<ControlRequest>(body, { .resource = resource });
}

inline constexpr std::string_view kHealthOperation{ "health" };
inline constexpr std::string_view kSipConfigOperation{ "config.sip" };
inline constexpr std::string_view kDevicesOperation{ "devices" };
inline constexpr std::string_view kDeviceOperation{ "device" };
inline constexpr std::string_view kStreamsOperation{ "streams" };
inline constexpr std::string_view kStreamOperation{ "stream" };
inline constexpr std::string_view kCatalogOperation{ "catalog" };
inline constexpr std::string_view kRenameDeviceOperation{ "rename_device" };
inline constexpr std::string_view kRenameChannelOperation{ "rename_channel" };
inline constexpr std::string_view kMapOperation{ "map" };
inline constexpr std::string_view kUnmapOperation{ "unmap" };
inline constexpr std::string_view kPreviewStartOperation{ "preview.start" };
inline constexpr std::string_view kPreviewStopOperation{ "preview.stop" };
inline constexpr std::string_view kPreviewHeartbeatOperation{
    "preview.heartbeat"
};
inline constexpr std::string_view kPtzOperation{ "ptz" };
inline constexpr std::string_view kPtzPositionOperation{ "ptz.position" };
inline constexpr std::string_view kRecordsOperation{ "records" };
inline constexpr std::string_view kPlaybackStartOperation{ "playback.start" };
inline constexpr std::string_view kRecordingOperation{ "recording" };
inline constexpr std::string_view kRecordingStartOperation{ "recording.start" };
inline constexpr std::string_view kRecordingStopOperation{ "recording.stop" };

inline std::string successJson(const auto& model, std::pmr::memory_resource* resource) {
    const auto encoded = ruvia::toJson(model, { .resource = resource });
    std::string result{ "{\"code\":0,\"message\":\"ok\",\"data\":" };
    result.append(encoded.data(), encoded.size());
    result.push_back('}');
    return result;
}

inline std::string operationJson(std::string_view message) {
    return "{\"code\":0,\"message\":" + service::utils::jsonQuoted(message) +
        "}";
}

} // namespace service::gb28181::control_protocol
