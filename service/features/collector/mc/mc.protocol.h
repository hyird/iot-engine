#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "service/features/collector/mc/mc.types.h"
#include "service/features/collector/polling/polling.protocol.h"
#include "service/features/collector/register_value/register_value.protocol.h"

namespace service::collector::mc {

// MELSEC QnA 兼容二进制 3E/4E 帧。地址与长度先校验，再访问或构造报文。
class FrameCodec final {
  public:
    struct Response {
        std::uint16_t endCode = 0;
        std::vector<std::uint8_t> data;
    };

    static std::optional<std::uint8_t> deviceCode(std::string_view area) {
        for (const auto& entry : {std::pair{"D", 0xa8}, {"W", 0xb4}, {"R", 0xaf},
             {"ZR", 0xb0}, {"M", 0x90}, {"X", 0x9c}, {"Y", 0x9d}, {"B", 0xa0},
             {"L", 0x92}, {"F", 0x93}, {"V", 0x94}, {"S", 0x98},
             {"TN", 0xc2}, {"CN", 0xc5}, {"TS", 0xc1}, {"CS", 0xc4}})
            if (entry.first == area) return static_cast<std::uint8_t>(entry.second);
        return std::nullopt;
    }

    static bool bitDevice(std::uint8_t code) {
        return code == 0x90 || code == 0x9c || code == 0x9d || code == 0xa0 ||
               code == 0x92 || code == 0x93 || code == 0x94 || code == 0x98 ||
               code == 0xc1 || code == 0xc4;
    }

    static std::vector<std::uint8_t> read(const Connection& connection, const Address& address,
                                         std::uint16_t serial) {
        return request(connection, address, serial, false, {});
    }

    // 位值输入为逐点 0/1，字设备输入为小端字节；不接受隐式截断。
    static std::vector<std::uint8_t> write(const Connection& connection, const Address& address,
                                          std::uint16_t serial, std::span<const std::uint8_t> values) {
        validateAddress(address);
        if (values.size() != static_cast<std::size_t>(address.count) * (address.bitAccess ? 1 : 2))
            throw std::invalid_argument("mc_write_size_mismatch");
        std::vector<std::uint8_t> payload;
        if (address.bitAccess) {
            payload.resize((values.size() + 1) / 2);
            for (std::size_t i = 0; i < values.size(); ++i) {
                if (values[i] > 1) throw std::invalid_argument("mc_bit_value_invalid");
                payload[i / 2] |= static_cast<std::uint8_t>(values[i] << (i % 2 == 0 ? 4 : 0));
            }
        } else payload.assign(values.begin(), values.end());
        return request(connection, address, serial, true, payload);
    }

    static std::optional<std::size_t> frameLength(std::span<const std::uint8_t> bytes,
                                                 FrameFormat format) {
        const auto header = format == FrameFormat::Binary4E ? 13U : 9U;
        if (bytes.size() < 2) return std::nullopt;
        if (bytes[0] != (format == FrameFormat::Binary4E ? 0xd4 : 0xd0) || bytes[1] != 0)
            throw std::invalid_argument("mc_response_header_invalid");
        if (bytes.size() < header) return std::nullopt;
        const auto length = readLe16(bytes, header - 2);
        if (length < 2 || length > 4096) throw std::invalid_argument("mc_response_length_invalid");
        return header + length;
    }

    static Response response(const Connection& connection, const Address& address,
                             std::uint16_t serial, bool write,
                             std::span<const std::uint8_t> bytes) {
        validateAddress(address);
        const auto length = frameLength(bytes, connection.frame);
        if (!length || bytes.size() != *length) throw std::invalid_argument("mc_response_incomplete");
        const auto offset = connection.frame == FrameFormat::Binary4E ? 6U : 2U;
        if (offset == 6 && (readLe16(bytes, 2) != serial || readLe16(bytes, 4) != 0))
            throw std::invalid_argument("mc_response_serial_mismatch");
        if (bytes[offset] != connection.network || bytes[offset + 1] != connection.station ||
            readLe16(bytes, offset + 2) != connection.moduleIo || bytes[offset + 4] != connection.multidrop)
            throw std::invalid_argument("mc_response_route_mismatch");
        Response result;
        result.endCode = readLe16(bytes, offset + 7);
        if (result.endCode != 0) return result;
        const auto data = bytes.subspan(offset + 9);
        const auto expected = write ? 0U : address.bitAccess ? (address.count + 1U) / 2U : address.count * 2U;
        if (data.size() != expected) throw std::invalid_argument("mc_response_data_size_mismatch");
        if (!write && address.bitAccess) {
            for (std::size_t i = 0; i < address.count; ++i) {
                const auto bit = static_cast<std::uint8_t>((data[i / 2] >> (i % 2 == 0 ? 4 : 0)) & 15);
                if (bit > 1) throw std::invalid_argument("mc_response_bit_invalid");
                result.data.push_back(bit);
            }
        } else result.data.assign(data.begin(), data.end());
        return result;
    }

  private:
    static std::uint16_t readLe16(std::span<const std::uint8_t> bytes, std::size_t offset) {
        return static_cast<std::uint16_t>(bytes[offset] | (static_cast<std::uint16_t>(bytes[offset + 1]) << 8));
    }

    static void appendLe16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
        bytes.push_back(static_cast<std::uint8_t>(value));
        bytes.push_back(static_cast<std::uint8_t>(value >> 8));
    }

    static void validateAddress(const Address& address) {
        const bool bits = bitDevice(address.deviceCode);
        const bool words = address.deviceCode == 0xa8 || address.deviceCode == 0xb4 ||
            address.deviceCode == 0xaf || address.deviceCode == 0xb0 ||
            address.deviceCode == 0xc2 || address.deviceCode == 0xc5;
        const auto stride = bits && !address.bitAccess ? 16U : 1U;
        if ((!bits && !words) || address.count == 0 || address.count > 960 || address.number > 0xffffff ||
            address.number + address.count * stride - 1 > 0xffffff ||
            (address.bitAccess && !bitDevice(address.deviceCode)))
            throw std::invalid_argument("mc_address_invalid");
    }

    static std::vector<std::uint8_t> request(const Connection& connection, const Address& address,
        std::uint16_t serial, bool write, std::span<const std::uint8_t> payload) {
        validateAddress(address);
        std::vector<std::uint8_t> bytes;
        if (connection.frame == FrameFormat::Binary4E) {
            bytes = {0x54, 0}; appendLe16(bytes, serial); appendLe16(bytes, 0);
        } else bytes = {0x50, 0};
        bytes.push_back(connection.network); bytes.push_back(connection.station);
        appendLe16(bytes, connection.moduleIo); bytes.push_back(connection.multidrop);
        appendLe16(bytes, static_cast<std::uint16_t>(12 + payload.size()));
        appendLe16(bytes, connection.monitoringTimer);
        appendLe16(bytes, write ? 0x1401 : 0x0401);
        appendLe16(bytes, address.bitAccess ? 1 : 0);
        bytes.push_back(static_cast<std::uint8_t>(address.number));
        bytes.push_back(static_cast<std::uint8_t>(address.number >> 8));
        bytes.push_back(static_cast<std::uint8_t>(address.number >> 16));
        bytes.push_back(address.deviceCode); appendLe16(bytes, address.count);
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        return bytes;
    }
};

class Exchange final {
  public:
    static constexpr bool sharedConnection = false;
    explicit Exchange(const DeviceDefinition& device) : connection_(device.mcConnection) {}
    std::vector<std::uint8_t> handshake() const { return {}; }
    void acceptHandshake(std::span<const std::uint8_t>) { throw std::invalid_argument("mc_unexpected_handshake"); }
    void stripPrefix(std::vector<std::uint8_t>&) const {}
    std::optional<std::size_t> frameLength(std::span<const std::uint8_t> bytes) const {
        return FrameCodec::frameLength(bytes, connection_.frame);
    }
    std::vector<std::uint8_t> read(const ElementDefinition& element) {
        return FrameCodec::read(connection_, address(element), ++serial_);
    }
    std::vector<std::uint8_t> write(const ElementDefinition& element, std::span<const std::uint8_t> data) {
        return FrameCodec::write(connection_, address(element), ++serial_, data);
    }
    polling::Reply response(const ElementDefinition& element, bool write, std::span<const std::uint8_t> bytes) const {
        auto result = FrameCodec::response(connection_, address(element), serial_, write, bytes);
        return {.data = std::move(result.data), .error = result.endCode ? "mc_end_code:" + std::to_string(result.endCode) : ""};
    }
    std::vector<std::uint8_t> encode(const ElementDefinition& element, std::string_view value) const {
        return register_value::encodeValue(element, value);
    }
    std::string decode(const ElementDefinition& element, std::span<const std::uint8_t> data) const {
        if (data.size() != register_value::width(element.dataType)) throw std::invalid_argument("mc_point_size_invalid");
        const auto result = register_value::numericJson(data, element);
        if (!result) throw std::invalid_argument("mc_point_type_invalid");
        (void)command::decimal(*result, element.id);
        return *result;
    }

  private:
    static Address address(const ElementDefinition& element) {
        const auto code = FrameCodec::deviceCode(element.area);
        if (!code || element.address < 0 || element.address > 0xffffff)
            throw std::invalid_argument("mc_point_address_invalid");
        const auto bits = element.dataType == "BOOL";
        return {*code, static_cast<std::uint32_t>(element.address),
            static_cast<std::uint16_t>(bits ? 1 : register_value::width(element.dataType) / 2), bits};
    }
    Connection connection_;
    std::uint16_t serial_ = 0;
};

class SessionFactory final : public ProtocolSessionFactory {
  public:
    const ProtocolDefinition& definition() const noexcept override { return kMcProtocol; }
  protected:
    std::unique_ptr<ProtocolSession> createDeviceSession(const LinkDefinition& link,
        std::string_view connectionId, std::string_view,
        const std::shared_ptr<const RuntimeSnapshot>& snapshot,
        std::vector<const DeviceDefinition*> devices) const override {
        return std::make_unique<polling::Session<Exchange>>(link, std::string(connectionId), snapshot, std::move(devices));
    }
};

} // namespace service::collector::mc
