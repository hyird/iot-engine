#pragma once

#include <ruvia/web/Model.h>

namespace service::gb28181 {

struct GbMediaPortsDto final {
    RUVIA_OPTIONAL_FIELD(http, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(https, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(rtsp, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(rtsps, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(rtmp, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(rtmps, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(rtc, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(srt, ruvia::Int64);
    RUVIA_MODEL(GbMediaPortsDto, http, https, rtsp, rtsps, rtmp, rtmps, rtc, srt);
};

struct GbMediaCapabilitiesDto final {
    RUVIA_OPTIONAL_FIELD(faac, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(ffmpeg, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(hls, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(mp4, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("rtp_proxy", rtpProxy, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(srt, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(sctp, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("web_rtc", webRtc, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(x264, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("video_stack", videoStack, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(tls, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(recording, ruvia::Bool);
    RUVIA_MODEL(GbMediaCapabilitiesDto, faac, ffmpeg, hls, mp4, rtpProxy, srt, sctp,
                webRtc, x264, videoStack, tls, recording);
};

struct GbHealthDto final {
    RUVIA_OPTIONAL_FIELD(status, ruvia::String);
    RUVIA_OPTIONAL_FIELD(service, ruvia::String);
    RUVIA_OPTIONAL_FIELD(enabled, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(started, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(error, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("media_ports", mediaPorts, GbMediaPortsDto);
    RUVIA_OPTIONAL_FIELD_NAME("media_capabilities", mediaCapabilities,
                              GbMediaCapabilitiesDto);
    RUVIA_MODEL(GbHealthDto, status, service, enabled, started, error, mediaPorts,
                mediaCapabilities);
};

struct GbSipConfigDto final {
    RUVIA_OPTIONAL_FIELD(domain, ruvia::String);
    RUVIA_OPTIONAL_FIELD(id, ruvia::String);
    RUVIA_OPTIONAL_FIELD(host, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("public_ip", publicIp, ruvia::String);
    RUVIA_OPTIONAL_FIELD(port, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(transport, ruvia::String);
    RUVIA_MODEL(GbSipConfigDto, domain, id, host, publicIp, port, transport);
};

struct GbChannelDto final {
    RUVIA_OPTIONAL_FIELD(id, ruvia::String);
    RUVIA_OPTIONAL_FIELD(name, ruvia::String);
    RUVIA_OPTIONAL_FIELD(manufacturer, ruvia::String);
    RUVIA_OPTIONAL_FIELD(online, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("ptz_type", ptzType, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD_NAME("ptz_capable", ptzCapable, ruvia::Bool);
    RUVIA_MODEL(GbChannelDto, id, name, manufacturer, online, ptzType, ptzCapable);
};

struct GbRecordDto final {
    RUVIA_OPTIONAL_FIELD_NAME("device_id", deviceId, ruvia::String);
    RUVIA_OPTIONAL_FIELD(name, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("file_path", filePath, ruvia::String);
    RUVIA_OPTIONAL_FIELD(address, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("start_time", startTime, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("end_time", endTime, ruvia::String);
    RUVIA_OPTIONAL_FIELD(type, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("recorder_id", recorderId, ruvia::String);
    RUVIA_MODEL(GbRecordDto, deviceId, name, filePath, address, startTime, endTime, type,
                recorderId);
};

struct GbDeviceDto final {
    RUVIA_OPTIONAL_FIELD(id, ruvia::String);
    RUVIA_OPTIONAL_FIELD(name, ruvia::String);
    RUVIA_OPTIONAL_FIELD(manufacturer, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("remote_address", remoteAddress, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("remote_ip", remoteIp, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("remote_port", remotePort, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("registration_source", registrationSource, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("mapped_device_id", mappedDeviceId, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("last_seen_at", lastSeenAt, ruvia::String);
    RUVIA_OPTIONAL_FIELD(online, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(channels, ruvia::BoxedArray<GbChannelDto>);
    RUVIA_OPTIONAL_FIELD(records, ruvia::BoxedArray<GbRecordDto>);
    RUVIA_MODEL(GbDeviceDto, id, name, manufacturer, remoteAddress, remoteIp, remotePort,
                registrationSource, mappedDeviceId, lastSeenAt, online, channels,
                records);
};

struct GbDeviceListDto final {
    RUVIA_OPTIONAL_FIELD(items, ruvia::BoxedArray<GbDeviceDto>);
    RUVIA_MODEL(GbDeviceListDto, items);
};

struct GbStreamDto final {
    RUVIA_OPTIONAL_FIELD(id, ruvia::String);
    RUVIA_OPTIONAL_FIELD(app, ruvia::String);
    RUVIA_OPTIONAL_FIELD(stream, ruvia::String);
    RUVIA_OPTIONAL_FIELD(schema, ruvia::String);
    RUVIA_OPTIONAL_FIELD(online, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("reader_count", readerCount, ruvia::Int64);
    RUVIA_MODEL(GbStreamDto, id, app, stream, schema, online, readerCount);
};

struct GbStreamListDto final {
    RUVIA_OPTIONAL_FIELD(items, ruvia::BoxedArray<GbStreamDto>);
    RUVIA_MODEL(GbStreamListDto, items);
};

struct GbPlayUrlsDto final {
    RUVIA_OPTIONAL_FIELD_NAME("http_flv", httpFlv, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("ws_flv", wsFlv, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("http_ts", httpTs, ruvia::String);
    RUVIA_OPTIONAL_FIELD(hls, ruvia::String);
    RUVIA_OPTIONAL_FIELD(webrtc, ruvia::String);
    RUVIA_OPTIONAL_FIELD(rtsp, ruvia::String);
    RUVIA_OPTIONAL_FIELD(rtmp, ruvia::String);
    RUVIA_MODEL(GbPlayUrlsDto, httpFlv, wsFlv, httpTs, hls, webrtc, rtsp, rtmp);
};

struct GbPreviewStartDto final {
    RUVIA_OPTIONAL_FIELD(sent, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("session_id", sessionId, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("device_id", deviceId, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("channel_id", channelId, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("stream_id", streamId, ruvia::String);
    RUVIA_OPTIONAL_FIELD(ssrc, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("rtp_port", rtpPort, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD_NAME("play_urls", playUrls, GbPlayUrlsDto);
    RUVIA_MODEL(GbPreviewStartDto, sent, sessionId, deviceId, channelId, streamId, ssrc,
                rtpPort, playUrls);
};

struct GbPreviewStopDto final {
    RUVIA_OPTIONAL_FIELD(stopped, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("session_id", sessionId, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("stream_id", streamId, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("bye_sent", byeSent, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("rtp_server_closed", rtpServerClosed, ruvia::Bool);
    RUVIA_MODEL(GbPreviewStopDto, stopped, sessionId, streamId, byeSent, rtpServerClosed);
};

struct GbActionDto final {
    RUVIA_OPTIONAL_FIELD(registered, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(sent, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("device_id", deviceId, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("channel_id", channelId, ruvia::String);
    RUVIA_OPTIONAL_FIELD(action, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("mapped_device_id", mappedDeviceId, ruvia::String);
    RUVIA_OPTIONAL_FIELD(speed, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(pan, ruvia::Double);
    RUVIA_OPTIONAL_FIELD(tilt, ruvia::Double);
    RUVIA_OPTIONAL_FIELD(zoom, ruvia::Double);
    RUVIA_OPTIONAL_FIELD(recording, ruvia::Bool);
    RUVIA_MODEL(GbActionDto, registered, sent, deviceId, channelId, action,
                mappedDeviceId, speed, pan, tilt, zoom, recording);
};

#define GB28181_RESPONSE(name, dataType)                                               \
    struct name final {                                                               \
        RUVIA_OPTIONAL_FIELD(code, ruvia::Int64);                                      \
        RUVIA_OPTIONAL_FIELD(message, ruvia::String);                                  \
        RUVIA_OPTIONAL_FIELD(data, dataType);                                          \
        RUVIA_MODEL(name, code, message, data);                                        \
    }

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
