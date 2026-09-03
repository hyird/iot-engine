#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <ruvia/core/Task.h>

#include "service/common/http.h"
#include "service/common/uuid.h"
#include "service/features/vpn/cidr.h"

namespace service::vpn {

namespace route_sync_detail {

inline std::string rowValue(const auto& row, std::size_t index) {
    return std::string(row[index].value().value_or(std::string_view{}));
}

inline std::int64_t integer(std::string_view value, std::int64_t fallback = 0) {
    return service::common::parseInt64(
               value.empty() ? std::nullopt : std::optional<std::string_view>(value))
        .value_or(fallback);
}

} // namespace route_sync_detail

// A virtual mapping must not collide with another virtual mapping or with a
// real target network. Real target networks themselves may repeat: each one
// is translated behind its owning Edge peer.
inline bool virtualMappingConflicts(
    const Ipv4Cidr& candidate, const std::optional<Ipv4Cidr>& realTarget,
    const std::optional<Ipv4Cidr>& existingVirtual) noexcept {
    return (realTarget && candidate.overlaps(*realTarget)) ||
           (existingVirtual && candidate.overlaps(*existingVirtual));
}

inline bool realTargetConflictsVirtual(
    const Ipv4Cidr& candidate, const std::optional<Ipv4Cidr>& existingVirtual) noexcept {
    return existingVirtual && candidate.overlaps(*existingVirtual);
}

// Reconcile the Edge bridge inventory with its VPN routes. The operation keeps a
// user-selected virtual network while the bridge prefix remains compatible. A
// prefix change necessarily selects a new equally-sized virtual network. Real
// LAN prefixes are local to each Edge peer and may repeat across sites; virtual
// prefixes remain globally unique so the Hub can route them unambiguously.
template <typename Db>
ruvia::Task<void> syncEdgeBridgeRoutes(Db& db, std::string_view peerId,
                                       std::string_view networkId,
                                       std::string_view edgeNodeId,
                                       std::string_view actorId) {
    struct RouteRecord final {
        std::string id;
        std::string lanInterface;
        std::optional<Ipv4Cidr> target;
        std::optional<Ipv4Cidr> virtualNetwork;
    };
    struct BridgeRecord final {
        std::string lanInterface;
        Ipv4Cidr target;
    };

    const auto bridgeRows = co_await db.query(R"sql(
SELECT name, device, ipv4, prefix_length
FROM edge_node_network
WHERE node_id = $1::uuid AND is_bridge = TRUE
  AND COALESCE(ipv4, '') <> '' AND prefix_length BETWEEN 1 AND 30
ORDER BY name)sql", service::common::dbParams(edgeNodeId));
    std::vector<BridgeRecord> bridges;
    for (const auto& row : bridgeRows) {
        const auto address = route_sync_detail::rowValue(row, 2);
        const auto prefix = route_sync_detail::integer(route_sync_detail::rowValue(row, 3));
        const auto target = networkCidr(address, static_cast<std::uint8_t>(prefix));
        if (!target || !isPrivateIpv4(*target))
            continue;
        const auto name = route_sync_detail::rowValue(row, 1).empty()
                              ? route_sync_detail::rowValue(row, 0)
                              : route_sync_detail::rowValue(row, 1);
        if (!name.empty())
            bridges.push_back(BridgeRecord{name, *target});
    }
    if (bridges.empty())
        service::common::fail(21008, "EdgeNode 尚未上报可映射的私有桥接 LAN 网段", 409);

    const auto currentRows = co_await db.query(R"sql(
SELECT id::text, lan_interface, target_cidr, virtual_cidr
FROM vpn_route WHERE edge_peer_id = $1::uuid ORDER BY id)sql",
                                               service::common::dbParams(peerId));
    std::vector<RouteRecord> current;
    current.reserve(currentRows.size());
    for (const auto& row : currentRows)
        current.push_back(RouteRecord{
            route_sync_detail::rowValue(row, 0), route_sync_detail::rowValue(row, 1),
            parseCidr(route_sync_detail::rowValue(row, 2), 1, 30),
            parseCidr(route_sync_detail::rowValue(row, 3), 1, 30)});

    const auto allRows = co_await db.query(
        "SELECT id::text, lan_interface, target_cidr, virtual_cidr FROM vpn_route");
    std::vector<RouteRecord> allRoutes;
    allRoutes.reserve(allRows.size());
    for (const auto& row : allRows)
        allRoutes.push_back(RouteRecord{
            route_sync_detail::rowValue(row, 0), route_sync_detail::rowValue(row, 1),
            parseCidr(route_sync_detail::rowValue(row, 2), 1, 30),
            parseCidr(route_sync_detail::rowValue(row, 3), 1, 30)});

    const auto conflicts = [&](const Ipv4Cidr& candidate, std::string_view excludedId) {
        for (const auto& route : allRoutes) {
            if (route.id == excludedId)
                continue;
            if (virtualMappingConflicts(candidate, route.target, route.virtualNetwork))
                return true;
        }
        return false;
    };
    const auto chooseVirtual = [&](const Ipv4Cidr& target,
                                   std::string_view excludedId) -> std::optional<Ipv4Cidr> {
        if (const auto preferred = mappedVirtualCidr(target);
            preferred && !preferred->overlaps(target) && !conflicts(*preferred, excludedId))
            return preferred;
        if (target.prefix < kVirtualLanPool.prefix)
            return std::nullopt;
        const auto blockSize = static_cast<std::uint64_t>(target.size());
        for (std::uint64_t offset = 0; offset < kVirtualLanPool.size(); offset += blockSize) {
            const Ipv4Cidr candidate{
                static_cast<std::uint32_t>(kVirtualLanPool.network + offset), target.prefix};
            if (!candidate.overlaps(target) && !conflicts(candidate, excludedId))
                return candidate;
        }
        return std::nullopt;
    };

    std::unordered_set<std::string> claimed;
    for (const auto& bridge : bridges) {
        auto existing = std::find_if(current.begin(), current.end(), [&](const auto& route) {
            return !claimed.contains(route.id) &&
                   ((route.target && route.target->network == bridge.target.network &&
                     route.target->prefix == bridge.target.prefix) ||
                    route.lanInterface == bridge.lanInterface);
        });
        std::string routeId;
        std::optional<Ipv4Cidr> virtualNetwork;
        if (existing != current.end()) {
            routeId = existing->id;
            claimed.emplace(routeId);
            if (existing->virtualNetwork &&
                existing->virtualNetwork->prefix == bridge.target.prefix &&
                kVirtualLanPool.contains(existing->virtualNetwork->network) &&
                kVirtualLanPool.contains(existing->virtualNetwork->network +
                                         existing->virtualNetwork->size() - 1U) &&
                !existing->virtualNetwork->overlaps(bridge.target) &&
                !conflicts(*existing->virtualNetwork, routeId))
                virtualNetwork = existing->virtualNetwork;
            else
                virtualNetwork = chooseVirtual(bridge.target, routeId);
        } else {
            virtualNetwork = chooseVirtual(bridge.target, {});
        }
        const auto excludedId = existing != current.end() ? existing->id : std::string{};
        for (const auto& route : allRoutes) {
            if (route.id == excludedId)
                continue;
            if (realTargetConflictsVirtual(bridge.target, route.virtualNetwork))
                service::common::fail(21002, "真实 LAN 与已有虚拟网段重叠", 409);
        }
        if (!virtualNetwork)
            service::common::fail(21009, "没有可用的全局唯一虚拟网段", 409);
        const auto targetCidr = bridge.target.text();
        const auto virtualCidr = virtualNetwork->text();
        if (existing != current.end()) {
            (void)co_await db.execute(R"sql(
UPDATE vpn_route SET network_id = $2::uuid, lan_interface = $3, target_cidr = $4,
    virtual_cidr = $5, mode = 'nat', nat_mode = 'masquerade', enabled = TRUE,
    status = 'active', last_error = '', updated_at = NOW()
WHERE id = $1::uuid)sql",
                                      service::common::dbParams(routeId, networkId,
                                                                bridge.lanInterface, targetCidr,
                                                                virtualCidr));
            for (auto& route : allRoutes)
                if (route.id == routeId) {
                    route.lanInterface = bridge.lanInterface;
                    route.target = bridge.target;
                    route.virtualNetwork = virtualNetwork;
                }
        } else {
            const auto id = service::common::nextUuidV7();
            (void)co_await db.execute(R"sql(
INSERT INTO vpn_route(id, network_id, edge_peer_id, lan_interface, target_cidr,
                      virtual_cidr, mode, nat_mode, enabled, status, created_by)
VALUES ($1::uuid, $2::uuid, $3::uuid, $4, $5, $6,
        'nat', 'masquerade', TRUE, 'active', $7::uuid))sql",
                                      service::common::dbParams(id, networkId, peerId,
                                                                bridge.lanInterface, targetCidr,
                                                                virtualCidr, actorId));
            allRoutes.push_back(
                RouteRecord{id, bridge.lanInterface, bridge.target, virtualNetwork});
        }
    }
}

} // namespace service::vpn
