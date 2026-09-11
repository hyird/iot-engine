#include "service/features/gb28181/gb28181.runtime.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <exception>
#include <memory_resource>
#include <set>
#include <stdexcept>

#include "service/common/http.h"
#include "service/common/log.h"
#include "service/common/timestamp.h"
#include "service/common/uuid.h"
#include "service/features/gb28181/gb28181.protocol.h"
#include "service/utils/redis.h"

namespace service::gb28181 {

namespace {

using control_protocol::ActionJson;
using control_protocol::ChannelJson;
using control_protocol::DeviceJson;
using control_protocol::DeviceListJson;
using control_protocol::HealthJson;
using control_protocol::MediaCapabilitiesJson;
using control_protocol::MediaPortsJson;
using control_protocol::PlayUrlsJson;
using control_protocol::PreviewStartJson;
using control_protocol::PreviewStopJson;
using control_protocol::RecordJson;
using control_protocol::SipConfigJson;
using control_protocol::StreamJson;
using control_protocol::StreamListJson;

std::string requiredText(const std::optional<ruvia::String>& value, std::string_view message) {
    if (!value || value->empty()) {
        service::common::fail(10001, std::string(message), 400);
    }
    return std::string(value->view());
}

std::int64_t requiredInteger(const std::optional<ruvia::Int64>& value, std::string_view message) {
    if (!value) {
        service::common::fail(10001, std::string(message), 400);
    }
    return static_cast<std::int64_t>(*value);
}

double requiredFinite(const std::optional<ruvia::Double>& value, std::string_view name, double minimum, double maximum) {
    if (!value || !std::isfinite(static_cast<double>(*value)) ||
        static_cast<double>(*value) < minimum ||
        static_cast<double>(*value) > maximum) {
        service::common::fail(10001, std::string(name) + " 超出允许范围", 400);
    }
    return static_cast<double>(*value);
}

void requireEnabled(bool enabled) {
    if (!enabled) {
        service::common::fail(10004, "GB28181 功能未启用", 404);
    }
}

void requireAction(std::string_view action) {
    static const std::set<std::string_view> actions{
        "left",
        "right",
        "up",
        "down",
        "zoomin",
        "zoomout",
        "stop"
    };
    if (!actions.contains(action)) {
        service::common::fail(10001, "不支持的云台动作", 400);
    }
}

std::pair<std::string, std::string> splitRemoteAddress(std::string_view address) {
    if (address.starts_with('[')) {
        const auto end = address.find(']');
        if (end != std::string_view::npos) {
            auto port = end + 1 < address.size() && address[end + 1] == ':'
                ? std::string(address.substr(end + 2))
                : std::string{};
            return { std::string(address.substr(1, end - 1)), std::move(port) };
        }
    }
    const auto colon = address.rfind(':');
    if (colon == std::string_view::npos || address.find(':') != colon) {
        return { std::string(address), {} };
    }
    return { std::string(address.substr(0, colon)),
             std::string(address.substr(colon + 1)) };
}

MediaPortsJson mediaPortsJson(ruvia::WebWorkerContext& context, const ZlmSdk::Ports& ports) {
    MediaPortsJson result(context);
    result.set<"http">(ports.http)
        .set<"https">(ports.https)
        .set<"rtsp">(ports.rtsp)
        .set<"rtsps">(ports.rtsps)
        .set<"rtmp">(ports.rtmp)
        .set<"rtmps">(ports.rtmps)
        .set<"rtc">(ports.rtc)
        .set<"srt">(ports.srt);
    return result;
}

MediaCapabilitiesJson mediaCapabilitiesJson(
    ruvia::WebWorkerContext& context,
    const ZlmSdk::Capabilities& capabilities
) {
    MediaCapabilitiesJson result(context);
    result.set<"faac">(capabilities.faac)
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
    return result;
}

HealthJson healthJson(ruvia::WebWorkerContext& context, bool enabled, std::string_view error = {}) {
    const auto started = sdkSupervisor().started();
    HealthJson result(context);
    result.set<"status">(started ? "ok" : (enabled ? "error" : "disabled"))
        .set<"service">("iot-engine-gb28181")
        .set<"enabled">(enabled)
        .set<"started">(started)
        .set<"error">(error)
        .set<"mediaPorts">(mediaPortsJson(context, sdkSupervisor().ports()))
        .set<"mediaCapabilities">(
            mediaCapabilitiesJson(context, sdkSupervisor().capabilities())
        );
    return result;
}

SipConfigJson sipConfigJson(ruvia::WebWorkerContext& context, std::string_view domain, std::string_view id, std::string_view host, std::string_view publicIp, std::int64_t port, std::string_view transport) {
    SipConfigJson result(context);
    result.set<"domain">(domain)
        .set<"id">(id)
        .set<"host">(host)
        .set<"publicIp">(publicIp)
        .set<"port">(port)
        .set<"transport">(transport);
    return result;
}

ChannelJson channelJson(ruvia::WebWorkerContext& context, const Channel& channel) {
    ChannelJson result(context);
    result.set<"id">(channel.id)
        .set<"name">(channel.displayName())
        .set<"reportedName">(channel.name)
        .set<"customName">(channel.customName)
        .set<"manufacturer">(channel.manufacturer)
        .set<"online">(channel.online)
        .set<"ptzType">(channel.ptzType)
        .set<"ptzCapable">(channel.ptzType > 0);
    return result;
}

RecordJson recordJson(ruvia::WebWorkerContext& context, const RecordItem& record) {
    RecordJson result(context);
    result.set<"deviceId">(record.deviceId)
        .set<"name">(record.name)
        .set<"filePath">(record.filePath)
        .set<"address">(record.address)
        .set<"startTime">(record.startTime)
        .set<"endTime">(record.endTime)
        .set<"type">(record.type)
        .set<"recorderId">(record.recorderId);
    return result;
}

DeviceJson deviceJson(ruvia::WebWorkerContext& context, const Device& device) {
    const auto [remoteIp, remotePort] = splitRemoteAddress(device.remoteAddress);
    ruvia::BoxedArray<ChannelJson> channels(
        ruvia::ModelOptions{ .resource = context.resource() }
    );
    for (const auto& channel : device.channels) {
        channels.emplace(channelJson(context, channel));
    }
    ruvia::BoxedArray<RecordJson> records(
        ruvia::ModelOptions{ .resource = context.resource() }
    );
    for (const auto& record : device.records) {
        records.emplace(recordJson(context, record));
    }

    DeviceJson result(context);
    result.set<"id">(device.id)
        .set<"name">(device.displayName())
        .set<"reportedName">(device.name)
        .set<"customName">(device.customName)
        .set<"manufacturer">(device.manufacturer)
        .set<"remoteAddress">(device.remoteAddress)
        .set<"remoteIp">(remoteIp)
        .set<"remotePort">(remotePort)
        .set<"registrationSource">(device.registrationSource)
        .set<"mappedDeviceId">(device.mappedDeviceId)
        .set<"lastSeenAt">(service::common::utcTimestamp(device.lastSeen))
        .set<"online">(device.online)
        .set<"channels">(std::move(channels))
        .set<"records">(std::move(records));
    return result;
}

StreamJson streamJson(ruvia::WebWorkerContext& context, const StreamStatus& stream) {
    StreamJson result(context);
    result.set<"id">(
              StreamStatus::identity(stream.app, stream.stream, stream.schema)
    )
        .set<"app">(stream.app)
        .set<"stream">(stream.stream)
        .set<"schema">(stream.schema)
        .set<"online">(stream.online)
        .set<"readerCount">(stream.readerCount);
    return result;
}

PlayUrlsJson playUrlsJson(ruvia::WebWorkerContext& context, const PlayUrls& urls) {
    PlayUrlsJson result(context);
    result.set<"httpFlv">(urls.httpFlv)
        .set<"wsFlv">(urls.wsFlv)
        .set<"httpTs">(urls.httpTs)
        .set<"hls">(urls.hls)
        .set<"webrtc">(urls.webRtc)
        .set<"rtsp">(urls.rtsp)
        .set<"rtmp">(urls.rtmp);
    return result;
}

PreviewStartJson previewStartJson(
    ruvia::WebWorkerContext& context,
    const SipServer::PreviewStartResult& value
) {
    PreviewStartJson result(context);
    result.set<"sent">(true)
        .set<"sessionId">(value.sessionId)
        .set<"deviceId">(value.deviceId)
        .set<"channelId">(value.channelId)
        .set<"streamId">(value.streamId)
        .set<"ssrc">(value.ssrc)
        .set<"rtpPort">(value.rtpPort)
        .set<"leaseTimeoutSeconds">(value.leaseTimeoutSeconds)
        .set<"playUrls">(playUrlsJson(context, value.playUrls));
    return result;
}

PreviewStopJson previewStopJson(
    ruvia::WebWorkerContext& context,
    const SipServer::PreviewStopResult& value
) {
    PreviewStopJson result(context);
    result.set<"stopped">(true)
        .set<"sessionId">(value.sessionId)
        .set<"streamId">(value.streamId)
        .set<"byeSent">(value.byeSent)
        .set<"rtpServerClosed">(value.rtpServerClosed);
    return result;
}

ActionJson actionJson(ruvia::WebWorkerContext& context) {
    return ActionJson(context);
}

std::string projectionChangeName(DeviceChange change) {
    switch (change) {
        case DeviceChange::Status:
            return "status";
        case DeviceChange::Catalog:
            return "catalog";
        case DeviceChange::Records:
            return "records";
        case DeviceChange::Mapping:
            return "mapping";
        case DeviceChange::DeviceName:
            return "device_name";
        case DeviceChange::ChannelName:
            return "channel_name";
    }
    return "status";
}

std::vector<service::message::StreamField>
deviceProjectionFields(const Device& device, DeviceChange change, std::size_t owner, std::string_view ownerToken) {
    using service::message::StreamField;
    std::vector<StreamField> fields{
        { "event_type", "gb28181.device" },
        { "schema_version", "1" },
        { "change", projectionChangeName(change) },
        { "aggregate_id", device.id },
        { "device_id", device.id },
        { "name", device.name },
        { "custom_name", device.customName },
        { "manufacturer", device.manufacturer },
        { "remote_address", device.remoteAddress },
        { "registration_source", device.registrationSource },
        { "mapped_device_id", device.mappedDeviceId },
        { "online", device.online ? "1" : "0" },
        { "last_seen_at", service::common::utcTimestamp(device.lastSeen) },
        { "session_generation", std::to_string(device.sessionGeneration) },
        { "owner_index", std::to_string(owner) },
        { "owner_token", std::string(ownerToken) },
        { "channel_count", std::to_string(device.channels.size()) },
        { "record_count", std::to_string(device.records.size()) },
    };
    fields.reserve(fields.size() + device.channels.size() * 6U + device.records.size() * 8U);
    for (std::size_t index = 0; index < device.channels.size(); ++index) {
        const auto& channel = device.channels[index];
        const auto prefix = "channel." + std::to_string(index) + ".";
        fields.emplace_back(prefix + "id", channel.id);
        fields.emplace_back(prefix + "name", channel.name);
        fields.emplace_back(prefix + "custom_name", channel.customName);
        fields.emplace_back(prefix + "manufacturer", channel.manufacturer);
        fields.emplace_back(prefix + "online", channel.online ? "1" : "0");
        fields.emplace_back(prefix + "ptz_type", std::to_string(channel.ptzType));
    }
    for (std::size_t index = 0; index < device.records.size(); ++index) {
        const auto& record = device.records[index];
        const auto prefix = "record." + std::to_string(index) + ".";
        fields.emplace_back(prefix + "device_id", record.deviceId);
        fields.emplace_back(prefix + "name", record.name);
        fields.emplace_back(prefix + "file_path", record.filePath);
        fields.emplace_back(prefix + "address", record.address);
        fields.emplace_back(prefix + "start_time", record.startTime);
        fields.emplace_back(prefix + "end_time", record.endTime);
        fields.emplace_back(prefix + "type", record.type);
        fields.emplace_back(prefix + "recorder_id", record.recorderId);
    }
    return fields;
}

std::vector<service::message::StreamField>
streamProjectionFields(const StreamStatus& stream, std::size_t owner, std::string_view ownerToken) {
    using service::message::StreamField;
    return {
        { "event_type", "gb28181.stream" },
        { "schema_version", "1" },
        { "aggregate_id", StreamStatus::identity(stream.app, stream.stream, stream.schema) },
        { "app", stream.app },
        { "stream", stream.stream },
        { "schema", stream.schema },
        { "online", stream.online ? "1" : "0" },
        { "reader_count", std::to_string(stream.readerCount) },
        { "owner_index", std::to_string(owner) },
        { "owner_token", std::string(ownerToken) },
    };
}

std::string jsonData(std::string value) {
    return "{\"code\":0,\"message\":\"ok\",\"data\":" +
        std::move(value) + "}";
}

std::string jsonAction(bool sent, std::string_view deviceId = {}, std::string_view channelId = {}) {
    std::string result = "{\"sent\":";
    result += sent ? "true" : "false";
    if (!deviceId.empty()) {
        result += ",\"device_id\":" + service::utils::jsonQuoted(deviceId);
    }
    if (!channelId.empty()) {
        result += ",\"channel_id\":" + service::utils::jsonQuoted(channelId);
    }
    result.push_back('}');
    return jsonData(std::move(result));
}

std::string jsonPreviewStart(const SipServer::PreviewStartResult& value) {
    std::string result = "{\"sent\":true,\"session_id\":" +
        service::utils::jsonQuoted(value.sessionId) +
        ",\"device_id\":" +
        service::utils::jsonQuoted(value.deviceId) +
        ",\"channel_id\":" +
        service::utils::jsonQuoted(value.channelId) +
        ",\"stream_id\":" +
        service::utils::jsonQuoted(value.streamId) +
        ",\"ssrc\":" +
        service::utils::jsonQuoted(value.ssrc) +
        ",\"rtp_port\":" + std::to_string(value.rtpPort) +
        ",\"lease_timeout_seconds\":" +
        std::to_string(value.leaseTimeoutSeconds) +
        ",\"play_urls\":{\"http_flv\":" +
        service::utils::jsonQuoted(value.playUrls.httpFlv) +
        ",\"ws_flv\":" +
        service::utils::jsonQuoted(value.playUrls.wsFlv) +
        ",\"http_ts\":" +
        service::utils::jsonQuoted(value.playUrls.httpTs) +
        ",\"hls\":" +
        service::utils::jsonQuoted(value.playUrls.hls) +
        ",\"webrtc\":" +
        service::utils::jsonQuoted(value.playUrls.webRtc) +
        ",\"rtsp\":" +
        service::utils::jsonQuoted(value.playUrls.rtsp) +
        ",\"rtmp\":" +
        service::utils::jsonQuoted(value.playUrls.rtmp) +
        "}}";
    return jsonData(std::move(result));
}

std::string jsonPreviewStop(const SipServer::PreviewStopResult& value) {
    return jsonData(
        "{\"stopped\":true,\"session_id\":" +
        service::utils::jsonQuoted(value.sessionId) + ",\"stream_id\":" +
        service::utils::jsonQuoted(value.streamId) + ",\"bye_sent\":" +
        (value.byeSent ? "true" : "false") +
        ",\"rtp_server_closed\":" +
        (value.rtpServerClosed ? "true" : "false") + "}"
    );
}

ruvia::Task<void> deleteOwnerIfMatches(const ruvia::RedisHandle& redis, std::string_view key, std::string_view owner) {
    static constexpr std::string_view script = R"lua(
if redis.call('GET', KEYS[1]) == ARGV[1] then
  return redis.call('DEL', KEYS[1])
end
return 0
)lua";
    const std::string keyValue(key);
    const std::string ownerValue(owner);
    const std::string_view keys[]{ keyValue };
    const std::string_view arguments[]{ ownerValue };
    const auto reply = co_await redis.eval(script, keys, arguments);
    if (reply.kind() != ruvia::RedisValue::Kind::kInteger) {
        service::message::redis::throwValue("GB28181 owner CAS delete", reply);
    }
    co_return;
}

ruvia::Task<void> publishReplyAndAcknowledge(
    const ruvia::RedisHandle& redis,
    std::string_view replyStream,
    std::string_view controlStream,
    std::string_view group,
    std::string_view messageId,
    std::string_view requestId,
    std::string_view operation,
    std::string_view status,
    std::string_view payload,
    std::string_view resultKey,
    std::string_view resultValue,
    std::string_view claimKey,
    std::string_view claimOwner
) {
    // Reply publication and PEL removal are one Redis transaction.  A worker
    // crash between the two cannot acknowledge a command whose response was
    // never made visible to the Service Worker.
    static constexpr std::string_view script = R"lua(
 if ARGV[7] ~= '' and redis.call('EXISTS', KEYS[3]) == 0 then
   redis.call('SET', KEYS[3], ARGV[7], 'EX', ARGV[8])
 end
 local reply = redis.call('XADD', KEYS[1], '*',
   'request_id', ARGV[1], 'operation', ARGV[2], 'status', ARGV[3],
   'payload', ARGV[4])
 redis.call('EXPIRE', KEYS[1], ARGV[9])
 redis.call('EXPIRE', KEYS[2], ARGV[10])
 local acknowledged = redis.call('XACK', KEYS[2], ARGV[5], ARGV[6])
 redis.call('XDEL', KEYS[2], ARGV[6])
 if ARGV[11] ~= '' and redis.call('GET', KEYS[4]) == ARGV[11] then
   redis.call('DEL', KEYS[4])
 end
return acknowledged
)lua";
    const std::string replyKey(replyStream);
    const std::string controlKey(controlStream);
    const std::string resultKeyValue(resultKey);
    const std::string claimKeyValue(claimKey);
    const std::string request(requestId);
    const std::string op(operation);
    const std::string state(status);
    const std::string body(payload);
    const std::string groupValue(group);
    const std::string id(messageId);
    const std::string result(resultValue);
    const std::string claim(claimOwner);
    const std::string resultTtl{ "600" };
    const std::string replyTtl{ "600" };
    const std::string controlTtl{ "600" };
    const std::string_view keys[]{ replyKey, controlKey, resultKeyValue, claimKeyValue };
    const std::string_view arguments[]{ request, op, state, body, groupValue, id, result, resultTtl, replyTtl, controlTtl, claim };
    const auto reply = co_await redis.eval(script, keys, arguments);
    if (reply.kind() != ruvia::RedisValue::Kind::kInteger) {
        service::message::redis::throwValue("GB28181 reply/ack", reply);
    }
    co_return;
}

std::optional<std::int64_t> projectionInteger(std::string_view value) {
    std::int64_t parsed{};
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size()) {
        return std::nullopt;
    }
    return parsed;
}

bool validProjectionStreamId(std::string_view value) {
    const auto separator = value.find('-');
    if (separator == std::string_view::npos || separator == 0 ||
        separator + 1 == value.size() ||
        value.find('-', separator + 1) != std::string_view::npos) {
        return false;
    }
    const auto unsignedInteger = [](std::string_view part) {
        std::uint64_t parsed{};
        const auto [end, error] = std::from_chars(
            part.data(),
            part.data() + part.size(),
            parsed
        );
        return error == std::errc{} && end == part.data() + part.size();
    };
    return unsignedInteger(value.substr(0, separator)) &&
        unsignedInteger(value.substr(separator + 1)) && value != "0-0";
}

struct ProjectionPublishResult final {
    std::string order;
    std::string current;
};

ProjectionPublishResult parseProjectionPublishResult(
    const ruvia::RedisValue& reply
) {
    if (reply.kind() != ruvia::RedisValue::Kind::kArray ||
        reply.array().size() != 2 ||
        reply.array()[0].kind() != ruvia::RedisValue::Kind::kString ||
        reply.array()[1].kind() != ruvia::RedisValue::Kind::kString) {
        service::message::redis::throwValue(
            "GB28181 projection publish result",
            reply
        );
    }
    ProjectionPublishResult result{
        std::string(reply.array()[0].string()),
        std::string(reply.array()[1].string())
    };
    if (!validProjectionStreamId(result.order) ||
        !validProjectionStreamId(result.current)) {
        throw std::runtime_error("GB28181 projection publish returned invalid stream ID");
    }
    return result;
}

bool projectionBoolean(std::string_view value) {
    return value == "t" || value == "true" || value == "1";
}

std::optional<DeviceChange> projectionDeviceChange(std::string_view value) {
    if (value == "status") {
        return DeviceChange::Status;
    }
    if (value == "catalog") {
        return DeviceChange::Catalog;
    }
    if (value == "records") {
        return DeviceChange::Records;
    }
    if (value == "mapping") {
        return DeviceChange::Mapping;
    }
    if (value == "device_name") {
        return DeviceChange::DeviceName;
    }
    if (value == "channel_name") {
        return DeviceChange::ChannelName;
    }
    return std::nullopt;
}

std::size_t projectionCount(std::string_view value, std::string_view field) {
    const auto parsed = projectionInteger(value);
    if (!parsed || *parsed < 0 || *parsed > 100000) {
        throw std::runtime_error("invalid GB28181 projection " + std::string(field));
    }
    return static_cast<std::size_t>(*parsed);
}

Device deviceFromProjection(const service::message::StreamMessage& message) {
    if (message.get("schema_version") != "1" ||
        message.get("event_type") != "gb28181.device") {
        throw std::runtime_error("unsupported GB28181 device projection");
    }
    Device device;
    device.id = std::string(message.get("device_id"));
    if (device.id.empty()) {
        throw std::runtime_error("GB28181 device projection has no id");
    }
    device.name = std::string(message.get("name"));
    device.customName = std::string(message.get("custom_name"));
    device.manufacturer = std::string(message.get("manufacturer"));
    device.remoteAddress = std::string(message.get("remote_address"));
    device.registrationSource = std::string(message.get("registration_source"));
    device.mappedDeviceId = std::string(message.get("mapped_device_id"));
    device.online = projectionBoolean(message.get("online"));
    if (const auto lastSeen = service::common::parseUtcTimestamp(
            message.get("last_seen_at")
        )) {
        device.lastSeen = *lastSeen;
    }
    if (const auto generation = projectionInteger(message.get("session_generation"))) {
        device.sessionGeneration = static_cast<std::uint64_t>(
            std::max<std::int64_t>(0, *generation)
        );
    }

    const auto channelCount = projectionCount(message.get("channel_count"), "channel_count");
    device.channels.reserve(channelCount);
    for (std::size_t index = 0; index < channelCount; ++index) {
        const auto prefix = "channel." + std::to_string(index) + ".";
        Channel channel;
        channel.id = std::string(message.get(prefix + "id"));
        channel.name = std::string(message.get(prefix + "name"));
        channel.customName = std::string(message.get(prefix + "custom_name"));
        channel.manufacturer =
            std::string(message.get(prefix + "manufacturer"));
        channel.online = projectionBoolean(message.get(prefix + "online"));
        channel.ptzType = static_cast<int>(projectionInteger(
                                               message.get(prefix + "ptz_type")
        )
                                               .value_or(-1));
        if (channel.id.empty()) {
            throw std::runtime_error("GB28181 projection channel has no id");
        }
        device.channels.push_back(std::move(channel));
    }

    const auto recordCount =
        projectionCount(message.get("record_count"), "record_count");
    device.records.reserve(recordCount);
    for (std::size_t index = 0; index < recordCount; ++index) {
        const auto prefix = "record." + std::to_string(index) + ".";
        RecordItem record;
        record.deviceId = std::string(message.get(prefix + "device_id"));
        record.name = std::string(message.get(prefix + "name"));
        record.filePath = std::string(message.get(prefix + "file_path"));
        record.address = std::string(message.get(prefix + "address"));
        record.startTime = std::string(message.get(prefix + "start_time"));
        record.endTime = std::string(message.get(prefix + "end_time"));
        record.type = std::string(message.get(prefix + "type"));
        record.recorderId = std::string(message.get(prefix + "recorder_id"));
        device.records.push_back(std::move(record));
    }
    return device;
}

StreamStatus streamFromProjection(const service::message::StreamMessage& message) {
    if (message.get("schema_version") != "1" ||
        message.get("event_type") != "gb28181.stream") {
        throw std::runtime_error("unsupported GB28181 stream projection");
    }
    StreamStatus stream;
    stream.app = std::string(message.get("app"));
    stream.stream = std::string(message.get("stream"));
    stream.schema = std::string(message.get("schema"));
    if (stream.app.empty() || stream.stream.empty() || stream.schema.empty()) {
        throw std::runtime_error("GB28181 stream projection is incomplete");
    }
    stream.online = projectionBoolean(message.get("online"));
    stream.readerCount = static_cast<int>(projectionInteger(
                                              message.get("reader_count")
    )
                                              .value_or(0));
    stream.readerCount = std::max(0, stream.readerCount);
    return stream;
}

ruvia::Task<bool> projectionOwnerMatches(const ruvia::RedisHandle& redis, std::string_view aggregateId, bool online, std::string_view ownerToken) {
    if (ownerToken.empty()) {
        co_return false;
    }
    const auto current = co_await redis.get(
        control_protocol::stream::owner(aggregateId)
    );
    if (online) {
        co_return current&& std::string_view(*current) == ownerToken;
    }
    // A legitimate offline event removes its owner key.  An old offline event
    // must not overwrite a newer online owner.
    co_return !current || std::string_view(*current) == ownerToken;
}

ruvia::Task<std::optional<service::message::StreamMessage>> readControlReply(
    const ruvia::WorkerHandle& worker,
    const ruvia::RedisHandle& redis,
    std::string_view stream,
    std::string_view requestId,
    ruvia::StopToken stop,
    std::chrono::system_clock::time_point deadline
) {
    const auto boundedRedis = redis.withOptions({ .stopToken = stop });
    while (!stop.stopRequested() &&
           std::chrono::system_clock::now() < deadline) {
        const auto reply = co_await service::message::redis::command(
            boundedRedis,
            { "XREAD", "COUNT", "1", "STREAMS", std::string(stream), "0-0" }
        );
        if (reply.null()) {
            (void)co_await ruvia::sleepFor(worker, std::chrono::milliseconds(100), stop);
            continue;
        }
        if (reply.kind() != ruvia::RedisValue::Kind::kArray) {
            service::message::redis::throwValue("XREAD GB28181 reply", reply);
        }
        for (const auto& streamReply : reply.array()) {
            if (streamReply.kind() != ruvia::RedisValue::Kind::kArray ||
                streamReply.array().size() != 2) {
                continue;
            }
            const auto& entries = streamReply.array()[1];
            if (entries.kind() != ruvia::RedisValue::Kind::kArray) {
                continue;
            }
            for (const auto& entry : entries.array()) {
                if (entry.kind() != ruvia::RedisValue::Kind::kArray ||
                    entry.array().size() != 2) {
                    continue;
                }
                const auto& id = entry.array()[0];
                const auto& fields = entry.array()[1];
                if (id.kind() != ruvia::RedisValue::Kind::kString ||
                    fields.kind() != ruvia::RedisValue::Kind::kArray) {
                    continue;
                }
                service::message::StreamMessage message;
                message.id = std::string(id.string());
                for (std::size_t index = 0; index + 1 < fields.array().size();
                     index += 2) {
                    const auto& name = fields.array()[index];
                    const auto& value = fields.array()[index + 1];
                    if (name.kind() != ruvia::RedisValue::Kind::kString ||
                        value.kind() != ruvia::RedisValue::Kind::kString) {
                        continue;
                    }
                    message.fields.push_back(
                        { std::string(name.string()), std::string(value.string()) }
                    );
                }
                if (message.get("request_id") == requestId) {
                    co_return message;
                }
            }
        }
    }
    if (stop.stopRequested()) {
        throw std::runtime_error("GB28181 operation was cancelled");
    }
    co_return std::nullopt;
}

ruvia::Task<void> markControlCancelled(const ruvia::RedisHandle& redis, std::string_view cancelKey) {
    ruvia::RedisSetOptions options;
    options.expiration =
        ruvia::RedisSetExpiration::expiresAfter(std::chrono::minutes(10));
    co_await redis.set(std::string(cancelKey), "1", std::move(options));
    co_return;
}

ruvia::Task<std::string> dispatchToCollector(
    ruvia::WebWorkerContext& context,
    std::string_view operation,
    std::string_view payload,
    std::string ownerKey,
    std::string_view ownerToken,
    ruvia::StopToken stop
) {
    const auto owner = control_protocol::stream::ownerIndex(ownerToken);
    const auto instance = control_protocol::stream::ownerInstance(ownerToken);
    if (!owner || !instance || !control_protocol::stream::completeOwnerToken(ownerToken)) {
        throw std::runtime_error("GB28181 owner token is invalid");
    }
    if (stop.stopRequested()) {
        throw std::runtime_error("GB28181 operation was cancelled");
    }
    const auto currentOwner = co_await context.redis().get(ownerKey);
    if (!currentOwner || std::string_view(*currentOwner) != ownerToken) {
        service::common::fail(10003, "GB28181 连接归属已变更或已过期", 404);
    }

    const auto requestId = service::common::nextUuidV7();
    const auto replyStream = control_protocol::stream::reply(requestId);
    const auto resultKey = control_protocol::stream::result(requestId);
    const auto cancelKey = control_protocol::stream::cancel(requestId);
    const auto now = std::chrono::system_clock::now();
    const auto deadline = now + std::chrono::seconds(15);
    const auto deadlineMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                deadline.time_since_epoch()
    )
                                .count();
    std::exception_ptr publishFailure;
    try {
        const auto controlKey = control_protocol::stream::control(*owner, *instance);
        std::vector<service::message::StreamField> fields{
            { "request_id", requestId },
            { "owner_key", std::string(ownerKey) },
            { "owner_token", std::string(ownerToken) },
            { "operation", std::string(operation) },
            { "payload", std::string(payload) },
            { "reply_stream", replyStream },
            { "result_key", resultKey },
            { "cancel_key", cancelKey },
            { "deadline_ms", std::to_string(deadlineMs) }
        };
        std::vector<std::string_view> args{ ownerToken };
        for (const auto& field : fields) {
            args.push_back(field.name);
            args.push_back(field.value);
        }
        const std::string_view keys[]{ controlKey, ownerKey };
        static constexpr std::string_view publishScript = R"lua(
if redis.call('GET',KEYS[2])~=ARGV[1] then return 0 end
redis.call('XADD',KEYS[1],'*',unpack(ARGV,2))
redis.call('EXPIRE',KEYS[1],600)
return 1
)lua";
        const auto published = co_await context.redis().eval(publishScript, keys, args);
        if (published.kind() != ruvia::RedisValue::Kind::kInteger) {
            service::message::redis::throwValue("GB28181 command publish", published);
        }
        if (published.integer() != 1) {
            service::common::fail(10003, "GB28181 connection owner expired", 409);
        }
    } catch (...) {
        publishFailure = std::current_exception();
    }
    if (publishFailure) {
        // XADD may have reached Redis before the client observed a transport
        // error.  Mark the request cancelled so an uncertain delivery cannot
        // execute a device side effect after the Service has failed it.
        try {
            co_await markControlCancelled(context.redis(), cancelKey);
        } catch (...) {
        }
        std::rethrow_exception(publishFailure);
    }

    std::optional<service::message::StreamMessage> response;
    std::exception_ptr failure;
    try {
        response = co_await readControlReply(context.worker(), context.redis(), replyStream, requestId, stop, deadline);
    } catch (...) {
        failure = std::current_exception();
    }
    if (failure) {
        // The Collector may have claimed the command before this wait was
        // cancelled.  Leave a short lived fence for it to check before any
        // device side effect.
        try {
            co_await markControlCancelled(context.redis(), cancelKey);
        } catch (...) {
        }
        try {
            (void)co_await context.redis().del(replyStream);
        } catch (...) {
        }
        std::rethrow_exception(failure);
    }
    if (!response) {
        try {
            co_await markControlCancelled(context.redis(), cancelKey);
        } catch (...) {
        }
        try {
            (void)co_await context.redis().del(replyStream);
        } catch (...) {
        }
        throw std::runtime_error(
            "GB28181 Collector did not reply before deadline"
        );
    }
    (void)co_await context.redis().del(replyStream);
    const auto status = response->get("status");
    const auto result = response->get("payload");
    if (status != "ok") {
        throw std::runtime_error(result.empty() ? "GB28181 Collector operation failed" : std::string(result));
    }
    co_return std::string(result);
}

ruvia::Task<std::optional<std::string>> ownerTokenFor(
    const ruvia::RedisHandle& redis,
    std::string_view key
) {
    const auto value = co_await redis.get(key);
    if (!value) {
        co_return std::nullopt;
    }
    co_return std::optional<std::string>(std::string(*value));
}

ruvia::Task<std::string> dispatchByOwnerKey(
    ruvia::WebWorkerContext& context,
    std::string_view operation,
    std::string_view payload,
    std::string ownerKey,
    ruvia::StopToken stop
) {
    const auto token = co_await ownerTokenFor(context.redis(), ownerKey);
    if (!token) {
        service::common::fail(10003, "GB28181 连接归属不存在或已过期", 404);
    }
    try {
        co_return co_await dispatchToCollector(context, operation, payload, ownerKey, *token, stop);
    } catch (const ruvia::HttpError&) {
        throw;
    } catch (const std::exception& error) {
        service::common::fail(10004, error.what(), 502);
    }
}

ruvia::Task<void> markProjectionDoneAndAcknowledge(
    const ruvia::RedisHandle& redis,
    std::string_view stream,
    std::string_view group,
    std::string_view messageId,
    std::string_view projectionId,
    bool applied
) {
    if (projectionId.empty()) {
        throw std::runtime_error("GB28181 projection has no projection_id");
    }
    static constexpr std::string_view script = R"lua(
 redis.call('SET', KEYS[1], ARGV[1], 'EX', ARGV[2])
 local acknowledged = redis.call('XACK', KEYS[2], ARGV[3], ARGV[4])
 redis.call('XDEL', KEYS[2], ARGV[4])
return acknowledged
)lua";
    const std::string doneKey(
        control_protocol::stream::projectionDone(projectionId)
    );
    const std::string streamKey(stream);
    const std::string appliedValue(applied ? "1" : "0");
    const std::string ttl{ "600" };
    const std::string groupValue(group);
    const std::string idValue(messageId);
    const std::string_view keys[]{ doneKey, streamKey };
    const std::string_view arguments[]{ appliedValue, ttl, groupValue, idValue };
    const auto reply = co_await redis.eval(script, keys, arguments);
    if (reply.kind() != ruvia::RedisValue::Kind::kInteger) {
        service::message::redis::throwValue("GB28181 projection done", reply);
    }
    co_return;
}

ruvia::Task<bool> configuredEnabled(const ruvia::RedisHandle& redis) {
    const auto value = co_await redis.hget(
        control_protocol::stream::kConfigKey,
        "enabled"
    );
    co_return value&& projectionBoolean(std::string_view(*value));
}

std::string configField(const std::pmr::vector<ruvia::RedisKeyValue>& fields, std::string_view name) {
    for (const auto& field : fields) {
        if (field.key() == name) {
            return std::string(field.value());
        }
    }
    return {};
}

} // namespace

void GbProjectionRuntime::start(ruvia::WebWorkerHandle worker, OwnerIndex workerIndex, OwnerIndex serviceWorkerCount) {
    {
        std::lock_guard lock(lifecycleMutex_);
        if (running_.load()) {
            if (worker_.id() != worker.id() || workerIndex_ != workerIndex) {
                throw std::logic_error(
                    "GB28181 projection runtime is already bound to another worker"
                );
            }
            return;
        }
        if (!worker.valid() || serviceWorkerCount == 0 ||
            workerIndex >= serviceWorkerCount) {
            throw std::runtime_error(
                "GB28181 projection runtime requires a valid Service Worker"
            );
        }
        worker_ = std::move(worker);
        workerIndex_ = workerIndex;
        serviceWorkerCount_ = serviceWorkerCount;
        state_ = std::make_shared<State>();
        running_.store(true);
    }

    auto ready = std::make_shared<std::promise<void>>();
    auto future = ready->get_future();
    auto stopped = std::make_shared<std::promise<void>>();
    stopped_ = stopped->get_future().share();
    const auto state = state_;
    const auto posted = worker_.post(
        [this, ready, stopped, state](ruvia::WebWorkerContext& context)
            -> ruvia::Task<void> {
            bool readySet = false;
            try {
                const auto redis = context.redis().withOptions(
                    { .stopToken = state->stop.token() }
                );
                co_await service::message::redis::ensureGroup(
                    redis,
                    control_protocol::stream::kProjection,
                    control_protocol::stream::kProjectionGroup
                );
                ready->set_value();
                readySet = true;
                co_await consume(context, state);
            } catch (...) {
                if (!readySet) {
                    try {
                        ready->set_exception(std::current_exception());
                    } catch (...) {
                    }
                } else {
                    LOG_WARN << "[GB28181][GbProjectionRuntime] consumer stopped";
                }
            }
            try {
                stopped->set_value();
            } catch (...) {
            }
            co_return;
        }
    );
    if (!posted.accepted()) {
        state->stop.requestStop();
        running_.store(false);
        std::lock_guard lock(lifecycleMutex_);
        worker_ = {};
        state_.reset();
        stopped_ = {};
        throw std::runtime_error(
            "service worker rejected GB28181 projection consumer"
        );
    }

    try {
        future.get();
    } catch (...) {
        state->stop.requestStop();
        if (stopped_.valid()) {
            stopped_.wait();
        }
        running_.store(false);
        std::lock_guard lock(lifecycleMutex_);
        worker_ = {};
        state_.reset();
        stopped_ = {};
        throw;
    }
}

void GbProjectionRuntime::stop() noexcept {
    std::shared_ptr<State> state;
    std::shared_future<void> stopped;
    {
        std::lock_guard lock(lifecycleMutex_);
        if (!running_.exchange(false)) {
            return;
        }
        state = state_;
        stopped = stopped_;
    }
    if (state) {
        state->stop.requestStop();
    }
    if (stopped.valid()) {
        stopped.wait();
    }
    std::lock_guard lock(lifecycleMutex_);
    worker_ = {};
    state_.reset();
    stopped_ = {};
}

ruvia::Task<void>
GbProjectionRuntime::consume(ruvia::WebWorkerContext& context, const std::shared_ptr<State>& state) {
    const auto stop =
        ruvia::combineStopTokens(context.stopToken(), state->stop.token());
    const auto redis = context.redis().withOptions({ .stopToken = stop });
    const std::string stream(control_protocol::stream::kProjection);
    const auto group = control_protocol::stream::kProjectionGroup;
    const auto consumer = service::runtime::instanceId() + ":service:" +
        std::to_string(workerIndex_);
    const std::vector<std::string> streams{ stream };
    auto nextReconcile = std::chrono::steady_clock::now();

    while (!stop.stopRequested()) {
        bool retry = false;
        std::string failure;
        try {
            co_await service::message::redis::ensureGroup(redis, stream, group);
            if (std::chrono::steady_clock::now() >= nextReconcile) {
                co_await reconcileExpiredOwners(context);
                nextReconcile = std::chrono::steady_clock::now() +
                    std::chrono::seconds(5);
            }
            auto batches = co_await service::message::redis::claimGroupMany(
                redis,
                streams,
                group,
                consumer,
                64
            );
            if (batches.empty()) {
                batches = co_await service::message::redis::readGroupMany(redis, streams, group, consumer, ">", 64);
            }
            if (batches.empty()) {
                (void)co_await ruvia::sleepFor(context.worker(), std::chrono::milliseconds(100), stop);
                continue;
            }
            for (const auto& batch : batches) {
                for (const auto& message : batch.messages) {
                    if (stop.stopRequested()) {
                        break;
                    }
                    const auto projectionId = message.get("projection_id");
                    const auto ownerToken = message.get("owner_token");
                    if (!service::common::isUuid(projectionId) || !control_protocol::stream::completeOwnerToken(ownerToken)) {
                        LOG_WARN << "[GB28181] invalid projection identity: " << message.id;
                        co_await service::message::redis::acknowledgeAndDelete(redis, batch.stream, group, message.id);
                        continue;
                    }
                    const auto projectionOrder = message.get("projection_order");
                    std::string cursorId(message.id);
                    if (!projectionOrder.empty()) {
                        if (!validProjectionStreamId(projectionOrder)) {
                            LOG_WARN << "[GB28181] invalid projection order: "
                                     << message.id;
                            co_await markProjectionDoneAndAcknowledge(
                                redis,
                                batch.stream,
                                group,
                                message.id,
                                projectionId,
                                false
                            );
                            continue;
                        }
                        cursorId = std::string(projectionOrder);
                    } else if (!validProjectionStreamId(cursorId)) {
                        LOG_WARN << "[GB28181] invalid projection stream ID: "
                                 << message.id;
                        co_await service::message::redis::acknowledgeAndDelete(
                            redis,
                            batch.stream,
                            group,
                            message.id
                        );
                        continue;
                    }
                    const auto eventType = message.get("event_type");
                    std::optional<Device> device;
                    std::optional<StreamStatus> streamStatus;
                    std::optional<DeviceChange> deviceChange;
                    std::string parseError;
                    try {
                        if (eventType == "gb28181.device") {
                            deviceChange =
                                projectionDeviceChange(message.get("change"));
                            if (!deviceChange) {
                                throw std::runtime_error(
                                    "GB28181 device projection has no valid change"
                                );
                            }
                            device = deviceFromProjection(message);
                        } else if (eventType == "gb28181.stream") {
                            streamStatus = streamFromProjection(message);
                        } else {
                            throw std::runtime_error(
                                "unsupported GB28181 projection event type"
                            );
                        }
                    } catch (const std::exception& error) {
                        parseError = error.what();
                    }
                    if (!parseError.empty()) {
                        LOG_WARN << "[GB28181][GbProjectionRuntime] dropping malformed projection: "
                                 << parseError;
                        co_await markProjectionDoneAndAcknowledge(
                            redis,
                            batch.stream,
                            group,
                            message.id,
                            projectionId,
                            false
                        );
                        continue;
                    }
                    bool applied = false;
                    if (device) {
                        if (co_await projectionOwnerMatches(
                                redis,
                                device->id,
                                device->online,
                                ownerToken
                            )) {
                            applied = co_await applyDeviceProjection(
                                context,
                                *device,
                                *deviceChange,
                                control_protocol::stream::owner(device->id),
                                std::string(ownerToken),
                                cursorId
                            );
                        }
                    } else if (streamStatus) {
                        const auto identity = StreamStatus::identity(
                            streamStatus->app,
                            streamStatus->stream,
                            streamStatus->schema
                        );
                        if (co_await projectionOwnerMatches(
                                redis,
                                identity,
                                streamStatus->online,
                                ownerToken
                            )) {
                            applied = co_await applyStreamProjection(
                                context,
                                *streamStatus,
                                control_protocol::stream::owner(identity),
                                std::string(ownerToken),
                                cursorId
                            );
                        }
                    }
                    co_await markProjectionDoneAndAcknowledge(
                        redis,
                        batch.stream,
                        group,
                        message.id,
                        projectionId,
                        applied
                    );
                }
            }
        } catch (const std::exception& error) {
            if (stop.stopRequested()) {
                break;
            }
            retry = true;
            failure = error.what();
        } catch (...) {
            if (stop.stopRequested()) {
                break;
            }
            retry = true;
        }
        if (retry && !stop.stopRequested()) {
            if (!failure.empty()) {
                LOG_WARN << "[GB28181][GbProjectionRuntime] projection failed: "
                         << failure;
            }
            (void)co_await ruvia::sleepFor(
                context.worker(),
                std::chrono::milliseconds(250),
                stop
            );
        }
    }
    co_return;
}

CollectorRuntime::CollectorRuntime(AppConfig config, ruvia::EventLoop loop, ruvia::RedisHandle redis, OwnerIndex index, OwnerIndex count)
    : config_(std::move(config)),
      loop_(std::move(loop)),
      worker_(loop_.handle()),
      redis_(std::move(redis)),
      index_(index),
      count_(count),
      scope_(worker_),
      projectionScope_(worker_),
      ownerToken_(service::runtime::instanceId() + ":collector:" + std::to_string(index_)),
      devices_([this](const Device& device, DeviceChange change) {
          enqueueDeviceProjection(device, change);
      }),
      streams_([this](const StreamStatus& stream) {
          enqueueStreamProjection(stream);
      }) {
    if (count_ == 0 || index_ >= count_) {
        throw std::invalid_argument(
            "GB28181 Collector owner index is outside worker count"
        );
    }
}

std::string CollectorRuntime::deviceOwnerToken(const Device& device) const {
    return ownerToken_ + ":session:" +
        std::to_string(device.sessionGeneration);
}

std::string CollectorRuntime::streamOwnerToken(const StreamStatus& stream) {
    const auto identity =
        StreamStatus::identity(stream.app, stream.stream, stream.schema);
    auto& generation = streamGenerations_[identity];
    const auto previous = streamOnline_.find(identity);
    if (stream.online && (previous == streamOnline_.end() || !previous->second)) {
        ++generation;
    }
    streamOnline_[identity] = stream.online;
    return ownerToken_ + ":stream:" + std::to_string(generation);
}

CollectorRuntime::~CollectorRuntime() {
    // A TaskScope owns coroutine frames that capture this object.  Releasing
    // the object while they are still active would turn a failed shutdown
    // into a use-after-free, so make lifecycle misuse fail closed.
    if (started_.load() || sdkRegistered_ || scope_.size() != 0 || projectionScope_.size() != 0) {
        std::terminate();
    }
}

void CollectorRuntime::requireCurrentLoop() const {
    if (!loop_.valid() || !worker_.valid() || !loop_.isCurrent()) {
        throw std::logic_error(
            "GB28181 CollectorRuntime operation must run on its owning loop"
        );
    }
    if (count_ == 0 || index_ >= count_) {
        throw std::logic_error("GB28181 CollectorRuntime owner is invalid");
    }
}

void CollectorRuntime::registerSdkRoute() {
    requireCurrentLoop();
    if (sdkRegistered_) {
        return;
    }

    SdkSupervisor::CollectorCallbacks callbacks;
    callbacks.onStreamChanged =
        [this](std::string app, std::string stream, std::string schema, bool online, int readerCount) {
            if (stopping_.load()) {
                return;
            }
            streams_.updateStreamChanged(app, stream, schema, online, readerCount);
        };
    callbacks.onRtpDetached = [this](std::string stream) {
        if (stopping_.load() || !sip_) {
            return;
        }
        // This callback is posted by SdkSupervisor to this Collector's loop.
        // The SIP session and its RTP server therefore remain in the same
        // owner for their entire lifetime.
        (void)sip_->stopPreviewByStream(stream);
    };
    sdkSupervisor().registerCollector(index_, loop_, std::move(callbacks));
    sdkRegistered_ = true;
}

void CollectorRuntime::unregisterSdkRoute() noexcept {
    if (!sdkRegistered_) {
        return;
    }
    sdkSupervisor().unregisterCollector(index_);
    sdkRegistered_ = false;
}

ruvia::Task<void> CollectorRuntime::initialize() {
    requireCurrentLoop();
    if (started_.load()) {
        co_return;
    }
    if (stopping_.load()) {
        throw std::logic_error("GB28181 CollectorRuntime cannot restart");
    }
    if (!config_.enabled) {
        co_await publishConfig();
        co_return;
    }

    if (config_.sip.domain.empty() || config_.sip.id.empty() ||
        config_.sip.publicIp.empty() || config_.sip.password.empty() ||
        config_.media.rtpPublicIp.empty() ||
        config_.media.playTokenSecret.size() < 16) {
        throw std::runtime_error(
            "GB28181 configuration requires domain, id, public IP, password, "
            "RTP IP and a media token secret of at least 16 characters"
        );
    }
    const auto randomRtpPort = config_.media.rtpPortRangeStart == 0 &&
        config_.media.rtpPortRangeEnd == 0;
    if (!randomRtpPort &&
        (config_.media.rtpPortRangeStart == 0 ||
         config_.media.rtpPortRangeStart > config_.media.rtpPortRangeEnd)) {
        throw std::runtime_error("GB28181 RTP port range is invalid");
    }
    if (config_.media.workerThreads <= 0) {
        throw std::runtime_error("ZLM worker thread count must be positive");
    }
    if (config_.media.logLevel < 0 || config_.media.logLevel > 4) {
        throw std::runtime_error("ZLM log level must be between 0 and 4");
    }
    if (config_.sip.deviceTimezoneOffsetMinutes < -24 * 60 ||
        config_.sip.deviceTimezoneOffsetMinutes > 24 * 60) {
        throw std::runtime_error("GB28181 device timezone offset is invalid");
    }
    if (!sdkSupervisor().started()) {
        throw std::runtime_error(
            "ZLMediaKit SDK supervisor must start before GB28181 Collector"
        );
    }

    std::exception_ptr startupFailure;
    try {
        registerSdkRoute();
        sip_ = std::make_shared<SipServer>(
            config_.sip,
            config_.media,
            devices_,
            sdkSupervisor().sdk(),
            loop_,
            [this](const std::string& stream, unsigned int viewerCount) {
                if (!stopping_.load()) {
                    streams_.updateViewerCount(stream, static_cast<int>(viewerCount));
                }
            },
            index_
        );
        sip_->setAcknowledgementHandler([this](ProjectionCompletion completion) {
            enqueueBarrier(std::move(completion), lastSipBarrierSequence_);
            lastSipBarrierSequence_ = projectionSequence_;
        });
        sip_->setStreamClosedHandler([this](const std::string& streamId) {
            for (const auto& stream : streams_.listStreams()) {
                if (stream.stream == streamId && stream.online) {
                    streams_.updateStreamChanged(stream.app, stream.stream, stream.schema, false, 0);
                }
            }
        });
        sip_->setPreviewClosedHandler([this](const std::string& sessionId) {
            if (!projectionScope_.stopRequested()) {
                projectionScope_.spawn(releasePreview(sessionId));
            }
        });
        sip_->start();
        started_.store(true);
        co_await publishConfig();
        scope_.spawn(controlLoop());
        scope_.spawn(refreshOwnerLeases());
    } catch (...) {
        startupFailure = std::current_exception();
    }
    if (startupFailure) {
        co_await shutdown();
        std::rethrow_exception(startupFailure);
    }
    co_return;
}

ruvia::Task<void> CollectorRuntime::shutdown() {
    requireCurrentLoop();
    if (scopeJoined_) {
        co_return;
    }
    stopping_.store(true);
    started_.store(false);
    scope_.requestStop();
    if (sip_) {
        sip_->stop();
    }
    unregisterSdkRoute();
    for (const auto& stream : streams_.listStreams()) {
        if (stream.online) {
            streams_.updateStreamChanged(stream.app, stream.stream, stream.schema, false, 0);
        }
    }
    // stop() emits the final local offline snapshots before closing admission.
    acceptingProjection_ = false;
    try {
        co_await scope_.join();
    } catch (const std::exception& error) {
        LOG_WARN << "[GB28181] control shutdown: " << error.what();
    }
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (projectionDrainRunning_ && std::chrono::steady_clock::now() < until) {
        (void)co_await ruvia::sleepFor(worker_, std::chrono::milliseconds(20));
    }
    projectionScope_.requestStop();
    try {
        co_await projectionScope_.join();
    } catch (...) {
    }
    // No producer or renewal coroutine can recreate an owner after this point.
    co_await clearOwnerLeases();
    for (auto& event : projectionQueue_) {
        if (auto* completion = std::get_if<ProjectionBarrier>(&event)) {
            completion->complete(false);
        }
    }
    if (!projectionQueue_.empty()) {
        LOG_WARN << "[GB28181] stopped with unconfirmed projections; no positive SIP acknowledgement was issued";
    }
    projectionQueue_.clear();
    scopeJoined_ = true;
    sip_.reset();
    co_return;
}

void CollectorRuntime::enqueueDeviceProjection(const Device& device, DeviceChange change) {
    if (!acceptingProjection_) {
        return;
    }
    projectionQueue_.push_back(DeviceProjection{ device, change, deviceOwnerToken(device), service::common::nextUuidV7(), ++projectionSequence_ });
    if (!projectionDrainRunning_) {
        projectionDrainRunning_ = true;
        projectionScope_.spawn(drainProjection());
    }
}

void CollectorRuntime::enqueueStreamProjection(const StreamStatus& stream) {
    if (!acceptingProjection_) {
        return;
    }
    projectionQueue_.push_back(StreamProjection{ stream, streamOwnerToken(stream), service::common::nextUuidV7(), ++projectionSequence_ });
    if (!projectionDrainRunning_) {
        projectionDrainRunning_ = true;
        projectionScope_.spawn(drainProjection());
    }
}

void CollectorRuntime::enqueueBarrier(ProjectionCompletion completion, std::uint64_t after) {
    if (!acceptingProjection_ || projectionScope_.stopRequested()) {
        completion(false);
        return;
    }
    projectionQueue_.push_back(ProjectionBarrier{ std::move(completion), after, projectionSequence_ });
    if (!projectionDrainRunning_) {
        projectionDrainRunning_ = true;
        projectionScope_.spawn(drainProjection());
    }
}

ruvia::Task<bool> CollectorRuntime::waitForProjection(ruvia::StopToken stop, std::uint64_t after) {
    auto [completion, receiver] = ruvia::makeOneShot<bool>(worker_);
    auto shared = std::make_shared<ruvia::OneShotCompletion<bool>>(std::move(completion));
    enqueueBarrier([shared](bool success) {
        (void)shared->complete(success);
    },
                   after);
    auto result = co_await receiver.waitFor(std::chrono::seconds(30), stop);
    co_return result.hasValue() && std::move(result).takeValue();
}

ruvia::Task<bool> CollectorRuntime::persistProjection(
    std::vector<service::message::StreamField> fields,
    std::string projectionId
) {
    fields.push_back({ "projection_id", projectionId });
    const auto redis = redis_.withOptions({ .timeout = std::chrono::seconds(3), .stopToken = projectionScope_.stopToken() });
    // The first stream ID is the actor's ordering cursor.  A later repair may
    // need a new Redis entry after the old one has been acknowledged/deleted,
    // but it must carry this original order so a retry cannot move the DB
    // cursor backwards or reapply an older event over a newer one.
    static constexpr std::string_view script = R"lua(
local function valid_id(id)
  return id ~= nil and string.match(id, '^%d+%-%d+$') ~= nil
end

local marker = redis.call('GET', KEYS[2])
local order = ARGV[1]
local current = ARGV[2]
if order ~= '' and not valid_id(order) then
  return redis.error_reply('invalid local GB28181 projection order')
end
if current ~= '' and not valid_id(current) then
  return redis.error_reply('invalid local GB28181 projection entry')
end

if marker then
  local separator = string.find(marker, '|', 1, true)
  if not separator or string.find(marker, '|', separator + 1, true) then
    return redis.error_reply('invalid GB28181 projection sent marker')
  end
  local marker_order = string.sub(marker, 1, separator - 1)
  local marker_current = string.sub(marker, separator + 1)
  if not valid_id(marker_order) or not valid_id(marker_current) then
    return redis.error_reply('invalid GB28181 projection sent marker IDs')
  end
  if order ~= '' and order ~= marker_order then
    return redis.error_reply('GB28181 projection order changed')
  end
  order = marker_order
  -- If the previous XADD succeeded but its reply was lost, the marker's
  -- current ID is authoritative and avoids a duplicate repair entry.
  current = marker_current
end

if current ~= '' then
  local existing = redis.call('XRANGE', KEYS[1], current, current)
  if #existing > 0 then
    -- A marker may have expired while this coroutine was waiting.  Restore
    -- it once, without refreshing an existing marker on every poll.
    if not marker then
      redis.call('SET', KEYS[2], order .. '|' .. current, 'EX', 86400)
    end
    return { order, current }
  end
end

local values = {}
for index = 3, #ARGV do
  values[#values + 1] = ARGV[index]
end
if order ~= '' then
  values[#values + 1] = 'projection_order'
  values[#values + 1] = order
end
local id = redis.call('XADD', KEYS[1], '*', unpack(values))
if order == '' then
  order = id
end
redis.call('SET', KEYS[2], order .. '|' .. id, 'EX', 86400)
return { order, id }
)lua";
    const auto sentKey = "iot:gb28181:projection:sent:" + projectionId;
    const std::string_view keys[]{ control_protocol::stream::kProjection, sentKey };
    const auto doneKey = control_protocol::stream::projectionDone(projectionId);
    std::string projectionOrder;
    std::string currentEntryId;
    while (!projectionScope_.stopRequested()) {
        bool retry = false;
        std::string failure;
        try {
            const auto done = co_await redis.get(doneKey);
            if (done) {
                if (*done != "0" && *done != "1") {
                    throw std::runtime_error("invalid GB28181 projection receipt");
                }
                co_return std::string_view(*done) == "1";
            }

            std::vector<std::string_view> args;
            args.reserve(2 + fields.size() * 2);
            args.push_back(projectionOrder);
            args.push_back(currentEntryId);
            for (const auto& field : fields) {
                args.push_back(field.name);
                args.push_back(field.value);
            }
            const auto published = co_await redis.eval(script, keys, args);
            const auto result = parseProjectionPublishResult(published);
            if (!projectionOrder.empty() &&
                projectionOrder != result.order) {
                throw std::runtime_error(
                    "GB28181 projection ordering ID changed during repair"
                );
            }
            projectionOrder = result.order;
            currentEntryId = result.current;
        } catch (const std::exception& error) {
            retry = true;
            failure = error.what();
        } catch (...) {
            retry = true;
        }
        if (retry) {
            // Keep the ordering IDs in this coroutine across Redis transport
            // failures.  In particular, an XADD may have committed while its
            // response was lost; the sent marker or the local IDs then make
            // the next attempt recoverable without a duplicate event.
            if (!projectionScope_.stopRequested()) {
                if (!failure.empty()) {
                    LOG_WARN << "[GB28181] projection awaits durable storage: "
                             << failure;
                }
                (void)co_await ruvia::sleepFor(
                    worker_,
                    std::chrono::milliseconds(250),
                    projectionScope_.stopToken()
                );
            }
            continue;
        }
        // Incremental projections must remain in actor order. A caller may
        // time out, but this queue cannot publish the next snapshot until the
        // preceding DB commit has a definitive receipt (or loses ownership).
        (void)co_await ruvia::sleepFor(worker_, std::chrono::milliseconds(20), projectionScope_.stopToken());
    }
    co_return false;
}

ruvia::Task<void> CollectorRuntime::publishDevice(const Device& device, DeviceChange change, std::string_view ownerToken, std::string_view projectionId) {
    const auto key = control_protocol::stream::owner(device.id);
    const auto current = devices_.findDevice(device.id);
    if (current && (current->sessionGeneration != device.sessionGeneration || (device.online && !current->online))) {
        currentProjectionSucceeded_ = false;
        co_return;
    }
    if (device.online && !(co_await retainOwner(key, std::string(ownerToken), device.id))) {
        currentProjectionSucceeded_ = false;
        co_return;
    }
    const auto persisted = co_await persistProjection(
        deviceProjectionFields(device, change, index_, ownerToken),
        std::string(projectionId)
    );
    currentProjectionSucceeded_ = currentProjectionSucceeded_ && persisted;
    if (!device.online) {
        co_await releaseOwner(key, std::string(ownerToken));
    }
}

ruvia::Task<void> CollectorRuntime::publishStream(const StreamStatus& stream, std::string_view ownerToken, std::string_view projectionId) {
    const auto identity = StreamStatus::identity(stream.app, stream.stream, stream.schema);
    const auto key = control_protocol::stream::owner(identity);
    if (stream.online && !(co_await retainOwner(key, std::string(ownerToken), {}, stream.stream))) {
        currentProjectionSucceeded_ = false;
        co_return;
    }
    const auto persisted = co_await persistProjection(
        streamProjectionFields(stream, index_, ownerToken),
        std::string(projectionId)
    );
    currentProjectionSucceeded_ = currentProjectionSucceeded_ && persisted;
    if (!stream.online) {
        co_await releaseOwner(key, std::string(ownerToken));
    }
}

ruvia::Task<void> CollectorRuntime::publishConfig() {
    // Configuration is immutable for the process lifetime.  Keeping one
    // Redis projection lets Service Workers answer config queries without a
    // process-global GB runtime or a cross-worker callback.
    co_await service::message::redis::setHash(
        redis_,
        control_protocol::stream::kConfigKey,
        { { "enabled", config_.enabled ? "1" : "0" },
          { "domain", config_.sip.domain },
          { "id", config_.sip.id },
          { "host", config_.sip.host },
          { "public_ip", config_.sip.publicIp },
          { "port", std::to_string(config_.sip.port) },
          { "transport", config_.sip.transport },
          { "registration_timeout_seconds",
            std::to_string(config_.sip.registrationTimeoutSeconds) },
          { "command_timeout_seconds",
            std::to_string(config_.sip.commandTimeoutSeconds) },
          { "invite_timeout_seconds",
            std::to_string(config_.sip.inviteTimeoutSeconds) },
          { "viewer_lease_timeout_seconds",
            std::to_string(config_.sip.viewerLeaseTimeoutSeconds) } }
    );
    co_return;
}

ruvia::Task<void> CollectorRuntime::refreshOwnerLeases() {
    requireCurrentLoop();
    while (!scope_.stopRequested()) {
        const auto sleep = co_await ruvia::sleepFor(
            worker_,
            std::chrono::seconds(5),
            scope_.stopToken()
        );
        if (sleep == ruvia::TimerSleepResult::kStopRequested ||
            scope_.stopRequested()) {
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        std::vector<std::pair<std::string, std::string>> toRenew;
        std::vector<std::pair<std::string, std::string>> expired;
        toRenew.reserve(ownerLeases_.size());
        expired.reserve(ownerLeases_.size());
        for (auto& [key, lease] : ownerLeases_) {
            if (lease.expiresAt <= now) {
                expired.emplace_back(key, lease.token);
                continue;
            }
            if (!lease.renewing) {
                lease.renewing = true;
                toRenew.emplace_back(key, lease.token);
            }
        }
        for (auto& [key, token] : expired) {
            expireOwnerLease(std::move(key), std::move(token));
        }
        for (auto& [key, token] : toRenew) {
            try {
                scope_.spawn(renewOwnerLease(std::move(key), std::move(token)));
            } catch (const std::exception& error) {
                if (const auto found = ownerLeases_.find(key);
                    found != ownerLeases_.end() && found->second.token == token) {
                    found->second.renewing = false;
                }
                LOG_WARN << "[GB28181][Collector] owner lease renewal task could not start: "
                         << error.what();
            }
        }
    }
    co_return;
}

ruvia::Task<void> CollectorRuntime::clearOwnerLeases() {
    requireCurrentLoop();
    std::vector<std::pair<std::string, std::string>> leases;
    leases.reserve(ownerLeases_.size());
    for (auto& [key, lease] : ownerLeases_) {
        if (lease.timer) {
            std::error_code ignored;
            lease.timer->cancel(ignored);
        }
        leases.emplace_back(key, lease.token);
    }
    ownerLeases_.clear();

    for (auto& [key, token] : leases) {
        try {
            co_await releaseOwner(std::move(key), std::move(token));
        } catch (const std::exception& error) {
            LOG_WARN << "[GB28181][Collector] owner cleanup failed: "
                     << error.what();
        }
    }
    co_return;
}

ruvia::Task<bool> CollectorRuntime::retainOwner(
    std::string key,
    std::string token,
    std::string deviceId,
    std::string streamId
) {
    requireCurrentLoop();
    if (stopping_.load() || key.empty() || token.empty() ||
        !control_protocol::stream::completeOwnerToken(token)) {
        co_return false;
    }

    if (const auto retired = retiredOwnerTokens_.find(key);
        retired != retiredOwnerTokens_.end() && retired->second == token) {
        co_return false;
    }
    constexpr auto kRedisLease = std::chrono::milliseconds(15000);
    constexpr auto kLocalLease = std::chrono::seconds(12);
    const auto operationStarted = std::chrono::steady_clock::now();
    const auto localDeadline = operationStarted + kLocalLease;

    const auto local = ownerLeases_.find(key);
    const bool hasLocal = local != ownerLeases_.end();
    const std::string previousToken =
        hasLocal ? local->second.token : std::string{};
    const auto previousDeadline =
        hasLocal ? local->second.expiresAt
                 : std::chrono::steady_clock::time_point::max();

    static constexpr std::string_view renewScript = R"lua(
if redis.call('GET', KEYS[1]) ~= ARGV[1] then return 0 end
return redis.call('PEXPIRE', KEYS[1], ARGV[2])
)lua";
    static constexpr std::string_view claimScript = R"lua(
local current=redis.call('GET',KEYS[1])
if current==ARGV[1] then
  redis.call('PEXPIRE',KEYS[1],ARGV[3])
  return 1
end
if ARGV[2]~='' then
  if current~=ARGV[2] then return 0 end
  redis.call('SET',KEYS[1],ARGV[1],'PX',ARGV[3])
  return 1
end
if current then return 0 end
local claimed=redis.call('SET',KEYS[1],ARGV[1],'PX',ARGV[3],'NX')
return claimed and 1 or 0
)lua";

    bool retained = false;
    try {
        const auto redis = redis_.withOptions(
            { .timeout = std::chrono::seconds(3) }
        );
        const std::string leaseTtl = std::to_string(kRedisLease.count());
        const std::string_view keys[]{ key };
        if (hasLocal && previousToken == token) {
            const std::string_view args[]{ token, leaseTtl };
            const auto reply = co_await redis.eval(renewScript, keys, args);
            if (reply.kind() != ruvia::RedisValue::Kind::kInteger) {
                service::message::redis::throwValue(
                    "renew GB28181 owner lease",
                    reply
                );
            }
            retained = reply.integer() == 1;
        } else {
            const std::string_view args[]{ token, previousToken, leaseTtl };
            const auto reply = co_await redis.eval(claimScript, keys, args);
            if (reply.kind() != ruvia::RedisValue::Kind::kInteger) {
                service::message::redis::throwValue(
                    "claim GB28181 owner lease",
                    reply
                );
            }
            retained = reply.integer() == 1;
        }
    } catch (const std::exception& error) {
        LOG_WARN << "[GB28181][Collector] owner lease claim failed: "
                 << error.what();
    } catch (...) {
        LOG_WARN << "[GB28181][Collector] owner lease claim failed";
    }

    const auto now = std::chrono::steady_clock::now();
    auto current = ownerLeases_.find(key);
    const bool localStillValid =
        !hasLocal || (current != ownerLeases_.end() && current->second.token == previousToken);
    if (!retained || now >= localDeadline || now >= previousDeadline || !localStillValid || scope_.stopRequested()) {
        if (hasLocal && current != ownerLeases_.end() && current->second.token == previousToken) {
            expireOwnerLease(key, previousToken);
        } else if (sip_ && !stopping_.load()) {
            invalidateOwnerTarget(key, token, deviceId, streamId);
        }
        if (key.starts_with("iot:gb28181:owner:")) {
            retiredOwnerTokens_[key] = token;
        }
        try {
            co_await releaseOwner(key, token);
        } catch (...) {
        }
        co_return false;
    }

    current = ownerLeases_.find(key);
    if (current != ownerLeases_.end() && current->second.token != token &&
        (!hasLocal || current->second.token != previousToken)) {
        try {
            co_await releaseOwner(key, token);
        } catch (...) {
        }
        if (sip_ && !stopping_.load()) {
            invalidateOwnerTarget(key, token, deviceId, streamId);
        }
        co_return false;
    }

    const auto deadline = localDeadline;
    if (deadline <= now) {
        try {
            co_await releaseOwner(key, token);
        } catch (...) {
        }
        if (sip_ && !stopping_.load()) {
            invalidateOwnerTarget(key, token, deviceId, streamId);
        }
        co_return false;
    }

    if (current != ownerLeases_.end() && current->second.token != token) {
        if (current->second.timer) {
            std::error_code ignored;
            current->second.timer->cancel(ignored);
        }
        ownerLeases_.erase(current);
        current = ownerLeases_.end();
    }
    if (current == ownerLeases_.end()) {
        OwnerLease lease;
        lease.token = token;
        lease.deviceId = std::move(deviceId);
        lease.streamId = std::move(streamId);
        lease.expiresAt = deadline;
        current = ownerLeases_.emplace(std::move(key), std::move(lease)).first;
    } else {
        if (!deviceId.empty()) {
            current->second.deviceId = std::move(deviceId);
        }
        if (!streamId.empty()) {
            current->second.streamId = std::move(streamId);
        }
        current->second.expiresAt = deadline;
        current->second.renewing = false;
    }
    armOwnerLeaseTimer(current->first);
    co_return true;
}

bool CollectorRuntime::ownsLocal(std::string_view key, std::string_view token) const {
    if (key.empty() || token.empty()) {
        return false;
    }
    const auto found = ownerLeases_.find(std::string(key));
    return found != ownerLeases_.end() && found->second.token == token &&
        found->second.expiresAt > std::chrono::steady_clock::now();
}

ruvia::Task<void> CollectorRuntime::releaseOwner(std::string key, std::string token) {
    requireCurrentLoop();
    if (key.empty() || token.empty()) {
        co_return;
    }

    if (const auto found = ownerLeases_.find(key);
        found != ownerLeases_.end() && found->second.token == token) {
        if (found->second.timer) {
            std::error_code ignored;
            found->second.timer->cancel(ignored);
        }
        ownerLeases_.erase(found);
    }

    static constexpr std::string_view script = R"lua(
if redis.call('GET',KEYS[1])==ARGV[1] then
  return redis.call('DEL',KEYS[1])
end
return 0
)lua";
    const auto redis = redis_.withOptions({ .timeout = std::chrono::seconds(3) });
    const std::string_view keys[]{ key };
    const std::string_view args[]{ token };
    const auto reply = co_await redis.eval(script, keys, args);
    if (reply.kind() != ruvia::RedisValue::Kind::kInteger) {
        service::message::redis::throwValue(
            "release GB28181 owner lease",
            reply
        );
    }
    co_return;
}

ruvia::Task<void> CollectorRuntime::renewOwnerLease(std::string key, std::string token) {
    requireCurrentLoop();
    const auto found = ownerLeases_.find(key);
    if (found == ownerLeases_.end() || found->second.token != token) {
        co_return;
    }
    const auto operationStarted = std::chrono::steady_clock::now();
    const auto previousDeadline = found->second.expiresAt;
    if (previousDeadline <= std::chrono::steady_clock::now()) {
        expireOwnerLease(key, token);
        co_return;
    }

    bool renewed = false;
    static constexpr std::string_view script = R"lua(
if redis.call('GET',KEYS[1])~=ARGV[1] then return 0 end
return redis.call('PEXPIRE',KEYS[1],ARGV[2])
)lua";
    try {
        const auto redis = redis_.withOptions(
            { .timeout = std::chrono::seconds(3) }
        );
        const std::string leaseTtl = "15000";
        const std::string_view keys[]{ key };
        const std::string_view args[]{ token, leaseTtl };
        const auto reply = co_await redis.eval(script, keys, args);
        if (reply.kind() != ruvia::RedisValue::Kind::kInteger) {
            service::message::redis::throwValue(
                "renew GB28181 owner lease",
                reply
            );
        }
        renewed = reply.integer() == 1;
    } catch (const std::exception& error) {
        LOG_WARN << "[GB28181][Collector] owner lease renewal failed: "
                 << error.what();
    } catch (...) {
        LOG_WARN << "[GB28181][Collector] owner lease renewal failed";
    }

    const auto now = std::chrono::steady_clock::now();
    const auto current = ownerLeases_.find(key);
    if (current == ownerLeases_.end() || current->second.token != token) {
        co_return;
    }
    current->second.renewing = false;
    if (!renewed || now >= previousDeadline) {
        expireOwnerLease(key, token);
        try {
            co_await releaseOwner(key, token);
        } catch (const std::exception& error) {
            LOG_WARN << "[GB28181][Collector] expired owner cleanup failed: "
                     << error.what();
        } catch (...) {
            LOG_WARN << "[GB28181][Collector] expired owner cleanup failed";
        }
        co_return;
    }

    current->second.expiresAt = operationStarted + std::chrono::seconds(12);
    armOwnerLeaseTimer(current->first);
    co_return;
}

void CollectorRuntime::armOwnerLeaseTimer(std::string key) {
    if (!loop_.valid() || !loop_.isCurrent()) {
        return;
    }
    const auto found = ownerLeases_.find(key);
    if (found == ownerLeases_.end()) {
        return;
    }
    auto& lease = found->second;
    if (!lease.timer) {
        lease.timer = std::make_unique<asio::steady_timer>(loop_.ioContext());
    }
    lease.timer->expires_at(lease.expiresAt);
    const auto timerToken = lease.token;
    lease.timer->async_wait(
        [this, key = std::move(key), timerToken](const std::error_code& error) {
            if (error) {
                return;
            }
            const auto current = ownerLeases_.find(key);
            if (current == ownerLeases_.end() || current->second.token != timerToken) {
                return;
            }
            if (current->second.expiresAt > std::chrono::steady_clock::now()) {
                armOwnerLeaseTimer(key);
            } else {
                expireOwnerLease(key, timerToken);
            }
        }
    );
}

void CollectorRuntime::expireOwnerLease(std::string key, std::string token) {
    if (!loop_.valid() || !loop_.isCurrent()) {
        return;
    }
    const auto found = ownerLeases_.find(key);
    if (found == ownerLeases_.end() || found->second.token != token) {
        return;
    }
    if (key.starts_with("iot:gb28181:owner:")) {
        retiredOwnerTokens_[key] = token;
    }
    auto deviceId = std::move(found->second.deviceId);
    auto streamId = std::move(found->second.streamId);
    ownerLeases_.erase(found);
    if (stopping_.load() || !sip_) {
        return;
    }
    invalidateOwnerTarget(key, token, deviceId, streamId);
}

void CollectorRuntime::invalidateOwnerTarget(const std::string& key, const std::string& token, const std::string& deviceId, const std::string& streamId) {
    if (!sip_ || stopping_.load()) {
        return;
    }
    constexpr std::string_view prefix = "iot:gb28181:session-owner:";
    if (key.starts_with(prefix)) {
        (void)sip_->stopPreview(key.substr(prefix.size()));
        return;
    }
    if (!deviceId.empty()) {
        const auto device = devices_.findDevice(deviceId);
        if (device && deviceOwnerToken(*device) == token) {
            sip_->invalidateDevice(deviceId, "owner_lease_lost");
        }
    }
    if (!streamId.empty()) {
        const auto current = ownerLeases_.find(key);
        if (current == ownerLeases_.end() || current->second.token == token) {
            (void)sip_->stopPreviewByStream(streamId);
        }
    }
}

ruvia::Task<void> CollectorRuntime::drainProjection() {
    requireCurrentLoop();
    while (!projectionQueue_.empty() && !projectionScope_.stopRequested()) {
        auto event = std::move(projectionQueue_.front());
        projectionQueue_.pop_front();
        bool retry = false;
        currentProjectionSucceeded_ = true;
        std::uint64_t eventSequence = 0;
        if (auto* device = std::get_if<DeviceProjection>(&event)) {
            eventSequence = device->sequence;
        }
        if (auto* stream = std::get_if<StreamProjection>(&event)) {
            eventSequence = stream->sequence;
        }
        try {
            if (auto* device = std::get_if<DeviceProjection>(&event)) {
                co_await publishDevice(device->device, device->change, device->ownerToken, device->projectionId);
            } else if (auto* stream = std::get_if<StreamProjection>(&event)) {
                co_await publishStream(stream->stream, stream->ownerToken, stream->projectionId);
            } else {
                const auto& barrier = std::get<ProjectionBarrier>(event);
                const bool failed = std::any_of(projectionFailures_.begin(), projectionFailures_.end(), [&](const auto& failure) {
                    return failure > barrier.after && failure <= barrier.through;
                });
                barrier.complete(!failed);
            }
        } catch (const std::exception& error) {
            retry = true;
            if (!projectionScope_.stopRequested()) {
                LOG_WARN << "[GB28181] projection awaits durable storage: " << error.what();
            }
        } catch (...) {
            retry = true;
        }
        if (!retry && eventSequence && !currentProjectionSucceeded_) {
            projectionFailures_.push_back(eventSequence);
        }
        auto oldestNeeded = std::min(lastSipBarrierSequence_, activeControlProjection_.value_or(projectionSequence_));
        for (const auto& queued : projectionQueue_) {
            if (const auto* barrier = std::get_if<ProjectionBarrier>(&queued)) {
                oldestNeeded = std::min(oldestNeeded, barrier->after);
            }
        }
        while (!projectionFailures_.empty() && projectionFailures_.front() <= oldestNeeded) {
            projectionFailures_.pop_front();
        }
        if (retry) {
            projectionQueue_.push_front(std::move(event));
            (void)co_await ruvia::sleepFor(worker_, std::chrono::milliseconds(250), projectionScope_.stopToken());
        }
    }
    projectionDrainRunning_ = false;
}

ruvia::Task<void> CollectorRuntime::controlLoop() {
    requireCurrentLoop();
    const auto stream = control_protocol::stream::control(index_);
    const auto group = control_protocol::stream::kControlGroup;
    const auto consumer = ownerToken_;
    const auto redis = redis_.withOptions({ .stopToken = scope_.stopToken() });
    const std::vector<std::string> streams{ stream };
    while (!scope_.stopRequested()) {
        bool failed = false;
        try {
            static constexpr std::string_view groupScript = R"lua(
local created=redis.pcall('XGROUP','CREATE',KEYS[1],ARGV[1],'0','MKSTREAM')
if type(created)=='table' and created.err and not string.find(created.err,'BUSYGROUP',1,true) then
 return redis.error_reply(created.err)
end
redis.call('EXPIRE',KEYS[1],600)
return 1
)lua";
            const std::string_view groupKeys[]{ stream };
            const std::string_view groupArgs[]{ group };
            const auto initialized = co_await redis.eval(groupScript, groupKeys, groupArgs);
            if (initialized.kind() != ruvia::RedisValue::Kind::kInteger) {
                service::message::redis::throwValue("GB28181 control group", initialized);
            }

            // This stream is exclusive to this process incarnation and owner.
            const auto pending = co_await service::message::redis::claimGroupMany(
                redis,
                streams,
                group,
                consumer,
                32,
                std::chrono::milliseconds(0)
            );
            for (const auto& batch : pending) {
                for (const auto& message : batch.messages) {
                    co_await handleControl(message);
                }
            }
            const auto messages = co_await service::message::redis::readGroupBlockingUntil(
                redis,
                stream,
                group,
                consumer,
                scope_.stopToken(),
                std::chrono::milliseconds(1000),
                32
            );
            for (const auto& message : messages) {
                co_await handleControl(message);
            }
        } catch (const std::exception& error) {
            failed = true;
            if (!scope_.stopRequested()) {
                LOG_WARN << "[GB28181] control consumer: " << error.what();
            }
        }
        if (failed) {
            (void)co_await ruvia::sleepFor(worker_, std::chrono::milliseconds(250), scope_.stopToken());
        }
    }
}

ruvia::Task<void> CollectorRuntime::handleControl(
    const service::message::StreamMessage& message
) {
    requireCurrentLoop();
    const auto requestId = std::string(message.get("request_id"));
    if (!service::common::isUuid(requestId)) {
        const std::string id = message.id;
        co_await service::message::redis::acknowledgeAndDelete(redis_, control_protocol::stream::control(index_), control_protocol::stream::kControlGroup, id);
        co_return;
    }
    const auto replyStream = control_protocol::stream::reply(requestId);
    const auto operation = std::string(message.get("operation"));
    const auto payload = std::string(message.get("payload"));
    const auto resultKey = "iot:gb28181:result:" + requestId;
    const auto claimKey = resultKey + ":claim";
    const auto cancelKey = control_protocol::stream::cancel(requestId);
    const auto key = std::string(message.get("owner_key"));
    const auto token = std::string(message.get("owner_token"));
    const auto deadlineText = message.get("deadline_ms");
    std::int64_t deadlineMs{};
    const auto parsed = std::from_chars(deadlineText.data(), deadlineText.data() + deadlineText.size(), deadlineMs);
    const bool validDeadline = !deadlineText.empty() && parsed.ec == std::errc{} &&
        parsed.ptr == deadlineText.data() + deadlineText.size();
    const auto nowMs = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()
        )
            .count();
    };
    std::string status, result, claimOwner;
    const auto fail = [&](int code, std::string_view reason) {
        status = "error";
        result = "{\"code\":" + std::to_string(code) + ",\"message\":" +
            service::utils::jsonQuoted(reason) + "}";
    };
    const auto redis = redis_.withOptions({ .timeout = std::chrono::seconds(3), .stopToken = scope_.stopToken() });
    const auto cached = co_await redis.get(resultKey);
    if (cached) {
        const std::string encoded(*cached);
        const auto delimiter = encoded.find('\n');
        status = delimiter == std::string::npos ? "ok" : encoded.substr(0, delimiter);
        result = delimiter == std::string::npos ? encoded : encoded.substr(delimiter + 1);
    } else if (!validDeadline || deadlineMs > nowMs() + 60000) {
        fail(400, "invalid command deadline");
    } else if (deadlineMs <= nowMs()) {
        fail(408, "GB28181 command expired");
    } else if (!ownsLocal(key, token)) {
        fail(409, "stale GB28181 connection owner");
    } else {
        // Claim and owner/cancel/deadline checks are atomic in Redis.  The
        // claim lives longer than the mandatory maximum command lifetime.
        static constexpr std::string_view script = R"lua(
if redis.call('GET', KEYS[1]) ~= ARGV[1] then return -1 end
if redis.call('EXISTS', KEYS[3]) == 1 then return -2 end
local t = redis.call('TIME')
if tonumber(t[1])*1000 + math.floor(tonumber(t[2])/1000) >= tonumber(ARGV[2]) then return -3 end
if redis.call('SET', KEYS[2], ARGV[1], 'NX', 'EX', 600) then return 1 end
return 0
)lua";
        const std::string deadlineValue(deadlineText);
        const std::string_view keys[]{ key, claimKey, cancelKey };
        const std::string_view args[]{ token, deadlineValue };
        const auto claimed = co_await redis.eval(script, keys, args);
        if (claimed.kind() != ruvia::RedisValue::Kind::kInteger) {
            service::message::redis::throwValue("GB28181 command claim", claimed);
        }
        if (claimed.integer() != 1) {
            fail(409, claimed.integer() == 0 ? "previous execution outcome is unknown; command will not be replayed" : "GB28181 command expired, cancelled or lost its owner");
        } else {
            claimOwner = token;
            const auto cancelled = co_await redis.get(cancelKey);
            if (cancelled || scope_.stopRequested() || deadlineMs <= nowMs() || !ownsLocal(key, token)) {
                fail(408, "GB28181 command cancelled or ownership expired while claiming");
            } else {
                try {
                    const auto beforeProjection = projectionSequence_;
                    activeControlProjection_ = beforeProjection;
                    result = co_await execute(operation, payload, scope_.stopToken());
                    if (!(co_await waitForProjection(scope_.stopToken(), beforeProjection))) {
                        throw std::runtime_error("GB28181 projection was not durably confirmed");
                    }
                    status = "ok";
                } catch (const std::exception& error) {
                    fail(500, error.what());
                } catch (...) {
                    fail(500, "GB28181 operation failed");
                }
                activeControlProjection_.reset();
            }
        }
    }
    // Preserve the completed result in this coroutine across Redis failures;
    // retry only its durable reply and acknowledgement, never the SDK action.
    while (!scope_.stopRequested()) {
        bool saved = false;
        try {
            co_await publishReplyAndAcknowledge(redis, replyStream, control_protocol::stream::control(index_), control_protocol::stream::kControlGroup, message.id, requestId, operation, status, result, resultKey, status + "\n" + result, claimKey, claimOwner);
            saved = true;
        } catch (const std::exception& error) {
            if (!scope_.stopRequested()) {
                LOG_WARN << "[GB28181] completed result awaits Redis: " << error.what();
            }
        }
        if (saved) {
            co_return;
        }
        (void)co_await ruvia::sleepFor(worker_, std::chrono::milliseconds(250), scope_.stopToken());
    }
}

ruvia::Task<void> CollectorRuntime::retainPreview(const SipServer::PreviewStartResult& preview) {
    const auto device = devices_.findDevice(preview.deviceId);
    if (!device || !device->online) {
        (void)sip_->stopPreview(preview.sessionId);
        throw std::runtime_error("GB28181 preview device lost its owner");
    }
    const auto token = deviceOwnerToken(*device);
    const auto key = control_protocol::stream::sessionOwner(preview.sessionId);
    std::exception_ptr failure;
    bool retained = false;
    try {
        retained = co_await retainOwner(key, token, preview.deviceId);
        if (retained) {
            previewOwners_[preview.sessionId] = token;
        }
    } catch (...) {
        failure = std::current_exception();
    }
    if (!retained || !ownsLocal(control_protocol::stream::owner(preview.deviceId), token)) {
        (void)sip_->stopPreview(preview.sessionId);
        co_await releaseOwner(key, token);
        if (failure) {
            std::rethrow_exception(failure);
        }
        throw std::runtime_error("GB28181 preview owner could not be retained");
    }
}

ruvia::Task<void> CollectorRuntime::releasePreview(std::string sessionId) {
    const auto found = previewOwners_.find(sessionId);
    if (found == previewOwners_.end()) {
        co_return;
    }
    const auto token = found->second;
    previewOwners_.erase(found);
    co_await releaseOwner(control_protocol::stream::sessionOwner(sessionId), token);
}

ruvia::Task<std::string>
CollectorRuntime::execute(std::string operation, std::string payload, ruvia::StopToken stop) {
    requireCurrentLoop();
    if (!started_.load()) {
        throw std::runtime_error("GB28181 Collector is not started");
    }
    if (stopping_.load() || stop.stopRequested()) {
        throw std::runtime_error("GB28181 operation was cancelled");
    }
    if (!sip_) {
        throw std::runtime_error("GB28181 SIP actor is unavailable");
    }

    std::pmr::monotonic_buffer_resource resource;
    auto request = control_protocol::parseRequest(
        payload.empty() ? std::string_view{ "{}" } : payload,
        &resource
    );
    if (!request) {
        throw std::invalid_argument("GB28181 RPC request body is invalid");
    }
    const auto& input = *request;

    if (operation == control_protocol::kCatalogOperation) {
        const auto id = requiredText(input.get<"deviceId">(), "设备编号不能为空");
        co_return jsonAction(sip_->queryCatalog(id), id);
    }
    if (operation == control_protocol::kRenameDeviceOperation) {
        const auto id = requiredText(input.get<"deviceId">(), "设备编号不能为空");
        const auto name = requiredText(input.get<"name">(), "名称不能为空");
        if (!devices_.updateDeviceName(id, name)) {
            throw std::runtime_error("GB28181 device does not exist");
        }
        co_return control_protocol::operationJson("摄像头名称已更新");
    }
    if (operation == control_protocol::kRenameChannelOperation) {
        const auto id = requiredText(input.get<"deviceId">(), "设备编号不能为空");
        const auto channel = requiredText(input.get<"channelId">(), "通道编号不能为空");
        const auto name = requiredText(input.get<"name">(), "名称不能为空");
        if (!devices_.updateChannelName(id, channel, name)) {
            throw std::runtime_error("GB28181 device or channel does not exist");
        }
        co_return control_protocol::operationJson("通道名称已更新");
    }
    if (operation == control_protocol::kMapOperation ||
        operation == control_protocol::kUnmapOperation) {
        const auto id = requiredText(input.get<"deviceId">(), "设备编号不能为空");
        const auto mapped = operation == control_protocol::kUnmapOperation
            ? std::string{}
            : requiredText(input.get<"mappedDeviceId">(), "映射设备编号不能为空");
        if (!devices_.updateMapping(id, mapped)) {
            throw std::runtime_error("GB28181 device does not exist");
        }
        co_return jsonAction(true, id);
    }
    if (operation == control_protocol::kPreviewStartOperation) {
        const auto id = requiredText(input.get<"deviceId">(), "设备编号不能为空");
        const auto channel = requiredText(input.get<"channelId">(), "通道编号不能为空");
        const auto result = sip_->startPreview(id, channel);
        if (!result) {
            throw std::runtime_error("GB28181 device or channel unavailable");
        }
        co_await retainPreview(*result);
        co_return jsonPreviewStart(*result);
    }
    if (operation == control_protocol::kPreviewStopOperation) {
        const auto session = requiredText(input.get<"sessionId">(), "会话编号不能为空");
        const auto result = sip_->stopPreview(session);
        if (!result) {
            throw std::runtime_error("GB28181 preview session does not exist");
        }
        co_return jsonPreviewStop(*result);
    }
    if (operation == control_protocol::kPreviewHeartbeatOperation) {
        const auto session = requiredText(input.get<"sessionId">(), "会话编号不能为空");
        co_return jsonAction(sip_->renewPreview(session));
    }
    if (operation == control_protocol::kPtzOperation) {
        const auto id = requiredText(input.get<"deviceId">(), "设备编号不能为空");
        const auto channel = requiredText(input.get<"channelId">(), "通道编号不能为空");
        const auto action = requiredText(input.get<"action">(), "云台动作不能为空");
        requireAction(action);
        const auto speedValue = input.get<"speed">();
        const auto speed = speedValue ? requiredInteger(speedValue, "speed 无效")
                                      : 80;
        if (speed < 0 || speed > 255) {
            throw std::invalid_argument("speed must be between 0 and 255");
        }
        co_return jsonAction(
            sip_->sendPtzControl(id, channel, action, static_cast<std::uint8_t>(speed)),
            id,
            channel
        );
    }
    if (operation == control_protocol::kPtzPositionOperation) {
        const auto id = requiredText(input.get<"deviceId">(), "设备编号不能为空");
        const auto channel = requiredText(input.get<"channelId">(), "通道编号不能为空");
        const auto pan = requiredFinite(input.get<"pan">(), "pan", 0.0, 360.0);
        const auto tilt =
            requiredFinite(input.get<"tilt">(), "tilt", -30.0, 90.0);
        const auto zoom =
            requiredFinite(input.get<"zoom">(), "zoom", 1.0, 1000.0);
        co_return jsonAction(
            sip_->sendPtzPreciseControl(id, channel, pan, tilt, zoom),
            id,
            channel
        );
    }
    if (operation == control_protocol::kRecordsOperation) {
        const auto id = requiredText(input.get<"deviceId">(), "设备编号不能为空");
        const auto channel = requiredText(input.get<"channelId">(), "通道编号不能为空");
        const auto start = requiredText(input.get<"startTime">(), "开始时间不能为空");
        const auto end = requiredText(input.get<"endTime">(), "结束时间不能为空");
        co_return jsonAction(sip_->queryRecords(id, channel, start, end), id, channel);
    }
    if (operation == control_protocol::kPlaybackStartOperation) {
        const auto id = requiredText(input.get<"deviceId">(), "设备编号不能为空");
        const auto channel = requiredText(input.get<"channelId">(), "通道编号不能为空");
        const auto start = requiredText(input.get<"startTime">(), "开始时间不能为空");
        const auto end = requiredText(input.get<"endTime">(), "结束时间不能为空");
        const auto result = sip_->startPlayback(id, channel, start, end);
        if (!result) {
            throw std::runtime_error("GB28181 device or channel unavailable");
        }
        co_await retainPreview(*result);
        co_return jsonPreviewStart(*result);
    }
    if (operation == control_protocol::kRecordingOperation ||
        operation == control_protocol::kRecordingStartOperation ||
        operation == control_protocol::kRecordingStopOperation) {
        const auto streamId = requiredText(input.get<"streamId">(), "流编号不能为空");
        ZlmSdk::OwnerScope owner(index_);
        auto& sdk = sdkSupervisor().sdk();
        if (operation == control_protocol::kRecordingOperation) {
            co_return jsonAction(sdk.isMp4Recording(streamId));
        }
        const auto changed = operation == control_protocol::kRecordingStartOperation
            ? sdk.startMp4Recording(streamId)
            : sdk.stopMp4Recording(streamId);
        co_return jsonAction(changed);
    }
    if (operation == control_protocol::kHealthOperation ||
        operation == control_protocol::kSipConfigOperation ||
        operation == control_protocol::kDevicesOperation ||
        operation == control_protocol::kDeviceOperation ||
        operation == control_protocol::kStreamsOperation ||
        operation == control_protocol::kStreamOperation) {
        throw std::runtime_error(
            "GB28181 queries must be served from the Service projection"
        );
    }
    throw std::invalid_argument("unsupported GB28181 operation");
}

ruvia::Task<std::string> GbControlHandler::handle(
    ruvia::WebWorkerContext& context,
    std::string_view operation,
    std::string_view payload,
    ruvia::StopToken stop
) {
    if (stop.stopRequested()) {
        service::common::fail(10004, "GB28181 operation cancelled", 503);
    }

    std::pmr::monotonic_buffer_resource resource;
    auto request = control_protocol::parseRequest(
        payload.empty() ? std::string_view{ "{}" } : payload,
        &resource
    );
    if (!request) {
        service::common::fail(10001, "GB28181 RPC 请求体无效", 400);
    }
    const auto& input = *request;
    const auto enabled = co_await configuredEnabled(context.redis());

    if (operation == control_protocol::kHealthOperation) {
        co_return control_protocol::successJson(
            healthJson(context, enabled),
            context.resource()
        );
    }
    if (operation == control_protocol::kSipConfigOperation) {
        requireEnabled(enabled);
        const auto fields = co_await context.redis().hgetAll(
            control_protocol::stream::kConfigKey
        );
        if (fields.empty()) {
            service::common::fail(10004, "GB28181 配置投影不可用", 503);
        }
        const auto port = projectionInteger(configField(fields, "port"));
        co_return control_protocol::successJson(
            sipConfigJson(context, configField(fields, "domain"), configField(fields, "id"), configField(fields, "host"), configField(fields, "public_ip"), port.value_or(0), configField(fields, "transport")),
            context.resource()
        );
    }

    if (operation == control_protocol::kDevicesOperation) {
        requireEnabled(enabled);
        const auto snapshot = co_await GbProjectionService::loadSnapshot(context);
        ruvia::BoxedArray<DeviceJson> items(
            ruvia::ModelOptions{ .resource = context.resource() }
        );
        for (const auto& device : snapshot.devices) {
            items.emplace(deviceJson(context, device));
        }
        DeviceListJson result(context);
        result.set<"items">(std::move(items));
        co_return control_protocol::successJson(result, context.resource());
    }
    if (operation == control_protocol::kDeviceOperation) {
        requireEnabled(enabled);
        const auto id = requiredText(input.get<"deviceId">(), "设备编号不能为空");
        const auto snapshot = co_await GbProjectionService::loadSnapshot(context);
        const auto value = std::find_if(
            snapshot.devices.begin(),
            snapshot.devices.end(),
            [&id](const Device& device) {
                return device.id == id;
            }
        );
        if (value == snapshot.devices.end()) {
            service::common::fail(10003, "设备不存在", 404);
        }
        co_return control_protocol::successJson(deviceJson(context, *value), context.resource());
    }
    if (operation == control_protocol::kStreamsOperation) {
        requireEnabled(enabled);
        const auto snapshot = co_await GbProjectionService::loadSnapshot(context);
        ruvia::BoxedArray<StreamJson> items(
            ruvia::ModelOptions{ .resource = context.resource() }
        );
        for (const auto& stream : snapshot.streams) {
            items.emplace(streamJson(context, stream));
        }
        StreamListJson result(context);
        result.set<"items">(std::move(items));
        co_return control_protocol::successJson(result, context.resource());
    }
    if (operation == control_protocol::kStreamOperation) {
        requireEnabled(enabled);
        const auto id = requiredText(input.get<"streamId">(), "流编号不能为空");
        const auto snapshot = co_await GbProjectionService::loadSnapshot(context);
        const auto value = std::find_if(
            snapshot.streams.begin(),
            snapshot.streams.end(),
            [&id](const StreamStatus& stream) {
                return StreamStatus::identity(stream.app, stream.stream, stream.schema) == id;
            }
        );
        if (value == snapshot.streams.end()) {
            service::common::fail(10003, "流不存在", 404);
        }
        co_return control_protocol::successJson(streamJson(context, *value), context.resource());
    }

    if (operation == control_protocol::kCatalogOperation ||
        operation == control_protocol::kRenameDeviceOperation ||
        operation == control_protocol::kRenameChannelOperation ||
        operation == control_protocol::kMapOperation ||
        operation == control_protocol::kUnmapOperation ||
        operation == control_protocol::kPreviewStartOperation ||
        operation == control_protocol::kPtzOperation ||
        operation == control_protocol::kPtzPositionOperation ||
        operation == control_protocol::kRecordsOperation ||
        operation == control_protocol::kPlaybackStartOperation) {
        requireEnabled(enabled);
        const auto deviceId = requiredText(input.get<"deviceId">(), "设备编号不能为空");
        if (operation == control_protocol::kRenameDeviceOperation) {
            (void)requiredText(input.get<"name">(), "名称不能为空");
        }
        if (operation == control_protocol::kRenameChannelOperation) {
            (void)requiredText(input.get<"channelId">(), "通道编号不能为空");
            (void)requiredText(input.get<"name">(), "名称不能为空");
        }
        if (operation == control_protocol::kPreviewStartOperation ||
            operation == control_protocol::kPtzOperation ||
            operation == control_protocol::kPtzPositionOperation ||
            operation == control_protocol::kRecordsOperation ||
            operation == control_protocol::kPlaybackStartOperation) {
            (void)requiredText(input.get<"channelId">(), "通道编号不能为空");
        }
        if (operation == control_protocol::kPtzOperation) {
            const auto action = requiredText(input.get<"action">(), "云台动作不能为空");
            requireAction(action);
            const auto speedValue = input.get<"speed">();
            const auto speed = speedValue ? requiredInteger(speedValue, "speed 无效")
                                          : 80;
            if (speed < 0 || speed > 255) {
                service::common::fail(10001, "speed 必须是 0 - 255 的整数", 400);
            }
        }
        if (operation == control_protocol::kPtzPositionOperation) {
            (void)requiredFinite(input.get<"pan">(), "pan", 0.0, 360.0);
            (void)requiredFinite(input.get<"tilt">(), "tilt", -30.0, 90.0);
            (void)requiredFinite(input.get<"zoom">(), "zoom", 1.0, 1000.0);
        }
        if (operation == control_protocol::kRecordsOperation ||
            operation == control_protocol::kPlaybackStartOperation) {
            (void)requiredText(input.get<"startTime">(), "开始时间不能为空");
            (void)requiredText(input.get<"endTime">(), "结束时间不能为空");
        }
        if (operation == control_protocol::kMapOperation) {
            (void)requiredText(input.get<"mappedDeviceId">(), "映射设备编号不能为空");
        }
        co_return co_await dispatchByOwnerKey(
            context,
            operation,
            payload,
            control_protocol::stream::owner(deviceId),
            stop
        );
    }

    if (operation == control_protocol::kPreviewStopOperation ||
        operation == control_protocol::kPreviewHeartbeatOperation) {
        requireEnabled(enabled);
        const auto sessionId = requiredText(input.get<"sessionId">(), "会话编号不能为空");
        co_return co_await dispatchByOwnerKey(
            context,
            operation,
            payload,
            control_protocol::stream::sessionOwner(sessionId),
            stop
        );
    }

    if (operation == control_protocol::kRecordingOperation ||
        operation == control_protocol::kRecordingStartOperation ||
        operation == control_protocol::kRecordingStopOperation) {
        requireEnabled(enabled);
        const auto streamId = requiredText(input.get<"streamId">(), "流编号不能为空");
        co_return co_await dispatchByOwnerKey(
            context,
            operation,
            payload,
            control_protocol::stream::owner(streamId),
            stop
        );
    }

    service::common::fail(10002, "不支持的 GB28181 操作", 400);
}
} // namespace service::gb28181
