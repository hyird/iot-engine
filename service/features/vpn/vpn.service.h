#pragma once

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
#include "service/features/edge/edge.transport.h"
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
    config += "\nPersistentKeepalive = 25\n";
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
        const std::vector<ruvia::DbOrderTerm> routeOrder{ { routes.column("virtual_cidr", "r") } };
        routes.select(routes.aggregate("string_agg", { routes.column("virtual_cidr", "r"), routes.value(", ") }, false, routeOrder))
            .from("vpn_route", "r")
            .andWhere(routes.binary(routes.column("edge_peer_id", "r"), Op::kEqual, routes.column("id", "p")))
            .andWhere(routes.column("enabled", "r"));
        ruvia::DbQuery allowed;
        const std::vector<ruvia::DbOrderTerm> allowedOrder{ { allowed.column("virtual_cidr", "access") } };
        allowed.select(allowed.aggregate("string_agg", { allowed.column("virtual_cidr", "access"), allowed.value(", ") }, false, allowedOrder))
            .from("vpn_effective_route_access", "access")
            .andWhere(allowed.binary(allowed.column("peer_id", "access"), Op::kEqual, allowed.column("id", "p")));
        ruvia::DbQuery addresses;
        const std::vector<ruvia::DbOrderTerm> addressOrder{ { addresses.column("edge_address", "access") } };
        addresses.select(addresses.aggregate("string_agg", { addresses.column("edge_address", "access"), addresses.value(", ") }, false, addressOrder))
            .from("vpn_effective_edge_access", "access")
            .andWhere(addresses.binary(addresses.column("peer_id", "access"), Op::kEqual, addresses.column("id", "p")));
        ruvia::DbQuery peersQuery;
        peersQuery.select(peersQuery.column("public_key", "p"))
            .addSelect(peersQuery.call("host", { peersQuery.column("assigned_ipv4", "p") }))
            .addSelect(peersQuery.column("peer_type", "p"))
            .addSelect(peersQuery.coalesce({ peersQuery.subquery(routes), peersQuery.value("") }))
            .addSelect(peersQuery.coalesce({ peersQuery.subquery(allowed), peersQuery.value("") }))
            .addSelect(peersQuery.coalesce({ peersQuery.subquery(addresses), peersQuery.value("") }))
            .from("vpn_peer", "p")
            .join(ruvia::DbJoinType::kInner, "vpn_network",
                peersQuery.binary(peersQuery.column("id", "n"), Op::kEqual, peersQuery.column("network_id", "p")), "n")
            .andWhere(peersQuery.binary(peersQuery.column("status", "p"), Op::kEqual, peersQuery.value("active")))
            .andWhere(peersQuery.binary(peersQuery.column("status", "n"), Op::kEqual, peersQuery.value("enabled")))
            .andWhere(peersQuery.unary(ruvia::DbUnaryOperator::kIsNull, peersQuery.column("deleted_at", "n")))
            .andWhere(peersQuery.binary(peersQuery.column("public_key", "p"), Op::kNotEqual, peersQuery.value("")))
            .andWhere(peersQuery.binary(peersQuery.unary(ruvia::DbUnaryOperator::kNot, peersQuery.column("client_managed", "p")),
                Op::kOr, peersQuery.call("vpn_desktop_user_authorized", { peersQuery.column("user_id", "p") })))
            .addOrderBy(peersQuery.column("id", "p"));
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
        handshake.update("vpn_peer").set("last_handshake_at", timestamp).set("updated_at", handshake.call("now"))
            .andWhere(handshake.binary(handshake.column("public_key"), ruvia::DbBinaryOperator::kEqual, handshake.value(publicKey)))
            .andWhere(handshake.binary(handshake.column("status"), ruvia::DbBinaryOperator::kEqual, handshake.value("active")))
            .andWhere(handshake.binary(handshake.unary(ruvia::DbUnaryOperator::kIsNull, handshake.column("last_handshake_at")),
                ruvia::DbBinaryOperator::kOr, handshake.binary(handshake.column("last_handshake_at"), ruvia::DbBinaryOperator::kLess, timestamp)));
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
    return service::common::parseInt64(
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

    using Op = ruvia::DbBinaryOperator;
    ruvia::DbQuery bridgeQuery;
    bridgeQuery.select({ bridgeQuery.column("name"), bridgeQuery.column("device"),
            bridgeQuery.column("ipv4"), bridgeQuery.column("prefix_length") })
        .from("edge_node_network")
        .andWhere(bridgeQuery.binary(bridgeQuery.column("node_id"), Op::kEqual,
            bridgeQuery.cast(bridgeQuery.value(edgeNodeId), ruvia::DbDataType::kUuid)))
        .andWhere(bridgeQuery.binary(bridgeQuery.column("is_bridge"), Op::kEqual, bridgeQuery.value(true)))
        .andWhere(bridgeQuery.binary(bridgeQuery.coalesce({ bridgeQuery.column("ipv4"), bridgeQuery.value("") }), Op::kNotEqual, bridgeQuery.value("")))
        .andWhere(bridgeQuery.between(bridgeQuery.column("prefix_length"), bridgeQuery.value(1), bridgeQuery.value(30)))
        .addOrderBy(bridgeQuery.column("name"));
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
    currentQuery.select({ currentQuery.cast(currentQuery.column("id"), ruvia::DbDataType::kText),
            currentQuery.column("lan_interface"), currentQuery.column("target_cidr"), currentQuery.column("virtual_cidr") })
        .from("vpn_route")
        .andWhere(currentQuery.binary(currentQuery.column("edge_peer_id"), Op::kEqual,
            currentQuery.cast(currentQuery.value(peerId), ruvia::DbDataType::kUuid)))
        .addOrderBy(currentQuery.column("id"));
    const auto currentRows = co_await db.query(currentQuery);
    std::vector<RouteRecord> current;
    current.reserve(currentRows.size());
    for (const auto& row : currentRows)
        current.push_back(RouteRecord{
            route_sync_detail::rowValue(row, 0), route_sync_detail::rowValue(row, 1),
            parseCidr(route_sync_detail::rowValue(row, 2), 1, 30),
            parseCidr(route_sync_detail::rowValue(row, 3), 1, 30)});

    ruvia::DbQuery allRoutesQuery;
    allRoutesQuery.select({ allRoutesQuery.cast(allRoutesQuery.column("id"), ruvia::DbDataType::kText),
        allRoutesQuery.column("lan_interface"), allRoutesQuery.column("target_cidr"), allRoutesQuery.column("virtual_cidr") }).from("vpn_route");
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
            route.update("vpn_route")
                .set("network_id", route.cast(route.value(networkId), ruvia::DbDataType::kUuid))
                .set("lan_interface", route.value(bridge.lanInterface)).set("target_cidr", route.value(targetCidr))
                .set("virtual_cidr", route.value(virtualCidr)).set("mode", route.value("nat"))
                .set("nat_mode", route.value("masquerade")).set("enabled", route.value(true))
                .set("status", route.value("active")).set("last_error", route.value(""))
                .set("updated_at", route.call("now"))
                .andWhere(route.binary(route.column("id"), Op::kEqual, route.cast(route.value(routeId), ruvia::DbDataType::kUuid)));
            (void)co_await db.execute(route);
            for (auto& route : allRoutes)
                if (route.id == routeId) {
                    route.lanInterface = bridge.lanInterface;
                    route.target = bridge.target;
                    route.virtualNetwork = virtualNetwork;
                }
        } else {
            const auto id = service::common::nextUuidV7();
            ruvia::DbQuery route;
            route.insertInto("vpn_route", { "id", "network_id", "edge_peer_id", "lan_interface", "target_cidr",
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
        peer.binary(peer.column("capability", "e"), Op::kJsonGet, peer.value("vpn")),
        Op::kJsonGetText, peer.value("supportsVpn")), peer.value("") }) });
    peer.select({ peer.column("id", "p"), peer.column("network_id", "p"), peer.column("edge_node_id", "p"),
            peer.call("host", { peer.column("assigned_ipv4", "p") }), peer.column("config_revision", "p"),
            peer.column("status", "p"), peer.column("hub_public_key", "n"), peer.column("hub_endpoint", "n"),
            peer.column("hub_listen_port", "n"), peer.cast(peer.column("created_by", "n"), ruvia::DbDataType::kText), peer.column("status", "n") })
        .from("vpn_peer", "p")
        .join(ruvia::DbJoinType::kInner, "vpn_network", peer.binary(peer.column("id", "n"), Op::kEqual, peer.column("network_id", "p")), "n")
        .join(ruvia::DbJoinType::kInner, "edge_node", peer.binary(peer.column("id", "e"), Op::kEqual, peer.column("edge_node_id", "p")), "e")
        .andWhere(peer.binary(peer.column("id", "p"), Op::kEqual, peer.cast(peer.value(peerId), ruvia::DbDataType::kUuid)))
        .andWhere(peer.binary(peer.column("peer_type", "p"), Op::kEqual, peer.value("edge")))
        .andWhere(peer.binary(peer.column("enrollment_status", "e"), Op::kEqual, peer.value("approved")))
        .andWhere(peer.binary(capability, Op::kIn, peer.list({ peer.value("true"), peer.value("t"), peer.value("1") })))
        .limit(1);
    const auto rows = co_await c.db().query(peer);
    if (rows.empty())
        co_return;

    const auto nodeId = detail::edgeConfigRowValue(rows.front(), 2);
    ruvia::DbQuery routes;
    routes.select({ routes.cast(routes.column("id"), ruvia::DbDataType::kText), routes.column("virtual_cidr"),
            routes.column("target_cidr"), routes.column("mode"), routes.column("nat_mode"), routes.column("enabled") })
        .from("vpn_route")
        .andWhere(routes.binary(routes.column("edge_peer_id"), Op::kEqual, routes.cast(routes.value(peerId), ruvia::DbDataType::kUuid)))
        .addOrderBy(routes.column("virtual_cidr")).limit(16);
    const auto routeRows = co_await c.db().query(routes);
    const auto requestId = service::common::nextUuidV7();
    std::uint8_t requestBytes[16]{};
    if (!service::edge::protocol::uuidBytes(requestId, requestBytes))
        co_return;
    auto envelope = service::edge::protocol::outbound(nodeId);
    if (!platformId.empty()) {
        std::uint8_t platformBytes[16]{};
        if (service::edge::protocol::uuidBytes(platformId, platformBytes))
            envelope.set_platform_id(
                service::edge::protocol::bytes(platformBytes, sizeof(platformBytes)));
    }
    auto* request = envelope.mutable_vpn_config_request();
    request->set_request_id(service::edge::protocol::bytes(requestBytes, sizeof(requestBytes)));
    const auto revisionText = detail::edgeConfigRowValue(rows.front(), 4);
    const auto nextVersion = service::common::parseInt64(
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
        service::common::parseInt64(std::optional<std::string_view>(portText))
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
    superseded.update("edge_task").set("status", superseded.value("failed"))
        .set("result", superseded.call("jsonb_build_object", {
            superseded.cast(superseded.value("configVersion"), ruvia::DbDataType::kText), superseded.binary(superseded.column("request"), Op::kJsonGetText, superseded.cast(superseded.value("configVersion"), ruvia::DbDataType::kText)),
            superseded.cast(superseded.value("errorCode"), ruvia::DbDataType::kText), superseded.cast(superseded.value("superseded"), ruvia::DbDataType::kText),
            superseded.cast(superseded.value("errorMessage"), ruvia::DbDataType::kText), superseded.cast(superseded.value("superseded by newer VPN configuration"), ruvia::DbDataType::kText) }))
        .set("updated_at", superseded.call("now")).set("completed_at", superseded.call("now"))
        .andWhere(superseded.binary(superseded.column("node_id"), Op::kEqual, superseded.cast(superseded.value(nodeId), ruvia::DbDataType::kUuid)))
        .andWhere(superseded.binary(superseded.column("task_type"), Op::kEqual, superseded.value("vpn")))
        .andWhere(superseded.binary(superseded.column("status"), Op::kNotIn, superseded.list({ superseded.value("succeeded"), superseded.value("failed") })))
        .andWhere(superseded.binary(superseded.binary(superseded.column("request"), Op::kJsonGetText, superseded.value("peerId")),
            Op::kEqual, superseded.cast(superseded.value(peerId), ruvia::DbDataType::kText)));
    ruvia::DbQuery task;
    task.with("superseded", superseded)
        .insertInto("edge_task", { "id", "node_id", "task_type", "request", "created_by" })
        .values({ task.cast(task.value(requestId), ruvia::DbDataType::kUuid), task.cast(task.value(nodeId), ruvia::DbDataType::kUuid),
            task.value("vpn"), task.call("jsonb_build_object", {
                task.cast(task.value("peerId"), ruvia::DbDataType::kText), task.cast(task.value(peerId), ruvia::DbDataType::kText),
                task.cast(task.value("configVersion"), ruvia::DbDataType::kText), task.cast(task.value(nextVersion), ruvia::DbDataType::kBigInt),
                task.cast(task.value("enabled"), ruvia::DbDataType::kText), task.cast(task.value(request->enabled()), ruvia::DbDataType::kBoolean) }),
            task.cast(task.value(createdBy), ruvia::DbDataType::kUuid) });
    (void)co_await c.db().execute(task);
    co_await service::edge::dispatch::enqueue(c.redis(), nodeId, wire);
    ruvia::DbQuery revision;
    revision.update("vpn_peer").set("config_revision", revision.value(nextVersion)).set("updated_at", revision.call("now"))
        .andWhere(revision.binary(revision.column("id"), Op::kEqual, revision.cast(revision.value(peerId), ruvia::DbDataType::kUuid)));
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
    network.select({ network.column("hub_private_key"), network.column("hub_public_key"),
            network.column("hub_endpoint"), network.column("hub_listen_port") })
        .from("vpn_network")
        .andWhere(network.binary(network.column("id"), ruvia::DbBinaryOperator::kEqual,
            network.cast(network.value(kDefaultNetworkId), ruvia::DbDataType::kUuid)))
        .andWhere(network.binary(network.column("name"), ruvia::DbBinaryOperator::kEqual, network.value(kDefaultNetworkName)))
        .andWhere(network.binary(network.column("status"), ruvia::DbBinaryOperator::kEqual, network.value("enabled")))
        .andWhere(network.unary(ruvia::DbUnaryOperator::kIsNull, network.column("deleted_at")))
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
    const auto storedPort = service::common::parseInt64(
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
        update.update("vpn_network").set("hub_private_key", update.value(config.privateKey))
            .set("hub_public_key", update.value(config.publicKey)).set("hub_endpoint", update.value(config.endpoint))
            .set("hub_listen_port", update.value(static_cast<int>(config.listenPort)))
            .set("updated_at", update.call("now"))
            .andWhere(update.binary(update.column("id"), ruvia::DbBinaryOperator::kEqual,
                update.cast(update.value(kDefaultNetworkId), ruvia::DbDataType::kUuid)));
        (void)co_await transaction.execute(update);
    }
    co_await transaction.commit();
    co_return config;
}

} // namespace service::vpn::hub_config
