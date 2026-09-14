#pragma once
#include "service/features/collector/collector.entity.h"
#include <set>
#include <utility>
#include <functional>
#include "service/common/message.h"

#include <cstdint>
#include <string>
#include <vector>

namespace service::collector {

using RealtimePointDefinition = service::message::realtime::Point;
using RealtimeDeviceDefinition = service::message::realtime::Device;

struct DtuDefinition {
    std::string key;
    std::string linkId;
    std::string targetId;
    std::string protocol;
    std::vector<std::uint8_t> registrationBytes;
    std::vector<DeviceDefinition> devices;
};

struct RuntimeSnapshot {
    std::vector<LinkDefinition> links;
    std::vector<DeviceDefinition> devices;
    std::vector<RealtimeDeviceDefinition> realtimeDevices;
};

} // namespace service::collector

namespace service::collector {
using ClientTargetKey = std::pair<std::string, std::string>;

struct RuntimeReconcilePlan {
    std::set<std::string, std::less<>> affectedLinks;
    std::set<std::string, std::less<>> restartLinks;
    std::set<ClientTargetKey> refreshClientSessions;
    std::set<ClientTargetKey> restartClientTargets;
};

} // namespace service::collector
