#pragma once

#include <array>
#include <stdexcept>
#include "service/features/collector/collector.types.h"

namespace service::collector {

// 采集和指令准备共同使用的协议定义；具体报文与握手仍归各协议实现。
inline constexpr ProtocolDefinition kModbusProtocol{
    "Modbus",
    ProtocolCapability::TcpServer | ProtocolCapability::TcpClient |
        ProtocolCapability::Registration | ProtocolCapability::Heartbeat |
        ProtocolCapability::Polling | ProtocolCapability::Discovery | ProtocolCapability::Commands,
    CommandLayout::WritableElements, CommandTransport::DeviceConfigured,
    ResponseTracking::EverySend, PacketAttribution::Connection};

inline constexpr ProtocolDefinition kS7Protocol{
    "S7",
    ProtocolCapability::TcpServer | ProtocolCapability::TcpClient |
        ProtocolCapability::Registration | ProtocolCapability::Heartbeat |
        ProtocolCapability::Polling | ProtocolCapability::Discovery | ProtocolCapability::Commands,
    CommandLayout::WritableElements, CommandTransport::Raw,
    ResponseTracking::EverySend, PacketAttribution::Connection};

inline constexpr ProtocolDefinition kSl651Protocol{
    "SL651", ProtocolCapability::TcpServer | ProtocolCapability::Commands |
        ProtocolCapability::UnsolicitedReports,
    CommandLayout::CompleteFunction, CommandTransport::Raw,
    ResponseTracking::CommandsOnly, PacketAttribution::ParsedFrame};

inline constexpr std::array kProtocolDefinitions{kModbusProtocol, kS7Protocol, kSl651Protocol};

[[nodiscard]] inline const ProtocolDefinition& protocolDefinition(std::string_view name) {
    for (const auto& definition : kProtocolDefinitions)
        if (definition.name == name) return definition;
    throw std::invalid_argument("command_invalid: unsupported protocol: " + std::string(name));
}

} // namespace service::collector
