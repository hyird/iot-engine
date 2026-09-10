#pragma once

#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>

#include <edge.pb.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <nanopb.pb.h>

#include "service/common/uuid.h"
#include "service/common/message.h"

#ifdef GetMessage
#undef GetMessage
#endif

namespace service::edge {
namespace pb = ::iot::edge::v1;
}

namespace service::edge::protocol {

using service::message::edge::kProtocolVersion;
using service::message::edge::kOldestCompatibleProtocolVersion;
using service::message::edge::kDefaultPlatformId;
using service::message::edge::kDefaultPublicBaseUrl;
using service::message::edge::kMaxMessageSize;

inline constexpr bool isCurrentProtocolVersion(std::uint32_t version) noexcept {
    return version == kProtocolVersion;
}

inline constexpr bool supportsProtocolVersion(std::uint32_t version) noexcept {
    return version >= kOldestCompatibleProtocolVersion && version <= kProtocolVersion;
}

inline constexpr bool terminalCommandResultState(pb::CommandState state) noexcept {
    return state == pb::COMMAND_STATE_SUCCEEDED ||
           state == pb::COMMAND_STATE_READBACK_MISMATCH ||
           state == pb::COMMAND_STATE_DEVICE_OFFLINE ||
           state == pb::COMMAND_STATE_TIMED_OUT ||
           state == pb::COMMAND_STATE_REJECTED || state == pb::COMMAND_STATE_FAILED;
}

inline std::string& publicBaseUrlStorage() {
    static std::string value(kDefaultPublicBaseUrl);
    return value;
}

inline std::string& platformIdStorage() {
    static std::string value(kDefaultPlatformId);
    return value;
}

inline std::string_view platformId() { return platformIdStorage(); }

inline std::string_view publicBaseUrl() { return publicBaseUrlStorage(); }

using service::message::edge::authKey;

inline bool validImei(std::string_view imei) {
    if (imei.size() != 15)
        return false;
    unsigned sum = 0;
    for (std::size_t index = 0; index < imei.size(); ++index) {
        if (imei[index] < '0' || imei[index] > '9')
            return false;
        unsigned digit = static_cast<unsigned>(imei[index] - '0');
        if ((index & 1U) != 0U) {
            digit *= 2U;
            if (digit > 9U)
                digit -= 9U;
        }
        sum += digit;
    }
    return sum % 10U == 0U;
}

using service::common::hexDigit;
using service::common::uuidBytes;

inline bool configurePlatformId(std::string_view platformId) {
    std::uint8_t value[16]{};
    if (!uuidBytes(platformId, value))
        return false;
    bool nonzero = false;
    for (const auto byte : value)
        nonzero = nonzero || byte != 0;
    if (!nonzero)
        return false;
    platformIdStorage().assign(platformId);
    return true;
}

inline bool validSessionPlatformId(std::string_view value) noexcept {
    if (value.size() != 16)
        return false;
    for (const unsigned char byte : value)
        if (byte != 0)
            return true;
    return false;
}

inline bool configurePublicBaseUrl(std::string_view publicBaseUrl) {
    const std::size_t schemeSize = publicBaseUrl.starts_with("https://")
                                       ? 8
                                   : publicBaseUrl.starts_with("http://") ? 7
                                                                            : 0;
    if (publicBaseUrl.size() > 255 || schemeSize == 0 ||
        publicBaseUrl.size() <= schemeSize || publicBaseUrl[schemeSize] == '/')
        return false;
    for (const unsigned char character : publicBaseUrl)
        if (std::iscntrl(character) || std::isspace(character))
            return false;
    while (publicBaseUrl.ends_with('/'))
        publicBaseUrl.remove_suffix(1);
    publicBaseUrlStorage().assign(publicBaseUrl);
    return true;
}

using service::common::uuidText;

inline std::string bytes(const std::uint8_t* data, std::size_t size) {
    if (size == 0)
        return {};
    return {reinterpret_cast<const char*>(data), size};
}

inline bool fitsNanopbLimits(const google::protobuf::Message& message) {
    const auto* descriptor = message.GetDescriptor();
    const auto* reflection = message.GetReflection();
    for (int fieldIndex = 0; fieldIndex < descriptor->field_count(); ++fieldIndex) {
        const auto* field = descriptor->field(fieldIndex);
        const auto& options = field->options();
        const NanoPBOptions* limits =
            options.HasExtension(::nanopb) ? &options.GetExtension(::nanopb) : nullptr;
        const int count = field->is_repeated() ? reflection->FieldSize(message, field)
                                                : (reflection->HasField(message, field) ? 1 : 0);
        if (limits && field->is_repeated() && limits->has_max_count() &&
            count > limits->max_count())
            return false;
        for (int index = 0; index < count; ++index) {
            if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING) {
                const auto& value = field->is_repeated()
                                        ? reflection->GetRepeatedStringReference(
                                              message, field, index, nullptr)
                                        : reflection->GetStringReference(message, field, nullptr);
                const auto limit =
                    field->type() == google::protobuf::FieldDescriptor::TYPE_BYTES
                        ? (limits && limits->has_max_size() ? limits->max_size() : -1)
                        : (limits && limits->has_max_length() ? limits->max_length() : -1);
                if (limit >= 0 && value.size() > static_cast<std::size_t>(limit))
                    return false;
            } else if (field->cpp_type() ==
                       google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
                const auto& value =
                    field->is_repeated() ? reflection->GetRepeatedMessage(message, field, index)
                                         : reflection->GetMessage(message, field);
                if (!fitsNanopbLimits(value))
                    return false;
            }
        }
    }
    return true;
}

inline std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

inline std::array<std::uint8_t, 16> randomUuidV7Bytes() {
    static thread_local std::mt19937_64 random(std::random_device{}());
    std::array<std::uint8_t, 16> value{};
    auto time = static_cast<std::uint64_t>(nowMs());
    for (int index = 5; index >= 0; --index) {
        value[static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(time & 0xffU);
        time >>= 8U;
    }
    for (std::size_t index = 6; index < value.size(); ++index)
        value[index] = static_cast<std::uint8_t>(random() & 0xffU);
    value[6] = static_cast<std::uint8_t>(0x70U | (value[6] & 0x0fU));
    value[8] = static_cast<std::uint8_t>(0x80U | (value[8] & 0x3fU));
    return value;
}

inline pb::Envelope outbound(std::string_view nodeId, std::uint64_t epoch = 0,
                             std::uint64_t sequence = 0,
                             std::uint32_t protocolVersion = kProtocolVersion) {
    pb::Envelope result;
    result.set_protocol_version(protocolVersion);
    result.set_session_epoch(epoch);
    result.set_sequence(sequence);
    result.set_created_at_ms(nowMs());
    const auto messageId = randomUuidV7Bytes();
    result.set_message_id(bytes(messageId.data(), messageId.size()));
    std::uint8_t platform[16]{};
    std::uint8_t node[16]{};
    if (uuidBytes(platformId(), platform))
        result.set_platform_id(bytes(platform, 16));
    if (uuidBytes(nodeId, node))
        result.set_node_id(bytes(node, 16));
    return result;
}

inline void bindSession(pb::Envelope& envelope, std::string_view platformBytes,
                        std::string_view nodeBytes, std::uint64_t epoch,
                        std::uint64_t sequence,
                        std::uint32_t protocolVersion = kProtocolVersion) {
    envelope.set_protocol_version(protocolVersion);
    envelope.set_session_epoch(epoch);
    envelope.set_sequence(sequence);
    envelope.set_platform_id(platformBytes);
    envelope.set_node_id(nodeBytes);
}

inline bool decode(std::string_view wire, pb::Envelope& output) {
    output.Clear();
    if (wire.size() > kMaxMessageSize ||
        !output.ParseFromArray(wire.data(), static_cast<int>(wire.size())) ||
        !output.IsInitialized() || !fitsNanopbLimits(output)) {
        output.Clear();
        return false;
    }
    return true;
}

inline std::string encode(const pb::Envelope& input) {
    const auto size = input.ByteSizeLong();
    if (size == 0 || size > kMaxMessageSize || !fitsNanopbLimits(input))
        return {};
    std::string wire;
    if (!input.SerializeToString(&wire))
        return {};
    return wire;
}

} // namespace service::edge::protocol
