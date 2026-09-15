#pragma once

#include <array>
#include <charconv>
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

inline constexpr ProtocolDefinition kMcProtocol{
    "MC", ProtocolCapability::TcpServer | ProtocolCapability::TcpClient |
        ProtocolCapability::Registration | ProtocolCapability::Heartbeat |
        ProtocolCapability::Polling | ProtocolCapability::Commands,
    CommandLayout::WritableElements, CommandTransport::Raw,
    ResponseTracking::EverySend, PacketAttribution::Connection};

inline constexpr ProtocolDefinition kFinsProtocol{
    "FINS", ProtocolCapability::TcpServer | ProtocolCapability::TcpClient |
        ProtocolCapability::Registration | ProtocolCapability::Heartbeat |
        ProtocolCapability::Polling | ProtocolCapability::Commands,
    CommandLayout::WritableElements, CommandTransport::Raw,
    ResponseTracking::EverySend, PacketAttribution::Connection};

inline constexpr ProtocolDefinition kDlt645Protocol{
    "DLT645", ProtocolCapability::TcpServer | ProtocolCapability::TcpClient |
        ProtocolCapability::Registration | ProtocolCapability::Heartbeat |
        ProtocolCapability::Polling | ProtocolCapability::Commands,
    CommandLayout::WritableElements, CommandTransport::Raw,
    ResponseTracking::EverySend, PacketAttribution::Connection};

inline constexpr std::array kProtocolDefinitions{kModbusProtocol, kS7Protocol, kSl651Protocol,
    kMcProtocol, kFinsProtocol, kDlt645Protocol};

[[nodiscard]] inline const ProtocolDefinition& protocolDefinition(std::string_view name) {
    for (const auto& definition : kProtocolDefinitions)
        if (definition.name == name) return definition;
    throw std::invalid_argument("command_invalid: unsupported protocol: " + std::string(name));
}


// 配置快照与数据库投影使用相同的字段转换，范围校验先于窄整数转换。
class ConnectionConfig final {
  public:
    static std::vector<message::StreamField> fields(const DeviceDefinition& device) {
        return {{"mc_frame", device.mcConnection.frame == mc::FrameFormat::Binary4E ? "4E" : "3E"},
            {"dlt645_version", device.dlt645Connection.version == dlt645::Version::V1997 ? "1997" : "2007"},
            {"mc_network", std::to_string(device.mcConnection.network)},
            {"mc_station", std::to_string(device.mcConnection.station)},
            {"mc_module_io", std::to_string(device.mcConnection.moduleIo)},
            {"mc_multidrop", std::to_string(device.mcConnection.multidrop)},
            {"mc_monitoring_timer", std::to_string(device.mcConnection.monitoringTimer)},
            {"fins_destination_network", std::to_string(device.finsConnection.destinationNetwork)},
            {"fins_destination_node", std::to_string(device.finsConnection.destinationNode)},
            {"fins_destination_unit", std::to_string(device.finsConnection.destinationUnit)},
            {"fins_source_network", std::to_string(device.finsConnection.sourceNetwork)},
            {"fins_source_node", std::to_string(device.finsConnection.sourceNode)},
            {"fins_source_unit", std::to_string(device.finsConnection.sourceUnit)},
            {"dlt645_wakeup_bytes", std::to_string(device.dlt645Connection.wakeupBytes)},
            {"dlt645_write_password", device.dlt645Connection.writePassword},
            {"dlt645_operator_code", device.dlt645Connection.operatorCode},
        };
    }

    static void apply(DeviceDefinition& device, const std::vector<message::StreamField>& fields) {
        const auto text = [&](std::string_view name, std::string_view fallback = "") {
            for (const auto& field : fields) if (field.name == name) return field.value;
            return std::string(fallback);
        };
        const auto integer = [&](std::string_view name, std::int64_t minimum, std::int64_t maximum, std::int64_t fallback) {
            const auto value = text(name, std::to_string(fallback));
            std::int64_t result = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || result < minimum || result > maximum)
                throw std::invalid_argument("invalid connection configuration: " + std::string(name));
            return result;
        };
        const auto frame = text("mc_frame", "3E"), version = text("dlt645_version", "2007");
        if (frame != "3E" && frame != "4E") throw std::invalid_argument("invalid MC frame format");
        if (version != "1997" && version != "2007") throw std::invalid_argument("invalid DLT645 edition");
        device.mcConnection.frame = frame == "4E" ? mc::FrameFormat::Binary4E : mc::FrameFormat::Binary3E;
        device.dlt645Connection.version = version == "1997" ? dlt645::Version::V1997 : dlt645::Version::V2007;
        device.mcConnection.network = static_cast<decltype(device.mcConnection.network)>(integer("mc_network", 0, 255, 0));
        device.mcConnection.station = static_cast<decltype(device.mcConnection.station)>(integer("mc_station", 0, 255, 255));
        device.mcConnection.moduleIo = static_cast<decltype(device.mcConnection.moduleIo)>(integer("mc_module_io", 0, 65535, 1023));
        device.mcConnection.multidrop = static_cast<decltype(device.mcConnection.multidrop)>(integer("mc_multidrop", 0, 255, 0));
        device.mcConnection.monitoringTimer = static_cast<decltype(device.mcConnection.monitoringTimer)>(integer("mc_monitoring_timer", 1, 65535, 16));
        device.finsConnection.destinationNetwork = static_cast<decltype(device.finsConnection.destinationNetwork)>(integer("fins_destination_network", 0, 127, 0));
        device.finsConnection.destinationNode = static_cast<decltype(device.finsConnection.destinationNode)>(integer("fins_destination_node", 0, 254, 0));
        device.finsConnection.destinationUnit = static_cast<decltype(device.finsConnection.destinationUnit)>(integer("fins_destination_unit", 0, 255, 0));
        device.finsConnection.sourceNetwork = static_cast<decltype(device.finsConnection.sourceNetwork)>(integer("fins_source_network", 0, 127, 0));
        device.finsConnection.sourceNode = static_cast<decltype(device.finsConnection.sourceNode)>(integer("fins_source_node", 0, 254, 0));
        device.finsConnection.sourceUnit = static_cast<decltype(device.finsConnection.sourceUnit)>(integer("fins_source_unit", 0, 255, 0));
        device.dlt645Connection.wakeupBytes = static_cast<decltype(device.dlt645Connection.wakeupBytes)>(integer("dlt645_wakeup_bytes", 0, 4, 4));
        device.dlt645Connection.writePassword = text("dlt645_write_password");
        device.dlt645Connection.operatorCode = text("dlt645_operator_code");
    }
};

} // namespace service::collector
