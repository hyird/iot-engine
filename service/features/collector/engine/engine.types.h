#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "service/features/collector/collector.protocol.h"

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

} // namespace service::collector
