#pragma once

#include <ruvia/web/Model.h>

namespace service::gb28181 {

RUVIA_REQUEST_MODEL(GbNameBody, RUVIA_OPTIONAL_FIELD(name, ruvia::String));

RUVIA_RESPONSE_MODEL(GbMediaPortsDto, RUVIA_OPTIONAL_FIELD(http, ruvia::Int64),
                     RUVIA_OPTIONAL_FIELD(https, ruvia::Int64),
                     RUVIA_OPTIONAL_FIELD(rtsp, ruvia::Int64),
                     RUVIA_OPTIONAL_FIELD(rtsps, ruvia::Int64),
                     RUVIA_OPTIONAL_FIELD(rtmp, ruvia::Int64),
                     RUVIA_OPTIONAL_FIELD(rtmps, ruvia::Int64),
                     RUVIA_OPTIONAL_FIELD(rtc, ruvia::Int64),
                     RUVIA_OPTIONAL_FIELD(srt, ruvia::Int64));

RUVIA_RESPONSE_MODEL(
    GbMediaCapabilitiesDto, RUVIA_OPTIONAL_FIELD(faac, ruvia::Bool),
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
    RUVIA_OPTIONAL_FIELD(recording, ruvia::Bool));

RUVIA_RESPONSE_MODEL(GbHealthDto, RUVIA_OPTIONAL_FIELD(status, ruvia::String),
                     RUVIA_OPTIONAL_FIELD(service, ruvia::String),
                     RUVIA_OPTIONAL_FIELD(enabled, ruvia::Bool),
                     RUVIA_OPTIONAL_FIELD(started, ruvia::Bool),
                     RUVIA_OPTIONAL_FIELD(error, ruvia::String),
                     RUVIA_OPTIONAL_FIELD_NAME("media_ports", mediaPorts,
                                               GbMediaPortsDto),
                     RUVIA_OPTIONAL_FIELD_NAME("media_capabilities",
                                               mediaCapabilities,
                                               GbMediaCapabilitiesDto));

RUVIA_RESPONSE_MODEL(GbSipConfigDto,
                     RUVIA_OPTIONAL_FIELD(domain, ruvia::String),
                     RUVIA_OPTIONAL_FIELD(id, ruvia::String),
                     RUVIA_OPTIONAL_FIELD(host, ruvia::String),
                     RUVIA_OPTIONAL_FIELD_NAME("public_ip", publicIp,
                                               ruvia::String),
                     RUVIA_OPTIONAL_FIELD(port, ruvia::Int64),
                     RUVIA_OPTIONAL_FIELD(transport, ruvia::String));

RUVIA_RESPONSE_MODEL(
    GbChannelDto, RUVIA_OPTIONAL_FIELD(id, ruvia::String),
    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("reported_name", reportedName, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("custom_name", customName, ruvia::String),
    RUVIA_OPTIONAL_FIELD(manufacturer, ruvia::String),
    RUVIA_OPTIONAL_FIELD(online, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("ptz_type", ptzType, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("ptz_capable", ptzCapable, ruvia::Bool));

RUVIA_RESPONSE_MODEL(
    GbRecordDto,
    RUVIA_OPTIONAL_FIELD_NAME("device_id", deviceId, ruvia::String),
    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("file_path", filePath, ruvia::String),
    RUVIA_OPTIONAL_FIELD(address, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("start_time", startTime, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("end_time", endTime, ruvia::String),
    RUVIA_OPTIONAL_FIELD(type, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("recorder_id", recorderId, ruvia::String));

RUVIA_RESPONSE_MODEL(
    GbDeviceDto, RUVIA_OPTIONAL_FIELD(id, ruvia::String),
    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("reported_name", reportedName, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("custom_name", customName, ruvia::String),
    RUVIA_OPTIONAL_FIELD(manufacturer, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("remote_address", remoteAddress, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("remote_ip", remoteIp, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("remote_port", remotePort, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("registration_source", registrationSource,
                              ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("mapped_device_id", mappedDeviceId,
                              ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("last_seen_at", lastSeenAt, ruvia::String),
    RUVIA_OPTIONAL_FIELD(online, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD(channels, ruvia::BoxedArray<GbChannelDto>),
    RUVIA_OPTIONAL_FIELD(records, ruvia::BoxedArray<GbRecordDto>));

RUVIA_RESPONSE_MODEL(GbDeviceListDto,
                     RUVIA_OPTIONAL_FIELD(items,
                                          ruvia::BoxedArray<GbDeviceDto>));

RUVIA_RESPONSE_MODEL(GbStreamDto, RUVIA_OPTIONAL_FIELD(id, ruvia::String),
                     RUVIA_OPTIONAL_FIELD(app, ruvia::String),
                     RUVIA_OPTIONAL_FIELD(stream, ruvia::String),
                     RUVIA_OPTIONAL_FIELD(schema, ruvia::String),
                     RUVIA_OPTIONAL_FIELD(online, ruvia::Bool),
                     RUVIA_OPTIONAL_FIELD_NAME("reader_count", readerCount,
                                               ruvia::Int64));

RUVIA_RESPONSE_MODEL(GbStreamListDto,
                     RUVIA_OPTIONAL_FIELD(items,
                                          ruvia::BoxedArray<GbStreamDto>));

RUVIA_RESPONSE_MODEL(GbPlayUrlsDto,
                     RUVIA_OPTIONAL_FIELD_NAME("http_flv", httpFlv,
                                               ruvia::String),
                     RUVIA_OPTIONAL_FIELD_NAME("ws_flv", wsFlv, ruvia::String),
                     RUVIA_OPTIONAL_FIELD_NAME("http_ts", httpTs,
                                               ruvia::String),
                     RUVIA_OPTIONAL_FIELD(hls, ruvia::String),
                     RUVIA_OPTIONAL_FIELD(webrtc, ruvia::String),
                     RUVIA_OPTIONAL_FIELD(rtsp, ruvia::String),
                     RUVIA_OPTIONAL_FIELD(rtmp, ruvia::String));

RUVIA_RESPONSE_MODEL(
    GbPreviewStartDto, RUVIA_OPTIONAL_FIELD(sent, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("session_id", sessionId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("device_id", deviceId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("channel_id", channelId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("stream_id", streamId, ruvia::String),
    RUVIA_OPTIONAL_FIELD(ssrc, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("rtp_port", rtpPort, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("lease_timeout_seconds", leaseTimeoutSeconds,
                              ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("play_urls", playUrls, GbPlayUrlsDto));

RUVIA_RESPONSE_MODEL(
    GbPreviewStopDto, RUVIA_OPTIONAL_FIELD(stopped, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("session_id", sessionId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("stream_id", streamId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("bye_sent", byeSent, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("rtp_server_closed", rtpServerClosed,
                              ruvia::Bool));

RUVIA_RESPONSE_MODEL(GbActionDto, RUVIA_OPTIONAL_FIELD(registered, ruvia::Bool),
                     RUVIA_OPTIONAL_FIELD(sent, ruvia::Bool),
                     RUVIA_OPTIONAL_FIELD_NAME("device_id", deviceId,
                                               ruvia::String),
                     RUVIA_OPTIONAL_FIELD_NAME("channel_id", channelId,
                                               ruvia::String),
                     RUVIA_OPTIONAL_FIELD(action, ruvia::String),
                     RUVIA_OPTIONAL_FIELD_NAME("mapped_device_id",
                                               mappedDeviceId, ruvia::String),
                     RUVIA_OPTIONAL_FIELD(speed, ruvia::Int64),
                     RUVIA_OPTIONAL_FIELD(pan, ruvia::Double),
                     RUVIA_OPTIONAL_FIELD(tilt, ruvia::Double),
                     RUVIA_OPTIONAL_FIELD(zoom, ruvia::Double),
                     RUVIA_OPTIONAL_FIELD(recording, ruvia::Bool));

#define GB28181_RESPONSE(name, dataType)                                       \
  RUVIA_RESPONSE_MODEL(name, RUVIA_OPTIONAL_FIELD(code, ruvia::Int64),         \
                       RUVIA_OPTIONAL_FIELD(message, ruvia::String),           \
                       RUVIA_OPTIONAL_FIELD(data, dataType))

GB28181_RESPONSE(GbHealthResponse, GbHealthDto);
GB28181_RESPONSE(GbSipConfigResponse, GbSipConfigDto);
GB28181_RESPONSE(GbDeviceListResponse, GbDeviceListDto);
GB28181_RESPONSE(GbDeviceResponse, GbDeviceDto);
GB28181_RESPONSE(GbStreamListResponse, GbStreamListDto);
GB28181_RESPONSE(GbStreamResponse, GbStreamDto);
GB28181_RESPONSE(GbPreviewStartResponse, GbPreviewStartDto);
GB28181_RESPONSE(GbPreviewStopResponse, GbPreviewStopDto);
GB28181_RESPONSE(GbActionResponse, GbActionDto);

#undef GB28181_RESPONSE

} // namespace service::gb28181
