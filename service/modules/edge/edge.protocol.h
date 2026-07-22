#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <string_view>

extern "C" {
#include <edge.pb.h>
#include <pb_decode.h>
#include <pb_encode.h>
}

namespace service::edge::protocol {

inline constexpr std::string_view kBootstrapPlatformId{
    "00000000-0000-7000-8000-000000000001"};
inline constexpr std::string_view kPublicPlatformUrl{"https://i.a-z.xin"};

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

inline std::string uuidText(const std::uint8_t value[16]) {
    constexpr char digits[] = "0123456789abcdef";
    std::string output;
    output.reserve(36);
    for (std::size_t index = 0; index < 16; ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10)
            output.push_back('-');
        output.push_back(digits[value[index] >> 4U]);
        output.push_back(digits[value[index] & 0x0fU]);
    }
    return output;
}

inline void setBytes(void* field, std::size_t capacity, const std::uint8_t* data,
                     std::size_t size) {
    const auto encoded = static_cast<pb_size_t>(size);
    std::memcpy(field, &encoded, sizeof(encoded));
    if (size != 0 && size <= capacity)
        std::memcpy(static_cast<std::uint8_t*>(field) + sizeof(encoded), data, size);
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

inline iot_edge_v1_Envelope outbound(std::string_view nodeId, std::uint64_t epoch = 0,
                                     std::uint64_t sequence = 0) {
    iot_edge_v1_Envelope result = iot_edge_v1_Envelope_init_zero;
    result.protocol_version = 1;
    result.session_epoch = epoch;
    result.sequence = sequence;
    result.created_at_ms = nowMs();
    const auto messageId = randomUuidV7Bytes();
    setBytes(&result.message_id, sizeof(result.message_id.bytes), messageId.data(),
             messageId.size());
    std::uint8_t platform[16]{};
    std::uint8_t node[16]{};
    if (uuidBytes(kBootstrapPlatformId, platform))
        setBytes(&result.platform_id, sizeof(result.platform_id.bytes), platform, 16);
    if (uuidBytes(nodeId, node))
        setBytes(&result.node_id, sizeof(result.node_id.bytes), node, 16);
    return result;
}

inline bool decode(std::string_view wire, iot_edge_v1_Envelope& output) {
    output = iot_edge_v1_Envelope_init_zero;
    auto stream = pb_istream_from_buffer(
        reinterpret_cast<const pb_byte_t*>(wire.data()), wire.size());
    return pb_decode(&stream, iot_edge_v1_Envelope_fields, &output);
}

inline std::string encode(const iot_edge_v1_Envelope& input) {
    std::string wire(iot_edge_v1_Envelope_size, '\0');
    auto stream = pb_ostream_from_buffer(reinterpret_cast<pb_byte_t*>(wire.data()), wire.size());
    if (!pb_encode(&stream, iot_edge_v1_Envelope_fields, &input))
        return {};
    wire.resize(stream.bytes_written);
    return wire;
}

} // namespace service::edge::protocol
