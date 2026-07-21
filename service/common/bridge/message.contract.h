#pragma once

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "service/common/uuid.h"

namespace service::bridge {

inline constexpr std::string_view kConfigStream = "iot:channel:config";
inline constexpr std::string_view kIngressStream = "iot:channel:packet:raw";
inline constexpr std::string_view kParsedStream = "iot:channel:packet:parsed";
inline constexpr std::string_view kEgressStream = "iot:channel:command";
inline constexpr std::string_view kProtocolHighTaskStream = "iot:channel:protocol:task:high";
inline constexpr std::string_view kProtocolNormalTaskStream = "iot:channel:protocol:task:normal";
inline constexpr std::string_view kProtocolTaskDepthPrefix = "iot:state:protocol:queue-depth:";
inline constexpr std::string_view kProtocolInflightPrefix = "iot:state:protocol:inflight:";
inline constexpr std::string_view kSessionStatePrefix = "iot:state:session:";
inline constexpr std::string_view kLinkEventStream = "iot:channel:link:event";
inline constexpr std::string_view kDeadLetterStream = "iot:channel:dead-letter";

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
    std::string linkId;
    std::string connectionId;
    std::string remoteAddress;
    std::int64_t occurredAtMs = 0;
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
    std::string source = "push";
    std::string valuesJson;
    std::vector<std::uint8_t> rawPayload;
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
    std::string connectionId;
    std::string payload;
    bool expectsResponse = true;
    std::int64_t responseTimeoutMs = 3000;
    std::int64_t createdAtMs = 0;
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

inline std::vector<StreamField> ingressFields(const IngressPacket& packet) {
    return {{"message_id", packet.messageId},
            {"link_id", packet.linkId},
            {"connection_id", packet.connectionId},
            {"remote_address", packet.remoteAddress},
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
    packet.linkId = std::string(require("link_id"));
    packet.connectionId = std::string(require("connection_id"));
    packet.remoteAddress = std::string(message.get("remote_address"));
    packet.occurredAtMs = integer("occurred_at_ms");
    const auto payload = require("payload_hex");
    packet.payload = fromHex(payload);
    if (packet.payload.empty() && !payload.empty())
        throw std::runtime_error("Invalid ingress HEX payload");
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
            {"source", message.source},
            {"values_json", message.valuesJson},
            {"raw_payload_hex", toHex(message.rawPayload)}};
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
    parsed.source = std::string(require("source"));
    parsed.valuesJson = std::string(require("values_json"));
    const auto payload = require("raw_payload_hex");
    parsed.rawPayload = fromHex(payload);
    if (parsed.rawPayload.empty() && !payload.empty())
        throw std::runtime_error("Invalid parsed packet HEX payload");
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
            {"connection_id", task.connectionId},
            {"payload_hex", task.payload},
            {"expects_response", task.expectsResponse ? "1" : "0"},
            {"response_timeout_ms", std::to_string(task.responseTimeoutMs)},
            {"created_at_ms", std::to_string(task.createdAtMs)}};
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
    task.connectionId = std::string(message.get("connection_id"));
    task.payload = std::string(require("payload_hex"));
    const auto expectsResponse = message.get("expects_response");
    task.expectsResponse = expectsResponse.empty() || expectsResponse == "1";
    task.responseTimeoutMs = integer("response_timeout_ms", false);
    if (task.responseTimeoutMs == 0)
        task.responseTimeoutMs = 3000;
    task.createdAtMs = integer("created_at_ms");
    return task;
}

} // namespace service::bridge
