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
        const auto rows = co_await context.db().query(R"sql(
SELECT p.public_key, host(p.assigned_ipv4), p.peer_type,
       COALESCE((SELECT string_agg(r.virtual_cidr, ', ' ORDER BY r.virtual_cidr) FROM vpn_route r WHERE r.edge_peer_id = p.id AND r.enabled), ''),
       COALESCE((SELECT string_agg(access.virtual_cidr, ', ' ORDER BY access.virtual_cidr) FROM vpn_effective_route_access access WHERE access.peer_id = p.id), ''),
       COALESCE((SELECT string_agg(access.edge_address, ', ' ORDER BY access.edge_address) FROM vpn_effective_edge_access access WHERE access.peer_id = p.id), '')
FROM vpn_peer p JOIN vpn_network n ON n.id = p.network_id
WHERE p.status = 'active' AND n.status = 'enabled' AND n.deleted_at IS NULL AND p.public_key <> ''
  AND (NOT p.client_managed OR vpn_desktop_user_authorized(p.user_id))
ORDER BY p.id)sql");
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
        (void)co_await context.db().execute(
            "UPDATE vpn_peer SET last_handshake_at = to_timestamp($2::double precision), "
            "updated_at = NOW() WHERE public_key = $1 AND status = 'active' "
            "AND (last_handshake_at IS NULL OR last_handshake_at < "
            "to_timestamp($2::double precision))",
            service::common::dbParams(publicKey, seconds));
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

} // namespace feature

// Queue a complete, versioned VPN configuration for an Edge peer. The actor is
// optional for background projection: in that case the VPN network owner is
// used so edge_task.created_by remains a valid audit subject.
template <typename Context>
ruvia::Task<void> queueEdgeConfig(Context& c, std::string_view peerId,
                                  std::string_view actorId = {},
                                  std::string_view platformId = {}) {
    const auto rows = co_await c.db().query(R"sql(
SELECT p.id, p.network_id, p.edge_node_id, host(p.assigned_ipv4), p.config_revision, p.status,
       n.hub_public_key, n.hub_endpoint, n.hub_listen_port, n.created_by::text, n.status
FROM vpn_peer p JOIN vpn_network n ON n.id = p.network_id
JOIN edge_node e ON e.id = p.edge_node_id
WHERE p.id = $1::uuid AND p.peer_type = 'edge'
  AND e.enrollment_status = 'approved'
  AND CASE lower(COALESCE(e.capability->'vpn'->>'supportsVpn', ''))
      WHEN 'true' THEN true WHEN 't' THEN true WHEN '1' THEN true ELSE false END
LIMIT 1)sql", service::common::dbParams(peerId));
    if (rows.empty())
        co_return;

    const auto nodeId = detail::edgeConfigRowValue(rows.front(), 2);
    const auto routeRows = co_await c.db().query(R"sql(
SELECT id::text, virtual_cidr, target_cidr, mode, nat_mode, enabled
FROM vpn_route WHERE edge_peer_id = $1::uuid ORDER BY virtual_cidr LIMIT 16)sql",
                                                 service::common::dbParams(peerId));
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
    (void)co_await c.db().execute(R"sql(
WITH superseded AS (
    UPDATE edge_task
    SET status = 'failed',
        result = jsonb_build_object(
            'configVersion', request->>'configVersion',
            'errorCode', 'superseded',
            'errorMessage', 'superseded by newer VPN configuration'),
        updated_at = NOW(), completed_at = NOW()
    WHERE node_id = $2::uuid AND task_type = 'vpn'
      AND status NOT IN ('succeeded', 'failed')
      AND request->>'peerId' = $3::text
)
    INSERT INTO edge_task(id, node_id, task_type, request, created_by)
VALUES ($1::uuid, $2::uuid, 'vpn',
        jsonb_build_object('peerId', $3::text, 'configVersion', $4::bigint,
                           'enabled', $6::boolean), $5::uuid))sql",
                                  service::common::dbParams(requestId, nodeId, peerId,
                                                            nextVersion, createdBy,
                                                            request->enabled()));
    co_await service::edge::dispatch::enqueue(c.redis(), nodeId, wire);
    (void)co_await c.db().execute(
        "UPDATE vpn_peer SET config_revision = $2, updated_at = NOW() WHERE id = $1::uuid",
        service::common::dbParams(peerId, nextVersion));
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
    (void)co_await transaction.query(
        "SELECT pg_advisory_xact_lock(5282804697543808067::bigint)");
    const auto rows = co_await transaction.query(R"sql(
SELECT hub_private_key, hub_public_key, hub_endpoint, hub_listen_port
FROM vpn_network
WHERE id = '00000000-0000-7000-8000-000000000004'::uuid
  AND name = 'iot-server' AND status = 'enabled' AND deleted_at IS NULL
LIMIT 1
FOR UPDATE)sql");
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
        (void)co_await transaction.execute(R"sql(
UPDATE vpn_network
SET hub_private_key = $1, hub_public_key = $2, hub_endpoint = $3,
    hub_listen_port = $4, updated_at = NOW()
WHERE id = '00000000-0000-7000-8000-000000000004'::uuid)sql",
                                            service::common::dbParams(
                                                config.privateKey, config.publicKey,
                                                config.endpoint, static_cast<int>(config.listenPort)));
    }
    co_await transaction.commit();
    co_return config;
}

} // namespace service::vpn::hub_config
