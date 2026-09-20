#pragma once
#include "service/features/edge/edge.config.h"

#include <ruvia/core/StopToken.h>

#include "service/utils/number.h"
#include <chrono>
#include <limits>
#include <sstream>
#include <ruvia/web/redis/RedisRepository.h>
#include <ruvia/web/WebWorker.h>
#include "service/features/vpn/firewall/firewall.transport.h"

#include "service/features/vpn/vpn.entity.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <edge.pb.h>
#include <google/protobuf/message.h>

#include "service/common/message.h"
#include "service/features/edge/edge.protocol.h"
#include "service/features/edge/session/session.service.h"
#include "service/utils/network.h"

namespace service::vpn::client_config {

inline std::string render(std::string_view privateKey, std::string_view address,
                          std::string_view hubPublicKey, std::string_view hubEndpoint,
                          std::uint16_t hubPort,
                          const std::vector<std::string>& allowedRoutes) {
    std::string config{"[Interface]\nPrivateKey = "};
    config += privateKey;
    config += "\nAddress = ";
    config += address;
    config += "/32\nMTU = 1280\n\n[Peer]\nPublicKey = ";
    config += hubPublicKey;
    config += "\nEndpoint = ";
    config += hubEndpoint;
    config += ":" + std::to_string(hubPort);
    config += "\nAllowedIPs = ";
    for (std::size_t index = 0; index < allowedRoutes.size(); ++index) {
        if (index != 0)
            config += ", ";
        config += allowedRoutes[index];
    }
    config += "\nPersistentKeepalive = 120\n";
    return config;
}

} // namespace service::vpn::client_config

#include <cstddef>
#include <optional>

#include <ruvia/core/Task.h>
#include <ruvia/web/db/DbQuery.h>

#include "service/common/http.h"
#include "service/common/uuid.h"
#include "service/utils/redis.h"

namespace service::vpn {

using service::message::vpn::kOverlayPool;
using service::message::vpn::kVirtualLanPool;
using service::message::vpn::mappedVirtualCidr;
using service::utils::network::Ipv4Cidr;
using service::utils::network::hostAddress;
using service::utils::network::isPrivateIpv4;
using service::utils::network::networkCidr;
using service::utils::network::parseCidr;
using service::utils::network::parseIpv4;

class VpnRuntimeService final {
  public:
    template <typename Transaction>
    static ruvia::Task<bool> acquireReconciliation(Transaction& transaction,
        std::pmr::memory_resource* resource, bool background) {
        ruvia::DbQuery lock(resource);
        lock.select(lock.call(background ? "pg_try_advisory_xact_lock" : "pg_advisory_xact_lock",
            { lock.cast(lock.value(std::int64_t{5282804697543808071}), ruvia::DbDataType::kBigInt) }));
        const auto rows = co_await transaction.query(lock);
        co_return !background || (!rows.empty() && rows[0][0].value().value_or("") == "t");
    }

    template <typename Transaction>
    static ruvia::Task<void> lockAddressAllocation(Transaction& transaction,
        std::pmr::memory_resource* resource) {
        ruvia::DbQuery lock(resource);
        lock.select(lock.call("pg_advisory_xact_lock",
            { lock.cast(lock.value(std::int64_t{5282804697543808068}), ruvia::DbDataType::kBigInt) }));
        (void)co_await transaction.query(lock);
    }

    struct Peer final {
        std::string publicKey;
        std::string assignedIpv4;
        std::string peerType;
        std::string sourceRoutes;
        std::string allowedRoutes;
        std::string edgeAddresses;
    };

    template <typename Context>
    static ruvia::Task<std::vector<Peer>> loadActivePeers(Context& context) {
        using Op = ruvia::DbBinaryOperator;
        ruvia::DbQuery routes;
        const std::vector<ruvia::DbOrderTerm> routeOrder{ { routes.column(service::vpn::persistence::VpnRouteEntity::columnName<"virtual_cidr">(), "r") } };
        routes.select(routes.aggregate("string_agg", { routes.column(service::vpn::persistence::VpnRouteEntity::columnName<"virtual_cidr">(), "r"), routes.value(", ") }, false, routeOrder))
            .from(service::vpn::persistence::VpnRouteEntity::tableName(), "r")
            .andWhere(routes.binary(routes.column(service::vpn::persistence::VpnRouteEntity::columnName<"edge_peer_id">(), "r"), Op::kEqual, routes.column(service::vpn::persistence::VpnPeerEntity::columnName<"id">(), "p")))
            .andWhere(routes.column(service::vpn::persistence::VpnRouteEntity::columnName<"enabled">(), "r"));
        ruvia::DbQuery allowed;
        const std::vector<ruvia::DbOrderTerm> allowedOrder{ { allowed.column(service::vpn::persistence::VpnEffectiveRouteAccessEntity::columnName<"virtual_cidr">(), "access") } };
        allowed.select(allowed.aggregate("string_agg", { allowed.column(service::vpn::persistence::VpnEffectiveRouteAccessEntity::columnName<"virtual_cidr">(), "access"), allowed.value(", ") }, false, allowedOrder))
            .from(service::vpn::persistence::VpnEffectiveRouteAccessEntity::tableName(), "access")
            .andWhere(allowed.binary(allowed.column(service::vpn::persistence::VpnEffectiveRouteAccessEntity::columnName<"peer_id">(), "access"), Op::kEqual, allowed.column(service::vpn::persistence::VpnPeerEntity::columnName<"id">(), "p")));
        ruvia::DbQuery addresses;
        const std::vector<ruvia::DbOrderTerm> addressOrder{ { addresses.column(service::vpn::persistence::VpnEffectiveEdgeAccessEntity::columnName<"edge_address">(), "access") } };
        addresses.select(addresses.aggregate("string_agg", { addresses.column(service::vpn::persistence::VpnEffectiveEdgeAccessEntity::columnName<"edge_address">(), "access"), addresses.value(", ") }, false, addressOrder))
            .from(service::vpn::persistence::VpnEffectiveEdgeAccessEntity::tableName(), "access")
            .andWhere(addresses.binary(addresses.column(service::vpn::persistence::VpnEffectiveEdgeAccessEntity::columnName<"peer_id">(), "access"), Op::kEqual, addresses.column(service::vpn::persistence::VpnPeerEntity::columnName<"id">(), "p")));
        ruvia::DbQuery peersQuery;
        peersQuery.select(peersQuery.column(service::vpn::persistence::VpnPeerEntity::columnName<"public_key">(), "p"))
            .addSelect(peersQuery.call("host", { peersQuery.column(service::vpn::persistence::VpnPeerEntity::columnName<"assigned_ipv4">(), "p") }))
            .addSelect(peersQuery.column(service::vpn::persistence::VpnPeerEntity::columnName<"peer_type">(), "p"))
            .addSelect(peersQuery.coalesce({ peersQuery.subquery(routes), peersQuery.value("") }))
            .addSelect(peersQuery.coalesce({ peersQuery.subquery(allowed), peersQuery.value("") }))
            .addSelect(peersQuery.coalesce({ peersQuery.subquery(addresses), peersQuery.value("") }))
            .from(service::vpn::persistence::VpnPeerEntity::tableName(), "p")
            .join(ruvia::DbJoinType::kInner, service::vpn::persistence::VpnNetworkEntity::tableName(),
                peersQuery.binary(peersQuery.column(service::vpn::persistence::VpnNetworkEntity::columnName<"id">(), "n"), Op::kEqual, peersQuery.column(service::vpn::persistence::VpnPeerEntity::columnName<"network_id">(), "p")), "n")
            .andWhere(peersQuery.binary(peersQuery.column(service::vpn::persistence::VpnPeerEntity::columnName<"status">(), "p"), Op::kEqual, peersQuery.value("active")))
            .andWhere(peersQuery.binary(peersQuery.column(service::vpn::persistence::VpnNetworkEntity::columnName<"status">(), "n"), Op::kEqual, peersQuery.value("enabled")))
            .andWhere(peersQuery.unary(ruvia::DbUnaryOperator::kIsNull, peersQuery.column(service::vpn::persistence::VpnNetworkEntity::columnName<"deleted_at">(), "n")))
            .andWhere(peersQuery.binary(peersQuery.column(service::vpn::persistence::VpnPeerEntity::columnName<"public_key">(), "p"), Op::kNotEqual, peersQuery.value("")))
            .andWhere(peersQuery.binary(peersQuery.unary(ruvia::DbUnaryOperator::kNot, peersQuery.column(service::vpn::persistence::VpnPeerEntity::columnName<"client_managed">(), "p")),
                Op::kOr, peersQuery.call("vpn_desktop_user_authorized", { peersQuery.column(service::vpn::persistence::VpnPeerEntity::columnName<"user_id">(), "p") })))
            .addOrderBy(peersQuery.column(service::vpn::persistence::VpnPeerEntity::columnName<"id">(), "p"));
        const auto rows = co_await context.db().query(peersQuery);
        std::vector<Peer> peers;
        peers.reserve(rows.size());
        for (const auto& row : rows)
            peers.push_back(Peer{
                std::string(row[0].value().value_or(std::string_view{})),
                std::string(row[1].value().value_or(std::string_view{})),
                std::string(row[2].value().value_or(std::string_view{})),
                std::string(row[3].value().value_or(std::string_view{})),
                std::string(row[4].value().value_or(std::string_view{})),
                std::string(row[5].value().value_or(std::string_view{})),
            });
        co_return peers;
    }

    template <typename Context>
    static ruvia::Task<void> updatePeerHandshake(Context& context,
                                                 std::string_view publicKey,
                                                 std::int64_t seconds) {
        ruvia::DbQuery handshake;
        const auto timestamp = handshake.call("to_timestamp", { handshake.cast(handshake.value(seconds), ruvia::DbDataType::kDouble) });
        handshake.update(service::vpn::persistence::VpnPeerEntity::tableName()).set(service::vpn::persistence::VpnPeerEntity::columnName<"last_handshake_at">(), timestamp).set(service::vpn::persistence::VpnPeerEntity::columnName<"updated_at">(), handshake.call("now"))
            .andWhere(handshake.binary(handshake.column(service::vpn::persistence::VpnPeerEntity::columnName<"public_key">()), ruvia::DbBinaryOperator::kEqual, handshake.value(publicKey)))
            .andWhere(handshake.binary(handshake.column(service::vpn::persistence::VpnPeerEntity::columnName<"status">()), ruvia::DbBinaryOperator::kEqual, handshake.value("active")))
            .andWhere(handshake.binary(handshake.unary(ruvia::DbUnaryOperator::kIsNull, handshake.column(service::vpn::persistence::VpnPeerEntity::columnName<"last_handshake_at">())),
                ruvia::DbBinaryOperator::kOr, handshake.binary(handshake.column(service::vpn::persistence::VpnPeerEntity::columnName<"last_handshake_at">()), ruvia::DbBinaryOperator::kLess, timestamp)));
        (void)co_await context.db().execute(handshake);
    }
};

namespace detail {

inline std::string edgeConfigRowValue(const auto& row, std::size_t index) {
    return std::string(row[index].value().value_or(std::string_view{}));
}

} // namespace detail

namespace feature {

namespace route_sync_detail {

inline std::string rowValue(const auto& row, std::size_t index) {
    return std::string(row[index].value().value_or(std::string_view{}));
}

inline std::int64_t integer(std::string_view value, std::int64_t fallback = 0) {
    return service::utils::parseInt64(
               value.empty() ? std::nullopt : std::optional<std::string_view>(value))
        .value_or(fallback);
}

} // namespace route_sync_detail

// A virtual mapping must not collide with another virtual mapping or with a
// real target network. Real target networks themselves may repeat: each one is
// translated behind its owning Edge peer.
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

// Reconcile the Edge bridge inventory with its VPN routes. The operation keeps
// a user-selected virtual network while the bridge prefix remains compatible.
// A prefix change necessarily selects a new equally-sized virtual network. Real
// LAN prefixes are local to each Edge peer and may repeat across sites; virtual
// prefixes remain globally unique so the Hub can route them unambiguously.
template <typename Db>
ruvia::Task<void> syncEdgeBridgeRoutes(Db& db, std::string_view peerId,
                                       std::string_view networkId,
                                       std::string_view edgeNodeId,
                                       std::string_view actorId, service::common::UuidV7Generator& uuidGenerator) {
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

    using Op = ruvia::DbBinaryOperator;
    ruvia::DbQuery bridgeQuery;
    bridgeQuery.select({ bridgeQuery.column(service::vpn::persistence::EdgeNodeNetworkEntity::columnName<"name">()), bridgeQuery.column(service::vpn::persistence::EdgeNodeNetworkEntity::columnName<"device">()),
            bridgeQuery.column(service::vpn::persistence::EdgeNodeNetworkEntity::columnName<"ipv4">()), bridgeQuery.column(service::vpn::persistence::EdgeNodeNetworkEntity::columnName<"prefix_length">()) })
        .from(service::vpn::persistence::EdgeNodeNetworkEntity::tableName())
        .andWhere(bridgeQuery.binary(bridgeQuery.column(service::vpn::persistence::EdgeNodeNetworkEntity::columnName<"node_id">()), Op::kEqual,
            bridgeQuery.cast(bridgeQuery.value(edgeNodeId), ruvia::DbDataType::kUuid)))
        .andWhere(bridgeQuery.binary(bridgeQuery.column(service::vpn::persistence::EdgeNodeNetworkEntity::columnName<"is_bridge">()), Op::kEqual, bridgeQuery.value(true)))
        .andWhere(bridgeQuery.binary(bridgeQuery.coalesce({ bridgeQuery.column(service::vpn::persistence::EdgeNodeNetworkEntity::columnName<"ipv4">()), bridgeQuery.value("") }), Op::kNotEqual, bridgeQuery.value("")))
        .andWhere(bridgeQuery.between(bridgeQuery.column(service::vpn::persistence::EdgeNodeNetworkEntity::columnName<"prefix_length">()), bridgeQuery.value(1), bridgeQuery.value(30)))
        .addOrderBy(bridgeQuery.column(service::vpn::persistence::EdgeNodeNetworkEntity::columnName<"name">()));
    const auto bridgeRows = co_await db.query(bridgeQuery);
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

    ruvia::DbQuery currentQuery;
    currentQuery.select({ currentQuery.cast(currentQuery.column(service::vpn::persistence::VpnRouteEntity::columnName<"id">()), ruvia::DbDataType::kText),
            currentQuery.column(service::vpn::persistence::VpnRouteEntity::columnName<"lan_interface">()), currentQuery.column(service::vpn::persistence::VpnRouteEntity::columnName<"target_cidr">()), currentQuery.column(service::vpn::persistence::VpnRouteEntity::columnName<"virtual_cidr">()) })
        .from(service::vpn::persistence::VpnRouteEntity::tableName())
        .andWhere(currentQuery.binary(currentQuery.column(service::vpn::persistence::VpnRouteEntity::columnName<"edge_peer_id">()), Op::kEqual,
            currentQuery.cast(currentQuery.value(peerId), ruvia::DbDataType::kUuid)))
        .addOrderBy(currentQuery.column(service::vpn::persistence::VpnRouteEntity::columnName<"id">()));
    const auto currentRows = co_await db.query(currentQuery);
    std::vector<RouteRecord> current;
    current.reserve(currentRows.size());
    for (const auto& row : currentRows)
        current.push_back(RouteRecord{
            route_sync_detail::rowValue(row, 0), route_sync_detail::rowValue(row, 1),
            parseCidr(route_sync_detail::rowValue(row, 2), 1, 30),
            parseCidr(route_sync_detail::rowValue(row, 3), 1, 30)});

    ruvia::DbQuery allRoutesQuery;
    allRoutesQuery.select({ allRoutesQuery.cast(allRoutesQuery.column(service::vpn::persistence::VpnRouteEntity::columnName<"id">()), ruvia::DbDataType::kText),
        allRoutesQuery.column(service::vpn::persistence::VpnRouteEntity::columnName<"lan_interface">()), allRoutesQuery.column(service::vpn::persistence::VpnRouteEntity::columnName<"target_cidr">()), allRoutesQuery.column(service::vpn::persistence::VpnRouteEntity::columnName<"virtual_cidr">()) }).from(service::vpn::persistence::VpnRouteEntity::tableName());
    const auto allRows = co_await db.query(allRoutesQuery);
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
            ruvia::DbQuery route;
            route.update(service::vpn::persistence::VpnRouteEntity::tableName())
                .set(service::vpn::persistence::VpnRouteEntity::columnName<"network_id">(), route.cast(route.value(networkId), ruvia::DbDataType::kUuid))
                .set(service::vpn::persistence::VpnRouteEntity::columnName<"lan_interface">(), route.value(bridge.lanInterface)).set(service::vpn::persistence::VpnRouteEntity::columnName<"target_cidr">(), route.value(targetCidr))
                .set(service::vpn::persistence::VpnRouteEntity::columnName<"virtual_cidr">(), route.value(virtualCidr)).set(service::vpn::persistence::VpnRouteEntity::columnName<"mode">(), route.value("nat"))
                .set(service::vpn::persistence::VpnRouteEntity::columnName<"nat_mode">(), route.value("masquerade")).set(service::vpn::persistence::VpnRouteEntity::columnName<"enabled">(), route.value(true))
                .set(service::vpn::persistence::VpnRouteEntity::columnName<"status">(), route.value("active")).set(service::vpn::persistence::VpnRouteEntity::columnName<"last_error">(), route.value(""))
                .set(service::vpn::persistence::VpnRouteEntity::columnName<"updated_at">(), route.call("now"))
                .andWhere(route.binary(route.column(service::vpn::persistence::VpnRouteEntity::columnName<"id">()), Op::kEqual, route.cast(route.value(routeId), ruvia::DbDataType::kUuid)));
            (void)co_await db.execute(route);
            for (auto& route : allRoutes)
                if (route.id == routeId) {
                    route.lanInterface = bridge.lanInterface;
                    route.target = bridge.target;
                    route.virtualNetwork = virtualNetwork;
                }
        } else {
            const auto id = uuidGenerator.next();
            ruvia::DbQuery route;
            route.insertInto(service::vpn::persistence::VpnRouteEntity::tableName(), { "id", "network_id", "edge_peer_id", "lan_interface", "target_cidr",
                "virtual_cidr", "mode", "nat_mode", "enabled", "status", "created_by" })
                .values({ route.cast(route.value(id), ruvia::DbDataType::kUuid),
                    route.cast(route.value(networkId), ruvia::DbDataType::kUuid),
                    route.cast(route.value(peerId), ruvia::DbDataType::kUuid), route.value(bridge.lanInterface),
                    route.value(targetCidr), route.value(virtualCidr), route.value("nat"), route.value("masquerade"),
                    route.value(true), route.value("active"), route.cast(route.value(actorId), ruvia::DbDataType::kUuid) });
            (void)co_await db.execute(route);
            allRoutes.push_back(
                RouteRecord{id, bridge.lanInterface, bridge.target, virtualNetwork});
        }
    }
}

} // namespace feature

// Queue a complete, versioned VPN configuration for an Edge peer. The actor is
// optional for background projection: in that case the VPN network owner is
// used so edge_task.created_by remains a valid audit subject.
template <typename Context>
ruvia::Task<void> queueEdgeConfig(Context& c, std::string_view peerId,
                                  std::string_view actorId = {},
                                  std::string_view platformId = {}) {
    using Op = ruvia::DbBinaryOperator;
    ruvia::DbQuery peer;
    const auto capability = peer.call("lower", { peer.coalesce({ peer.binary(
        peer.binary(peer.column(service::vpn::persistence::EdgeNodeEntity::columnName<"capability">(), "e"), Op::kJsonGet, peer.value("vpn")),
        Op::kJsonGetText, peer.value("supportsVpn")), peer.value("") }) });
    peer.select({ peer.column(service::vpn::persistence::VpnPeerEntity::columnName<"id">(), "p"), peer.column(service::vpn::persistence::VpnPeerEntity::columnName<"network_id">(), "p"), peer.column(service::vpn::persistence::VpnPeerEntity::columnName<"edge_node_id">(), "p"),
            peer.call("host", { peer.column(service::vpn::persistence::VpnPeerEntity::columnName<"assigned_ipv4">(), "p") }), peer.column(service::vpn::persistence::VpnPeerEntity::columnName<"config_revision">(), "p"),
            peer.column(service::vpn::persistence::VpnPeerEntity::columnName<"status">(), "p"), peer.column(service::vpn::persistence::VpnNetworkEntity::columnName<"hub_public_key">(), "n"), peer.column(service::vpn::persistence::VpnNetworkEntity::columnName<"hub_endpoint">(), "n"),
            peer.column(service::vpn::persistence::VpnNetworkEntity::columnName<"hub_listen_port">(), "n"), peer.cast(peer.column(service::vpn::persistence::VpnNetworkEntity::columnName<"created_by">(), "n"), ruvia::DbDataType::kText), peer.column(service::vpn::persistence::VpnNetworkEntity::columnName<"status">(), "n") })
        .from(service::vpn::persistence::VpnPeerEntity::tableName(), "p")
        .join(ruvia::DbJoinType::kInner, service::vpn::persistence::VpnNetworkEntity::tableName(), peer.binary(peer.column(service::vpn::persistence::VpnNetworkEntity::columnName<"id">(), "n"), Op::kEqual, peer.column(service::vpn::persistence::VpnPeerEntity::columnName<"network_id">(), "p")), "n")
        .join(ruvia::DbJoinType::kInner, service::vpn::persistence::EdgeNodeEntity::tableName(), peer.binary(peer.column(service::vpn::persistence::EdgeNodeEntity::columnName<"id">(), "e"), Op::kEqual, peer.column(service::vpn::persistence::VpnPeerEntity::columnName<"edge_node_id">(), "p")), "e")
        .andWhere(peer.binary(peer.column(service::vpn::persistence::VpnPeerEntity::columnName<"id">(), "p"), Op::kEqual, peer.cast(peer.value(peerId), ruvia::DbDataType::kUuid)))
        .andWhere(peer.binary(peer.column(service::vpn::persistence::VpnPeerEntity::columnName<"peer_type">(), "p"), Op::kEqual, peer.value("edge")))
        .andWhere(peer.binary(peer.column(service::vpn::persistence::EdgeNodeEntity::columnName<"enrollment_status">(), "e"), Op::kEqual, peer.value("approved")))
        .andWhere(peer.binary(capability, Op::kIn, peer.list({ peer.value("true"), peer.value("t"), peer.value("1") })))
        .limit(1);
    const auto rows = co_await c.db().query(peer);
    if (rows.empty())
        co_return;

    const auto nodeId = detail::edgeConfigRowValue(rows.front(), 2);
    ruvia::DbQuery routes;
    routes.select({ routes.cast(routes.column(service::vpn::persistence::VpnRouteEntity::columnName<"id">()), ruvia::DbDataType::kText), routes.column(service::vpn::persistence::VpnRouteEntity::columnName<"virtual_cidr">()),
            routes.column(service::vpn::persistence::VpnRouteEntity::columnName<"target_cidr">()), routes.column(service::vpn::persistence::VpnRouteEntity::columnName<"mode">()), routes.column(service::vpn::persistence::VpnRouteEntity::columnName<"nat_mode">()), routes.column(service::vpn::persistence::VpnRouteEntity::columnName<"enabled">()) })
        .from(service::vpn::persistence::VpnRouteEntity::tableName())
        .andWhere(routes.binary(routes.column(service::vpn::persistence::VpnRouteEntity::columnName<"edge_peer_id">()), Op::kEqual, routes.cast(routes.value(peerId), ruvia::DbDataType::kUuid)))
        .addOrderBy(routes.column(service::vpn::persistence::VpnRouteEntity::columnName<"virtual_cidr">())).limit(16);
    const auto routeRows = co_await c.db().query(routes);
    const auto requestId = c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next();
    std::uint8_t requestBytes[16]{};
    if (!service::edge::protocol::uuidBytes(requestId, requestBytes))
        co_return;
    auto envelope = service::edge::protocol::outbound(c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next(), service::message::utcNowMilliseconds(), c.template workerState<service::edge::config::PlatformIdentity>().id, nodeId);
    if (!platformId.empty()) {
        std::uint8_t platformBytes[16]{};
        if (service::edge::protocol::uuidBytes(platformId, platformBytes))
            envelope.set_platform_id(
                service::edge::protocol::bytes(platformBytes, sizeof(platformBytes)));
    }
    auto* request = envelope.mutable_vpn_config_request();
    request->set_request_id(service::edge::protocol::bytes(requestBytes, sizeof(requestBytes)));
    const auto revisionText = detail::edgeConfigRowValue(rows.front(), 4);
    const auto nextVersion = service::utils::parseInt64(
                                 std::optional<std::string_view>(revisionText))
                                 .value_or(0) +
                             1;
    request->set_config_version(static_cast<std::uint64_t>(nextVersion));
    const auto revoked = detail::edgeConfigRowValue(rows.front(), 5) == "revoked";
    const auto networkDisabled = detail::edgeConfigRowValue(rows.front(), 10) != "enabled";
    request->set_enabled(!revoked && !networkDisabled);
    request->set_hub_public_key(detail::edgeConfigRowValue(rows.front(), 6));
    request->set_hub_endpoint(detail::edgeConfigRowValue(rows.front(), 7));
    const auto portText = detail::edgeConfigRowValue(rows.front(), 8);
    request->set_hub_listen_port(static_cast<std::uint32_t>(
        service::utils::parseInt64(std::optional<std::string_view>(portText))
            .value_or(51820)));
    request->set_edge_address(detail::edgeConfigRowValue(rows.front(), 3) + "/32");
    if (request->enabled()) {
        for (const auto& row : routeRows) {
            auto* route = request->add_routes();
            route->set_route_id(detail::edgeConfigRowValue(row, 0));
            route->set_virtual_cidr(detail::edgeConfigRowValue(row, 1));
            route->set_target_cidr(detail::edgeConfigRowValue(row, 2));
            route->set_mode(detail::edgeConfigRowValue(row, 3));
            route->set_nat_mode(detail::edgeConfigRowValue(row, 4));
            route->set_enabled(detail::edgeConfigRowValue(row, 5) == "t");
        }
    }
    const auto wire = service::edge::protocol::encode(envelope);
    if (wire.empty())
        co_return;
    const auto createdBy = actorId.empty()
                               ? detail::edgeConfigRowValue(rows.front(), 9)
                               : std::string(actorId);
    if (createdBy.empty())
        co_return;
    ruvia::DbQuery superseded;
    superseded.update(service::vpn::persistence::EdgeTaskEntity::tableName()).set(service::vpn::persistence::EdgeTaskEntity::columnName<"status">(), superseded.value("failed"))
        .set(service::vpn::persistence::EdgeTaskEntity::columnName<"result">(), superseded.call("jsonb_build_object", {
            superseded.cast(superseded.value("configVersion"), ruvia::DbDataType::kText), superseded.binary(superseded.column(service::vpn::persistence::EdgeTaskEntity::columnName<"request">()), Op::kJsonGetText, superseded.cast(superseded.value("configVersion"), ruvia::DbDataType::kText)),
            superseded.cast(superseded.value("errorCode"), ruvia::DbDataType::kText), superseded.cast(superseded.value("superseded"), ruvia::DbDataType::kText),
            superseded.cast(superseded.value("errorMessage"), ruvia::DbDataType::kText), superseded.cast(superseded.value("superseded by newer VPN configuration"), ruvia::DbDataType::kText) }))
        .set(service::vpn::persistence::EdgeTaskEntity::columnName<"updated_at">(), superseded.call("now")).set(service::vpn::persistence::EdgeTaskEntity::columnName<"completed_at">(), superseded.call("now"))
        .andWhere(superseded.binary(superseded.column(service::vpn::persistence::EdgeTaskEntity::columnName<"node_id">()), Op::kEqual, superseded.cast(superseded.value(nodeId), ruvia::DbDataType::kUuid)))
        .andWhere(superseded.binary(superseded.column(service::vpn::persistence::EdgeTaskEntity::columnName<"task_type">()), Op::kEqual, superseded.value("vpn")))
        .andWhere(superseded.binary(superseded.column(service::vpn::persistence::EdgeTaskEntity::columnName<"status">()), Op::kNotIn, superseded.list({ superseded.value("succeeded"), superseded.value("failed") })))
        .andWhere(superseded.binary(superseded.binary(superseded.column(service::vpn::persistence::EdgeTaskEntity::columnName<"request">()), Op::kJsonGetText, superseded.value("peerId")),
            Op::kEqual, superseded.cast(superseded.value(peerId), ruvia::DbDataType::kText)));
    ruvia::DbQuery task;
    task.with("superseded", superseded)
        .insertInto(service::vpn::persistence::EdgeTaskEntity::tableName(), { "id", "node_id", "task_type", "request", "created_by" })
        .values({ task.cast(task.value(requestId), ruvia::DbDataType::kUuid), task.cast(task.value(nodeId), ruvia::DbDataType::kUuid),
            task.value("vpn"), task.call("jsonb_build_object", {
                task.cast(task.value("peerId"), ruvia::DbDataType::kText), task.cast(task.value(peerId), ruvia::DbDataType::kText),
                task.cast(task.value("configVersion"), ruvia::DbDataType::kText), task.cast(task.value(nextVersion), ruvia::DbDataType::kBigInt),
                task.cast(task.value("enabled"), ruvia::DbDataType::kText), task.cast(task.value(request->enabled()), ruvia::DbDataType::kBoolean) }),
            task.cast(task.value(createdBy), ruvia::DbDataType::kUuid) });
    (void)co_await c.db().execute(task);
    co_await service::edge::dispatch::enqueue(c.redis(), nodeId, wire);
    ruvia::DbQuery revision;
    revision.update(service::vpn::persistence::VpnPeerEntity::tableName()).set(service::vpn::persistence::VpnPeerEntity::columnName<"config_revision">(), revision.value(nextVersion)).set(service::vpn::persistence::VpnPeerEntity::columnName<"updated_at">(), revision.call("now"))
        .andWhere(revision.binary(revision.column(service::vpn::persistence::VpnPeerEntity::columnName<"id">()), Op::kEqual, revision.cast(revision.value(peerId), ruvia::DbDataType::kUuid)));
    (void)co_await c.db().execute(revision);
}

} // namespace service::vpn

#include <array>
#include <memory>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include "service/features/vpn/wireguard/wireguard.transport.h"

namespace service::vpn::hub_config {

inline constexpr std::string_view kDefaultNetworkId{
    "00000000-0000-7000-8000-000000000004"};
inline constexpr std::string_view kDefaultNetworkName{"iot-server"};

inline std::string rowValue(const auto& row, std::size_t index) {
    return std::string(row[index].value().value_or(std::string_view{}));
}

inline int base64Value(char value) noexcept {
    if (value >= 'A' && value <= 'Z')
        return value - 'A';
    if (value >= 'a' && value <= 'z')
        return value - 'a' + 26;
    if (value >= '0' && value <= '9')
        return value - '0' + 52;
    if (value == '+')
        return 62;
    if (value == '/')
        return 63;
    return -1;
}

inline bool decodeKey(std::string_view input, std::array<unsigned char, 32>& output) noexcept {
    if (input.size() != 44 || input.back() != '=')
        return false;
    std::size_t outputIndex = 0;
    for (std::size_t index = 0; index < input.size(); index += 4) {
        const auto first = base64Value(input[index]);
        const auto second = base64Value(input[index + 1]);
        const auto third = input[index + 2] == '=' ? 0 : base64Value(input[index + 2]);
        const auto fourth = input[index + 3] == '=' ? 0 : base64Value(input[index + 3]);
        if (first < 0 || second < 0 || third < 0 || fourth < 0 ||
            (index + 2 == input.size() - 1 && input[index + 2] != '=') ||
            (index + 3 == input.size() - 1 && input[index + 3] != '='))
            return false;
        if (outputIndex < output.size())
            output[outputIndex++] = static_cast<unsigned char>((first << 2) | (second >> 4));
        if (index + 2 < input.size() - 1 && outputIndex < output.size())
            output[outputIndex++] = static_cast<unsigned char>((second << 4) | (third >> 2));
        if (index + 3 < input.size() - 1 && outputIndex < output.size())
            output[outputIndex++] = static_cast<unsigned char>((third << 6) | fourth);
    }
    return outputIndex == output.size();
}

inline std::string encodeKey(const std::array<unsigned char, 32>& input) {
    constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.resize(44, '=');
    std::size_t out = 0;
    for (std::size_t index = 0; index < input.size(); index += 3) {
        const auto left = input.size() - index;
        const auto value = (static_cast<unsigned>(input[index]) << 16) |
                           (left > 1 ? static_cast<unsigned>(input[index + 1]) << 8 : 0U) |
                           (left > 2 ? static_cast<unsigned>(input[index + 2]) : 0U);
        output[out++] = alphabet[(value >> 18) & 0x3fU];
        output[out++] = alphabet[(value >> 12) & 0x3fU];
        if (left > 1)
            output[out++] = alphabet[(value >> 6) & 0x3fU];
        if (left > 2)
            output[out++] = alphabet[value & 0x3fU];
    }
    return output;
}

inline bool derivePublicKey(std::string_view privateKey, std::string& publicKey) {
    std::array<unsigned char, 32> privateBytes{};
    if (!decodeKey(privateKey, privateBytes))
        return false;
    using KeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    KeyPtr key(EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, privateBytes.data(),
                                            privateBytes.size()),
              &EVP_PKEY_free);
    if (!key)
        return false;
    std::array<unsigned char, 32> publicBytes{};
    std::size_t publicSize = publicBytes.size();
    if (EVP_PKEY_get_raw_public_key(key.get(), publicBytes.data(), &publicSize) != 1 ||
        publicSize != publicBytes.size())
        return false;
    publicKey = encodeKey(publicBytes);
    return true;
}

inline bool generateKeyPair(std::string& privateKey, std::string& publicKey) {
    std::array<unsigned char, 32> privateBytes{};
    if (RAND_bytes(privateBytes.data(), static_cast<int>(privateBytes.size())) != 1)
        return false;
    privateKey = encodeKey(privateBytes);
    return derivePublicKey(privateKey, publicKey);
}

template <typename Context>
ruvia::Task<std::optional<wireguard::HubConfig>> loadOrInitialize(
    Context& context, const wireguard::HubConfig& fallback) {
    auto transaction = co_await context.db().beginTransaction();
    ruvia::DbQuery lock;
    lock.select(lock.call("pg_advisory_xact_lock", {
        lock.cast(lock.value(std::int64_t{5282804697543808067}), ruvia::DbDataType::kBigInt) }));
    (void)co_await transaction.query(lock);
    ruvia::DbQuery network;
    network.select({ network.column(service::vpn::persistence::VpnNetworkEntity::columnName<"hub_private_key">()), network.column(service::vpn::persistence::VpnNetworkEntity::columnName<"hub_public_key">()),
            network.column(service::vpn::persistence::VpnNetworkEntity::columnName<"hub_endpoint">()), network.column(service::vpn::persistence::VpnNetworkEntity::columnName<"hub_listen_port">()) })
        .from(service::vpn::persistence::VpnNetworkEntity::tableName())
        .andWhere(network.binary(network.column(service::vpn::persistence::VpnNetworkEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual,
            network.cast(network.value(kDefaultNetworkId), ruvia::DbDataType::kUuid)))
        .andWhere(network.binary(network.column(service::vpn::persistence::VpnNetworkEntity::columnName<"name">()), ruvia::DbBinaryOperator::kEqual, network.value(kDefaultNetworkName)))
        .andWhere(network.binary(network.column(service::vpn::persistence::VpnNetworkEntity::columnName<"status">()), ruvia::DbBinaryOperator::kEqual, network.value("enabled")))
        .andWhere(network.unary(ruvia::DbUnaryOperator::kIsNull, network.column(service::vpn::persistence::VpnNetworkEntity::columnName<"deleted_at">())))
        .limit(1).lock({ .mode = ruvia::DbRowLock::kUpdate });
    const auto rows = co_await transaction.query(network);
    if (rows.empty()) {
        co_await transaction.commit();
        if (!wireguard::validKey(fallback.privateKey))
            co_return std::nullopt;
        co_return fallback;
    }

    auto config = fallback;
    const auto storedPrivateKey = rowValue(rows.front(), 0);
    const auto storedPublicKey = rowValue(rows.front(), 1);
    const auto storedEndpoint = rowValue(rows.front(), 2);
    const auto storedPortText = rowValue(rows.front(), 3);
    const auto storedPort = service::utils::parseInt64(
        std::optional<std::string_view>(storedPortText));
    config.endpoint = storedEndpoint.empty() ? fallback.endpoint : storedEndpoint;
    config.listenPort = storedPort && *storedPort > 0 && *storedPort <= 65535
                            ? static_cast<std::uint16_t>(*storedPort)
                            : fallback.listenPort;

    bool persist = false;
    if (storedPrivateKey.empty()) {
        if (wireguard::validKey(fallback.privateKey) &&
            derivePublicKey(fallback.privateKey, config.publicKey)) {
            config.privateKey = fallback.privateKey;
        } else if (!generateKeyPair(config.privateKey, config.publicKey)) {
            co_return std::nullopt;
        }
        persist = true;
    } else {
        if (!wireguard::validKey(storedPrivateKey) ||
            !derivePublicKey(storedPrivateKey, config.publicKey))
            co_return std::nullopt;
        config.privateKey = storedPrivateKey;
        persist = storedPublicKey != config.publicKey;
    }
    if (storedEndpoint.empty() && !fallback.endpoint.empty())
        persist = true;
    if (!storedPort || *storedPort != config.listenPort)
        persist = true;

    if (persist) {
        ruvia::DbQuery update;
        update.update(service::vpn::persistence::VpnNetworkEntity::tableName()).set(service::vpn::persistence::VpnNetworkEntity::columnName<"hub_private_key">(), update.value(config.privateKey))
            .set(service::vpn::persistence::VpnNetworkEntity::columnName<"hub_public_key">(), update.value(config.publicKey)).set(service::vpn::persistence::VpnNetworkEntity::columnName<"hub_endpoint">(), update.value(config.endpoint))
            .set(service::vpn::persistence::VpnNetworkEntity::columnName<"hub_listen_port">(), update.value(static_cast<int>(config.listenPort)))
            .set(service::vpn::persistence::VpnNetworkEntity::columnName<"updated_at">(), update.call("now"))
            .andWhere(update.binary(update.column(service::vpn::persistence::VpnNetworkEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual,
                update.cast(update.value(kDefaultNetworkId), ruvia::DbDataType::kUuid)));
        (void)co_await transaction.execute(update);
    }
    co_await transaction.commit();
    co_return config;
}

} // namespace service::vpn::hub_config

namespace service::vpn {

class VpnHubService final {
  public:
    static ruvia::Task<wireguard::RuntimeStatus> status(
        ruvia::WebWorkerContext& context,
        const wireguard::HubConfig& fallback
    ) {
        const auto config = co_await hub_config::loadOrInitialize(context, fallback);
        if (!config) {
            co_return wireguard::RuntimeStatus{
                .supported = true,
                .configured = false,
                .code = "hub_config_missing",
                .message = "WireGuard Hub 配置尚未初始化",
                .peerCount = 0
            };
        }
        co_return wireguard::controller().status(*config);
    }

    static ruvia::Task<wireguard::RuntimeStatus> reconcile(
        ruvia::WebWorkerContext& context,
        const wireguard::HubConfig& fallback,
        bool background = false
    ) {
        // The interface is an external singleton. All workers run the same job,
        // but only the transaction owner applies a particular reconciliation.
        // A separate worker-local connection keeps the lock while business
        // queries use their ordinary connection, without pool self-deadlock.
        auto ownership = co_await context.db("vpn-coordination").beginTransaction();
        if (!(co_await VpnRuntimeService::acquireReconciliation(ownership, context.pool(), background))) {
            co_return wireguard::RuntimeStatus{ .code = "reconciliation_in_progress" };
        }
        const auto instance = service::runtime::instanceId();
        auto schedules = context.redis().getRepository<ReconciliationSchedule>();
        if (background) {
            const ruvia::DbFindOptions options{
                .where = ReconciliationSchedule::column<"id">() == instance};
            if (co_await schedules.exists(options)) {
                co_return wireguard::RuntimeStatus{ .code = "reconciliation_current" };
            }
        }
        // Keep address allocation behind the complete kernel reconciliation.
        // Otherwise an older snapshot could install a revoked key after its
        // address has already been returned to a newly enrolled client.
        co_await VpnRuntimeService::lockAddressAllocation(ownership, context.pool());
        auto result = co_await reconcileLocal(context, fallback);
        if (result.configured) {
            ReconciliationSchedule schedule(context.pool());
            schedule.set<"id">(instance);
            const ruvia::RedisWriteOptions expiration{.ttl = std::chrono::seconds(10)};
            (void)co_await schedules.upsert(schedule, expiration);
        }
        co_await ownership.commit();
        co_return result;
    }

    static ruvia::Task<void> removePeer(ruvia::WebWorkerContext& context,
        const wireguard::HubConfig& fallback, std::string_view payload) {
            if (!wireguard::validKey(payload)) {
                service::common::fail(10002, "WireGuard Peer 公钥无效", 400);
            }
            auto ownership = co_await context.db("vpn-coordination").beginTransaction();
            (void)co_await VpnRuntimeService::acquireReconciliation(ownership, context.pool(), false);
            const auto config = co_await hub_config::loadOrInitialize(context, fallback);
            if (config) {
                const auto result = wireguard::controller().removePeer(*config, payload);
                if (result.supported && !result.configured) {
                    service::common::fail(21005, "VPN Hub peer removal failed: " + result.message, 503);
                }
            }
            co_await ownership.commit();
    }

  private:
    static ruvia::Task<wireguard::RuntimeStatus> reconcileLocal(
        ruvia::WebWorkerContext& context,
        const wireguard::HubConfig& fallback
    ) {
        const auto config = co_await hub_config::loadOrInitialize(context, fallback);
        if (!config) {
            co_return wireguard::RuntimeStatus{
                .supported = true,
                .configured = false,
                .code = "hub_config_missing",
                .message = "WireGuard Hub 配置尚未初始化",
                .peerCount = 0
            };
        }
        auto& controller = wireguard::controller();
        auto result = controller.configure(*config);
        if (!result.configured) {
            co_return result;
        }
        const auto peers = co_await VpnRuntimeService::loadActivePeers(context);
        // Remove obsolete keys before assigning a recycled address to a new peer.
        // A failed inventory or removal must not grant access to a stale key.
        const auto currentPeers = controller.peerKeys(*config);
        if (!currentPeers) {
            co_return wireguard::RuntimeStatus{
                .supported = true,
                .configured = false,
                .code = "peer_inventory_failed",
                .message = "Unable to read WireGuard peers before reconciliation"
            };
        }
        std::unordered_set<std::string> expected;
        for (const auto& peerRecord : peers) {
            if (wireguard::validKey(peerRecord.publicKey) && parseIpv4(peerRecord.assignedIpv4)) {
                expected.insert(peerRecord.publicKey);
            }
        }
        for (const auto& publicKey : *currentPeers) {
            if (!expected.contains(publicKey)) {
                const auto removed = controller.removePeer(*config, publicKey);
                if (!removed.configured) {
                    co_return removed;
                }
            }
        }
        std::vector<firewall::ClientAccess> clients;
        std::vector<wireguard::Peer> configured;
        std::vector<std::string> expectedRoutes;
        std::size_t configuredPeers = 0;
        for (const auto& peerRecord : peers) {
            const auto& publicKey = peerRecord.publicKey;
            const auto& assigned = peerRecord.assignedIpv4;
            const auto& peerType = peerRecord.peerType;
            if (!wireguard::validKey(publicKey) || !parseIpv4(assigned)) {
                continue;
            }
            wireguard::Peer peer;
            peer.publicKey = publicKey;
            peer.allowedIps.emplace_back(assigned + "/32");
            if (peerType == "edge") {
                firewall::ClientAccess client{ .assignedIpv4 = assigned };
                std::stringstream routes(peerRecord.sourceRoutes);
                std::string route;
                while (std::getline(routes, route, ',')) {
                    if (!route.empty() && route.front() == ' ') {
                        route.erase(route.begin());
                    }
                    if (!route.empty()) {
                        peer.allowedIps.push_back(route);
                        client.sourceRoutes.push_back(std::move(route));
                    }
                }
                std::stringstream allowedRoutes(peerRecord.allowedRoutes);
                while (std::getline(allowedRoutes, route, ',')) {
                    if (!route.empty() && route.front() == ' ') {
                        route.erase(route.begin());
                    }
                    if (!route.empty()) {
                        client.allowedRoutes.push_back(std::move(route));
                    }
                }
                clients.push_back(std::move(client));
            } else if (peerType == "windows") {
                firewall::ClientAccess client{ .assignedIpv4 = assigned };
                std::stringstream routes(peerRecord.allowedRoutes);
                std::string route;
                while (std::getline(routes, route, ',')) {
                    if (!route.empty() && route.front() == ' ') {
                        route.erase(route.begin());
                    }
                    if (!route.empty()) {
                        client.allowedRoutes.push_back(std::move(route));
                    }
                }
                std::stringstream edgeAddresses(peerRecord.edgeAddresses);
                std::string edgeAddress;
                while (std::getline(edgeAddresses, edgeAddress, ',')) {
                    if (!edgeAddress.empty() && edgeAddress.front() == ' ') {
                        edgeAddress.erase(edgeAddress.begin());
                    }
                    if (!edgeAddress.empty()) {
                        client.edgeAddresses.push_back(std::move(edgeAddress));
                    }
                }
                clients.push_back(std::move(client));
            }
            expectedRoutes.insert(expectedRoutes.end(), peer.allowedIps.begin(), peer.allowedIps.end());
            configured.push_back(std::move(peer));
        }

        const auto firewallResult = firewall::apply(config->interfaceName, clients);
        if (!firewallResult.configured) {
            co_return wireguard::RuntimeStatus{
                .supported = true,
                .configured = false,
                .code = "firewall_configure_failed",
                .message = firewallResult.message,
                .peerCount = configuredPeers
            };
        }
        // Install the new address permissions before enabling its new key.
        // Recycled addresses must never inherit a previous client's firewall access.
        for (const auto& peer : configured) {
            const auto peerResult = controller.upsertPeer(*config, peer);
            if (!peerResult.configured) {
                co_return peerResult;
            }
            ++configuredPeers;
        }
        const auto routeResult = controller.reconcileRoutes(*config, expectedRoutes);
        if (!routeResult.configured) {
            co_return routeResult;
        }
        if (const auto handshakes = controller.peerHandshakes(*config)) {
            for (const auto& [publicKey, seconds] : *handshakes) {
                if (seconds == 0 || seconds > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                    continue;
                }
                co_await VpnRuntimeService::updatePeerHandshake(
                    context,
                    publicKey,
                    static_cast<std::int64_t>(seconds)
                );
            }
        }
        result.peerCount = configuredPeers;
        result.message = "WireGuard hub is configured";
        co_return result;
    }

};

} // namespace service::vpn

namespace service::vpn {

class VpnControlService final {
  public:
    explicit VpnControlService(
        wireguard::HubConfig fallback,
        std::string platformId =
            std::string(service::edge::protocol::kDefaultPlatformId)
    )
        : fallback_(std::move(fallback)), platformId_(std::move(platformId)) {}

    ruvia::Task<std::string> executeOperation(ruvia::WebWorkerContext& context, std::string_view operation, std::string_view payload, ruvia::StopToken stop) const {
        if (stop.stopRequested()) {
            service::common::fail(10004, "VPN background operation cancelled", 503);
        }

        if (operation == "queue-edge-config") {
            const auto separator = payload.find('\n');
            if (separator == std::string_view::npos || separator == 0 ||
                separator + 1 >= payload.size() ||
                !service::common::isUuid(payload.substr(0, separator)) ||
                !service::common::isUuid(payload.substr(separator + 1))) {
                service::common::fail(10002, "VPN Edge 配置请求无效", 400);
            }
            co_await queueEdgeConfig(context, payload.substr(0, separator), payload.substr(separator + 1), platformId_);
            co_return "{}";
        }

        if (operation == "reconcile" || operation == "wireguard-reconcile" ||
            operation == "firewall-reconcile") {
            const auto result = co_await VpnHubService::reconcile(context, fallback_);
            if (operation == "reconcile" && result.supported && !result.configured &&
                result.code != "hub_config_missing") {
                service::common::fail(21005, "VPN Hub reconciliation failed: " + result.message, 503);
            }
            co_return runtimeStatusJson(result);
        }

        if (operation == "wireguard-status") {
            const auto result = co_await VpnHubService::status(context, fallback_);
            co_return runtimeStatusJson(result);
        }

        if (operation == "wireguard-remove-peer") {
            co_await VpnHubService::removePeer(context, fallback_, payload);
            co_return "{}";
        }

        service::common::fail(10002, "Unknown VPN background operation", 400);
    }

  private:
    static std::string runtimeStatusJson(const wireguard::RuntimeStatus& result) {
    return "{\"platformSupported\":" +
        std::string(result.supported ? "true" : "false") +
        ",\"configured\":" + std::string(result.configured ? "true" : "false") +
        ",\"code\":" + service::utils::jsonQuoted(result.code) +
        ",\"message\":" + service::utils::jsonQuoted(result.message) +
        ",\"runtimePeerCount\":" + std::to_string(result.peerCount) + "}";
}

    const wireguard::HubConfig fallback_;
    const std::string platformId_;
};

} // namespace service::vpn
