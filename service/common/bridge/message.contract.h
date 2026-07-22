#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "service/common/uuid.h"

namespace service::bridge {

inline constexpr std::string_view kConfigStreamPrefix = "iot:channel:config:worker:";
inline constexpr std::string_view kIngressStreamPrefix = "iot:channel:packet:raw:worker:";
inline constexpr std::string_view kParsedStreamPrefix = "iot:channel:packet:parsed:worker:";
inline constexpr std::string_view kEgressStreamPrefix = "iot:channel:socket:egress:worker:";
inline constexpr std::string_view kCommandStreamPrefix = "iot:channel:command:worker:";
inline constexpr std::string_view kCommandResultStreamPrefix =
    "iot:channel:command:result:worker:";
inline constexpr std::string_view kLinkEventStreamPrefix = "iot:channel:link:event:worker:";
inline constexpr std::string_view kControlStreamPrefix = "iot:channel:control:worker:";
inline constexpr std::string_view kDeadLetterStreamPrefix = "iot:channel:dead-letter:worker:";
inline constexpr std::string_view kProtocolTaskDepthPrefix = "iot:state:protocol:queue-depth:";
inline constexpr std::string_view kProtocolInflightPrefix = "iot:state:protocol:inflight:";
inline constexpr std::string_view kSessionStatePrefix = "iot:state:session:";

inline std::string workerStream(std::string_view prefix, std::size_t workerIndex,
                                std::string_view suffix = {}) {
    return std::string(prefix) + std::to_string(workerIndex) + std::string(suffix);
}

inline std::string configStream(std::size_t workerIndex) {
    return workerStream(kConfigStreamPrefix, workerIndex);
}

inline std::string ingressStream(std::size_t workerIndex) {
    return workerStream(kIngressStreamPrefix, workerIndex);
}

inline std::string parsedStream(std::size_t workerIndex) {
    return workerStream(kParsedStreamPrefix, workerIndex);
}

inline std::string egressStream(std::size_t workerIndex) {
    return workerStream(kEgressStreamPrefix, workerIndex);
}

inline std::string commandStream(std::size_t workerIndex, bool highPriority) {
    return workerStream(kCommandStreamPrefix, workerIndex,
                        highPriority ? ":high" : ":normal");
}

inline std::string commandResultStream(std::size_t workerIndex) {
    return workerStream(kCommandResultStreamPrefix, workerIndex);
}

inline std::string linkEventStream(std::size_t workerIndex) {
    return workerStream(kLinkEventStreamPrefix, workerIndex);
}

inline std::string controlStream(std::size_t workerIndex) {
    return workerStream(kControlStreamPrefix, workerIndex);
}

inline std::string deadLetterStream(std::size_t workerIndex) {
    return workerStream(kDeadLetterStreamPrefix, workerIndex);
}

enum class BridgeDirection { NorthToSouth, SouthToNorth };

struct StreamField {
    std::string name;
    std::string value;
};

struct StreamMessage {
    std::string id;
    std::vector<StreamField> fields;

    [[nodiscard]] std::string_view get(std::string_view name) const noexcept {
        for (const auto& field : fields)
            if (field.name == name)
                return field.value;
        return {};
    }
};

struct IngressPacket {
    std::string messageId;
    std::string workerInstanceId;
    std::string linkId;
    std::string connectionId;
    std::string remoteAddress;
    std::uint64_t sessionEpoch = 0;
    std::int64_t occurredAtMs = 0;
    std::vector<std::uint8_t> payload;
};

struct ConnectionEvent {
    std::string messageId;
    std::string workerInstanceId;
    std::string eventType;
    std::string linkId;
    std::string connectionId;
    std::string remoteAddress;
    std::string targetId;
    std::string reason;
    std::uint64_t sessionEpoch = 0;
    std::int64_t occurredAtMs = 0;
};

struct EgressPacket {
    std::string messageId;
    std::string workerInstanceId;
    std::string causationId;
    std::string connectionId;
    std::uint64_t sessionEpoch = 0;
    std::int64_t createdAtMs = 0;
    std::vector<std::uint8_t> payload;
};

struct ParsedDeviceMessage {
    std::string messageId;
    std::string causationId;
    std::string linkId;
    std::string deviceId;
    std::string deviceCode;
    std::string protocol;
    std::string connectionId;
    std::int64_t occurredAtMs = 0;
    std::int64_t observedAtMs = 0;
    std::int64_t storageInterval = 1;
    std::int64_t onlineWindowMs = 300000;
    std::string source = "push";
    std::string valuesJson;
    std::vector<std::vector<std::uint8_t>> rawPayloads;
};

enum class ProtocolTaskPriority { High, Normal };

struct ProtocolTask {
    std::string messageId;
    std::string causationId;
    std::string groupKey;
    std::string protocol;
    std::string transport;
    std::string kind;
    std::string linkId;
    std::string deviceId;
    std::string deviceCode;
    std::string connectionId;
    std::string payload;
    std::string readbackPayload;
    std::string expectedReadbackData;
    std::string expectedValue;
    bool expectsResponse = true;
    std::int64_t responseTimeoutMs = 3000;
    std::int64_t createdAtMs = 0;
    std::int64_t attempt = 1;
    std::int64_t maxAttempts = 3;
    std::uint64_t sessionEpoch = 0;
};

inline std::int64_t utcNowMilliseconds() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

inline std::string nextMessageId() { return service::common::nextUuidV7(); }

inline std::string protocolTaskDepthKey(std::string_view groupKey) {
    return std::string(kProtocolTaskDepthPrefix) + std::string(groupKey);
}

inline std::string protocolInflightKey(std::string_view groupKey) {
    return std::string(kProtocolInflightPrefix) + std::string(groupKey);
}

inline std::string sessionStateKey(std::string_view connectionId) {
    return std::string(kSessionStatePrefix) + std::string(connectionId);
}

inline std::string toHex(const std::vector<std::uint8_t>& bytes) {
    static constexpr std::array<char, 16> digits{'0', '1', '2', '3', '4', '5', '6', '7',
                                                 '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        result.push_back(digits[(byte >> 4U) & 0x0FU]);
        result.push_back(digits[byte & 0x0FU]);
    }
    return result;
}

inline std::vector<std::uint8_t> fromHex(std::string_view value) {
    auto nibble = [](char character) -> int {
        if (character >= '0' && character <= '9')
            return character - '0';
        if (character >= 'a' && character <= 'f')
            return character - 'a' + 10;
        if (character >= 'A' && character <= 'F')
            return character - 'A' + 10;
        return -1;
    };
    if (value.size() % 2 != 0)
        return {};
    std::vector<std::uint8_t> result;
    result.reserve(value.size() / 2);
    for (std::size_t index = 0; index < value.size(); index += 2) {
        const auto high = nibble(value[index]);
        const auto low = nibble(value[index + 1]);
        if (high < 0 || low < 0)
            return {};
        result.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return result;
}

inline std::string rawPayloadsJson(const std::vector<std::vector<std::uint8_t>>& payloads) {
    std::string result{"["};
    for (std::size_t index = 0; index < payloads.size(); ++index) {
        if (index != 0)
            result.push_back(',');
        result.push_back('"');
        result += toHex(payloads[index]);
        result.push_back('"');
    }
    result.push_back(']');
    return result;
}

inline std::vector<std::vector<std::uint8_t>> rawPayloadsFromJson(std::string_view value) {
    std::vector<std::vector<std::uint8_t>> result;
    std::size_t offset = 0;
    const auto skipWhitespace = [&] {
        while (offset < value.size() && (value[offset] == ' ' || value[offset] == '\n' ||
                                         value[offset] == '\r' || value[offset] == '\t'))
            ++offset;
    };
    skipWhitespace();
    if (offset == value.size() || value[offset++] != '[')
        return {};
    skipWhitespace();
    if (offset < value.size() && value[offset] == ']') {
        ++offset;
        skipWhitespace();
        return offset == value.size() ? result : std::vector<std::vector<std::uint8_t>>{};
    }
    while (offset < value.size()) {
        if (value[offset++] != '"')
            return {};
        const auto end = value.find('"', offset);
        if (end == std::string_view::npos)
            return {};
        const auto hex = value.substr(offset, end - offset);
        auto payload = fromHex(hex);
        if (payload.empty() && !hex.empty())
            return {};
        result.push_back(std::move(payload));
        offset = end + 1;
        skipWhitespace();
        if (offset == value.size())
            return {};
        if (value[offset] == ']') {
            ++offset;
            skipWhitespace();
            return offset == value.size() ? result : std::vector<std::vector<std::uint8_t>>{};
        }
        if (value[offset++] != ',')
            return {};
        skipWhitespace();
    }
    return {};
}

inline std::vector<StreamField> ingressFields(const IngressPacket& packet) {
    return {{"event_type", "packet"},
            {"message_id", packet.messageId},
            {"worker_instance_id", packet.workerInstanceId},
            {"link_id", packet.linkId},
            {"connection_id", packet.connectionId},
            {"remote_address", packet.remoteAddress},
            {"session_epoch", std::to_string(packet.sessionEpoch)},
            {"occurred_at_ms", std::to_string(packet.occurredAtMs)},
            {"payload_hex", toHex(packet.payload)}};
}

inline IngressPacket ingressFrom(const StreamMessage& message) {
    const auto require = [&message](std::string_view name) {
        const auto value = message.get(name);
        if (value.empty())
            throw std::runtime_error("Missing ingress field: " + std::string(name));
        return value;
    };
    const auto eventType = message.get("event_type");
    if (!eventType.empty() && eventType != "packet")
        throw std::runtime_error("Ingress message is not a packet");
    const auto integer = [&require](std::string_view name) {
        const auto value = require(name);
        std::int64_t result = 0;
        const auto [end, error] =
            std::from_chars(value.data(), value.data() + value.size(), result);
        if (error != std::errc{} || end != value.data() + value.size())
            throw std::runtime_error("Invalid ingress integer field: " + std::string(name));
        return result;
    };
    IngressPacket packet;
    packet.messageId = std::string(require("message_id"));
    packet.workerInstanceId = std::string(require("worker_instance_id"));
    packet.linkId = std::string(require("link_id"));
    packet.connectionId = std::string(require("connection_id"));
    packet.remoteAddress = std::string(message.get("remote_address"));
    packet.sessionEpoch = static_cast<std::uint64_t>(integer("session_epoch"));
    packet.occurredAtMs = integer("occurred_at_ms");
    const auto payload = require("payload_hex");
    packet.payload = fromHex(payload);
    if (packet.payload.empty() && !payload.empty())
        throw std::runtime_error("Invalid ingress HEX payload");
    return packet;
}

inline std::vector<StreamField> connectionEventFields(const ConnectionEvent& event) {
    return {{"event_type", event.eventType},
            {"message_id", event.messageId},
            {"worker_instance_id", event.workerInstanceId},
            {"link_id", event.linkId},
            {"connection_id", event.connectionId},
            {"remote_address", event.remoteAddress},
            {"target_id", event.targetId},
            {"reason", event.reason},
            {"session_epoch", std::to_string(event.sessionEpoch)},
            {"occurred_at_ms", std::to_string(event.occurredAtMs)}};
}

inline ConnectionEvent connectionEventFrom(const StreamMessage& message) {
    const auto require = [&message](std::string_view name) {
        const auto value = message.get(name);
        if (value.empty())
            throw std::runtime_error("Missing connection event field: " + std::string(name));
        return value;
    };
    const auto integer = [&require](std::string_view name) {
        const auto value = require(name);
        std::int64_t result = 0;
        const auto [end, error] =
            std::from_chars(value.data(), value.data() + value.size(), result);
        if (error != std::errc{} || end != value.data() + value.size() || result < 0)
            throw std::runtime_error("Invalid connection event integer: " + std::string(name));
        return result;
    };
    ConnectionEvent event;
    event.eventType = std::string(require("event_type"));
    if (event.eventType != "connected" && event.eventType != "disconnected")
        throw std::runtime_error("Invalid connection event type");
    event.messageId = std::string(require("message_id"));
    event.workerInstanceId = std::string(require("worker_instance_id"));
    event.linkId = std::string(require("link_id"));
    event.connectionId = std::string(require("connection_id"));
    event.remoteAddress = std::string(message.get("remote_address"));
    event.targetId = std::string(message.get("target_id"));
    event.reason = std::string(message.get("reason"));
    event.sessionEpoch = static_cast<std::uint64_t>(integer("session_epoch"));
    event.occurredAtMs = integer("occurred_at_ms");
    return event;
}

inline std::vector<StreamField> egressFields(const EgressPacket& packet) {
    return {{"message_id", packet.messageId},
            {"worker_instance_id", packet.workerInstanceId},
            {"causation_id", packet.causationId},
            {"connection_id", packet.connectionId},
            {"session_epoch", std::to_string(packet.sessionEpoch)},
            {"created_at_ms", std::to_string(packet.createdAtMs)},
            {"payload_hex", toHex(packet.payload)}};
}

inline EgressPacket egressFrom(const StreamMessage& message) {
    const auto require = [&message](std::string_view name) {
        const auto value = message.get(name);
        if (value.empty())
            throw std::runtime_error("Missing egress field: " + std::string(name));
        return value;
    };
    const auto integer = [&require](std::string_view name) {
        const auto value = require(name);
        std::int64_t result = 0;
        const auto [end, error] =
            std::from_chars(value.data(), value.data() + value.size(), result);
        if (error != std::errc{} || end != value.data() + value.size() || result < 0)
            throw std::runtime_error("Invalid egress integer field: " + std::string(name));
        return result;
    };
    EgressPacket packet;
    packet.messageId = std::string(require("message_id"));
    packet.workerInstanceId = std::string(require("worker_instance_id"));
    packet.causationId = std::string(message.get("causation_id"));
    packet.connectionId = std::string(require("connection_id"));
    packet.sessionEpoch = static_cast<std::uint64_t>(integer("session_epoch"));
    packet.createdAtMs = integer("created_at_ms");
    const auto payload = require("payload_hex");
    packet.payload = fromHex(payload);
    if (packet.payload.empty())
        throw std::runtime_error("Invalid or empty egress HEX payload");
    return packet;
}

inline std::vector<StreamField> parsedFields(const ParsedDeviceMessage& message) {
    return {{"message_id", message.messageId},
            {"causation_id", message.causationId},
            {"link_id", message.linkId},
            {"device_id", message.deviceId},
            {"device_code", message.deviceCode},
            {"protocol", message.protocol},
            {"connection_id", message.connectionId},
            {"occurred_at_ms", std::to_string(message.occurredAtMs)},
            {"observed_at_ms", std::to_string(message.observedAtMs)},
            {"storage_interval", std::to_string(message.storageInterval)},
            {"online_window_ms", std::to_string(message.onlineWindowMs)},
            {"source", message.source},
            {"values_json", message.valuesJson},
            {"raw_payload_hex", rawPayloadsJson(message.rawPayloads)}};
}

inline ParsedDeviceMessage parsedFrom(const StreamMessage& message) {
    const auto require = [&message](std::string_view name) {
        const auto value = message.get(name);
        if (value.empty())
            throw std::runtime_error("Missing parsed packet field: " + std::string(name));
        return value;
    };
    const auto integer = [&require](std::string_view name) {
        const auto value = require(name);
        std::int64_t result = 0;
        const auto [end, error] =
            std::from_chars(value.data(), value.data() + value.size(), result);
        if (error != std::errc{} || end != value.data() + value.size())
            throw std::runtime_error("Invalid parsed packet integer: " + std::string(name));
        return result;
    };
    ParsedDeviceMessage parsed;
    parsed.messageId = std::string(require("message_id"));
    parsed.causationId = std::string(require("causation_id"));
    parsed.linkId = std::string(require("link_id"));
    parsed.deviceId = std::string(require("device_id"));
    parsed.deviceCode = std::string(require("device_code"));
    parsed.protocol = std::string(require("protocol"));
    parsed.connectionId = std::string(require("connection_id"));
    parsed.occurredAtMs = integer("occurred_at_ms");
    parsed.observedAtMs = integer("observed_at_ms");
    const auto storageInterval = message.get("storage_interval");
    if (!storageInterval.empty()) {
        std::int64_t result = 0;
        const auto [end, error] = std::from_chars(
            storageInterval.data(), storageInterval.data() + storageInterval.size(), result);
        if (error != std::errc{} || end != storageInterval.data() + storageInterval.size())
            throw std::runtime_error("Invalid parsed packet storage interval");
        parsed.storageInterval = std::clamp<std::int64_t>(result, 1, 86400);
    }
    const auto onlineWindow = message.get("online_window_ms");
    if (!onlineWindow.empty()) {
        std::int64_t result = 0;
        const auto [end, error] =
            std::from_chars(onlineWindow.data(), onlineWindow.data() + onlineWindow.size(), result);
        if (error != std::errc{} || end != onlineWindow.data() + onlineWindow.size())
            throw std::runtime_error("Invalid parsed packet online window");
        parsed.onlineWindowMs = std::clamp<std::int64_t>(result, 1000, 2592000000LL);
    }
    parsed.source = std::string(require("source"));
    parsed.valuesJson = std::string(require("values_json"));
    const auto payload = require("raw_payload_hex");
    parsed.rawPayloads = rawPayloadsFromJson(payload);
    if (parsed.rawPayloads.empty())
        throw std::runtime_error("Invalid parsed packet HEX payload array");
    return parsed;
}

inline std::vector<StreamField> protocolTaskFields(const ProtocolTask& task) {
    return {{"message_id", task.messageId},
            {"causation_id", task.causationId},
            {"group_key", task.groupKey},
            {"protocol", task.protocol},
            {"transport", task.transport},
            {"kind", task.kind},
            {"link_id", task.linkId},
            {"device_id", task.deviceId},
            {"device_code", task.deviceCode},
            {"connection_id", task.connectionId},
            {"payload_hex", task.payload},
            {"readback_payload_hex", task.readbackPayload},
            {"expected_readback_hex", task.expectedReadbackData},
            {"expected_value", task.expectedValue},
            {"expects_response", task.expectsResponse ? "1" : "0"},
            {"response_timeout_ms", std::to_string(task.responseTimeoutMs)},
            {"created_at_ms", std::to_string(task.createdAtMs)},
            {"attempt", std::to_string(task.attempt)},
            {"max_attempts", std::to_string(task.maxAttempts)},
            {"session_epoch", std::to_string(task.sessionEpoch)}};
}

inline ProtocolTask protocolTaskFrom(const StreamMessage& message) {
    const auto require = [&message](std::string_view name) {
        const auto value = message.get(name);
        if (value.empty())
            throw std::runtime_error("Missing protocol task field: " + std::string(name));
        return value;
    };
    const auto integer = [&message, &require](std::string_view name, bool required = true) {
        const auto value = message.get(name);
        if (value.empty()) {
            if (required)
                (void)require(name);
            return std::int64_t{0};
        }
        std::int64_t result = 0;
        const auto [end, error] =
            std::from_chars(value.data(), value.data() + value.size(), result);
        if (error != std::errc{} || end != value.data() + value.size())
            throw std::runtime_error("Invalid protocol task integer field: " + std::string(name));
        return result;
    };
    ProtocolTask task;
    task.messageId = std::string(require("message_id"));
    task.causationId = std::string(message.get("causation_id"));
    task.groupKey = std::string(require("group_key"));
    task.protocol = std::string(require("protocol"));
    task.transport = std::string(require("transport"));
    if (task.transport != "TCP" && task.transport != "RTU" && task.transport != "RAW")
        throw std::runtime_error("Invalid protocol task transport");
    task.kind = std::string(require("kind"));
    task.linkId = std::string(require("link_id"));
    task.deviceId = std::string(message.get("device_id"));
    task.deviceCode = std::string(message.get("device_code"));
    task.connectionId = std::string(message.get("connection_id"));
    task.payload = std::string(require("payload_hex"));
    task.readbackPayload = std::string(message.get("readback_payload_hex"));
    task.expectedReadbackData = std::string(message.get("expected_readback_hex"));
    task.expectedValue = std::string(message.get("expected_value"));
    const auto expectsResponse = message.get("expects_response");
    task.expectsResponse = expectsResponse.empty() || expectsResponse == "1";
    task.responseTimeoutMs = integer("response_timeout_ms", false);
    if (task.responseTimeoutMs == 0)
        task.responseTimeoutMs = 3000;
    task.createdAtMs = integer("created_at_ms");
    task.attempt = integer("attempt", false);
    if (task.attempt == 0)
        task.attempt = 1;
    task.maxAttempts = integer("max_attempts", false);
    if (task.maxAttempts == 0)
        task.maxAttempts = 3;
    task.attempt = std::clamp<std::int64_t>(task.attempt, 1, 100);
    task.maxAttempts = std::clamp<std::int64_t>(task.maxAttempts, task.attempt, 100);
    task.sessionEpoch = static_cast<std::uint64_t>(integer("session_epoch", false));
    return task;
}

} // namespace service::bridge
