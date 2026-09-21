#pragma once
#include "service/common/timestamp.h"
#include "service/common/http.h"
#include <string_view>
#include <optional>
#include <cmath>

#include <ruvia/web/Validation.h>
#include "service/utils/text.h"

#include <cstdint>
#include <string>

#include <ruvia/web/Model.h>

namespace service::gb28181 {

inline bool isGbIdentifier(const ruvia::String& value) { return !service::utils::trim(value.view()).empty(); }

RUVIA_REQUEST_MODEL(GbDeviceParams,
    RUVIA_REQUIRED_FIELD(deviceId, ruvia::String, RUVIA_MAX(128, "deviceId 长度超出限制"), RUVIA_CUSTOM("deviceId 不能为空白", isGbIdentifier)));

RUVIA_REQUEST_MODEL(GbChannelParams,
    RUVIA_REQUIRED_FIELD(deviceId, ruvia::String, RUVIA_MAX(128, "deviceId 长度超出限制"), RUVIA_CUSTOM("deviceId 不能为空白", isGbIdentifier)),
    RUVIA_REQUIRED_FIELD(channelId, ruvia::String, RUVIA_MAX(128, "channelId 长度超出限制"), RUVIA_CUSTOM("channelId 不能为空白", isGbIdentifier)));

RUVIA_REQUEST_MODEL(GbStreamParams,
    RUVIA_REQUIRED_FIELD(streamId, ruvia::String, RUVIA_MAX(128, "streamId 长度超出限制"), RUVIA_CUSTOM("streamId 不能为空白", isGbIdentifier)));

RUVIA_REQUEST_MODEL(GbSessionParams,
    RUVIA_REQUIRED_FIELD(sessionId, ruvia::String, RUVIA_MAX(128, "sessionId 长度超出限制"), RUVIA_CUSTOM("sessionId 不能为空白", isGbIdentifier)));

RUVIA_REQUEST_MODEL(GbPtzParams,
    RUVIA_REQUIRED_FIELD(deviceId, ruvia::String, RUVIA_MAX(128, "deviceId 长度超出限制"), RUVIA_CUSTOM("deviceId 不能为空白", isGbIdentifier)),
    RUVIA_REQUIRED_FIELD(channelId, ruvia::String, RUVIA_MAX(128, "channelId 长度超出限制"), RUVIA_CUSTOM("channelId 不能为空白", isGbIdentifier)),
    RUVIA_REQUIRED_FIELD(action, ruvia::String, RUVIA_MAX(128, "action 长度超出限制"), RUVIA_CUSTOM("action 不能为空白", isGbIdentifier), RUVIA_ONE_OF("不支持的云台动作", "left", "right", "up", "down", "zoomin", "zoomout", "stop")));

inline bool isGbMappingId(const ruvia::String& value) {
    return service::common::isUuid(service::utils::trim(value.view()));
}

inline bool isGbRecordTimestamp(const ruvia::String& value) {
    return !service::common::canonicalUtcTimestamp(service::utils::trim(value.view())).empty();
}

inline bool isGbFiniteNumber(const ruvia::Double& value) {
    return std::isfinite(static_cast<double>(value));
}

RUVIA_REQUEST_MODEL(GbNameBody,
    RUVIA_REQUIRED_FIELD(name, ruvia::String, RUVIA_MAX(255, "name 长度超出限制"), RUVIA_CUSTOM("name 不能为空白", isGbIdentifier)));

RUVIA_REQUEST_MODEL(GbMappingBody,
    RUVIA_REQUIRED_FIELD_NAME("mapped_device_id", mappedDeviceId, ruvia::String, RUVIA_MAX(128, "mapped_device_id 长度超出限制"), RUVIA_CUSTOM("mapped_device_id 必须是 UUID", isGbMappingId)));

RUVIA_REQUEST_MODEL(GbPtzBody,
    RUVIA_OPTIONAL_FIELD(speed, ruvia::Int64, RUVIA_NULLABLE, RUVIA_DEFAULT(80), RUVIA_MIN(0, "speed 必须在 0 - 255 之间"), RUVIA_MAX(255, "speed 必须在 0 - 255 之间")));

RUVIA_REQUEST_MODEL(GbPositionBody,
    RUVIA_REQUIRED_FIELD(pan, ruvia::Double, RUVIA_MIN(0, "pan 必须在 0 - 360 之间"), RUVIA_MAX(360, "pan 必须在 0 - 360 之间"), RUVIA_CUSTOM("pan 必须是有限数字", isGbFiniteNumber)),
    RUVIA_REQUIRED_FIELD(tilt, ruvia::Double, RUVIA_MIN(-30, "tilt 必须在 -30 - 90 之间"), RUVIA_MAX(90, "tilt 必须在 -30 - 90 之间"), RUVIA_CUSTOM("tilt 必须是有限数字", isGbFiniteNumber)),
    RUVIA_REQUIRED_FIELD(zoom, ruvia::Double, RUVIA_MIN(1, "zoom 必须在 1 - 1000 之间"), RUVIA_MAX(1000, "zoom 必须在 1 - 1000 之间"), RUVIA_CUSTOM("zoom 必须是有限数字", isGbFiniteNumber)));

RUVIA_REQUEST_MODEL(GbRecordBody,
    RUVIA_REQUIRED_FIELD_NAME("start_time", startTime, ruvia::String, RUVIA_MAX(128, "start_time 长度超出限制"), RUVIA_CUSTOM("start_time 必须是有效的 RFC 3339 时间", isGbRecordTimestamp)),
    RUVIA_REQUIRED_FIELD_NAME("end_time", endTime, ruvia::String, RUVIA_MAX(128, "end_time 长度超出限制"), RUVIA_CUSTOM("end_time 必须是有效的 RFC 3339 时间", isGbRecordTimestamp)));

RUVIA_RESPONSE_MODEL(GbMediaPortsDto, RUVIA_OPTIONAL_FIELD(http, ruvia::Int64), RUVIA_OPTIONAL_FIELD(https, ruvia::Int64), RUVIA_OPTIONAL_FIELD(rtsp, ruvia::Int64), RUVIA_OPTIONAL_FIELD(rtsps, ruvia::Int64), RUVIA_OPTIONAL_FIELD(rtmp, ruvia::Int64), RUVIA_OPTIONAL_FIELD(rtmps, ruvia::Int64), RUVIA_OPTIONAL_FIELD(rtc, ruvia::Int64), RUVIA_OPTIONAL_FIELD(srt, ruvia::Int64));

RUVIA_RESPONSE_MODEL(
    GbMediaCapabilitiesDto,
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

RUVIA_RESPONSE_MODEL(GbHealthDto, RUVIA_OPTIONAL_FIELD(status, ruvia::String), RUVIA_OPTIONAL_FIELD(service, ruvia::String), RUVIA_OPTIONAL_FIELD(enabled, ruvia::Bool), RUVIA_OPTIONAL_FIELD(started, ruvia::Bool), RUVIA_OPTIONAL_FIELD(error, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("media_ports", mediaPorts, GbMediaPortsDto), RUVIA_OPTIONAL_FIELD_NAME("media_capabilities", mediaCapabilities, GbMediaCapabilitiesDto));

RUVIA_RESPONSE_MODEL(GbSipConfigDto, RUVIA_OPTIONAL_FIELD(domain, ruvia::String), RUVIA_OPTIONAL_FIELD(id, ruvia::String), RUVIA_OPTIONAL_FIELD(host, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("public_ip", publicIp, ruvia::String), RUVIA_OPTIONAL_FIELD(port, ruvia::Int64), RUVIA_OPTIONAL_FIELD(transport, ruvia::String));

RUVIA_RESPONSE_MODEL(
    GbChannelDto,
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
    GbRecordDto,
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
    GbDeviceDto,
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
    RUVIA_OPTIONAL_FIELD(channels, ruvia::BoxedArray<GbChannelDto>),
    RUVIA_OPTIONAL_FIELD(records, ruvia::BoxedArray<GbRecordDto>)
);

RUVIA_RESPONSE_MODEL(GbDeviceListDto, RUVIA_OPTIONAL_FIELD(items, ruvia::BoxedArray<GbDeviceDto>));

RUVIA_RESPONSE_MODEL(GbStreamDto, RUVIA_OPTIONAL_FIELD(id, ruvia::String), RUVIA_OPTIONAL_FIELD(app, ruvia::String), RUVIA_OPTIONAL_FIELD(stream, ruvia::String), RUVIA_OPTIONAL_FIELD(schema, ruvia::String), RUVIA_OPTIONAL_FIELD(online, ruvia::Bool), RUVIA_OPTIONAL_FIELD_NAME("reader_count", readerCount, ruvia::Int64));

RUVIA_RESPONSE_MODEL(GbStreamListDto, RUVIA_OPTIONAL_FIELD(items, ruvia::BoxedArray<GbStreamDto>));

RUVIA_RESPONSE_MODEL(GbPlayUrlsDto, RUVIA_OPTIONAL_FIELD_NAME("http_flv", httpFlv, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("ws_flv", wsFlv, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("http_ts", httpTs, ruvia::String), RUVIA_OPTIONAL_FIELD(hls, ruvia::String), RUVIA_OPTIONAL_FIELD(webrtc, ruvia::String), RUVIA_OPTIONAL_FIELD(rtsp, ruvia::String), RUVIA_OPTIONAL_FIELD(rtmp, ruvia::String));

RUVIA_RESPONSE_MODEL(
    GbPreviewStartDto,
    RUVIA_OPTIONAL_FIELD(sent, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("session_id", sessionId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("device_id", deviceId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("channel_id", channelId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("stream_id", streamId, ruvia::String),
    RUVIA_OPTIONAL_FIELD(ssrc, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("rtp_port", rtpPort, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("lease_timeout_seconds", leaseTimeoutSeconds, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("play_urls", playUrls, GbPlayUrlsDto)
);

RUVIA_RESPONSE_MODEL(
    GbPreviewStopDto,
    RUVIA_OPTIONAL_FIELD(stopped, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("session_id", sessionId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("stream_id", streamId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("bye_sent", byeSent, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("rtp_server_closed", rtpServerClosed, ruvia::Bool)
);

RUVIA_RESPONSE_MODEL(GbActionDto, RUVIA_OPTIONAL_FIELD(registered, ruvia::Bool), RUVIA_OPTIONAL_FIELD(sent, ruvia::Bool), RUVIA_OPTIONAL_FIELD_NAME("device_id", deviceId, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("channel_id", channelId, ruvia::String), RUVIA_OPTIONAL_FIELD(action, ruvia::String), RUVIA_OPTIONAL_FIELD_NAME("mapped_device_id", mappedDeviceId, ruvia::String), RUVIA_OPTIONAL_FIELD(speed, ruvia::Int64), RUVIA_OPTIONAL_FIELD(pan, ruvia::Double), RUVIA_OPTIONAL_FIELD(tilt, ruvia::Double), RUVIA_OPTIONAL_FIELD(zoom, ruvia::Double), RUVIA_OPTIONAL_FIELD(recording, ruvia::Bool));

// The feature RPC uses the same wire names as the public DTOs, but these
// request models deliberately belong to the module.  Keeping the decode side
// here avoids making the feature's runtime models part of the domain API.
namespace rpc_wire {

RUVIA_REQUEST_MODEL(
    MediaPorts,
    RUVIA_OPTIONAL_FIELD(http, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(https, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(rtsp, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(rtsps, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(rtmp, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(rtmps, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(rtc, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(srt, ruvia::Int64)
);

RUVIA_REQUEST_MODEL(
    MediaCapabilities,
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

RUVIA_REQUEST_MODEL(
    Health,
    RUVIA_OPTIONAL_FIELD(status, ruvia::String),
    RUVIA_OPTIONAL_FIELD(service, ruvia::String),
    RUVIA_OPTIONAL_FIELD(enabled, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD(started, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD(error, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("media_ports", mediaPorts, MediaPorts),
    RUVIA_OPTIONAL_FIELD_NAME("media_capabilities", mediaCapabilities, MediaCapabilities)
);

RUVIA_REQUEST_MODEL(
    SipConfig,
    RUVIA_OPTIONAL_FIELD(domain, ruvia::String),
    RUVIA_OPTIONAL_FIELD(id, ruvia::String),
    RUVIA_OPTIONAL_FIELD(host, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("public_ip", publicIp, ruvia::String),
    RUVIA_OPTIONAL_FIELD(port, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(transport, ruvia::String)
);

RUVIA_REQUEST_MODEL(
    Channel,
    RUVIA_OPTIONAL_FIELD(id, ruvia::String),
    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("reported_name", reportedName, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("custom_name", customName, ruvia::String),
    RUVIA_OPTIONAL_FIELD(manufacturer, ruvia::String),
    RUVIA_OPTIONAL_FIELD(online, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("ptz_type", ptzType, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("ptz_capable", ptzCapable, ruvia::Bool)
);

RUVIA_REQUEST_MODEL(
    Record,
    RUVIA_OPTIONAL_FIELD_NAME("device_id", deviceId, ruvia::String),
    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("file_path", filePath, ruvia::String),
    RUVIA_OPTIONAL_FIELD(address, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("start_time", startTime, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("end_time", endTime, ruvia::String),
    RUVIA_OPTIONAL_FIELD(type, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("recorder_id", recorderId, ruvia::String)
);

RUVIA_REQUEST_MODEL(
    Device,
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
    RUVIA_OPTIONAL_FIELD(channels, ruvia::Array<Channel>),
    RUVIA_OPTIONAL_FIELD(records, ruvia::Array<Record>)
);

RUVIA_REQUEST_MODEL(DeviceList, RUVIA_OPTIONAL_FIELD(items, ruvia::Array<Device>));

RUVIA_REQUEST_MODEL(
    Stream,
    RUVIA_OPTIONAL_FIELD(id, ruvia::String),
    RUVIA_OPTIONAL_FIELD(app, ruvia::String),
    RUVIA_OPTIONAL_FIELD(stream, ruvia::String),
    RUVIA_OPTIONAL_FIELD(schema, ruvia::String),
    RUVIA_OPTIONAL_FIELD(online, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("reader_count", readerCount, ruvia::Int64)
);

RUVIA_REQUEST_MODEL(StreamList, RUVIA_OPTIONAL_FIELD(items, ruvia::Array<Stream>));

RUVIA_REQUEST_MODEL(
    PlayUrls,
    RUVIA_OPTIONAL_FIELD_NAME("http_flv", httpFlv, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("ws_flv", wsFlv, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("http_ts", httpTs, ruvia::String),
    RUVIA_OPTIONAL_FIELD(hls, ruvia::String),
    RUVIA_OPTIONAL_FIELD(webrtc, ruvia::String),
    RUVIA_OPTIONAL_FIELD(rtsp, ruvia::String),
    RUVIA_OPTIONAL_FIELD(rtmp, ruvia::String)
);

RUVIA_REQUEST_MODEL(
    PreviewStart,
    RUVIA_OPTIONAL_FIELD(sent, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("session_id", sessionId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("device_id", deviceId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("channel_id", channelId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("stream_id", streamId, ruvia::String),
    RUVIA_OPTIONAL_FIELD(ssrc, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("rtp_port", rtpPort, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("lease_timeout_seconds", leaseTimeoutSeconds, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("play_urls", playUrls, PlayUrls)
);

RUVIA_REQUEST_MODEL(
    PreviewStop,
    RUVIA_OPTIONAL_FIELD(stopped, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("session_id", sessionId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("stream_id", streamId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("bye_sent", byeSent, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("rtp_server_closed", rtpServerClosed, ruvia::Bool)
);

RUVIA_REQUEST_MODEL(
    Action,
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

#define GB28181_RPC_RESPONSE(name, dataType) \
    RUVIA_REQUEST_MODEL(name, RUVIA_OPTIONAL_FIELD(code, ruvia::Int64), RUVIA_OPTIONAL_FIELD(message, ruvia::String), RUVIA_OPTIONAL_FIELD(data, dataType))

GB28181_RPC_RESPONSE(HealthResponse, Health);
GB28181_RPC_RESPONSE(SipConfigResponse, SipConfig);
GB28181_RPC_RESPONSE(DeviceListResponse, DeviceList);
GB28181_RPC_RESPONSE(DeviceResponse, Device);
GB28181_RPC_RESPONSE(StreamListResponse, StreamList);
GB28181_RPC_RESPONSE(StreamResponse, Stream);
GB28181_RPC_RESPONSE(PreviewStartResponse, PreviewStart);
GB28181_RPC_RESPONSE(PreviewStopResponse, PreviewStop);
GB28181_RPC_RESPONSE(ActionResponse, Action);

#undef GB28181_RPC_RESPONSE

} // namespace rpc_wire



RUVIA_RESPONSE_MODEL(GbActionResponse,
    RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::String),
    RUVIA_REQUIRED_FIELD(data, GbActionDto));

RUVIA_RESPONSE_MODEL(GbPreviewStartResponse,
    RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::String),
    RUVIA_REQUIRED_FIELD(data, GbPreviewStartDto));

RUVIA_RESPONSE_MODEL(GbPreviewStopResponse,
    RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::String),
    RUVIA_REQUIRED_FIELD(data, GbPreviewStopDto));

RUVIA_RESPONSE_MODEL(GbStreamResponse,
    RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::String),
    RUVIA_REQUIRED_FIELD(data, GbStreamDto));

RUVIA_RESPONSE_MODEL(GbStreamListResponse,
    RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::String),
    RUVIA_REQUIRED_FIELD(data, GbStreamListDto));

RUVIA_RESPONSE_MODEL(GbDeviceResponse,
    RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::String),
    RUVIA_REQUIRED_FIELD(data, GbDeviceDto));

RUVIA_RESPONSE_MODEL(GbDeviceListResponse,
    RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::String),
    RUVIA_REQUIRED_FIELD(data, GbDeviceListDto));

RUVIA_RESPONSE_MODEL(GbSipConfigResponse,
    RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::String),
    RUVIA_REQUIRED_FIELD(data, GbSipConfigDto));

RUVIA_RESPONSE_MODEL(GbHealthResponse,
    RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::String),
    RUVIA_REQUIRED_FIELD(data, GbHealthDto));

} // namespace service::gb28181
