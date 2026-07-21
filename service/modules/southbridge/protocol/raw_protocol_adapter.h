#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "service/modules/southbridge/protocol/protocol_adapter.h"

namespace service::southbridge {

class RawProtocolAdapter final : public ProtocolAdapter {
  public:
    explicit RawProtocolAdapter(std::string protocol) : protocol_(std::move(protocol)) {}

    [[nodiscard]] std::string_view protocol() const noexcept override { return protocol_; }

    [[nodiscard]] std::optional<std::string>
    identify(const bridge::IngressPacket& packet,
             const std::vector<DtuDefinition>& candidates) const override {
        std::vector<const DtuDefinition*> protocolCandidates;
        for (const auto& candidate : candidates)
            if (candidate.protocol == protocol_)
                protocolCandidates.push_back(&candidate);
        if (protocolCandidates.size() == 1 && protocolCandidates.front()->registrationBytes.empty())
            return protocolCandidates.front()->key;
        if (protocol_ == "SL651") {
            const auto deviceCode = sl651DeviceCode(packet.payload);
            if (!deviceCode)
                return std::nullopt;
            const DtuDefinition* matched = nullptr;
            for (const auto* candidate : protocolCandidates) {
                const auto contains = std::any_of(
                    candidate->devices.begin(), candidate->devices.end(), [&](const auto& device) {
                        return normalizedSl651Code(device.code) == *deviceCode;
                    });
                if (!contains)
                    continue;
                if (matched && matched->key != candidate->key)
                    return std::nullopt;
                matched = candidate;
            }
            return matched ? std::optional<std::string>(matched->key) : std::nullopt;
        }
        if (protocol_ != "Modbus" || packet.payload.empty())
            return std::nullopt;
        const auto slaveId = modbusUnitId(packet.payload);
        const DtuDefinition* matched = nullptr;
        for (const auto* candidate : protocolCandidates) {
            const auto contains =
                std::any_of(candidate->devices.begin(), candidate->devices.end(),
                            [&](const auto& device) { return device.slaveId == slaveId; });
            if (!contains)
                continue;
            if (matched && matched->key != candidate->key)
                return std::nullopt;
            matched = candidate;
        }
        if (!matched)
            return std::nullopt;
        return matched->key;
    }

    [[nodiscard]] std::vector<bridge::ParsedDeviceMessage>
    parse(const bridge::IngressPacket& packet, const DtuDefinition& dtu) const override {
        const DeviceDefinition* device = nullptr;
        if (protocol_ == "Modbus" && !packet.payload.empty()) {
            const auto slaveId = modbusUnitId(packet.payload);
            const auto match =
                std::find_if(dtu.devices.begin(), dtu.devices.end(),
                             [&](const auto& current) { return current.slaveId == slaveId; });
            if (match != dtu.devices.end())
                device = &*match;
        }
        if (!device && dtu.devices.size() == 1)
            device = &dtu.devices.front();
        if (!device)
            return {};
        bridge::ParsedDeviceMessage message;
        message.messageId = bridge::nextMessageId();
        message.causationId = packet.messageId;
        message.linkId = packet.linkId;
        message.deviceId = device->id;
        message.deviceCode = device->code;
        message.protocol = protocol_;
        message.connectionId = packet.connectionId;
        message.occurredAtMs = packet.occurredAtMs;
        message.rawPayload = packet.payload;
        return {std::move(message)};
    }

  private:
    static std::string normalizedSl651Code(std::string_view code) {
        if (code.size() >= 10)
            return std::string(code.substr(code.size() - 10));
        return std::string(10 - code.size(), '0') + std::string(code);
    }

    static std::optional<std::string> sl651DeviceCode(const std::vector<std::uint8_t>& bytes) {
        if (bytes.size() < 8 || bytes[0] != 0x7E || bytes[1] != 0x7E)
            return std::nullopt;
        std::string code;
        code.reserve(10);
        static constexpr std::array<char, 16> digits{'0', '1', '2', '3', '4', '5', '6', '7',
                                                     '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
        for (std::size_t index = 3; index < 8; ++index) {
            const auto byte = bytes[index];
            if ((byte >> 4U) > 9 || (byte & 0x0FU) > 9)
                return std::nullopt;
            code.push_back(digits[byte >> 4U]);
            code.push_back(digits[byte & 0x0FU]);
        }
        return code;
    }

    static std::uint8_t modbusUnitId(const std::vector<std::uint8_t>& bytes) noexcept {
        if (bytes.size() >= 7 && bytes[2] == 0 && bytes[3] == 0)
            return bytes[6];
        return bytes.front();
    }

    std::string protocol_;
};

} // namespace service::southbridge
