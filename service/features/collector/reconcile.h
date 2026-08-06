#pragma once

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "service/features/collector/types.h"

namespace service::collector {

using ClientTargetKey = std::pair<std::string, std::string>;

struct RuntimeReconcilePlan {
    std::set<std::string, std::less<>> affectedLinks;
    std::set<std::string, std::less<>> restartLinks;
    std::set<ClientTargetKey> refreshClientSessions;
    std::set<ClientTargetKey> restartClientTargets;
};

namespace detail {

inline const LinkDefinition* findLink(
    const std::map<std::string, LinkDefinition, std::less<>>& links,
    std::string_view id) {
    const auto current = links.find(id);
    return current == links.end() ? nullptr : &current->second;
}

inline bool targetAllowsSessionRefresh(const RuntimeSnapshot& snapshot,
                                       const ClientTargetKey& target) {
    return std::none_of(snapshot.devices.begin(), snapshot.devices.end(),
                        [&target](const auto& device) {
                            return device.linkId == target.first &&
                                   device.targetId == target.second &&
                                   (device.protocol != "Modbus" ||
                                    device.modbusMode != "TCP");
                        });
}

inline void addChangedDeviceTransport(
    RuntimeReconcilePlan& plan,
    const std::map<std::string, LinkDefinition, std::less<>>& links,
    const DeviceDefinition& device) {
    plan.affectedLinks.insert(device.linkId);
    const auto* link = findLink(links, device.linkId);
    if (!link || link->mode != "TCP Client") {
        plan.restartLinks.insert(device.linkId);
        return;
    }
    if (device.targetId.empty()) {
        plan.restartLinks.insert(device.linkId);
        return;
    }
    if (link->protocol == "Modbus" && device.modbusMode == "TCP")
        plan.refreshClientSessions.emplace(device.linkId, device.targetId);
    else
        plan.restartClientTargets.emplace(device.linkId, device.targetId);
}

} // namespace detail

[[nodiscard]] inline RuntimeReconcilePlan
planRuntimeReconcile(const RuntimeSnapshot& previous, const RuntimeSnapshot& next) {
    std::map<std::string, LinkDefinition, std::less<>> previousLinks;
    std::map<std::string, LinkDefinition, std::less<>> nextLinks;
    std::map<std::string, DeviceDefinition, std::less<>> previousDevices;
    std::map<std::string, DeviceDefinition, std::less<>> nextDevices;

    for (const auto& link : previous.links)
        previousLinks.insert_or_assign(link.id, link);
    for (const auto& link : next.links)
        nextLinks.insert_or_assign(link.id, link);
    for (const auto& device : previous.devices)
        previousDevices.insert_or_assign(device.id, device);
    for (const auto& device : next.devices)
        nextDevices.insert_or_assign(device.id, device);

    RuntimeReconcilePlan plan;
    for (const auto& [id, link] : previousLinks) {
        const auto current = nextLinks.find(id);
        if (current == nextLinks.end() || current->second != link)
            plan.affectedLinks.insert(id);
    }
    for (const auto& [id, link] : nextLinks) {
        const auto old = previousLinks.find(id);
        if (old == previousLinks.end() || old->second != link)
            plan.affectedLinks.insert(id);
    }

    for (const auto& [id, device] : previousDevices) {
        const auto current = nextDevices.find(id);
        if (current == nextDevices.end() || current->second != device) {
            detail::addChangedDeviceTransport(plan, previousLinks, device);
            if (current != nextDevices.end())
                detail::addChangedDeviceTransport(plan, nextLinks, current->second);
        }
    }
    for (const auto& [id, device] : nextDevices) {
        if (!previousDevices.contains(id))
            detail::addChangedDeviceTransport(plan, nextLinks, device);
    }
    for (auto current = plan.refreshClientSessions.begin();
         current != plan.refreshClientSessions.end();) {
        if (!detail::targetAllowsSessionRefresh(previous, *current) ||
            !detail::targetAllowsSessionRefresh(next, *current)) {
            plan.restartClientTargets.insert(*current);
            current = plan.refreshClientSessions.erase(current);
        } else
            ++current;
    }
    return plan;
}

} // namespace service::collector
