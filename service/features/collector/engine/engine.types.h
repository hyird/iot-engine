#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "service/features/collector/collector.types.h"

namespace service::collector {

struct ProtocolConnectionInfo {
    std::string connectionId;
    std::string linkId;
    std::string remoteAddress;
    std::string targetId;
    std::uint64_t sessionEpoch = 0;
};

struct ProtocolSessionRefresh {
    std::string connectionId;
    std::vector<ProtocolAction> retiredActions;
    std::vector<ProtocolAction> startedActions;
};

struct DebugPacketIdentity {
    std::string_view linkId;
    std::string_view targetId;
    std::string_view direction;
    std::string_view knownDevice;
    bool deviceOnly = false;
    std::span<const std::uint8_t> bytes;
    const std::set<std::string>* boundDevices = nullptr;
};

} // namespace service::collector
