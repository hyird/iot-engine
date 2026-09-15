#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "service/features/collector/fins/fins.types.h"
#include "service/features/collector/polling/polling.protocol.h"
#include "service/features/collector/register_value/register_value.protocol.h"

namespace service::collector::fins {

class FrameCodec final {
  public:
    struct Response {
        std::uint16_t endCode = 0;
        std::vector<std::uint8_t> data;
    };

    static std::optional<std::uint8_t> memoryArea(std::string_view area, bool bits) {
        if (area == "D") return bits ? 0x02 : 0x82;
        if (area == "CIO") return bits ? 0x30 : 0xb0;
        if (area == "W") return bits ? 0x31 : 0xb1;
        if (area == "H") return bits ? 0x32 : 0xb2;
        if (area == "A") return bits ? 0x33 : 0xb3;
        return std::nullopt;
    }

    static std::vector<std::uint8_t> nodeRequest(std::uint8_t sourceNode) {
        auto bytes = header(0, 4);
        appendBe32(bytes, sourceNode);
        return bytes;
    }

    static Connection nodeResponse(Connection connection, std::span<const std::uint8_t> bytes) {
        validateHeader(bytes, 1);
        if (bytes.size() != 24 || readBe32(bytes, 16) < 1 || readBe32(bytes, 16) > 254 ||
            readBe32(bytes, 20) < 1 || readBe32(bytes, 20) > 254)
            throw std::invalid_argument("fins_node_response_invalid");
        const auto client = static_cast<std::uint8_t>(readBe32(bytes, 16));
        if (connection.sourceNode != 0 && connection.sourceNode != client)
            throw std::invalid_argument("fins_source_node_mismatch");
        connection.sourceNode = client;
        if (connection.destinationNode == 0)
            connection.destinationNode = static_cast<std::uint8_t>(readBe32(bytes, 20));
        return connection;
    }

    static std::optional<std::size_t> frameLength(std::span<const std::uint8_t> bytes) {
        constexpr std::uint8_t signature[]{'F', 'I', 'N', 'S'};
        for (std::size_t i = 0; i < bytes.size() && i < 4; ++i)
            if (bytes[i] != signature[i]) throw std::invalid_argument("fins_tcp_header_invalid");
        if (bytes.size() < 8) return std::nullopt;
        const auto length = readBe32(bytes, 4);
        if (length < 8 || length > 4096) throw std::invalid_argument("fins_tcp_length_invalid");
        return 8 + length;
    }

    static std::vector<std::uint8_t> read(const Connection& connection, const Address& address,
                                         std::uint8_t serviceId) {
        return request(connection, address, serviceId, false, {});
    }

    static std::vector<std::uint8_t> write(const Connection& connection, const Address& address,
                                          std::uint8_t serviceId, std::span<const std::uint8_t> data) {
        if (data.size() != static_cast<std::size_t>(address.count) * (address.bitAccess ? 1 : 2))
            throw std::invalid_argument("fins_write_size_mismatch");
        if (address.bitAccess)
            for (auto value : data) if (value > 1) throw std::invalid_argument("fins_bit_value_invalid");
        return request(connection, address, serviceId, true, data);
    }

    static Response response(const Connection& connection, const Address& address,
        std::uint8_t serviceId, bool write, std::span<const std::uint8_t> bytes) {
        validateHeader(bytes, 2);
        if (bytes.size() < 30 || (bytes[16] & 0x40) == 0 || bytes[17] != 0 ||
            bytes[19] != connection.sourceNetwork || bytes[20] != connection.sourceNode ||
            bytes[21] != connection.sourceUnit || bytes[22] != connection.destinationNetwork ||
            bytes[23] != connection.destinationNode || bytes[24] != connection.destinationUnit ||
            bytes[25] != serviceId || bytes[26] != 1 || bytes[27] != (write ? 2 : 1))
            throw std::invalid_argument("fins_response_route_or_service_mismatch");
        Response result;
        result.endCode = static_cast<std::uint16_t>((bytes[28] << 8) | bytes[29]);
        if (result.endCode != 0) return result;
        const auto expected = write ? 0U : address.count * (address.bitAccess ? 1U : 2U);
        if (bytes.size() != 30 + expected) throw std::invalid_argument("fins_response_data_size_mismatch");
        result.data.assign(bytes.begin() + 30, bytes.end());
        if (address.bitAccess)
            for (auto value : result.data) if (value > 1) throw std::invalid_argument("fins_response_bit_invalid");
        return result;
    }

  private:
    static std::uint32_t readBe32(std::span<const std::uint8_t> bytes, std::size_t offset) {
        std::uint32_t result = 0;
        for (std::size_t i = 0; i < 4; ++i) result = (result << 8) | bytes[offset + i];
        return result;
    }

    static void appendBe32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
        for (int i = 3; i >= 0; --i) bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }

    static std::vector<std::uint8_t> header(std::uint32_t command, std::uint32_t dataLength) {
        std::vector<std::uint8_t> bytes{'F', 'I', 'N', 'S'};
        appendBe32(bytes, 8 + dataLength); appendBe32(bytes, command); appendBe32(bytes, 0);
        return bytes;
    }

    static void validateHeader(std::span<const std::uint8_t> bytes, std::uint32_t command) {
        const auto length = frameLength(bytes);
        if (!length || bytes.size() != *length || bytes.size() < 16)
            throw std::invalid_argument("fins_tcp_response_incomplete");
        if (readBe32(bytes, 8) != command || readBe32(bytes, 12) != 0)
            throw std::invalid_argument("fins_tcp_response_error");
    }

    static std::vector<std::uint8_t> request(const Connection& connection, const Address& address,
        std::uint8_t serviceId, bool write, std::span<const std::uint8_t> data) {
        const auto area = address.memoryArea;
        const bool validArea = address.bitAccess
            ? (area == 0x02 || (area >= 0x30 && area <= 0x33))
            : (area == 0x82 || (area >= 0xb0 && area <= 0xb3));
        if (!validArea || !connection.sourceNode || !connection.destinationNode || connection.sourceNode > 254 ||
            connection.destinationNode > 254 || address.count == 0 || address.count > 480 ||
            address.bit > 15 || (!address.bitAccess && address.bit != 0) ||
            static_cast<std::uint32_t>(address.word) + (address.bitAccess ? (address.bit + address.count - 1) / 16 : address.count - 1) > 65535)
            throw std::invalid_argument("fins_address_invalid");
        auto bytes = header(2, static_cast<std::uint32_t>(18 + data.size()));
        bytes.insert(bytes.end(), {0x80, 0, 2, connection.destinationNetwork, connection.destinationNode,
            connection.destinationUnit, connection.sourceNetwork, connection.sourceNode,
            connection.sourceUnit, serviceId, 1, static_cast<std::uint8_t>(write ? 2 : 1),
            address.memoryArea, static_cast<std::uint8_t>(address.word >> 8),
            static_cast<std::uint8_t>(address.word), address.bit,
            static_cast<std::uint8_t>(address.count >> 8), static_cast<std::uint8_t>(address.count)});
        bytes.insert(bytes.end(), data.begin(), data.end());
        return bytes;
    }
};

class Exchange final {
  public:
    static constexpr bool sharedConnection = false;
    explicit Exchange(const DeviceDefinition& device) : connection_(device.finsConnection) {}
    std::vector<std::uint8_t> handshake() const { return FrameCodec::nodeRequest(connection_.sourceNode); }
    void acceptHandshake(std::span<const std::uint8_t> bytes) { connection_ = FrameCodec::nodeResponse(connection_, bytes); }
    void stripPrefix(std::vector<std::uint8_t>&) const {}
    std::optional<std::size_t> frameLength(std::span<const std::uint8_t> bytes) const { return FrameCodec::frameLength(bytes); }
    std::vector<std::uint8_t> read(const ElementDefinition& element) {
        return FrameCodec::read(connection_, address(element), ++serviceId_);
    }
    std::vector<std::uint8_t> write(const ElementDefinition& element, std::span<const std::uint8_t> data) {
        return FrameCodec::write(connection_, address(element), ++serviceId_, data);
    }
    polling::Reply response(const ElementDefinition& element, bool write, std::span<const std::uint8_t> bytes) const {
        auto result = FrameCodec::response(connection_, address(element), serviceId_, write, bytes);
        return {.data = std::move(result.data), .error = result.endCode ? "fins_end_code:" + std::to_string(result.endCode) : ""};
    }
    std::vector<std::uint8_t> encode(const ElementDefinition& element, std::string_view value) const {
        return register_value::encodeValue(element, value);
    }
    std::string decode(const ElementDefinition& element, std::span<const std::uint8_t> data) const {
        if (data.size() != register_value::width(element.dataType)) throw std::invalid_argument("fins_point_size_invalid");
        const auto result = register_value::numericJson(data, element);
        if (!result) throw std::invalid_argument("fins_point_type_invalid");
        (void)command::decimal(*result, element.id);
        return *result;
    }

  private:
    static Address address(const ElementDefinition& element) {
        const auto bits = element.dataType == "BOOL";
        const auto area = FrameCodec::memoryArea(element.area, bits);
        if (!area || element.address < 0 || element.address > 65535 || element.startBit < 0 || element.startBit > 15)
            throw std::invalid_argument("fins_point_address_invalid");
        return {*area, static_cast<std::uint16_t>(element.address), static_cast<std::uint8_t>(element.startBit),
            static_cast<std::uint16_t>(bits ? 1 : register_value::width(element.dataType) / 2), bits};
    }
    Connection connection_;
    std::uint8_t serviceId_ = 0;
};

class SessionFactory final : public ProtocolSessionFactory {
  public:
    const ProtocolDefinition& definition() const noexcept override { return kFinsProtocol; }
  protected:
    std::unique_ptr<ProtocolSession> createDeviceSession(const LinkDefinition& link,
        std::string_view connectionId, std::string_view,
        const std::shared_ptr<const RuntimeSnapshot>& snapshot,
        std::vector<const DeviceDefinition*> devices) const override {
        return std::make_unique<polling::Session<Exchange>>(link, std::string(connectionId), snapshot, std::move(devices));
    }
};

} // namespace service::collector::fins
