#pragma once

#include <array>
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

namespace service::edge {
namespace pb = ::iot::edge::v1;
}

namespace service::edge::protocol {

inline constexpr std::string_view kBootstrapPlatformId{
    "00000000-0000-7000-8000-000000000001"};
inline constexpr std::string_view kPublicPlatformUrl{"https://i.a-z.xin"};
inline constexpr std::size_t kMaxMessageSize{16U * 1024U};

inline std::string authKey(std::string_view imei) {
    return "iot:edge:auth:" + std::string(imei);
}

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

inline int hexDigit(char value) {
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

inline bool uuidBytes(std::string_view value, std::uint8_t output[16]) {
    if (value.size() != 36)
        return false;
    std::size_t byte = 0;
    for (std::size_t index = 0; index < value.size();) {
        if (value[index] == '-') {
            ++index;
            continue;
        }
        if (index + 1 >= value.size() || byte >= 16)
            return false;
        const int high = hexDigit(value[index]);
        const int low = hexDigit(value[index + 1]);
        if (high < 0 || low < 0)
            return false;
        output[byte++] = static_cast<std::uint8_t>((high << 4U) | low);
        index += 2;
    }
    return byte == 16;
}

inline std::string uuidText(std::string_view value) {
    if (value.size() != 16)
        return {};
    constexpr char digits[] = "0123456789abcdef";
    std::string output;
    output.reserve(36);
    for (std::size_t index = 0; index < 16; ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10)
            output.push_back('-');
        const auto byte = static_cast<std::uint8_t>(value[index]);
        output.push_back(digits[byte >> 4U]);
        output.push_back(digits[byte & 0x0fU]);
    }
    return output;
}

inline std::string uuidText(const std::uint8_t value[16]) {
    return uuidText(std::string_view(reinterpret_cast<const char*>(value), 16));
}

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
                             std::uint64_t sequence = 0) {
    pb::Envelope result;
    result.set_protocol_version(1);
    result.set_session_epoch(epoch);
    result.set_sequence(sequence);
    result.set_created_at_ms(nowMs());
    const auto messageId = randomUuidV7Bytes();
    result.set_message_id(bytes(messageId.data(), messageId.size()));
    std::uint8_t platform[16]{};
    std::uint8_t node[16]{};
    if (uuidBytes(kBootstrapPlatformId, platform))
        result.set_platform_id(bytes(platform, 16));
    if (uuidBytes(nodeId, node))
        result.set_node_id(bytes(node, 16));
    return result;
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
