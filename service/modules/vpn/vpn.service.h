#pragma once

#include <memory>
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <openssl/rand.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <ruvia/web/Context.h>
#include <ruvia/web/ModelObject.h>
#include <ruvia/web/db/DbQuery.h>

#include "service/common/http.h"
#include "service/common/uuid.h"
#include "service/middleware/rpc.h"
#include "service/modules/vpn/vpn.entity.h"
#include "service/modules/vpn/vpn.types.h"
#include "service/utils/crypto.h"
#include "service/utils/json.h"
#include "service/utils/number.h"

namespace service::vpn {

namespace cidr = module;

inline constexpr std::string_view kDefaultNetworkId{
    "00000000-0000-7000-8000-000000000004"
};
inline constexpr std::string_view kDefaultNetworkName{ "iot-server" };
inline constexpr std::string_view kDefaultOverlayCidr{ "100.96.0.0/16" };

namespace detail {

inline std::vector<std::string> textArrayJson(std::string_view raw) {
    const auto fieldJson = "{\"values\":" + std::string(raw) + "}";
    const auto object = ruvia::JsonValue::parse(fieldJson);
    const auto value = object ? object->get<ruvia::Array<ruvia::String>>("values") : std::nullopt;
    if (!value) {
        return {};
    }
    std::vector<std::string> result;
    result.reserve(value->size());
    for (const auto& item : *value) {
        result.emplace_back(item.view());
    }
    return result;
}

inline std::string jsonArray(const std::vector<std::string>& values) {
    std::string result{ "[" };
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            result.push_back(',');
        }
        result += service::utils::jsonQuoted(values[index]);
    }
    result.push_back(']');
    return result;
}

inline ruvia::DbExpression jsonKey(ruvia::DbQuery& query, std::string_view key) {
    return query.cast(query.value(key), ruvia::DbDataType::kText);
}

inline bool validKey(std::string_view value) noexcept {
    if (value.size() != 44 || value.back() != '=') {
        return false;
    }
    for (const auto character : value) {
        if (!((character >= 'A' && character <= 'Z') ||
              (character >= 'a' && character <= 'z') ||
              (character >= '0' && character <= '9') || character == '+' || character == '/' ||
              character == '=')) {
            return false;
        }
    }
    return true;
}

inline int base64Value(char value) noexcept {
    if (value >= 'A' && value <= 'Z') {
        return value - 'A';
    }
    if (value >= 'a' && value <= 'z') {
        return value - 'a' + 26;
    }
    if (value >= '0' && value <= '9') {
        return value - '0' + 52;
    }
    if (value == '+') {
        return 62;
    }
    if (value == '/') {
        return 63;
    }
    return -1;
}

inline bool decodeKey(std::string_view input, std::array<unsigned char, 32>& output) noexcept {
    if (input.size() != 44 || input.back() != '=') {
        return false;
    }
    std::size_t outputIndex = 0;
    for (std::size_t index = 0; index < input.size(); index += 4) {
        const auto first = base64Value(input[index]);
        const auto second = base64Value(input[index + 1]);
        const auto third = input[index + 2] == '=' ? 0 : base64Value(input[index + 2]);
        const auto fourth = input[index + 3] == '=' ? 0 : base64Value(input[index + 3]);
        if (first < 0 || second < 0 || third < 0 || fourth < 0 ||
            (index + 2 == input.size() - 1 && input[index + 2] != '=') ||
            (index + 3 == input.size() - 1 && input[index + 3] != '=')) {
            return false;
        }
        if (outputIndex < output.size()) {
            output[outputIndex++] = static_cast<unsigned char>((first << 2) | (second >> 4));
        }
        if (index + 2 < input.size() - 1 && outputIndex < output.size()) {
            output[outputIndex++] = static_cast<unsigned char>((second << 4) | (third >> 2));
        }
        if (index + 3 < input.size() - 1 && outputIndex < output.size()) {
            output[outputIndex++] = static_cast<unsigned char>((third << 6) | fourth);
        }
    }
    return outputIndex == output.size();
}

inline bool validManagedKey(std::string_view value) noexcept {
    if (value.size() != 44 || value.back() != '=') {
        return false;
    }
    for (std::size_t index = 0; index < 43; ++index) {
        if (base64Value(value[index]) < 0) {
            return false;
        }
    }
    if ((base64Value(value[42]) & 3) != 0) {
        return false;
    }
    std::array<unsigned char, 32> decoded{};
    return decodeKey(value, decoded) &&
        std::any_of(decoded.begin(), decoded.end(), [](auto byte) {
               return byte != 0;
           });
}

template <typename Db>
ruvia::Task<void> validateSelectedEdges(Db& db, std::string_view networkId, const std::vector<std::string>& ids) {
    if (ids.empty()) {
        co_return;
    }
    using Op = ruvia::DbBinaryOperator;
    ruvia::DbQuery query;
    std::vector<ruvia::DbQuery::Expr> selectedIds;
    selectedIds.reserve(ids.size());
    for (const auto& id : ids) {
        selectedIds.push_back(query.cast(query.value(id), ruvia::DbDataType::kUuid));
    }
    query.select(query.column(service::vpn::entities::EdgeNodeEntity::columnName<"id">(), "edge"))
        .distinct()
        .from(service::vpn::entities::EdgeNodeEntity::tableName(), "edge")
        .join(ruvia::DbJoinType::kInner, service::vpn::entities::VpnPeerEntity::tableName(), query.binary(query.column(service::vpn::entities::VpnPeerEntity::columnName<"edge_node_id">(), "peer"), Op::kEqual, query.column(service::vpn::entities::EdgeNodeEntity::columnName<"id">(), "edge")), "peer")
        .join(ruvia::DbJoinType::kInner, service::vpn::entities::VpnNetworkEntity::tableName(), query.binary(query.column(service::vpn::entities::VpnNetworkEntity::columnName<"id">(), "network"), Op::kEqual, query.column(service::vpn::entities::VpnPeerEntity::columnName<"network_id">(), "peer")), "network")
        .where(query.binary(query.column(service::vpn::entities::EdgeNodeEntity::columnName<"id">(), "edge"), Op::kIn, query.list(selectedIds)))
        .andWhere(query.binary(query.column(service::vpn::entities::EdgeNodeEntity::columnName<"enrollment_status">(), "edge"), Op::kEqual, query.value("approved")))
        .andWhere(query.binary(query.column(service::vpn::entities::VpnPeerEntity::columnName<"peer_type">(), "peer"), Op::kEqual, query.value("edge")))
        .andWhere(query.binary(query.column(service::vpn::entities::VpnPeerEntity::columnName<"status">(), "peer"), Op::kEqual, query.value("active")))
        .andWhere(query.binary(query.column(service::vpn::entities::VpnPeerEntity::columnName<"network_id">(), "peer"), Op::kEqual, query.cast(query.value(networkId), ruvia::DbDataType::kUuid)))
        .andWhere(query.binary(query.column(service::vpn::entities::VpnNetworkEntity::columnName<"status">(), "network"), Op::kEqual, query.value("enabled")))
        .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull, query.column(service::vpn::entities::VpnNetworkEntity::columnName<"deleted_at">(), "network")));
    const auto rows = co_await db.query(query);
    if (rows.size() != ids.size()) {
        service::common::fail(21001, "所选 Edge 节点尚未批准或没有可用的 VPN 网络", 400);
    }
}

inline std::string randomToken() {
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        throw std::runtime_error("VPN enrollment token generation failed");
    }
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 0x0fU]);
    }
    return result;
}

inline std::string hostText(std::uint32_t address) {
    return std::to_string((address >> 24U) & 0xffU) + "." +
        std::to_string((address >> 16U) & 0xffU) + "." +
        std::to_string((address >> 8U) & 0xffU) + "." +
        std::to_string(address & 0xffU);
}

inline bool validInterface(std::string_view value) noexcept {
    if (value.empty() || value.size() > 32) {
        return false;
    }
    for (const auto character : value) {
        if (!std::isalnum(static_cast<unsigned char>(character)) && character != '_' &&
            character != '-' && character != '.' && character != ':') {
            return false;
        }
    }
    return true;
}

inline std::string rowValue(const auto& row, std::size_t index) {
    return std::string(row[index].value().value_or(std::string_view{}));
}

template <typename Db>
ruvia::Task<void> advisoryLock(Db& db, std::pmr::memory_resource* resource, std::int64_t key) {
    ruvia::DbQuery lock(resource);
    lock.select(lock.call("pg_advisory_xact_lock", { lock.cast(lock.value(key), ruvia::DbDataType::kBigInt) }));
    (void)co_await db.query(lock);
}

inline std::string renderClientConfig(std::string_view privateKey, std::string_view address, std::string_view hubPublicKey, std::string_view hubEndpoint, std::uint16_t hubPort, const std::vector<std::string>& allowedRoutes) {
    std::string config{ "[Interface]\nPrivateKey = " };
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
        if (index != 0) {
            config += ", ";
        }
        config += allowedRoutes[index];
    }
    config += "\nPersistentKeepalive = 25\n";
    return config;
}

} // namespace detail

namespace route_sync_detail {

inline std::int64_t integer(std::string_view value, std::int64_t fallback = 0) {
    return service::utils::parseInt64(
               value.empty() ? std::nullopt : std::optional<std::string_view>(value)
    )
        .value_or(fallback);
}

} // namespace route_sync_detail

// Reconcile the Edge bridge inventory with its VPN routes. This database
// projection belongs to the management module; the VPN feature only receives
// the resulting edge configuration through its Redis control operation.
template <typename Db>
ruvia::Task<void> syncEdgeBridgeRoutes(Db& db, std::string_view peerId, std::string_view networkId, std::string_view edgeNodeId, std::string_view actorId, service::common::UuidV7Generator& uuidGenerator) {
    struct RouteRecord final {
        std::string id;
        std::string lanInterface;
        std::optional<cidr::Ipv4Cidr> target;
        std::optional<cidr::Ipv4Cidr> virtualNetwork;
    };

    struct BridgeRecord final {
        std::string lanInterface;
        cidr::Ipv4Cidr target;
    };

    using Op = ruvia::DbBinaryOperator;
    ruvia::DbQuery bridgeQuery;
    bridgeQuery
        .select({ bridgeQuery.column(service::vpn::entities::EdgeNodeNetworkEntity::columnName<"name">()), bridgeQuery.column(service::vpn::entities::EdgeNodeNetworkEntity::columnName<"device">()), bridgeQuery.column(service::vpn::entities::EdgeNodeNetworkEntity::columnName<"ipv4">()), bridgeQuery.column(service::vpn::entities::EdgeNodeNetworkEntity::columnName<"prefix_length">()) })
        .from(service::vpn::entities::EdgeNodeNetworkEntity::tableName())
        .where(bridgeQuery.binary(
            bridgeQuery.column(service::vpn::entities::EdgeNodeNetworkEntity::columnName<"node_id">()),
            Op::kEqual,
            bridgeQuery.cast(bridgeQuery.value(edgeNodeId), ruvia::DbDataType::kUuid)
        ))
        .andWhere(bridgeQuery.binary(bridgeQuery.column(service::vpn::entities::EdgeNodeNetworkEntity::columnName<"is_bridge">()), Op::kEqual, bridgeQuery.value(true)))
        .andWhere(bridgeQuery.binary(
            bridgeQuery.coalesce({ bridgeQuery.column(service::vpn::entities::EdgeNodeNetworkEntity::columnName<"ipv4">()), bridgeQuery.value("") }),
            Op::kNotEqual,
            bridgeQuery.value("")
        ))
        .andWhere(bridgeQuery.between(bridgeQuery.column(service::vpn::entities::EdgeNodeNetworkEntity::columnName<"prefix_length">()), bridgeQuery.value(1), bridgeQuery.value(30)))
        .orderBy(bridgeQuery.column(service::vpn::entities::EdgeNodeNetworkEntity::columnName<"name">()));
    const auto bridgeRows = co_await db.query(bridgeQuery);
    std::vector<BridgeRecord> bridges;
    for (const auto& row : bridgeRows) {
        const auto address = detail::rowValue(row, 2);
        const auto prefix = route_sync_detail::integer(detail::rowValue(row, 3));
        const auto target = cidr::networkCidr(address, static_cast<std::uint8_t>(prefix));
        if (!target || !cidr::isPrivateIpv4(*target)) {
            continue;
        }
        const auto name = detail::rowValue(row, 1).empty()
            ? detail::rowValue(row, 0)
            : detail::rowValue(row, 1);
        if (!name.empty()) {
            bridges.push_back(BridgeRecord{ name, *target });
        }
    }
    if (bridges.empty()) {
        service::common::fail(21008, "EdgeNode 尚未上报可映射的私有桥接 LAN 网段", 409);
    }

    ruvia::DbQuery currentQuery;
    currentQuery
        .select({ currentQuery.cast(currentQuery.column(service::vpn::entities::VpnRouteEntity::columnName<"id">()), ruvia::DbDataType::kText), currentQuery.column(service::vpn::entities::VpnRouteEntity::columnName<"lan_interface">()), currentQuery.column(service::vpn::entities::VpnRouteEntity::columnName<"target_cidr">()), currentQuery.column(service::vpn::entities::VpnRouteEntity::columnName<"virtual_cidr">()) })
        .from(service::vpn::entities::VpnRouteEntity::tableName())
        .where(currentQuery.binary(
            currentQuery.column(service::vpn::entities::VpnRouteEntity::columnName<"edge_peer_id">()),
            Op::kEqual,
            currentQuery.cast(currentQuery.value(peerId), ruvia::DbDataType::kUuid)
        ))
        .orderBy(currentQuery.column(service::vpn::entities::VpnRouteEntity::columnName<"id">()));
    const auto currentRows = co_await db.query(currentQuery);
    std::vector<RouteRecord> current;
    current.reserve(currentRows.size());
    for (const auto& row : currentRows) {
        current.push_back(RouteRecord{ detail::rowValue(row, 0), detail::rowValue(row, 1), cidr::parseCidr(detail::rowValue(row, 2), 1, 30), cidr::parseCidr(detail::rowValue(row, 3), 1, 30) });
    }

    ruvia::DbQuery allRoutesQuery;
    allRoutesQuery
        .select({ allRoutesQuery.cast(allRoutesQuery.column(service::vpn::entities::VpnRouteEntity::columnName<"id">()), ruvia::DbDataType::kText), allRoutesQuery.column(service::vpn::entities::VpnRouteEntity::columnName<"lan_interface">()), allRoutesQuery.column(service::vpn::entities::VpnRouteEntity::columnName<"target_cidr">()), allRoutesQuery.column(service::vpn::entities::VpnRouteEntity::columnName<"virtual_cidr">()) })
        .from(service::vpn::entities::VpnRouteEntity::tableName());
    const auto allRows = co_await db.query(allRoutesQuery);
    std::vector<RouteRecord> allRoutes;
    allRoutes.reserve(allRows.size());
    for (const auto& row : allRows) {
        allRoutes.push_back(RouteRecord{ detail::rowValue(row, 0), detail::rowValue(row, 1), cidr::parseCidr(detail::rowValue(row, 2), 1, 30), cidr::parseCidr(detail::rowValue(row, 3), 1, 30) });
    }

    const auto conflicts = [&](const cidr::Ipv4Cidr& candidate, std::string_view excludedId) {
        for (const auto& route : allRoutes) {
            if (route.id == excludedId) {
                continue;
            }
            if (cidr::virtualMappingConflicts(candidate, route.target, route.virtualNetwork)) {
                return true;
            }
        }
        return false;
    };
    const auto chooseVirtual = [&](const cidr::Ipv4Cidr& target,
                                   std::string_view excludedId) -> std::optional<cidr::Ipv4Cidr> {
        if (const auto preferred = cidr::mappedVirtualCidr(target);
            preferred && !preferred->overlaps(target) && !conflicts(*preferred, excludedId)) {
            return preferred;
        }
        if (target.prefix < cidr::kVirtualLanPool.prefix) {
            return std::nullopt;
        }
        const auto blockSize = static_cast<std::uint64_t>(target.size());
        for (std::uint64_t offset = 0; offset < cidr::kVirtualLanPool.size(); offset += blockSize) {
            const cidr::Ipv4Cidr candidate{
                static_cast<std::uint32_t>(cidr::kVirtualLanPool.network + offset),
                target.prefix
            };
            if (!candidate.overlaps(target) && !conflicts(candidate, excludedId)) {
                return candidate;
            }
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
        std::optional<cidr::Ipv4Cidr> virtualNetwork;
        if (existing != current.end()) {
            routeId = existing->id;
            claimed.emplace(routeId);
            if (existing->virtualNetwork &&
                existing->virtualNetwork->prefix == bridge.target.prefix &&
                cidr::kVirtualLanPool.contains(existing->virtualNetwork->network) &&
                cidr::kVirtualLanPool.contains(existing->virtualNetwork->network + existing->virtualNetwork->size() - 1U) &&
                !existing->virtualNetwork->overlaps(bridge.target) &&
                !conflicts(*existing->virtualNetwork, routeId)) {
                virtualNetwork = existing->virtualNetwork;
            } else {
                virtualNetwork = chooseVirtual(bridge.target, routeId);
            }
        } else {
            virtualNetwork = chooseVirtual(bridge.target, {});
        }
        const auto excludedId = existing != current.end() ? existing->id : std::string{};
        for (const auto& route : allRoutes) {
            if (route.id == excludedId) {
                continue;
            }
            if (cidr::realTargetConflictsVirtual(bridge.target, route.virtualNetwork)) {
                service::common::fail(21002, "真实 LAN 与已有虚拟网段重叠", 409);
            }
        }
        if (!virtualNetwork) {
            service::common::fail(21009, "没有可用的全局唯一虚拟网段", 409);
        }
        const auto targetCidr = bridge.target.text();
        const auto virtualCidr = virtualNetwork->text();
        if (existing != current.end()) {
            ruvia::DbQuery route;
            route.update(service::vpn::entities::VpnRouteEntity::tableName())
                .set(service::vpn::entities::VpnRouteEntity::columnName<"network_id">(), route.cast(route.value(networkId), ruvia::DbDataType::kUuid))
                .set(service::vpn::entities::VpnRouteEntity::columnName<"lan_interface">(), route.value(bridge.lanInterface))
                .set(service::vpn::entities::VpnRouteEntity::columnName<"target_cidr">(), route.value(targetCidr))
                .set(service::vpn::entities::VpnRouteEntity::columnName<"virtual_cidr">(), route.value(virtualCidr))
                .set(service::vpn::entities::VpnRouteEntity::columnName<"mode">(), route.value("nat"))
                .set(service::vpn::entities::VpnRouteEntity::columnName<"nat_mode">(), route.value("masquerade"))
                .set(service::vpn::entities::VpnRouteEntity::columnName<"enabled">(), route.value(true))
                .set(service::vpn::entities::VpnRouteEntity::columnName<"status">(), route.value("active"))
                .set(service::vpn::entities::VpnRouteEntity::columnName<"last_error">(), route.value(""))
                .set(service::vpn::entities::VpnRouteEntity::columnName<"updated_at">(), route.call("now"))
                .where(route.binary(route.column(service::vpn::entities::VpnRouteEntity::columnName<"id">()), Op::kEqual, route.cast(route.value(routeId), ruvia::DbDataType::kUuid)));
            (void)co_await db.execute(route);
            for (auto& route : allRoutes) {
                if (route.id == routeId) {
                    route.lanInterface = bridge.lanInterface;
                    route.target = bridge.target;
                    route.virtualNetwork = virtualNetwork;
                }
            }
        } else {
            const auto id = uuidGenerator.next();
            ruvia::DbQuery route;
            route.insertInto(service::vpn::entities::VpnRouteEntity::tableName(), { "id", "network_id", "edge_peer_id", "lan_interface", "target_cidr", "virtual_cidr", "mode", "nat_mode", "enabled", "status", "created_by" })
                .values({ route.cast(route.value(id), ruvia::DbDataType::kUuid), route.cast(route.value(networkId), ruvia::DbDataType::kUuid), route.cast(route.value(peerId), ruvia::DbDataType::kUuid), route.value(bridge.lanInterface), route.value(targetCidr), route.value(virtualCidr), route.value("nat"), route.value("masquerade"), route.value(true), route.value("active"), route.cast(route.value(actorId), ruvia::DbDataType::kUuid) });
            (void)co_await db.execute(route);
            allRoutes.push_back(
                RouteRecord{ id, bridge.lanInterface, bridge.target, virtualNetwork }
            );
        }
    }
}

class VpnService final {
  private:
    static std::int64_t integer(std::string_view, std::int64_t = 0);
    static void requireUuid(std::string_view, std::string_view);
    template <typename Context>
    static ruvia::Task<std::string> ensureDefaultNetwork(Context&);
    template <typename Context>
    static ruvia::Task<void> audit(Context&, std::string_view, std::string_view, std::string_view, std::string_view, std::string_view, std::string_view);
    template <typename Context>
    static ruvia::Task<std::string> control(Context&, std::string_view, std::string);
    template <typename Context>
    static ruvia::Task<void> queueEdgeConfig(Context&, std::string_view);
    template <typename Context>
    static ruvia::Task<std::string> clientConfigJson(Context&, std::string_view, std::string_view);
    template <typename Db>
    static ruvia::Task<std::optional<std::uint32_t>> allocateAddressFromDb(
        Db&,
        std::string_view,
        const cidr::Ipv4Cidr&
    );
    template <typename Context>
    static ruvia::Task<void> validateAllowedRoutes(Context&, std::string_view, const std::vector<std::string>&);
    template <typename Context>
    static ruvia::Task<void> reconcileHub(Context&);

  public:
    static VpnService& instance() {
        static thread_local VpnService value;
        return value;
    }

    template <typename Context>
    ruvia::Task<std::string> networks(Context& c, std::int64_t page, std::int64_t pageSize, std::optional<std::string> keyword, std::optional<std::string> status) {
        using Op = ruvia::DbBinaryOperator;
        page = std::max<std::int64_t>(1, page);
        pageSize = std::clamp<std::int64_t>(pageSize, 1, 100);
        if (page - 1 > std::numeric_limits<std::int64_t>::max() / pageSize) {
            service::common::fail(21001, "分页超出允许范围", 400);
        }
        const auto defaultNetworkId = co_await ensureDefaultNetwork(c);
        ruvia::DbQuery count(c.pool());
        count.select(count.aggregate("count", { count.star() }))
            .from(service::vpn::entities::VpnNetworkEntity::tableName(), "n")
            .where(count.binary(count.column(service::vpn::entities::VpnNetworkEntity::columnName<"id">(), "n"), Op::kEqual, count.cast(count.value(defaultNetworkId), ruvia::DbDataType::kUuid)))
            .andWhere(count.unary(ruvia::DbUnaryOperator::kIsNull, count.column(service::vpn::entities::VpnNetworkEntity::columnName<"deleted_at">(), "n")));
        if (keyword && !keyword->empty()) {
            const auto pattern = "%" + *keyword + "%";
            count.andWhere(count.binary(count.column(service::vpn::entities::VpnNetworkEntity::columnName<"name">(), "n"), Op::kILike, count.value(pattern)));
        }
        if (status && !status->empty()) {
            count.andWhere(count.binary(count.column(service::vpn::entities::VpnNetworkEntity::columnName<"status">(), "n"), Op::kEqual, count.value(*status)));
        }
        const auto countRows = co_await c.db().query(count);
        const auto total = integer(detail::rowValue(countRows.front(), 0));
        ruvia::DbQuery peerCount(c.pool());
        peerCount.select(peerCount.aggregate("count", { peerCount.star() }))
            .from(service::vpn::entities::VpnPeerEntity::tableName(), "p")
            .where(peerCount.binary(peerCount.column(service::vpn::entities::VpnPeerEntity::columnName<"network_id">(), "p"), Op::kEqual, peerCount.column(service::vpn::entities::VpnNetworkEntity::columnName<"id">(), "n")))
            .andWhere(peerCount.binary(peerCount.column(service::vpn::entities::VpnPeerEntity::columnName<"status">(), "p"), Op::kNotEqual, peerCount.value("revoked")));
        ruvia::DbQuery routeCount(c.pool());
        routeCount.select(routeCount.aggregate("count", { routeCount.star() }))
            .from(service::vpn::entities::VpnRouteEntity::tableName(), "r")
            .where(routeCount.binary(routeCount.column(service::vpn::entities::VpnRouteEntity::columnName<"network_id">(), "r"), Op::kEqual, routeCount.column(service::vpn::entities::VpnNetworkEntity::columnName<"id">(), "n")))
            .andWhere(routeCount.binary(routeCount.column(service::vpn::entities::VpnRouteEntity::columnName<"enabled">(), "r"), Op::kEqual, routeCount.value(true)));
        ruvia::DbQuery listQuery(c.pool());
        listQuery
            .select(listQuery.call(
                "jsonb_build_object",
                { detail::jsonKey(listQuery, "id"), listQuery.column(service::vpn::entities::VpnNetworkEntity::columnName<"id">(), "n"), detail::jsonKey(listQuery, "name"), listQuery.column(service::vpn::entities::VpnNetworkEntity::columnName<"name">(), "n"), detail::jsonKey(listQuery, "overlayCidr"), listQuery.column(service::vpn::entities::VpnNetworkEntity::columnName<"overlay_cidr">(), "n"), detail::jsonKey(listQuery, "hubPublicKey"), listQuery.column(service::vpn::entities::VpnNetworkEntity::columnName<"hub_public_key">(), "n"), detail::jsonKey(listQuery, "hubEndpoint"), listQuery.column(service::vpn::entities::VpnNetworkEntity::columnName<"hub_endpoint">(), "n"), detail::jsonKey(listQuery, "hubListenPort"), listQuery.column(service::vpn::entities::VpnNetworkEntity::columnName<"hub_listen_port">(), "n"), detail::jsonKey(listQuery, "status"), listQuery.column(service::vpn::entities::VpnNetworkEntity::columnName<"status">(), "n"), detail::jsonKey(listQuery, "peerCount"), listQuery.subquery(peerCount), detail::jsonKey(listQuery, "routeCount"), listQuery.subquery(routeCount), detail::jsonKey(listQuery, "createdAt"), listQuery.call("iot_utc_timestamp", { listQuery.column(service::vpn::entities::VpnNetworkEntity::columnName<"created_at">(), "n") }), detail::jsonKey(listQuery, "updatedAt"), listQuery.call("iot_utc_timestamp", { listQuery.column(service::vpn::entities::VpnNetworkEntity::columnName<"updated_at">(), "n") }) }
            ))
            .from(service::vpn::entities::VpnNetworkEntity::tableName(), "n")
            .where(listQuery.binary(listQuery.column(service::vpn::entities::VpnNetworkEntity::columnName<"id">(), "n"), Op::kEqual, listQuery.cast(listQuery.value(defaultNetworkId), ruvia::DbDataType::kUuid)))
            .andWhere(listQuery.unary(ruvia::DbUnaryOperator::kIsNull, listQuery.column(service::vpn::entities::VpnNetworkEntity::columnName<"deleted_at">(), "n")));
        if (keyword && !keyword->empty()) {
            const auto pattern = "%" + *keyword + "%";
            listQuery.andWhere(listQuery.binary(listQuery.column(service::vpn::entities::VpnNetworkEntity::columnName<"name">(), "n"), Op::kILike, listQuery.value(pattern)));
        }
        if (status && !status->empty()) {
            listQuery.andWhere(listQuery.binary(listQuery.column(service::vpn::entities::VpnNetworkEntity::columnName<"status">(), "n"), Op::kEqual, listQuery.value(*status)));
        }
        listQuery.orderBy(listQuery.column(service::vpn::entities::VpnNetworkEntity::columnName<"created_at">(), "n"), ruvia::DbOrderDirection::kDesc)
            .addOrderBy(listQuery.column(service::vpn::entities::VpnNetworkEntity::columnName<"id">(), "n"), ruvia::DbOrderDirection::kDesc)
            .limit(static_cast<std::uint64_t>(pageSize))
            .offset(static_cast<std::uint64_t>((page - 1) * pageSize));
        const auto rows = co_await c.db().query(listQuery);
        std::string list{ "[" };
        for (std::size_t index = 0; index < rows.size(); ++index) {
            if (index != 0) {
                list.push_back(',');
            }
            list += detail::rowValue(rows[index], 0);
        }
        list.push_back(']');
        co_return "{\"list\":" + list + ",\"total\":" + std::to_string(total) +
            ",\"page\":" + std::to_string(page) + ",\"pageSize\":" +
            std::to_string(pageSize) + ",\"totalPages\":" +
            std::to_string(total == 0 ? 0 : (total + pageSize - 1) / pageSize) + "}";
    }

    template <typename Context>
    ruvia::Task<std::string> network(Context& c, std::string_view id) {
        using Op = ruvia::DbBinaryOperator;
        requireUuid(id, "VPN 网络 ID 无效");
        ruvia::DbQuery peerRows(c.pool());
        const auto peerObject = peerRows.call(
            "jsonb_build_object",
            { detail::jsonKey(peerRows, "id"), peerRows.column(service::vpn::entities::VpnPeerEntity::columnName<"id">(), "p"), detail::jsonKey(peerRows, "peerType"), peerRows.column(service::vpn::entities::VpnPeerEntity::columnName<"peer_type">(), "p"), detail::jsonKey(peerRows, "edgeNodeId"), peerRows.column(service::vpn::entities::VpnPeerEntity::columnName<"edge_node_id">(), "p"), detail::jsonKey(peerRows, "userId"), peerRows.column(service::vpn::entities::VpnPeerEntity::columnName<"user_id">(), "p"), detail::jsonKey(peerRows, "name"), peerRows.column(service::vpn::entities::VpnPeerEntity::columnName<"name">(), "p"), detail::jsonKey(peerRows, "publicKey"), peerRows.column(service::vpn::entities::VpnPeerEntity::columnName<"public_key">(), "p"), detail::jsonKey(peerRows, "assignedIpv4"), peerRows.call("host", { peerRows.column(service::vpn::entities::VpnPeerEntity::columnName<"assigned_ipv4">(), "p") }), detail::jsonKey(peerRows, "allowedRoutes"), peerRows.column(service::vpn::entities::VpnPeerEntity::columnName<"allowed_routes">(), "p"), detail::jsonKey(peerRows, "status"), peerRows.column(service::vpn::entities::VpnPeerEntity::columnName<"status">(), "p"), detail::jsonKey(peerRows, "configRevision"), peerRows.column(service::vpn::entities::VpnPeerEntity::columnName<"config_revision">(), "p"), detail::jsonKey(peerRows, "lastHandshakeAt"), peerRows.call("iot_utc_timestamp", { peerRows.column(service::vpn::entities::VpnPeerEntity::columnName<"last_handshake_at">(), "p") }) }
        );
        const std::array peerOrder{
            ruvia::DbOrderTerm{ peerRows.column(service::vpn::entities::VpnPeerEntity::columnName<"created_at">(), "p") }
        };
        peerRows
            .select(peerRows.aggregate("jsonb_agg", { peerObject }, false, peerOrder))
            .from(service::vpn::entities::VpnPeerEntity::tableName(), "p")
            .where(peerRows.binary(peerRows.column(service::vpn::entities::VpnPeerEntity::columnName<"network_id">(), "p"), Op::kEqual, peerRows.column(service::vpn::entities::VpnNetworkEntity::columnName<"id">(), "n")));

        ruvia::DbQuery routeRows(c.pool());
        const auto routeObject = routeRows.call(
            "jsonb_build_object",
            { detail::jsonKey(routeRows, "id"), routeRows.column(service::vpn::entities::VpnRouteEntity::columnName<"id">(), "r"), detail::jsonKey(routeRows, "edgePeerId"), routeRows.column(service::vpn::entities::VpnRouteEntity::columnName<"edge_peer_id">(), "r"), detail::jsonKey(routeRows, "lanInterface"), routeRows.column(service::vpn::entities::VpnRouteEntity::columnName<"lan_interface">(), "r"), detail::jsonKey(routeRows, "targetCidr"), routeRows.column(service::vpn::entities::VpnRouteEntity::columnName<"target_cidr">(), "r"), detail::jsonKey(routeRows, "virtualCidr"), routeRows.column(service::vpn::entities::VpnRouteEntity::columnName<"virtual_cidr">(), "r"), detail::jsonKey(routeRows, "mode"), routeRows.column(service::vpn::entities::VpnRouteEntity::columnName<"mode">(), "r"), detail::jsonKey(routeRows, "natMode"), routeRows.column(service::vpn::entities::VpnRouteEntity::columnName<"nat_mode">(), "r"), detail::jsonKey(routeRows, "status"), routeRows.column(service::vpn::entities::VpnRouteEntity::columnName<"status">(), "r"), detail::jsonKey(routeRows, "enabled"), routeRows.column(service::vpn::entities::VpnRouteEntity::columnName<"enabled">(), "r"), detail::jsonKey(routeRows, "lastError"), routeRows.column(service::vpn::entities::VpnRouteEntity::columnName<"last_error">(), "r") }
        );
        const std::array routeOrder{
            ruvia::DbOrderTerm{ routeRows.column(service::vpn::entities::VpnRouteEntity::columnName<"created_at">(), "r") }
        };
        routeRows
            .select(routeRows.aggregate("jsonb_agg", { routeObject }, false, routeOrder))
            .from(service::vpn::entities::VpnRouteEntity::tableName(), "r")
            .where(routeRows.binary(routeRows.column(service::vpn::entities::VpnRouteEntity::columnName<"network_id">(), "r"), Op::kEqual, routeRows.column(service::vpn::entities::VpnNetworkEntity::columnName<"id">(), "n")));

        ruvia::DbQuery query(c.pool());
        const auto emptyJson = query.cast(query.value("[]"), ruvia::DbDataType::kJsonb);
        query
            .select(query.cast(
                query.call(
                    "jsonb_build_object",
                    { detail::jsonKey(query, "id"), query.column(service::vpn::entities::VpnNetworkEntity::columnName<"id">(), "n"), detail::jsonKey(query, "name"), query.column(service::vpn::entities::VpnNetworkEntity::columnName<"name">(), "n"), detail::jsonKey(query, "overlayCidr"), query.column(service::vpn::entities::VpnNetworkEntity::columnName<"overlay_cidr">(), "n"), detail::jsonKey(query, "hubPublicKey"), query.column(service::vpn::entities::VpnNetworkEntity::columnName<"hub_public_key">(), "n"), detail::jsonKey(query, "hubEndpoint"), query.column(service::vpn::entities::VpnNetworkEntity::columnName<"hub_endpoint">(), "n"), detail::jsonKey(query, "hubListenPort"), query.column(service::vpn::entities::VpnNetworkEntity::columnName<"hub_listen_port">(), "n"), detail::jsonKey(query, "status"), query.column(service::vpn::entities::VpnNetworkEntity::columnName<"status">(), "n"), detail::jsonKey(query, "createdAt"), query.call("iot_utc_timestamp", { query.column(service::vpn::entities::VpnNetworkEntity::columnName<"created_at">(), "n") }), detail::jsonKey(query, "updatedAt"), query.call("iot_utc_timestamp", { query.column(service::vpn::entities::VpnNetworkEntity::columnName<"updated_at">(), "n") }), detail::jsonKey(query, "peers"), query.coalesce({ query.subquery(peerRows), emptyJson }), detail::jsonKey(query, "routes"), query.coalesce({ query.subquery(routeRows), emptyJson }) }
                ),
                ruvia::DbDataType::kText
            ))
            .from(service::vpn::entities::VpnNetworkEntity::tableName(), "n")
            .where(query.binary(query.column(service::vpn::entities::VpnNetworkEntity::columnName<"id">(), "n"), Op::kEqual, query.cast(query.value(id), ruvia::DbDataType::kUuid)))
            .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull, query.column(service::vpn::entities::VpnNetworkEntity::columnName<"deleted_at">(), "n")));
        const auto rows = co_await c.db().query(query);
        if (rows.empty()) {
            service::common::fail(21004, "VPN 网络不存在", 404);
        }
        co_return detail::rowValue(rows.front(), 0);
    }

    template <typename Context>
    ruvia::Task<std::string> createNetwork(Context& c, const VpnNetworkInput&) {
        (void)co_await ensureDefaultNetwork(c);
        service::common::fail(21003, "VPN 使用默认 iot-server，无需新建 VPN 网络", 409);
        co_return std::string{};
    }

    template <typename Context>
    ruvia::Task<void> updateNetwork(Context& c, std::string_view id, const VpnNetworkInput&) {
        requireUuid(id, "VPN 网络 ID 无效");
        (void)co_await ensureDefaultNetwork(c);
        service::common::fail(21003, "VPN 使用 iot-server，网络配置不可修改", 409);
    }

    template <typename Context>
    ruvia::Task<void> removeNetwork(Context& c, std::string_view id) {
        requireUuid(id, "VPN 网络 ID 无效");
        (void)co_await ensureDefaultNetwork(c);
        service::common::fail(21003, "默认 iot-server VPN 网络不能删除", 409);
    }

    template <typename Context>
    ruvia::Task<std::string> routes(Context& c, std::optional<std::string> networkId, std::optional<std::string> edgeNodeId = std::nullopt) {
        using Op = ruvia::DbBinaryOperator;
        ruvia::DbQuery query(c.pool());
        query
            .select(query.call(
                "jsonb_build_object",
                { detail::jsonKey(query, "id"), query.column(service::vpn::entities::VpnRouteEntity::columnName<"id">(), "r"), detail::jsonKey(query, "networkId"), query.column(service::vpn::entities::VpnRouteEntity::columnName<"network_id">(), "r"), detail::jsonKey(query, "edgePeerId"), query.column(service::vpn::entities::VpnRouteEntity::columnName<"edge_peer_id">(), "r"), detail::jsonKey(query, "edgeNodeId"), query.column(service::vpn::entities::VpnPeerEntity::columnName<"edge_node_id">(), "p"), detail::jsonKey(query, "lanInterface"), query.column(service::vpn::entities::VpnRouteEntity::columnName<"lan_interface">(), "r"), detail::jsonKey(query, "targetCidr"), query.column(service::vpn::entities::VpnRouteEntity::columnName<"target_cidr">(), "r"), detail::jsonKey(query, "virtualCidr"), query.column(service::vpn::entities::VpnRouteEntity::columnName<"virtual_cidr">(), "r"), detail::jsonKey(query, "mode"), query.column(service::vpn::entities::VpnRouteEntity::columnName<"mode">(), "r"), detail::jsonKey(query, "natMode"), query.column(service::vpn::entities::VpnRouteEntity::columnName<"nat_mode">(), "r"), detail::jsonKey(query, "status"), query.column(service::vpn::entities::VpnRouteEntity::columnName<"status">(), "r"), detail::jsonKey(query, "enabled"), query.column(service::vpn::entities::VpnRouteEntity::columnName<"enabled">(), "r"), detail::jsonKey(query, "lastError"), query.column(service::vpn::entities::VpnRouteEntity::columnName<"last_error">(), "r"), detail::jsonKey(query, "createdAt"), query.call("iot_utc_timestamp", { query.column(service::vpn::entities::VpnRouteEntity::columnName<"created_at">(), "r") }), detail::jsonKey(query, "updatedAt"), query.call("iot_utc_timestamp", { query.column(service::vpn::entities::VpnRouteEntity::columnName<"updated_at">(), "r") }) }
            ))
            .from(service::vpn::entities::VpnRouteEntity::tableName(), "r")
            .join(ruvia::DbJoinType::kInner, service::vpn::entities::VpnPeerEntity::tableName(), query.binary(query.column(service::vpn::entities::VpnPeerEntity::columnName<"id">(), "p"), Op::kEqual, query.column(service::vpn::entities::VpnRouteEntity::columnName<"edge_peer_id">(), "r")), "p");
        if (networkId && !networkId->empty()) {
            requireUuid(*networkId, "VPN 网络 ID 无效");
            query.where(query.binary(query.column(service::vpn::entities::VpnRouteEntity::columnName<"network_id">(), "r"), Op::kEqual, query.cast(query.value(*networkId), ruvia::DbDataType::kUuid)));
        }
        if (edgeNodeId && !edgeNodeId->empty()) {
            requireUuid(*edgeNodeId, "Edge 节点 ID 无效");
            query.andWhere(query.binary(query.column(service::vpn::entities::VpnPeerEntity::columnName<"edge_node_id">(), "p"), Op::kEqual, query.cast(query.value(*edgeNodeId), ruvia::DbDataType::kUuid)));
        }
        query.orderBy(query.column(service::vpn::entities::VpnRouteEntity::columnName<"created_at">(), "r"), ruvia::DbOrderDirection::kDesc)
            .limit(1000);
        const auto rows = co_await c.db().query(query);
        std::string result{ "[" };
        for (std::size_t index = 0; index < rows.size(); ++index) {
            if (index != 0) {
                result.push_back(',');
            }
            result += detail::rowValue(rows[index], 0);
        }
        result.push_back(']');
        co_return result;
    }

    template <typename Context>
    ruvia::Task<std::string> createRoute(Context& c, const VpnRouteInput& payload) {
        const auto defaultNetworkId = co_await ensureDefaultNetwork(c);
        const auto networkId = payload.networkId;
        if (networkId != defaultNetworkId) {
            service::common::fail(21003, "VPN 仅使用默认 iot-server 网络", 409);
        }
        const auto edgePeerId = payload.edgePeerId;
        const auto targetText = payload.targetCidr;
        const auto target = cidr::parseCidr(targetText, 1, 30);
        if (!target || !cidr::isPrivateIpv4(*target)) {
            service::common::fail(21001, "真实 LAN 必须是有效的私有 IPv4 网段", 400);
        }
        using Op = ruvia::DbBinaryOperator;
        ruvia::DbQuery edge(c.pool());
        edge.select({ edge.column(service::vpn::entities::VpnPeerEntity::columnName<"network_id">(), "p"), edge.column(service::vpn::entities::VpnPeerEntity::columnName<"peer_type">(), "p"), edge.column(service::vpn::entities::VpnPeerEntity::columnName<"edge_node_id">(), "p") })
            .from(service::vpn::entities::VpnPeerEntity::tableName(), "p")
            .join(ruvia::DbJoinType::kInner, service::vpn::entities::VpnNetworkEntity::tableName(), edge.binary(edge.column(service::vpn::entities::VpnNetworkEntity::columnName<"id">(), "n"), Op::kEqual, edge.column(service::vpn::entities::VpnPeerEntity::columnName<"network_id">(), "p")), "n")
            .where(edge.binary(edge.column(service::vpn::entities::VpnPeerEntity::columnName<"id">(), "p"), Op::kEqual, edge.cast(edge.value(edgePeerId), ruvia::DbDataType::kUuid)))
            .andWhere(edge.binary(edge.column(service::vpn::entities::VpnPeerEntity::columnName<"status">(), "p"), Op::kNotEqual, edge.value("revoked")))
            .andWhere(edge.binary(edge.column(service::vpn::entities::VpnNetworkEntity::columnName<"status">(), "n"), Op::kEqual, edge.value("enabled")))
            .limit(1);
        const auto edgeRows = co_await c.db().query(edge);
        if (edgeRows.empty() || detail::rowValue(edgeRows.front(), 1) != "edge" ||
            detail::rowValue(edgeRows.front(), 0) != networkId) {
            service::common::fail(21004, "Edge Peer 不属于指定 VPN 网络", 404);
        }
        const auto targetCidr = target->text();
        auto transaction = co_await c.db().beginTransaction();
        co_await detail::advisoryLock(transaction, c.pool(), 5282804697543808068LL);
        co_await syncEdgeBridgeRoutes(transaction, edgePeerId, networkId, detail::rowValue(edgeRows.front(), 2), c.userId, *c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>());
        co_await transaction.commit();
        ruvia::DbQuery mappedQuery(c.pool());
        mappedQuery
            .select({ mappedQuery.cast(mappedQuery.column(service::vpn::entities::VpnRouteEntity::columnName<"id">()), ruvia::DbDataType::kText), mappedQuery.column(service::vpn::entities::VpnRouteEntity::columnName<"virtual_cidr">()) })
            .from(service::vpn::entities::VpnRouteEntity::tableName())
            .where(mappedQuery.binary(mappedQuery.column(service::vpn::entities::VpnRouteEntity::columnName<"edge_peer_id">()), Op::kEqual, mappedQuery.cast(mappedQuery.value(edgePeerId), ruvia::DbDataType::kUuid)))
            .andWhere(mappedQuery.binary(mappedQuery.column(service::vpn::entities::VpnRouteEntity::columnName<"target_cidr">()), Op::kEqual, mappedQuery.value(targetCidr)))
            .limit(1);
        const auto mapped = co_await c.db().query(mappedQuery);
        if (mapped.empty()) {
            service::common::fail(21004, "真实 LAN 不是 EdgeNode 的桥接网段", 409);
        }
        const auto requestedVirtual = payload.virtualCidr;
        if (requestedVirtual && *requestedVirtual != detail::rowValue(mapped.front(), 1)) {
            service::common::fail(21003, "虚拟网段由平台自动生成，请使用编辑修改", 409);
        }
        const auto id = detail::rowValue(mapped.front(), 0);
        co_await audit(c, c.userId, "vpn.route.ensure", "vpn_route", id, "success", "{}");
        co_await queueEdgeConfig(c, edgePeerId);
        (void)co_await reconcileHub(c);
        co_return id;
    }

    template <typename Context>
    ruvia::Task<void> updateRoute(Context& c, std::string_view id, const VpnRoutePatch& payload) {
        using Op = ruvia::DbBinaryOperator;
        requireUuid(id, "VPN 路由 ID 无效");
        ruvia::DbQuery current(c.pool());
        current.select({ current.column(service::vpn::entities::VpnRouteEntity::columnName<"edge_peer_id">()), current.column(service::vpn::entities::VpnRouteEntity::columnName<"network_id">()), current.column(service::vpn::entities::VpnRouteEntity::columnName<"lan_interface">()), current.column(service::vpn::entities::VpnRouteEntity::columnName<"target_cidr">()), current.column(service::vpn::entities::VpnRouteEntity::columnName<"virtual_cidr">()), current.column(service::vpn::entities::VpnRouteEntity::columnName<"mode">()), current.column(service::vpn::entities::VpnRouteEntity::columnName<"enabled">()) })
            .from(service::vpn::entities::VpnRouteEntity::tableName())
            .where(current.binary(current.column(service::vpn::entities::VpnRouteEntity::columnName<"id">()), Op::kEqual, current.cast(current.value(id), ruvia::DbDataType::kUuid)));
        const auto rows = co_await c.db().query(current);
        if (rows.empty()) {
            service::common::fail(21004, "VPN 路由不存在", 404);
        }
        const auto edgePeerId = detail::rowValue(rows.front(), 0);
        const auto currentInterface = detail::rowValue(rows.front(), 2);
        const auto currentTarget = detail::rowValue(rows.front(), 3);
        const auto currentVirtual = detail::rowValue(rows.front(), 4);
        const auto currentMode = detail::rowValue(rows.front(), 5);
        const auto currentEnabled = detail::rowValue(rows.front(), 6) == "t";
        const auto requestedTarget = payload.targetCidr;
        const auto requestedInterface = payload.lanInterface;
        const auto requestedMode = payload.mode;
        const auto requestedEnabled = payload.enabled;
        if ((requestedTarget && *requestedTarget != currentTarget) ||
            (requestedInterface && *requestedInterface != currentInterface) ||
            (requestedMode && *requestedMode != currentMode) ||
            (requestedEnabled && *requestedEnabled != currentEnabled)) {
            service::common::fail(21003, "VPN 映射中只有虚拟网段可以修改", 409);
        }
        const auto targetText = currentTarget;
        const auto virtualText = payload.virtualCidr.value_or(currentVirtual);
        const auto target = cidr::parseCidr(targetText, 1, 30);
        const auto virtualNetwork = cidr::parseCidr(virtualText, 1, 30);
        if (!target || !virtualNetwork || target->prefix != virtualNetwork->prefix ||
            !cidr::isPrivateIpv4(*target) || !cidr::kVirtualLanPool.contains(virtualNetwork->network) ||
            !cidr::kVirtualLanPool.contains(virtualNetwork->network + virtualNetwork->size() - 1U) ||
            virtualNetwork->overlaps(*target)) {
            service::common::fail(21001, "VPN 路由 CIDR 无效或不是等长私网映射", 400);
        }
        if (currentMode != "nat" || !currentEnabled) {
            service::common::fail(21003, "VPN 自动映射只能保持启用的 NAT 模式", 409);
        }
        auto transaction = co_await c.db().beginTransaction();
        co_await detail::advisoryLock(transaction, c.pool(), 5282804697543808068LL);
        ruvia::DbQuery conflictsQuery(c.pool());
        conflictsQuery
            .select({ conflictsQuery.column(service::vpn::entities::VpnRouteEntity::columnName<"target_cidr">()), conflictsQuery.column(service::vpn::entities::VpnRouteEntity::columnName<"virtual_cidr">()) })
            .from(service::vpn::entities::VpnRouteEntity::tableName())
            .where(conflictsQuery.binary(
                conflictsQuery.column(service::vpn::entities::VpnRouteEntity::columnName<"id">()),
                Op::kNotEqual,
                conflictsQuery.cast(conflictsQuery.value(id), ruvia::DbDataType::kUuid)
            ));
        const auto conflicts = co_await transaction.query(conflictsQuery);
        for (const auto& row : conflicts) {
            const auto otherTarget = cidr::parseCidr(detail::rowValue(row, 0), 1, 30);
            const auto otherVirtual = cidr::parseCidr(detail::rowValue(row, 1), 1, 30);
            if ((otherTarget && virtualNetwork->overlaps(*otherTarget)) ||
                (otherVirtual && virtualNetwork->overlaps(*otherVirtual))) {
                service::common::fail(21002, "虚拟网段与全局已有网段重叠", 409);
            }
        }
        const auto virtualCidr = virtualNetwork->text();
        ruvia::DbQuery update(c.pool());
        update.update(service::vpn::entities::VpnRouteEntity::tableName())
            .set(service::vpn::entities::VpnRouteEntity::columnName<"virtual_cidr">(), update.value(virtualCidr))
            .set(service::vpn::entities::VpnRouteEntity::columnName<"last_error">(), update.value(""))
            .set(service::vpn::entities::VpnRouteEntity::columnName<"updated_at">(), update.call("now"))
            .where(update.binary(update.column(service::vpn::entities::VpnRouteEntity::columnName<"id">()), Op::kEqual, update.cast(update.value(id), ruvia::DbDataType::kUuid)));
        (void)co_await transaction.execute(update);
        co_await transaction.commit();
        co_await audit(c, c.userId, "vpn.route.update", "vpn_route", id, "success", "{}");
        co_await queueEdgeConfig(c, edgePeerId);
        (void)co_await reconcileHub(c);
    }

    template <typename Context>
    ruvia::Task<void> removeRoute(Context& c, std::string_view id) {
        requireUuid(id, "VPN 路由 ID 无效");
        service::common::fail(21003, "VPN 桥接网段映射不能删除，请撤销 Edge Peer", 409);
    }

    template <typename Context>
    ruvia::Task<std::string> peers(Context& c, std::optional<std::string> networkId, std::optional<std::string> edgeNodeId = std::nullopt) {
        using Op = ruvia::DbBinaryOperator;
        ruvia::DbQuery query(c.pool());
        query
            .select(query.call(
                "jsonb_build_object",
                { detail::jsonKey(query, "id"), query.column(service::vpn::entities::VpnPeerEntity::columnName<"id">(), "p"), detail::jsonKey(query, "networkId"), query.column(service::vpn::entities::VpnPeerEntity::columnName<"network_id">(), "p"), detail::jsonKey(query, "peerType"), query.column(service::vpn::entities::VpnPeerEntity::columnName<"peer_type">(), "p"), detail::jsonKey(query, "edgeNodeId"), query.column(service::vpn::entities::VpnPeerEntity::columnName<"edge_node_id">(), "p"), detail::jsonKey(query, "userId"), query.column(service::vpn::entities::VpnPeerEntity::columnName<"user_id">(), "p"), detail::jsonKey(query, "name"), query.column(service::vpn::entities::VpnPeerEntity::columnName<"name">(), "p"), detail::jsonKey(query, "publicKey"), query.column(service::vpn::entities::VpnPeerEntity::columnName<"public_key">(), "p"), detail::jsonKey(query, "assignedIpv4"), query.call("host", { query.column(service::vpn::entities::VpnPeerEntity::columnName<"assigned_ipv4">(), "p") }), detail::jsonKey(query, "allowedRoutes"), query.column(service::vpn::entities::VpnPeerEntity::columnName<"allowed_routes">(), "p"), detail::jsonKey(query, "status"), query.column(service::vpn::entities::VpnPeerEntity::columnName<"status">(), "p"), detail::jsonKey(query, "configRevision"), query.column(service::vpn::entities::VpnPeerEntity::columnName<"config_revision">(), "p"), detail::jsonKey(query, "lastHandshakeAt"), query.call("iot_utc_timestamp", { query.column(service::vpn::entities::VpnPeerEntity::columnName<"last_handshake_at">(), "p") }), detail::jsonKey(query, "createdAt"), query.call("iot_utc_timestamp", { query.column(service::vpn::entities::VpnPeerEntity::columnName<"created_at">(), "p") }) }
            ))
            .from(service::vpn::entities::VpnPeerEntity::tableName(), "p");
        if (networkId && !networkId->empty()) {
            requireUuid(*networkId, "VPN 网络 ID 无效");
            query.where(query.binary(query.column(service::vpn::entities::VpnPeerEntity::columnName<"network_id">(), "p"), Op::kEqual, query.cast(query.value(*networkId), ruvia::DbDataType::kUuid)));
        }
        if (edgeNodeId && !edgeNodeId->empty()) {
            requireUuid(*edgeNodeId, "Edge 节点 ID 无效");
            query.andWhere(query.binary(query.column(service::vpn::entities::VpnPeerEntity::columnName<"edge_node_id">(), "p"), Op::kEqual, query.cast(query.value(*edgeNodeId), ruvia::DbDataType::kUuid)));
        }
        query.orderBy(query.column(service::vpn::entities::VpnPeerEntity::columnName<"created_at">(), "p"), ruvia::DbOrderDirection::kDesc)
            .limit(1000);
        const auto rows = co_await c.db().query(query);
        std::string result{ "[" };
        for (std::size_t index = 0; index < rows.size(); ++index) {
            if (index != 0) {
                result.push_back(',');
            }
            result += detail::rowValue(rows[index], 0);
        }
        result.push_back(']');
        co_return result;
    }

    template <typename Context>
    ruvia::Task<std::string> createPeer(Context& c, const VpnPeerInput& payload) {
        const auto defaultNetworkId = co_await ensureDefaultNetwork(c);
        const auto requestedNetworkId = payload.networkId;
        if (requestedNetworkId && !service::common::isUuid(*requestedNetworkId)) {
            service::common::fail(21001, "VPN 网络 ID 无效", 400);
        }
        if (requestedNetworkId && *requestedNetworkId != defaultNetworkId) {
            service::common::fail(21003, "VPN 仅使用默认 iot-server 网络", 409);
        }
        const auto networkId = defaultNetworkId;
        const auto peerType = payload.peerType;
        if (peerType != "windows" && peerType != "edge") {
            service::common::fail(21001, "Peer 类型只支持 windows 或 edge", 400);
        }
        const auto name = payload.name;
        auto publicKey = payload.publicKey.value_or("");
        if (!publicKey.empty() && !detail::validKey(publicKey)) {
            service::common::fail(21001, "WireGuard 公钥格式无效", 400);
        }
        std::string edgeNodeId;
        std::string userId = c.userId;
        std::string reusableEdgePeerId;
        if (peerType == "edge") {
            edgeNodeId = payload.edgeNodeId.value_or("");
            if (!service::common::isUuid(edgeNodeId)) {
                service::common::fail(21001, "Edge 节点 ID 无效", 400);
            }
        } else if (publicKey.empty()) {
            service::common::fail(21001, "Windows Peer 必须提供公钥", 400);
        }
        using Op = ruvia::DbBinaryOperator;
        ruvia::DbQuery networkQuery(c.pool());
        networkQuery
            .select({ networkQuery.column(service::vpn::entities::VpnNetworkEntity::columnName<"overlay_cidr">()), networkQuery.column(service::vpn::entities::VpnNetworkEntity::columnName<"status">()) })
            .from(service::vpn::entities::VpnNetworkEntity::tableName())
            .where(networkQuery.binary(networkQuery.column(service::vpn::entities::VpnNetworkEntity::columnName<"id">()), Op::kEqual, networkQuery.cast(networkQuery.value(networkId), ruvia::DbDataType::kUuid)))
            .andWhere(networkQuery.unary(ruvia::DbUnaryOperator::kIsNull, networkQuery.column(service::vpn::entities::VpnNetworkEntity::columnName<"deleted_at">())));
        const auto networkRows = co_await c.db().query(networkQuery);
        if (networkRows.empty()) {
            service::common::fail(21004, "VPN 网络不存在", 404);
        }
        if (detail::rowValue(networkRows.front(), 1) != "enabled") {
            service::common::fail(21003, "VPN 网络已停用", 409);
        }
        if (peerType == "edge") {
            ruvia::DbQuery edgeQuery(c.pool());
            const auto vpnCapability = edgeQuery.binary(
                edgeQuery.binary(edgeQuery.column(service::vpn::entities::EdgeNodeEntity::columnName<"capability">()), Op::kJsonGet, edgeQuery.cast(edgeQuery.value("vpn"), ruvia::DbDataType::kText)),
                Op::kJsonGetText,
                edgeQuery.cast(edgeQuery.value("publicKey"), ruvia::DbDataType::kText)
            );
            edgeQuery
                .select(edgeQuery.coalesce({ vpnCapability, edgeQuery.value("") }))
                .from(service::vpn::entities::EdgeNodeEntity::tableName())
                .where(edgeQuery.binary(edgeQuery.column(service::vpn::entities::EdgeNodeEntity::columnName<"id">()), Op::kEqual, edgeQuery.cast(edgeQuery.value(edgeNodeId), ruvia::DbDataType::kUuid)))
                .andWhere(edgeQuery.binary(edgeQuery.column(service::vpn::entities::EdgeNodeEntity::columnName<"enrollment_status">()), Op::kEqual, edgeQuery.value("approved")));
            const auto edgeRows = co_await c.db().query(edgeQuery);
            if (edgeRows.empty()) {
                service::common::fail(21004, "Edge 节点不存在或尚未批准", 404);
            }
            const auto reportedKey = detail::rowValue(edgeRows.front(), 0);
            publicKey = detail::validKey(reportedKey) ? reportedKey : std::string{};
            ruvia::DbQuery existingQuery(c.pool());
            const auto revokedFirst = existingQuery.caseWhen(
                { { existingQuery.binary(existingQuery.column(service::vpn::entities::VpnPeerEntity::columnName<"status">()), Op::kEqual, existingQuery.value("revoked")),
                    existingQuery.cast(existingQuery.value(1), ruvia::DbDataType::kInteger) } },
                existingQuery.cast(existingQuery.value(0), ruvia::DbDataType::kInteger)
            );
            existingQuery
                .select({ existingQuery.cast(existingQuery.column(service::vpn::entities::VpnPeerEntity::columnName<"id">()), ruvia::DbDataType::kText), existingQuery.column(service::vpn::entities::VpnPeerEntity::columnName<"status">()) })
                .from(service::vpn::entities::VpnPeerEntity::tableName())
                .where(existingQuery.binary(existingQuery.column(service::vpn::entities::VpnPeerEntity::columnName<"network_id">()), Op::kEqual, existingQuery.cast(existingQuery.value(networkId), ruvia::DbDataType::kUuid)))
                .andWhere(existingQuery.binary(existingQuery.column(service::vpn::entities::VpnPeerEntity::columnName<"edge_node_id">()), Op::kEqual, existingQuery.cast(existingQuery.value(edgeNodeId), ruvia::DbDataType::kUuid)))
                .orderBy(revokedFirst)
                .addOrderBy(existingQuery.column(service::vpn::entities::VpnPeerEntity::columnName<"created_at">()), ruvia::DbOrderDirection::kDesc)
                .limit(1);
            const auto existing = co_await c.db().query(existingQuery);
            if (!existing.empty()) {
                if (detail::rowValue(existing.front(), 1) != "revoked") {
                    service::common::fail(21002, "该 Edge 节点已经加入 VPN 网络", 409);
                }
                reusableEdgePeerId = detail::rowValue(existing.front(), 0);
            }
            userId.clear();
        }
        const auto overlay = cidr::parseCidr(detail::rowValue(networkRows.front(), 0), 16, 30);
        if (!overlay) {
            service::common::fail(21005, "VPN 网络 Overlay 配置损坏", 500);
        }
        const auto allowedRoutes = payload.allowedRoutes;
        if (allowedRoutes.size() > 64) {
            service::common::fail(21001, "Peer 最多授权 64 条路由", 400);
        }
        co_await validateAllowedRoutes(c, networkId, allowedRoutes);
        const auto id = reusableEdgePeerId.empty() ? c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next()
                                                   : reusableEdgePeerId;
        const auto status = publicKey.empty() ? "pending" : "active";
        const auto allowedRoutesJson = detail::jsonArray(allowedRoutes);
        auto transaction = co_await c.db().beginTransaction();
        co_await detail::advisoryLock(transaction, c.pool(), 5282804697543808068LL);
        if (reusableEdgePeerId.empty()) {
            const auto assigned = co_await allocateAddressFromDb(transaction, networkId, *overlay);
            const auto assignedText = detail::hostText(*assigned);
            ruvia::DbQuery insert(c.pool());
            insert.insertInto(service::vpn::entities::VpnPeerEntity::tableName(), { "id", "network_id", "peer_type", "edge_node_id", "user_id", "name", "public_key", "assigned_ipv4", "allowed_routes", "status" })
                .values({ insert.cast(insert.value(id), ruvia::DbDataType::kUuid), insert.cast(insert.value(networkId), ruvia::DbDataType::kUuid), insert.value(peerType), insert.cast(insert.nullIf(insert.cast(insert.value(edgeNodeId), ruvia::DbDataType::kText), insert.cast(insert.value(""), ruvia::DbDataType::kText)), ruvia::DbDataType::kUuid), insert.cast(insert.nullIf(insert.cast(insert.value(userId), ruvia::DbDataType::kText), insert.cast(insert.value(""), ruvia::DbDataType::kText)), ruvia::DbDataType::kUuid), insert.value(name), insert.value(publicKey), insert.cast(insert.value(assignedText), ruvia::DbDataType::kInet), insert.cast(insert.value(allowedRoutesJson), ruvia::DbDataType::kJsonb), insert.value(status) });
            (void)co_await transaction.execute(insert);
        } else {
            ruvia::DbQuery reactivation(c.pool());
            reactivation
                .update(service::vpn::entities::VpnPeerEntity::tableName())
                .set(service::vpn::entities::VpnPeerEntity::columnName<"name">(), reactivation.value(name))
                .set(service::vpn::entities::VpnPeerEntity::columnName<"public_key">(), reactivation.value(publicKey))
                .set(service::vpn::entities::VpnPeerEntity::columnName<"allowed_routes">(), reactivation.cast(reactivation.value(allowedRoutesJson), ruvia::DbDataType::kJsonb))
                .set(service::vpn::entities::VpnPeerEntity::columnName<"status">(), reactivation.value(status))
                .set(service::vpn::entities::VpnPeerEntity::columnName<"revoked_at">(), reactivation.nullValue())
                .set(service::vpn::entities::VpnPeerEntity::columnName<"updated_at">(), reactivation.call("now"))
                .where(reactivation.binary(reactivation.column(service::vpn::entities::VpnPeerEntity::columnName<"id">()), Op::kEqual, reactivation.cast(reactivation.value(id), ruvia::DbDataType::kUuid)))
                .andWhere(reactivation.binary(reactivation.column(service::vpn::entities::VpnPeerEntity::columnName<"peer_type">()), Op::kEqual, reactivation.value("edge")))
                .andWhere(reactivation.binary(reactivation.column(service::vpn::entities::VpnPeerEntity::columnName<"status">()), Op::kEqual, reactivation.value("revoked")))
                .returning({ reactivation.column(service::vpn::entities::VpnPeerEntity::columnName<"id">()) });
            const auto reactivated = co_await transaction.query(reactivation);
            if (reactivated.empty()) {
                service::common::fail(21002, "该 Edge 节点已经加入 VPN 网络", 409);
            }
        }
        if (peerType == "edge") {
            co_await syncEdgeBridgeRoutes(transaction, id, networkId, edgeNodeId, c.userId, *c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>());
        }
        co_await transaction.commit();
        co_await audit(c, c.userId, reusableEdgePeerId.empty() ? "vpn.peer.create" : "vpn.peer.reactivate", "vpn_peer", id, "success", "{}");
        if (peerType == "edge") {
            co_await queueEdgeConfig(c, id);
        }
        (void)co_await reconcileHub(c);
        co_return id;
    }

    template <typename Context>
    ruvia::Task<void> revokePeer(Context& c, std::string_view id) {
        requireUuid(id, "VPN Peer ID 无效");
        using Op = ruvia::DbBinaryOperator;
        ruvia::DbQuery update(c.pool());
        update.update(service::vpn::entities::VpnPeerEntity::tableName())
            .set(service::vpn::entities::VpnPeerEntity::columnName<"status">(), update.value("revoked"))
            .set(service::vpn::entities::VpnPeerEntity::columnName<"revoked_at">(), update.call("now"))
            .set(service::vpn::entities::VpnPeerEntity::columnName<"updated_at">(), update.call("now"))
            .where(update.binary(update.column(service::vpn::entities::VpnPeerEntity::columnName<"id">()), Op::kEqual, update.cast(update.value(id), ruvia::DbDataType::kUuid)))
            .andWhere(update.binary(update.column(service::vpn::entities::VpnPeerEntity::columnName<"status">()), Op::kNotEqual, update.value("revoked")))
            .returning({ update.column(service::vpn::entities::VpnPeerEntity::columnName<"public_key">()), update.column(service::vpn::entities::VpnPeerEntity::columnName<"edge_node_id">()) });
        const auto rows = co_await c.db().query(update);
        if (rows.empty()) {
            service::common::fail(21004, "VPN Peer 不存在或已撤销", 404);
        }
        const auto key = detail::rowValue(rows.front(), 0);
        if (detail::validKey(key)) {
            (void)co_await control(c, "wireguard-remove-peer", key);
        }
        const auto edgeNode = detail::rowValue(rows.front(), 1);
        if (!edgeNode.empty()) {
            co_await queueEdgeConfig(c, id);
        }
        co_await audit(c, c.userId, "vpn.peer.revoke", "vpn_peer", id, "success", "{}");
        (void)co_await reconcileHub(c);
    }

    template <typename Context>
    ruvia::Task<void> syncPeer(Context& c, std::string_view id) {
        requireUuid(id, "VPN Peer ID 无效");
        using Op = ruvia::DbBinaryOperator;
        ruvia::DbQuery current(c.pool());
        current
            .select({ current.column(service::vpn::entities::VpnPeerEntity::columnName<"edge_node_id">()), current.column(service::vpn::entities::VpnPeerEntity::columnName<"peer_type">()), current.column(service::vpn::entities::VpnPeerEntity::columnName<"status">()), current.cast(current.column(service::vpn::entities::VpnPeerEntity::columnName<"network_id">()), ruvia::DbDataType::kText) })
            .from(service::vpn::entities::VpnPeerEntity::tableName())
            .where(current.binary(current.column(service::vpn::entities::VpnPeerEntity::columnName<"id">()), Op::kEqual, current.cast(current.value(id), ruvia::DbDataType::kUuid)));
        const auto rows = co_await c.db().query(current);
        if (rows.empty()) {
            service::common::fail(21004, "VPN Peer 不存在", 404);
        }
        if (detail::rowValue(rows.front(), 1) != "edge") {
            service::common::fail(21001, "只有 Edge Peer 支持重新下发", 400);
        }
        if (detail::rowValue(rows.front(), 2) == "revoked") {
            service::common::fail(21003, "VPN Peer 已撤销", 409);
        }
        const auto edgeNodeId = detail::rowValue(rows.front(), 0);
        const auto networkId = detail::rowValue(rows.front(), 3);
        auto transaction = co_await c.db().beginTransaction();
        co_await detail::advisoryLock(transaction, c.pool(), 5282804697543808068LL);
        co_await syncEdgeBridgeRoutes(transaction, id, networkId, edgeNodeId, c.userId, *c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>());
        co_await transaction.commit();
        co_await queueEdgeConfig(c, id);
        (void)co_await reconcileHub(c);
    }

    template <typename Context>
    ruvia::Task<void> rotatePeerKey(Context& c, std::string_view id, const VpnPeerKeyInput& payload) {
        using Op = ruvia::DbBinaryOperator;
        requireUuid(id, "VPN Peer ID 无效");
        const auto publicKey = payload.publicKey;
        if (!detail::validKey(publicKey)) {
            service::common::fail(21001, "WireGuard 公钥格式无效", 400);
        }
        ruvia::DbQuery currentQuery(c.pool());
        currentQuery
            .select({ currentQuery.column(service::vpn::entities::VpnPeerEntity::columnName<"public_key">()), currentQuery.column(service::vpn::entities::VpnPeerEntity::columnName<"edge_node_id">()), currentQuery.column(service::vpn::entities::VpnPeerEntity::columnName<"client_managed">()) })
            .from(service::vpn::entities::VpnPeerEntity::tableName())
            .where(currentQuery.binary(currentQuery.column(service::vpn::entities::VpnPeerEntity::columnName<"id">()), Op::kEqual, currentQuery.cast(currentQuery.value(id), ruvia::DbDataType::kUuid)))
            .andWhere(currentQuery.binary(currentQuery.column(service::vpn::entities::VpnPeerEntity::columnName<"status">()), Op::kNotEqual, currentQuery.value("revoked")));
        const auto current = co_await c.db().query(currentQuery);
        if (current.empty()) {
            service::common::fail(21004, "VPN Peer 不存在或已撤销", 404);
        }
        if (detail::rowValue(current.front(), 2) == "t") {
            service::common::fail(21003, "托管客户端密钥只能由客户端管理", 409);
        }
        const auto oldKey = detail::rowValue(current.front(), 0);
        const auto edgeNodeId = detail::rowValue(current.front(), 1);
        ruvia::DbQuery update(c.pool());
        update.update(service::vpn::entities::VpnPeerEntity::tableName())
            .set(service::vpn::entities::VpnPeerEntity::columnName<"public_key">(), update.value(publicKey))
            .set(service::vpn::entities::VpnPeerEntity::columnName<"status">(), update.value("active"))
            .set(service::vpn::entities::VpnPeerEntity::columnName<"revoked_at">(), update.nullValue())
            .set(service::vpn::entities::VpnPeerEntity::columnName<"config_revision">(), update.binary(update.column(service::vpn::entities::VpnPeerEntity::columnName<"config_revision">()), Op::kAdd, update.cast(update.value(1), ruvia::DbDataType::kBigInt)))
            .set(service::vpn::entities::VpnPeerEntity::columnName<"updated_at">(), update.call("now"))
            .where(update.binary(update.column(service::vpn::entities::VpnPeerEntity::columnName<"id">()), Op::kEqual, update.cast(update.value(id), ruvia::DbDataType::kUuid)))
            .andWhere(update.binary(update.column(service::vpn::entities::VpnPeerEntity::columnName<"status">()), Op::kNotEqual, update.value("revoked")))
            .returning({ update.column(service::vpn::entities::VpnPeerEntity::columnName<"public_key">()), update.column(service::vpn::entities::VpnPeerEntity::columnName<"edge_node_id">()) });
        const auto updated = co_await c.db().query(update);
        if (updated.empty()) {
            service::common::fail(21004, "VPN Peer 不存在或已撤销", 404);
        }
        if (detail::validKey(oldKey) && oldKey != publicKey) {
            (void)co_await control(c, "wireguard-remove-peer", oldKey);
        }
        co_await audit(c, c.userId, "vpn.peer.rotate_key", "vpn_peer", id, "success", "{}");
        co_await reconcileHub(c);
        if (!edgeNodeId.empty()) {
            co_await queueEdgeConfig(c, id);
        }
    }

    template <typename Context>
    ruvia::Task<std::string> createEnrollment(Context& c, const VpnEnrollmentInput& payload) {
        const auto defaultNetworkId = co_await ensureDefaultNetwork(c);
        const auto requestedNetworkId = payload.networkId;
        if (requestedNetworkId && !service::common::isUuid(*requestedNetworkId)) {
            service::common::fail(21001, "VPN 网络 ID 无效", 400);
        }
        if (requestedNetworkId && *requestedNetworkId != defaultNetworkId) {
            service::common::fail(21003, "VPN 仅使用默认 iot-server 网络", 409);
        }
        const auto networkId = defaultNetworkId;
        using Op = ruvia::DbBinaryOperator;
        ruvia::DbQuery networkQuery(c.pool());
        networkQuery
            .select(networkQuery.column(service::vpn::entities::VpnNetworkEntity::columnName<"id">()))
            .from(service::vpn::entities::VpnNetworkEntity::tableName())
            .where(networkQuery.binary(networkQuery.column(service::vpn::entities::VpnNetworkEntity::columnName<"id">()), Op::kEqual, networkQuery.cast(networkQuery.value(networkId), ruvia::DbDataType::kUuid)))
            .andWhere(networkQuery.binary(networkQuery.column(service::vpn::entities::VpnNetworkEntity::columnName<"status">()), Op::kEqual, networkQuery.value("enabled")))
            .andWhere(networkQuery.unary(ruvia::DbUnaryOperator::kIsNull, networkQuery.column(service::vpn::entities::VpnNetworkEntity::columnName<"deleted_at">())));
        const auto network = co_await c.db().query(networkQuery);
        if (network.empty()) {
            service::common::fail(21004, "VPN 网络不存在或已停用", 404);
        }
        ruvia::DbQuery routeQuery(c.pool());
        routeQuery
            .select(routeQuery.column(service::vpn::entities::VpnRouteEntity::columnName<"virtual_cidr">(), "r"))
            .from(service::vpn::entities::VpnRouteEntity::tableName(), "r")
            .join(ruvia::DbJoinType::kInner, service::vpn::entities::VpnPeerEntity::tableName(), routeQuery.binary(routeQuery.column(service::vpn::entities::VpnPeerEntity::columnName<"id">(), "p"), Op::kEqual, routeQuery.column(service::vpn::entities::VpnRouteEntity::columnName<"edge_peer_id">(), "r")), "p")
            .join(ruvia::DbJoinType::kInner, service::vpn::entities::EdgeNodeEntity::tableName(), routeQuery.binary(routeQuery.column(service::vpn::entities::EdgeNodeEntity::columnName<"id">(), "e"), Op::kEqual, routeQuery.column(service::vpn::entities::VpnPeerEntity::columnName<"edge_node_id">(), "p")), "e")
            .where(routeQuery.binary(routeQuery.column(service::vpn::entities::VpnRouteEntity::columnName<"network_id">(), "r"), Op::kEqual, routeQuery.cast(routeQuery.value(networkId), ruvia::DbDataType::kUuid)))
            .andWhere(routeQuery.binary(routeQuery.column(service::vpn::entities::VpnRouteEntity::columnName<"enabled">(), "r"), Op::kEqual, routeQuery.value(true)))
            .andWhere(routeQuery.binary(routeQuery.column(service::vpn::entities::VpnRouteEntity::columnName<"status">(), "r"), Op::kEqual, routeQuery.value("active")))
            .andWhere(routeQuery.binary(routeQuery.column(service::vpn::entities::VpnPeerEntity::columnName<"status">(), "p"), Op::kEqual, routeQuery.value("active")))
            .andWhere(routeQuery.binary(routeQuery.column(service::vpn::entities::VpnPeerEntity::columnName<"peer_type">(), "p"), Op::kEqual, routeQuery.value("edge")))
            .andWhere(routeQuery.binary(routeQuery.column(service::vpn::entities::EdgeNodeEntity::columnName<"enrollment_status">(), "e"), Op::kEqual, routeQuery.value("approved")))
            .orderBy(routeQuery.column(service::vpn::entities::VpnRouteEntity::columnName<"virtual_cidr">(), "r"));
        const auto routeRows = co_await c.db().query(routeQuery);
        std::vector<std::string> routes;
        routes.reserve(routeRows.size());
        for (const auto& row : routeRows) {
            routes.push_back(detail::rowValue(row, 0));
        }
        if (routes.empty()) {
            service::common::fail(21003, "当前账户没有可访问的 VPN 设备", 403);
        }
        if (routes.size() > 64) {
            service::common::fail(21001, "当前账户可访问的 VPN 路由超过 64 条", 409);
        }
        co_await validateAllowedRoutes(c, networkId, routes);
        const auto seconds = payload.expiresInSec.value_or(600);
        if (seconds < 60 || seconds > 3600) {
            service::common::fail(21001, "Enrollment 有效期必须在 60 - 3600 秒之间", 400);
        }
        const auto token = detail::randomToken();
        const auto hash = service::utils::sha256(token);
        const auto routesJson = detail::jsonArray(routes);
        const auto id = c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next();
        ruvia::DbQuery insert(c.pool());
        const auto expiryInterval = insert.cast(
            insert.value(std::to_string(seconds) + " seconds"),
            ruvia::DbDataType::kInterval
        );
        insert
            .insertInto(service::vpn::entities::VpnEnrollmentEntity::tableName(), { "id", "token_hash", "network_id", "allowed_routes", "expires_at", "created_by" })
            .values({ insert.cast(insert.value(id), ruvia::DbDataType::kUuid), insert.value(hash), insert.cast(insert.value(networkId), ruvia::DbDataType::kUuid), insert.cast(insert.value(routesJson), ruvia::DbDataType::kJsonb), insert.binary(insert.call("now"), Op::kAdd, expiryInterval), insert.cast(insert.value(c.userId), ruvia::DbDataType::kUuid) });
        (void)co_await c.db().execute(insert);
        co_await audit(c, c.userId, "vpn.enrollment.create", "vpn_enrollment", id, "success", "{}");
        co_return "{\"id\":" + service::utils::jsonQuoted(id) + ",\"token\":" +
            service::utils::jsonQuoted(token) + ",\"expiresInSec\":" +
            std::to_string(seconds) + "}";
    }

    template <typename Context>
    ruvia::Task<std::string> enrollClient(Context& c, const VpnClientEnrollmentInput& payload) {
        using Op = ruvia::DbBinaryOperator;
        const auto token = payload.token;
        const auto publicKey = payload.publicKey;
        if (!detail::validKey(publicKey)) {
            service::common::fail(21001, "WireGuard 公钥格式无效", 400);
        }
        const auto tokenHash = service::utils::sha256(token);
        auto transaction = co_await c.db().beginTransaction();
        co_await detail::advisoryLock(transaction, c.pool(), 5282804697543808068LL);
        ruvia::DbQuery consume(c.pool());
        consume
            .update(service::vpn::entities::VpnEnrollmentEntity::tableName())
            .set(service::vpn::entities::VpnEnrollmentEntity::columnName<"used_at">(), consume.call("now"))
            .where(consume.binary(consume.column(service::vpn::entities::VpnEnrollmentEntity::columnName<"token_hash">()), Op::kEqual, consume.value(tokenHash)))
            .andWhere(consume.unary(ruvia::DbUnaryOperator::kIsNull, consume.column(service::vpn::entities::VpnEnrollmentEntity::columnName<"used_at">())))
            .andWhere(consume.binary(consume.column(service::vpn::entities::VpnEnrollmentEntity::columnName<"expires_at">()), Op::kGreater, consume.call("now")))
            .returning({ consume.column(service::vpn::entities::VpnEnrollmentEntity::columnName<"id">()), consume.column(service::vpn::entities::VpnEnrollmentEntity::columnName<"network_id">()), consume.column(service::vpn::entities::VpnEnrollmentEntity::columnName<"allowed_routes">()), consume.column(service::vpn::entities::VpnEnrollmentEntity::columnName<"created_by">()) });
        const auto enrollment = co_await transaction.query(consume);
        if (enrollment.empty()) {
            // An enrollment owns its peer ID so a committed request can be
            // retried with the same key after a lost response or Hub failure.
            ruvia::DbQuery retryQuery(c.pool());
            retryQuery
                .select({ retryQuery.cast(retryQuery.column(service::vpn::entities::VpnPeerEntity::columnName<"id">(), "p"), ruvia::DbDataType::kText), retryQuery.cast(retryQuery.column(service::vpn::entities::VpnPeerEntity::columnName<"user_id">(), "p"), ruvia::DbDataType::kText) })
                .from(service::vpn::entities::VpnEnrollmentEntity::tableName(), "e")
                .join(ruvia::DbJoinType::kInner, service::vpn::entities::VpnPeerEntity::tableName(), retryQuery.binary(retryQuery.column(service::vpn::entities::VpnPeerEntity::columnName<"id">(), "p"), Op::kEqual, retryQuery.column(service::vpn::entities::VpnEnrollmentEntity::columnName<"id">(), "e")), "p")
                .where(retryQuery.binary(retryQuery.column(service::vpn::entities::VpnEnrollmentEntity::columnName<"token_hash">(), "e"), Op::kEqual, retryQuery.value(tokenHash)))
                .andWhere(retryQuery.unary(ruvia::DbUnaryOperator::kIsNotNull, retryQuery.column(service::vpn::entities::VpnEnrollmentEntity::columnName<"used_at">(), "e")))
                .andWhere(retryQuery.binary(retryQuery.column(service::vpn::entities::VpnEnrollmentEntity::columnName<"expires_at">(), "e"), Op::kGreater, retryQuery.call("now")))
                .andWhere(retryQuery.binary(retryQuery.column(service::vpn::entities::VpnPeerEntity::columnName<"public_key">(), "p"), Op::kEqual, retryQuery.value(publicKey)))
                .andWhere(retryQuery.binary(retryQuery.column(service::vpn::entities::VpnPeerEntity::columnName<"peer_type">(), "p"), Op::kEqual, retryQuery.value("windows")))
                .andWhere(retryQuery.binary(retryQuery.column(service::vpn::entities::VpnPeerEntity::columnName<"status">(), "p"), Op::kEqual, retryQuery.value("active")));
            const auto retry = co_await transaction.query(retryQuery);
            if (!retry.empty()) {
                const auto peerId = detail::rowValue(retry.front(), 0);
                const auto userId = detail::rowValue(retry.front(), 1);
                co_await transaction.commit();
                co_await reconcileHub(c);
                co_return co_await clientConfigJson(c, peerId, userId);
            }
            service::common::fail(21006, "Enrollment token 无效、已使用或已过期", 401);
        }
        const auto networkId = detail::rowValue(enrollment.front(), 1);
        ruvia::DbQuery networkQuery(c.pool());
        networkQuery
            .select(networkQuery.column(service::vpn::entities::VpnNetworkEntity::columnName<"overlay_cidr">()))
            .from(service::vpn::entities::VpnNetworkEntity::tableName())
            .where(networkQuery.binary(networkQuery.column(service::vpn::entities::VpnNetworkEntity::columnName<"id">()), Op::kEqual, networkQuery.cast(networkQuery.value(networkId), ruvia::DbDataType::kUuid)))
            .andWhere(networkQuery.binary(networkQuery.column(service::vpn::entities::VpnNetworkEntity::columnName<"status">()), Op::kEqual, networkQuery.value("enabled")))
            .andWhere(networkQuery.unary(ruvia::DbUnaryOperator::kIsNull, networkQuery.column(service::vpn::entities::VpnNetworkEntity::columnName<"deleted_at">())));
        const auto network = co_await transaction.query(networkQuery);
        if (network.empty()) {
            service::common::fail(21004, "VPN 网络不存在或已停用", 409);
        }
        const auto overlay = cidr::parseCidr(detail::rowValue(network.front(), 0), 16, 30);
        if (!overlay) {
            service::common::fail(21005, "VPN 网络 Overlay 配置损坏", 500);
        }
        const auto assigned = co_await allocateAddressFromDb(transaction, networkId, *overlay);
        const auto name = payload.name.value_or("Windows client");
        if (name.empty() || name.size() > 100) {
            service::common::fail(21001, "Peer 名称长度无效", 400);
        }
        const auto id = detail::rowValue(enrollment.front(), 0);
        const auto creatorId = detail::rowValue(enrollment.front(), 3);
        const auto assignedAddress = detail::hostText(*assigned);
        const auto allowedRoutesJson = detail::rowValue(enrollment.front(), 2);
        ruvia::DbQuery insert(c.pool());
        insert
            .insertInto(service::vpn::entities::VpnPeerEntity::tableName(), { "id", "network_id", "peer_type", "user_id", "name", "public_key", "assigned_ipv4", "allowed_routes", "status" })
            .values({ insert.cast(insert.value(id), ruvia::DbDataType::kUuid), insert.cast(insert.value(networkId), ruvia::DbDataType::kUuid), insert.value("windows"), insert.cast(insert.value(creatorId), ruvia::DbDataType::kUuid), insert.value(name), insert.value(publicKey), insert.cast(insert.value(assignedAddress), ruvia::DbDataType::kInet), insert.cast(insert.value(allowedRoutesJson), ruvia::DbDataType::kJsonb), insert.value("active") });
        (void)co_await transaction.execute(insert);
        co_await transaction.commit();
        (void)co_await reconcileHub(c);
        co_return co_await clientConfigJson(c, id, detail::rowValue(enrollment.front(), 3));
    }

    template <typename Context>
    ruvia::Task<std::string> sessions(Context& c) {
        using Op = ruvia::DbBinaryOperator;
        ruvia::DbQuery query(c.pool());
        const auto staleWindow = query.cast(query.value("3 minutes"), ruvia::DbDataType::kInterval);
        const auto online = query.binary(
            query.column(service::vpn::entities::VpnPeerEntity::columnName<"last_handshake_at">(), "p"),
            Op::kGreater,
            query.binary(query.call("now"), Op::kSubtract, staleWindow)
        );
        query
            .select(query.call(
                "jsonb_build_object",
                { detail::jsonKey(query, "peerId"), query.column(service::vpn::entities::VpnPeerEntity::columnName<"id">(), "p"), detail::jsonKey(query, "networkId"), query.column(service::vpn::entities::VpnPeerEntity::columnName<"network_id">(), "p"), detail::jsonKey(query, "name"), query.column(service::vpn::entities::VpnPeerEntity::columnName<"name">(), "p"), detail::jsonKey(query, "peerType"), query.column(service::vpn::entities::VpnPeerEntity::columnName<"peer_type">(), "p"), detail::jsonKey(query, "assignedIpv4"), query.call("host", { query.column(service::vpn::entities::VpnPeerEntity::columnName<"assigned_ipv4">(), "p") }), detail::jsonKey(query, "status"), query.column(service::vpn::entities::VpnPeerEntity::columnName<"status">(), "p"), detail::jsonKey(query, "lastHandshakeAt"), query.call("iot_utc_timestamp", { query.column(service::vpn::entities::VpnPeerEntity::columnName<"last_handshake_at">(), "p") }), detail::jsonKey(query, "online"), query.caseWhen({ { online, query.cast(query.value(true), ruvia::DbDataType::kBoolean) } }, query.cast(query.value(false), ruvia::DbDataType::kBoolean)) }
            ))
            .from(service::vpn::entities::VpnPeerEntity::tableName(), "p")
            .where(query.binary(query.column(service::vpn::entities::VpnPeerEntity::columnName<"status">(), "p"), Op::kEqual, query.value("active")))
            .orderBy(query.column(service::vpn::entities::VpnPeerEntity::columnName<"updated_at">(), "p"), ruvia::DbOrderDirection::kDesc)
            .limit(1000);
        const auto rows = co_await c.db().query(query);
        std::string result{ "[" };
        for (std::size_t index = 0; index < rows.size(); ++index) {
            if (index != 0) {
                result.push_back(',');
            }
            result += detail::rowValue(rows[index], 0);
        }
        result.push_back(']');
        co_return result;
    }

    template <typename Context>
    ruvia::Task<std::string> diagnostics(Context& c) {
        auto runtime = co_await control(c, "wireguard-status", std::string{});
        if (runtime.size() < 2 || runtime.front() != '{' || runtime.back() != '}') {
            service::common::fail(21005, "VPN 运行状态响应无效", 502);
        }
        runtime.pop_back();
        using Op = ruvia::DbBinaryOperator;
        ruvia::DbQuery peerCounts(c.pool());
        const auto activePeers = peerCounts.filter(
            peerCounts.aggregate("count", { peerCounts.star() }),
            peerCounts.binary(peerCounts.column(service::vpn::entities::VpnPeerEntity::columnName<"status">()), Op::kEqual, peerCounts.value("active"))
        );
        const auto revokedPeers = peerCounts.filter(
            peerCounts.aggregate("count", { peerCounts.star() }),
            peerCounts.binary(peerCounts.column(service::vpn::entities::VpnPeerEntity::columnName<"status">()), Op::kEqual, peerCounts.value("revoked"))
        );
        peerCounts.select({ activePeers, revokedPeers }).from(service::vpn::entities::VpnPeerEntity::tableName());
        ruvia::DbQuery routeCounts(c.pool());
        const auto enabledRoutes = routeCounts.filter(
            routeCounts.aggregate("count", { routeCounts.star() }),
            routeCounts.binary(routeCounts.column(service::vpn::entities::VpnRouteEntity::columnName<"enabled">()), Op::kEqual, routeCounts.value(true))
        );
        const auto errorRoutes = routeCounts.filter(
            routeCounts.aggregate("count", { routeCounts.star() }),
            routeCounts.binary(routeCounts.column(service::vpn::entities::VpnRouteEntity::columnName<"status">()), Op::kEqual, routeCounts.value("error"))
        );
        routeCounts.select({ enabledRoutes, errorRoutes }).from(service::vpn::entities::VpnRouteEntity::tableName());
        const auto peers = co_await c.db().query(peerCounts);
        const auto routes = co_await c.db().query(routeCounts);
        runtime += ",\"activePeerCount\":" + detail::rowValue(peers.front(), 0) +
            ",\"revokedPeerCount\":" + detail::rowValue(peers.front(), 1) +
            ",\"enabledRouteCount\":" + detail::rowValue(routes.front(), 0) +
            ",\"errorRouteCount\":" + detail::rowValue(routes.front(), 1) + "}";
        co_return runtime;
    }

    template <typename Context>
    ruvia::Task<std::string> desktopDevices(Context& c);
    template <typename Context>
    ruvia::Task<std::string> desktopCreatePeer(Context& c, const VpnDesktopPeerInput& payload);
    template <typename Context>
    ruvia::Task<std::string> desktopUpdatePeer(Context& c, std::string_view id, const VpnDesktopSelectionInput& payload);
    template <typename Context>
    ruvia::Task<std::string> desktopPeerConfig(Context& c, std::string_view id);
    template <typename Context>
    ruvia::Task<void> desktopDeletePeer(Context& c, std::string_view id);

  private:
};

inline std::int64_t VpnService::integer(std::string_view value, std::int64_t fallback) {
    std::int64_t result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    return error == std::errc{} && end == value.data() + value.size() ? result : fallback;
}

inline void VpnService::requireUuid(std::string_view id, std::string_view message) {
    if (!service::common::isUuid(id)) {
        service::common::fail(21001, std::string(message), 400);
    }
}

template <typename Context>
ruvia::Task<std::string> VpnService::ensureDefaultNetwork(Context& c) {
    const auto hubPublicKey =
        std::string(c.env().get("VPN_HUB_PUBLIC_KEY").value_or(""));
    const auto hubEndpoint =
        std::string(c.env().get("VPN_HUB_ENDPOINT").value_or(""));
    const auto hubListenPort = c.env().template get<std::uint16_t>("VPN_HUB_LISTEN_PORT").value_or(51820);
    auto transaction = co_await c.db().beginTransaction();
    co_await detail::advisoryLock(transaction, c.pool(), 5282804697543808067LL);
    using Op = ruvia::DbBinaryOperator;
    ruvia::DbQuery existingQuery(c.pool());
    existingQuery
        .select(existingQuery.cast(existingQuery.column(service::vpn::entities::VpnNetworkEntity::columnName<"id">()), ruvia::DbDataType::kText))
        .from(service::vpn::entities::VpnNetworkEntity::tableName())
        .where(existingQuery.binary(existingQuery.column(service::vpn::entities::VpnNetworkEntity::columnName<"name">()), Op::kEqual, existingQuery.value(kDefaultNetworkName)))
        .andWhere(existingQuery.unary(ruvia::DbUnaryOperator::kIsNull, existingQuery.column(service::vpn::entities::VpnNetworkEntity::columnName<"deleted_at">())))
        .limit(1);
    const auto existing = co_await transaction.query(existingQuery);
    std::string networkId = std::string(kDefaultNetworkId);
    if (!existing.empty()) {
        networkId = detail::rowValue(existing.front(), 0);
    } else {
        ruvia::DbQuery insert(c.pool());
        insert
            .insertInto(service::vpn::entities::VpnNetworkEntity::tableName(), { "id", "name", "overlay_cidr", "hub_public_key", "hub_endpoint", "hub_listen_port", "created_by" })
            .values({ insert.cast(insert.value(kDefaultNetworkId), ruvia::DbDataType::kUuid), insert.value(kDefaultNetworkName), insert.value(kDefaultOverlayCidr), insert.value(hubPublicKey), insert.value(hubEndpoint), insert.value(static_cast<int>(hubListenPort)), insert.cast(insert.value(c.userId), ruvia::DbDataType::kUuid) })
            .onConflict({ .columns = { "id" }, .update = { { "name", insert.excluded(service::vpn::entities::VpnNetworkEntity::columnName<"name">()) }, { "overlay_cidr", insert.excluded(service::vpn::entities::VpnNetworkEntity::columnName<"overlay_cidr">()) }, { "deleted_at", insert.nullValue() }, { "updated_at", insert.call("now") } } })
            .returning({ insert.cast(insert.column(service::vpn::entities::VpnNetworkEntity::columnName<"id">()), ruvia::DbDataType::kText) });
        const auto inserted = co_await transaction.query(insert);
        if (inserted.empty()) {
            service::common::fail(21005, "默认 iot-server VPN 网络创建失败", 500);
        }
        networkId = detail::rowValue(inserted.front(), 0);
    }
    ruvia::DbQuery update(c.pool());
    update
        .update(service::vpn::entities::VpnNetworkEntity::tableName())
        .set(service::vpn::entities::VpnNetworkEntity::columnName<"name">(), update.value(kDefaultNetworkName))
        .set(service::vpn::entities::VpnNetworkEntity::columnName<"overlay_cidr">(), update.value(kDefaultOverlayCidr))
        .set(service::vpn::entities::VpnNetworkEntity::columnName<"status">(), update.value("enabled"))
        .set(service::vpn::entities::VpnNetworkEntity::columnName<"deleted_at">(), update.nullValue())
        .set(service::vpn::entities::VpnNetworkEntity::columnName<"updated_at">(), update.call("now"))
        .where(update.binary(update.column(service::vpn::entities::VpnNetworkEntity::columnName<"id">()), Op::kEqual, update.cast(update.value(networkId), ruvia::DbDataType::kUuid)));
    (void)co_await transaction.execute(update);
    co_await transaction.commit();
    co_return networkId;
}

template <typename Db>
ruvia::Task<std::optional<std::uint32_t>> VpnService::allocateAddressFromDb(
    Db& db,
    std::string_view networkId,
    const cidr::Ipv4Cidr& overlay
) {
    using Op = ruvia::DbBinaryOperator;
    ruvia::DbQuery query;
    const auto reusable = query.binary(
        query.binary(query.column(service::vpn::entities::VpnPeerEntity::columnName<"peer_type">()), Op::kNotEqual, query.value("windows")),
        Op::kOr,
        query.binary(query.column(service::vpn::entities::VpnPeerEntity::columnName<"status">()), Op::kNotEqual, query.value("revoked"))
    );
    query
        .select(query.call("host", { query.column(service::vpn::entities::VpnPeerEntity::columnName<"assigned_ipv4">()) }))
        .from(service::vpn::entities::VpnPeerEntity::tableName())
        .where(query.binary(query.column(service::vpn::entities::VpnPeerEntity::columnName<"network_id">()), Op::kEqual, query.cast(query.value(networkId), ruvia::DbDataType::kUuid)))
        .andWhere(reusable);
    const auto rows = co_await db.query(query);
    std::unordered_set<std::uint32_t> used;
    for (const auto& row : rows) {
        if (const auto value = cidr::parseIpv4(detail::rowValue(row, 0))) {
            used.emplace(*value);
        }
    }
    for (std::uint32_t offset = 2; offset + 1 < overlay.size(); ++offset) {
        const auto candidate = *cidr::hostAddress(overlay, offset);
        if (!used.contains(candidate)) {
            co_return candidate;
        }
    }
    service::common::fail(21007, "VPN Overlay 地址池已耗尽", 409);
}

template <typename Context>
ruvia::Task<void> VpnService::validateAllowedRoutes(
    Context& c,
    std::string_view networkId,
    const std::vector<std::string>& routes
) {
    if (routes.empty()) {
        co_return;
    }
    for (const auto& route : routes) {
        if (!cidr::parseCidr(route, 1, 30)) {
            service::common::fail(21001, "Peer 授权路由 CIDR 无效", 400);
        }
    }
    using Op = ruvia::DbBinaryOperator;
    ruvia::DbQuery query(c.pool());
    query
        .select(query.column(service::vpn::entities::VpnRouteEntity::columnName<"virtual_cidr">()))
        .from(service::vpn::entities::VpnRouteEntity::tableName())
        .where(query.binary(query.column(service::vpn::entities::VpnRouteEntity::columnName<"network_id">()), Op::kEqual, query.cast(query.value(networkId), ruvia::DbDataType::kUuid)))
        .andWhere(query.binary(query.column(service::vpn::entities::VpnRouteEntity::columnName<"enabled">()), Op::kEqual, query.value(true)));
    const auto rows = co_await c.db().query(query);
    std::unordered_set<std::string> allowed;
    for (const auto& row : rows) {
        allowed.emplace(detail::rowValue(row, 0));
    }
    for (const auto& route : routes) {
        if (!allowed.contains(route)) {
            service::common::fail(21003, "Peer 授权路由不属于该 VPN 网络", 403);
        }
    }
}

template <typename Context>
ruvia::Task<std::string> VpnService::clientConfigJson(Context& c, std::string_view peerId, std::string_view userId) {
    using Op = ruvia::DbBinaryOperator;
    requireUuid(peerId, "VPN Peer ID 无效");
    ruvia::DbQuery routes(c.pool());
    const std::array routeOrder{
        ruvia::DbOrderTerm{ routes.column(service::vpn::entities::VpnRouteEntity::columnName<"virtual_cidr">(), "r") }
    };
    routes
        .select(routes.aggregate("jsonb_agg", { routes.column(service::vpn::entities::VpnRouteEntity::columnName<"virtual_cidr">(), "r") }, false, routeOrder))
        .from(service::vpn::entities::VpnRouteEntity::tableName(), "r")
        .join(ruvia::DbJoinType::kInner, service::vpn::entities::VpnPeerEntity::tableName(), routes.binary(routes.column(service::vpn::entities::VpnPeerEntity::columnName<"id">(), "edge_peer"), Op::kEqual, routes.column(service::vpn::entities::VpnRouteEntity::columnName<"edge_peer_id">(), "r")), "edge_peer")
        .join(ruvia::DbJoinType::kInner, service::vpn::entities::EdgeNodeEntity::tableName(), routes.binary(routes.column(service::vpn::entities::EdgeNodeEntity::columnName<"id">(), "e"), Op::kEqual, routes.column(service::vpn::entities::VpnPeerEntity::columnName<"edge_node_id">(), "edge_peer")), "e")
        .where(routes.binary(routes.column(service::vpn::entities::VpnRouteEntity::columnName<"network_id">(), "r"), Op::kEqual, routes.column(service::vpn::entities::VpnNetworkEntity::columnName<"id">(), "n")))
        .andWhere(routes.binary(routes.column(service::vpn::entities::VpnRouteEntity::columnName<"enabled">(), "r"), Op::kEqual, routes.value(true)))
        .andWhere(routes.binary(routes.column(service::vpn::entities::VpnRouteEntity::columnName<"status">(), "r"), Op::kEqual, routes.value("active")))
        .andWhere(routes.binary(routes.column(service::vpn::entities::VpnPeerEntity::columnName<"status">(), "edge_peer"), Op::kEqual, routes.value("active")))
        .andWhere(routes.binary(routes.column(service::vpn::entities::EdgeNodeEntity::columnName<"enrollment_status">(), "e"), Op::kEqual, routes.value("approved")));
    ruvia::DbQuery query(c.pool());
    const auto emptyRoutes = query.cast(query.value("[]"), ruvia::DbDataType::kJsonb);
    query
        .select({ query.column(service::vpn::entities::VpnPeerEntity::columnName<"id">(), "p"), query.column(service::vpn::entities::VpnPeerEntity::columnName<"name">(), "p"), query.call("host", { query.column(service::vpn::entities::VpnPeerEntity::columnName<"assigned_ipv4">(), "p") }), query.cast(query.column(service::vpn::entities::VpnPeerEntity::columnName<"allowed_routes">(), "p"), ruvia::DbDataType::kText), query.column(service::vpn::entities::VpnNetworkEntity::columnName<"hub_public_key">(), "n"), query.column(service::vpn::entities::VpnNetworkEntity::columnName<"hub_endpoint">(), "n"), query.column(service::vpn::entities::VpnNetworkEntity::columnName<"hub_listen_port">(), "n"), query.column(service::vpn::entities::VpnNetworkEntity::columnName<"status">(), "n"), query.column(service::vpn::entities::VpnPeerEntity::columnName<"status">(), "p"), query.column(service::vpn::entities::VpnPeerEntity::columnName<"user_id">(), "p"), query.cast(query.coalesce({ query.subquery(routes), emptyRoutes }), ruvia::DbDataType::kText) })
        .from(service::vpn::entities::VpnPeerEntity::tableName(), "p")
        .join(ruvia::DbJoinType::kInner, service::vpn::entities::VpnNetworkEntity::tableName(), query.binary(query.column(service::vpn::entities::VpnNetworkEntity::columnName<"id">(), "n"), Op::kEqual, query.column(service::vpn::entities::VpnPeerEntity::columnName<"network_id">(), "p")), "n")
        .where(query.binary(query.column(service::vpn::entities::VpnPeerEntity::columnName<"id">(), "p"), Op::kEqual, query.cast(query.value(peerId), ruvia::DbDataType::kUuid)))
        .andWhere(query.binary(query.column(service::vpn::entities::VpnPeerEntity::columnName<"peer_type">(), "p"), Op::kEqual, query.value("windows")))
        .andWhere(query.binary(query.column(service::vpn::entities::VpnPeerEntity::columnName<"user_id">(), "p"), Op::kEqual, query.cast(query.value(userId), ruvia::DbDataType::kUuid)))
        .limit(1);
    const auto rows = co_await c.db().query(query);
    if (rows.empty()) {
        service::common::fail(21004, "Windows VPN Peer 不存在或不属于当前用户", 404);
    }
    const auto& row = rows.front();
    if (detail::rowValue(row, 7) != "enabled" || detail::rowValue(row, 8) != "active") {
        service::common::fail(21003, "VPN Peer 当前不可用", 409);
    }
    const auto hubKey = detail::rowValue(row, 4);
    if (!detail::validKey(hubKey)) {
        service::common::fail(21005, "Hub 公钥尚未配置", 503);
    }
    const auto allowed = detail::rowValue(row, 10);
    auto allowedValues = detail::textArrayJson(allowed);
    if (allowedValues.empty()) {
        service::common::fail(21008, "VPN 当前没有可用虚拟网段", 409);
    }
    const auto endpoint = detail::rowValue(row, 5);
    const auto port = detail::rowValue(row, 6);
    const auto portValue = integer(port);
    if (endpoint.empty() || portValue < 1 || portValue > 65535) {
        service::common::fail(21005, "Hub 公网端点尚未配置", 503);
    }
    const auto config = detail::renderClientConfig(
        "<client-private-key>",
        detail::rowValue(row, 2),
        hubKey,
        endpoint,
        static_cast<std::uint16_t>(portValue),
        allowedValues
    );
    co_return "{\"peerId\":" + service::utils::jsonQuoted(detail::rowValue(row, 0)) +
        ",\"name\":" + service::utils::jsonQuoted(detail::rowValue(row, 1)) +
        ",\"assignedIpv4\":" + service::utils::jsonQuoted(detail::rowValue(row, 2)) +
        ",\"allowedRoutes\":" + detail::jsonArray(allowedValues) +
        ",\"config\":" + service::utils::jsonQuoted(config) + "}";
}

template <typename Context>
ruvia::Task<std::string> VpnService::desktopDevices(Context& c) {
    using Op = ruvia::DbBinaryOperator;
    ruvia::DbQuery routeQuery(c.pool());
    const std::array routeOrder{
        ruvia::DbOrderTerm{ routeQuery.column(service::vpn::entities::VpnRouteEntity::columnName<"virtual_cidr">(), "route") }
    };
    routeQuery
        .select(routeQuery.aggregate("jsonb_agg", { routeQuery.column(service::vpn::entities::VpnRouteEntity::columnName<"virtual_cidr">(), "route") }, false, routeOrder))
        .from(service::vpn::entities::VpnRouteEntity::tableName(), "route")
        .where(routeQuery.binary(routeQuery.column(service::vpn::entities::VpnRouteEntity::columnName<"edge_peer_id">(), "route"), Op::kEqual, routeQuery.column(service::vpn::entities::VpnPeerEntity::columnName<"id">(), "peer")))
        .andWhere(routeQuery.binary(routeQuery.column(service::vpn::entities::VpnRouteEntity::columnName<"network_id">(), "route"), Op::kEqual, routeQuery.column(service::vpn::entities::VpnNetworkEntity::columnName<"id">(), "network")))
        .andWhere(routeQuery.binary(routeQuery.column(service::vpn::entities::VpnRouteEntity::columnName<"enabled">(), "route"), Op::kEqual, routeQuery.value(true)))
        .andWhere(routeQuery.binary(routeQuery.column(service::vpn::entities::VpnRouteEntity::columnName<"status">(), "route"), Op::kEqual, routeQuery.value("active")));
    ruvia::DbQuery query(c.pool());
    const auto emptyRoutes = query.cast(query.value("[]"), ruvia::DbDataType::kJsonb);
    query
        .select({ query.call("jsonb_build_object", { detail::jsonKey(query, "id"), query.column(service::vpn::entities::EdgeNodeEntity::columnName<"id">(), "edge"), detail::jsonKey(query, "name"), query.coalesce({ query.column(service::vpn::entities::EdgeNodeEntity::columnName<"name">(), "edge"), query.value("") }), detail::jsonKey(query, "imei"), query.column(service::vpn::entities::EdgeNodeEntity::columnName<"imei">(), "edge"), detail::jsonKey(query, "virtualCidrs"), query.coalesce({ query.subquery(routeQuery), emptyRoutes }), detail::jsonKey(query, "assignedIpv4"), query.call("host", { query.column(service::vpn::entities::VpnPeerEntity::columnName<"assigned_ipv4">(), "peer") }) }), query.cast(query.column(service::vpn::entities::EdgeNodeEntity::columnName<"id">(), "edge"), ruvia::DbDataType::kText) })
        .from(service::vpn::entities::EdgeNodeEntity::tableName(), "edge")
        .join(ruvia::DbJoinType::kInner, service::vpn::entities::VpnPeerEntity::tableName(), query.binary(query.column(service::vpn::entities::VpnPeerEntity::columnName<"edge_node_id">(), "peer"), Op::kEqual, query.column(service::vpn::entities::EdgeNodeEntity::columnName<"id">(), "edge")), "peer")
        .join(ruvia::DbJoinType::kInner, service::vpn::entities::VpnNetworkEntity::tableName(), query.binary(query.column(service::vpn::entities::VpnNetworkEntity::columnName<"id">(), "network"), Op::kEqual, query.column(service::vpn::entities::VpnPeerEntity::columnName<"network_id">(), "peer")), "network")
        .where(query.binary(query.column(service::vpn::entities::VpnPeerEntity::columnName<"peer_type">(), "peer"), Op::kEqual, query.value("edge")))
        .andWhere(query.binary(query.column(service::vpn::entities::VpnPeerEntity::columnName<"status">(), "peer"), Op::kEqual, query.value("active")))
        .andWhere(query.binary(query.column(service::vpn::entities::EdgeNodeEntity::columnName<"enrollment_status">(), "edge"), Op::kEqual, query.value("approved")))
        .andWhere(query.binary(query.column(service::vpn::entities::VpnNetworkEntity::columnName<"name">(), "network"), Op::kEqual, query.value(kDefaultNetworkName)))
        .andWhere(query.binary(query.column(service::vpn::entities::VpnNetworkEntity::columnName<"status">(), "network"), Op::kEqual, query.value("enabled")))
        .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull, query.column(service::vpn::entities::VpnNetworkEntity::columnName<"deleted_at">(), "network")))
        .orderBy(query.coalesce({ query.column(service::vpn::entities::EdgeNodeEntity::columnName<"name">(), "edge"), query.value("") }))
        .addOrderBy(query.column(service::vpn::entities::EdgeNodeEntity::columnName<"id">(), "edge"));
    const auto rows = co_await c.db().query(query);
    std::string result{ "[" };
    for (std::size_t index = 0; index < rows.size(); ++index) {
        if (index) {
            result += ',';
        }
        // Use the same live session source as EdgeService::fillNode. Database
        // telemetry timestamps do not represent the lifetime of a WebSocket.
        const auto session = co_await c.redis().get("iot:edge:session:" + detail::rowValue(rows[index], 1));
        const auto device = detail::rowValue(rows[index], 0);
        result += std::string("{\"online\":") + (session.has_value() ? "true," : "false,") + device.substr(1);
    }
    result += ']';
    co_return result;
}

template <typename Context>
ruvia::Task<std::string> VpnService::desktopCreatePeer(Context& c, const VpnDesktopPeerInput& payload) {
    const auto name = payload.name;
    const auto publicKey = payload.publicKey;
    if (!detail::validManagedKey(publicKey)) {
        service::common::fail(21001, "WireGuard 公钥格式无效", 400);
    }
    const auto ids = payload.edgeNodeIds;
    auto tx = co_await c.db().beginTransaction();
    // Serialize allocation and the public-key retry lookup together. A lost POST
    // response can safely be retried with the locally persisted keypair.
    co_await detail::advisoryLock(tx, c.pool(), 5282804697543808068LL);
    using Op = ruvia::DbBinaryOperator;
    ruvia::DbQuery existingQuery(c.pool());
    existingQuery
        .select({ existingQuery.cast(existingQuery.column(service::vpn::entities::VpnPeerEntity::columnName<"id">()), ruvia::DbDataType::kText), existingQuery.cast(existingQuery.column(service::vpn::entities::VpnPeerEntity::columnName<"user_id">()), ruvia::DbDataType::kText), existingQuery.column(service::vpn::entities::VpnPeerEntity::columnName<"status">()), existingQuery.column(service::vpn::entities::VpnPeerEntity::columnName<"client_managed">()) })
        .from(service::vpn::entities::VpnPeerEntity::tableName())
        .where(existingQuery.binary(existingQuery.column(service::vpn::entities::VpnPeerEntity::columnName<"public_key">()), Op::kEqual, existingQuery.value(publicKey)))
        .lock({ .mode = ruvia::DbRowLock::kUpdate });
    const auto existing = co_await tx.query(existingQuery);
    if (!existing.empty()) {
        const auto& row = existing.front();
        if (detail::rowValue(row, 1) != c.userId || detail::rowValue(row, 3) != "t") {
            service::common::fail(21002, "该 WireGuard 公钥已经被使用", 409);
        }
        if (detail::rowValue(row, 2) == "revoked") {
            service::common::fail(21009, "该 Windows VPN 注册已撤销", 410);
        }
        if (detail::rowValue(row, 2) != "active") {
            service::common::fail(21002, "该 WireGuard 公钥已经被使用", 409);
        }
        const auto id = detail::rowValue(row, 0);
        co_await tx.commit();
        co_await reconcileHub(c);
        // POST retries recover the original registration even if an Edge has
        // since been removed. Selection changes use the dedicated PATCH route.
        co_return co_await desktopPeerConfig(c, id);
    }
    ruvia::DbQuery networkQuery(c.pool());
    networkQuery
        .select({ networkQuery.cast(networkQuery.column(service::vpn::entities::VpnNetworkEntity::columnName<"id">()), ruvia::DbDataType::kText), networkQuery.column(service::vpn::entities::VpnNetworkEntity::columnName<"overlay_cidr">()) })
        .from(service::vpn::entities::VpnNetworkEntity::tableName())
        .where(networkQuery.binary(networkQuery.column(service::vpn::entities::VpnNetworkEntity::columnName<"name">()), Op::kEqual, networkQuery.value(kDefaultNetworkName)))
        .andWhere(networkQuery.binary(networkQuery.column(service::vpn::entities::VpnNetworkEntity::columnName<"status">()), Op::kEqual, networkQuery.value("enabled")))
        .andWhere(networkQuery.unary(ruvia::DbUnaryOperator::kIsNull, networkQuery.column(service::vpn::entities::VpnNetworkEntity::columnName<"deleted_at">())))
        .lock({ .mode = ruvia::DbRowLock::kShare });
    const auto network = co_await tx.query(networkQuery);
    if (network.empty()) {
        service::common::fail(21004, "VPN 网络不存在或已停用", 404);
    }
    const auto networkId = detail::rowValue(network.front(), 0);
    const auto overlay = cidr::parseCidr(detail::rowValue(network.front(), 1), 16, 30);
    if (!overlay) {
        service::common::fail(21005, "VPN 网络 Overlay 配置损坏", 500);
    }
    co_await detail::validateSelectedEdges(tx, networkId, ids);
    const auto id = c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next();
    const auto assigned = co_await allocateAddressFromDb(tx, networkId, *overlay);
    if (!assigned) {
        service::common::fail(21002, "VPN 网络地址已用尽", 409);
    }
    const auto assignedText = detail::hostText(*assigned);
    ruvia::DbQuery insert(c.pool());
    insert
        .insertInto(service::vpn::entities::VpnPeerEntity::tableName(), { "id", "network_id", "peer_type", "user_id", "name", "public_key", "assigned_ipv4", "allowed_routes", "status", "client_managed" })
        .values({ insert.cast(insert.value(id), ruvia::DbDataType::kUuid), insert.cast(insert.value(networkId), ruvia::DbDataType::kUuid), insert.value("windows"), insert.cast(insert.value(c.userId), ruvia::DbDataType::kUuid), insert.value(name), insert.value(publicKey), insert.cast(insert.value(assignedText), ruvia::DbDataType::kInet), insert.cast(insert.value("[]"), ruvia::DbDataType::kJsonb), insert.value("active"), insert.value(true) });
    (void)co_await tx.execute(insert);
    for (const auto& edge : ids) {
        ruvia::DbQuery selection(c.pool());
        selection
            .insertInto(service::vpn::entities::VpnPeerEdgeSelectionEntity::tableName(), { "peer_id", "edge_node_id" })
            .values({ selection.cast(selection.value(id), ruvia::DbDataType::kUuid), selection.cast(selection.value(edge), ruvia::DbDataType::kUuid) });
        (void)co_await tx.execute(selection);
    }
    co_await tx.commit();
    co_await audit(c, c.userId, "vpn.desktop.enroll", "vpn_peer", id, "success", "{}");
    (void)co_await reconcileHub(c);
    co_return co_await desktopPeerConfig(c, id);
}

template <typename Context>
ruvia::Task<std::string> VpnService::desktopUpdatePeer(Context& c, std::string_view id, const VpnDesktopSelectionInput& payload) {
    requireUuid(id, "VPN Peer ID 无效");
    const auto ids = payload.edgeNodeIds;
    auto tx = co_await c.db().beginTransaction();
    // Lock the parent before replacing children so concurrent PATCH requests
    // cannot merge two selections or race revocation.
    using Op = ruvia::DbBinaryOperator;
    ruvia::DbQuery parent(c.pool());
    parent
        .select(parent.cast(parent.column(service::vpn::entities::VpnPeerEntity::columnName<"network_id">()), ruvia::DbDataType::kText))
        .from(service::vpn::entities::VpnPeerEntity::tableName())
        .where(parent.binary(parent.column(service::vpn::entities::VpnPeerEntity::columnName<"id">()), Op::kEqual, parent.cast(parent.value(id), ruvia::DbDataType::kUuid)))
        .andWhere(parent.binary(parent.column(service::vpn::entities::VpnPeerEntity::columnName<"user_id">()), Op::kEqual, parent.cast(parent.value(c.userId), ruvia::DbDataType::kUuid)))
        .andWhere(parent.binary(parent.column(service::vpn::entities::VpnPeerEntity::columnName<"peer_type">()), Op::kEqual, parent.value("windows")))
        .andWhere(parent.binary(parent.column(service::vpn::entities::VpnPeerEntity::columnName<"client_managed">()), Op::kEqual, parent.value(true)))
        .andWhere(parent.binary(parent.column(service::vpn::entities::VpnPeerEntity::columnName<"status">()), Op::kEqual, parent.value("active")))
        .lock({ .mode = ruvia::DbRowLock::kUpdate });
    const auto rows = co_await tx.query(parent);
    if (rows.empty()) {
        service::common::fail(21004, "Windows VPN 配置不存在或不属于当前用户", 404);
    }
    co_await detail::validateSelectedEdges(tx, detail::rowValue(rows.front(), 0), ids);
    ruvia::DbQuery removal(c.pool());
    removal
        .deleteFrom(service::vpn::entities::VpnPeerEdgeSelectionEntity::tableName())
        .where(removal.binary(removal.column(service::vpn::entities::VpnPeerEdgeSelectionEntity::columnName<"peer_id">()), Op::kEqual, removal.cast(removal.value(id), ruvia::DbDataType::kUuid)));
    (void)co_await tx.execute(removal);
    for (const auto& edge : ids) {
        ruvia::DbQuery selection(c.pool());
        selection
            .insertInto(service::vpn::entities::VpnPeerEdgeSelectionEntity::tableName(), { "peer_id", "edge_node_id" })
            .values({ selection.cast(selection.value(id), ruvia::DbDataType::kUuid), selection.cast(selection.value(edge), ruvia::DbDataType::kUuid) });
        (void)co_await tx.execute(selection);
    }
    co_await tx.commit();
    co_await audit(c, c.userId, "vpn.desktop.select", "vpn_peer", id, "success", "{}");
    (void)co_await reconcileHub(c);
    co_return co_await desktopPeerConfig(c, id);
}

template <typename Context>
ruvia::Task<std::string> VpnService::desktopPeerConfig(Context& c, std::string_view id) {
    using Op = ruvia::DbBinaryOperator;
    requireUuid(id, "VPN Peer ID 无效");
    ruvia::DbQuery selectionQuery(c.pool());
    const std::array selectionOrder{
        ruvia::DbOrderTerm{ selectionQuery.column(service::vpn::entities::VpnPeerEdgeSelectionEntity::columnName<"edge_node_id">(), "selection") }
    };
    selectionQuery
        .select(selectionQuery.aggregate("jsonb_agg", { selectionQuery.column(service::vpn::entities::VpnPeerEdgeSelectionEntity::columnName<"edge_node_id">(), "selection") }, false, selectionOrder))
        .from(service::vpn::entities::VpnPeerEdgeSelectionEntity::tableName(), "selection")
        .where(selectionQuery.binary(selectionQuery.column(service::vpn::entities::VpnPeerEdgeSelectionEntity::columnName<"peer_id">(), "selection"), Op::kEqual, selectionQuery.column(service::vpn::entities::VpnPeerEntity::columnName<"id">(), "peer")));

    ruvia::DbQuery routeAccess(c.pool());
    routeAccess
        .select(routeAccess.alias(routeAccess.column(service::vpn::entities::VpnEffectiveRouteAccessEntity::columnName<"virtual_cidr">(), "access"), "route"))
        .from(service::vpn::entities::VpnEffectiveRouteAccessEntity::tableName(), "access")
        .where(routeAccess.binary(routeAccess.column(service::vpn::entities::VpnEffectiveRouteAccessEntity::columnName<"peer_id">(), "access"), Op::kEqual, routeAccess.column(service::vpn::entities::VpnPeerEntity::columnName<"id">(), "peer")));
    ruvia::DbQuery edgeAccess(c.pool());
    edgeAccess
        .select(edgeAccess.binary(edgeAccess.column(service::vpn::entities::VpnEffectiveEdgeAccessEntity::columnName<"edge_address">(), "access"), Op::kConcat, edgeAccess.cast(edgeAccess.value("/32"), ruvia::DbDataType::kText)))
        .from(service::vpn::entities::VpnEffectiveEdgeAccessEntity::tableName(), "access")
        .where(edgeAccess.binary(edgeAccess.column(service::vpn::entities::VpnEffectiveEdgeAccessEntity::columnName<"peer_id">(), "access"), Op::kEqual, edgeAccess.column(service::vpn::entities::VpnPeerEntity::columnName<"id">(), "peer")));
    routeAccess.combine(ruvia::DbSetOperation::kUnion, edgeAccess);

    ruvia::DbQuery allowedQuery(c.pool());
    const std::array allowedOrder{
        ruvia::DbOrderTerm{ allowedQuery.column("route", "allowed") }
    };
    allowedQuery
        .select(allowedQuery.aggregate("jsonb_agg", { allowedQuery.column("route", "allowed") }, false, allowedOrder))
        .from(routeAccess, "allowed");

    ruvia::DbQuery edgeAddressQuery(c.pool());
    const std::array edgeAddressOrder{
        ruvia::DbOrderTerm{ edgeAddressQuery.column(service::vpn::entities::VpnEffectiveEdgeAccessEntity::columnName<"edge_address">(), "access") }
    };
    edgeAddressQuery
        .select(edgeAddressQuery.aggregate(
            "jsonb_agg",
            { edgeAddressQuery.column(service::vpn::entities::VpnEffectiveEdgeAccessEntity::columnName<"edge_address">(), "access") },
            false,
            edgeAddressOrder
        ))
        .from(service::vpn::entities::VpnEffectiveEdgeAccessEntity::tableName(), "access")
        .where(edgeAddressQuery.binary(edgeAddressQuery.column(service::vpn::entities::VpnEffectiveEdgeAccessEntity::columnName<"peer_id">(), "access"), Op::kEqual, edgeAddressQuery.column(service::vpn::entities::VpnPeerEntity::columnName<"id">(), "peer")));

    ruvia::DbQuery query(c.pool());
    const auto emptyRoutes = query.cast(query.value("[]"), ruvia::DbDataType::kJsonb);
    const auto networkEnabled = query.binary(query.column(service::vpn::entities::VpnNetworkEntity::columnName<"status">(), "network"), Op::kEqual, query.value("enabled"));
    const auto allowedRoutes = query.caseWhen(
        { { networkEnabled,
            query.coalesce({ query.subquery(allowedQuery), emptyRoutes }) } },
        emptyRoutes
    );
    const auto edgeAddresses = query.caseWhen(
        { { networkEnabled,
            query.coalesce({ query.subquery(edgeAddressQuery), emptyRoutes }) } },
        emptyRoutes
    );
    query
        .select({ query.call("jsonb_build_object", { detail::jsonKey(query, "peerId"), query.column(service::vpn::entities::VpnPeerEntity::columnName<"id">(), "peer"), detail::jsonKey(query, "name"), query.column(service::vpn::entities::VpnPeerEntity::columnName<"name">(), "peer"), detail::jsonKey(query, "publicKey"), query.column(service::vpn::entities::VpnPeerEntity::columnName<"public_key">(), "peer"), detail::jsonKey(query, "networkEnabled"), networkEnabled, detail::jsonKey(query, "assignedIpv4"), query.call("host", { query.column(service::vpn::entities::VpnPeerEntity::columnName<"assigned_ipv4">(), "peer") }), detail::jsonKey(query, "hubPublicKey"), query.column(service::vpn::entities::VpnNetworkEntity::columnName<"hub_public_key">(), "network"), detail::jsonKey(query, "hubEndpoint"), query.column(service::vpn::entities::VpnNetworkEntity::columnName<"hub_endpoint">(), "network"), detail::jsonKey(query, "hubListenPort"), query.column(service::vpn::entities::VpnNetworkEntity::columnName<"hub_listen_port">(), "network"), detail::jsonKey(query, "mtu"), query.cast(query.value(1280), ruvia::DbDataType::kInteger), detail::jsonKey(query, "persistentKeepalive"), query.cast(query.value(25), ruvia::DbDataType::kInteger), detail::jsonKey(query, "configRevision"), query.column(service::vpn::entities::VpnPeerEntity::columnName<"config_revision">(), "peer"), detail::jsonKey(query, "edgeNodeIds"), query.coalesce({ query.subquery(selectionQuery), emptyRoutes }), detail::jsonKey(query, "allowedRoutes"), allowedRoutes, detail::jsonKey(query, "edgeAddresses"), edgeAddresses }), query.column(service::vpn::entities::VpnNetworkEntity::columnName<"hub_public_key">(), "network"), query.column(service::vpn::entities::VpnNetworkEntity::columnName<"hub_endpoint">(), "network") })
        .from(service::vpn::entities::VpnPeerEntity::tableName(), "peer")
        .join(ruvia::DbJoinType::kInner, service::vpn::entities::VpnNetworkEntity::tableName(), query.binary(query.column(service::vpn::entities::VpnNetworkEntity::columnName<"id">(), "network"), Op::kEqual, query.column(service::vpn::entities::VpnPeerEntity::columnName<"network_id">(), "peer")), "network")
        .where(query.binary(query.column(service::vpn::entities::VpnPeerEntity::columnName<"id">(), "peer"), Op::kEqual, query.cast(query.value(id), ruvia::DbDataType::kUuid)))
        .andWhere(query.binary(query.column(service::vpn::entities::VpnPeerEntity::columnName<"user_id">(), "peer"), Op::kEqual, query.cast(query.value(c.userId), ruvia::DbDataType::kUuid)))
        .andWhere(query.binary(query.column(service::vpn::entities::VpnPeerEntity::columnName<"peer_type">(), "peer"), Op::kEqual, query.value("windows")))
        .andWhere(query.binary(query.column(service::vpn::entities::VpnPeerEntity::columnName<"client_managed">(), "peer"), Op::kEqual, query.value(true)))
        .andWhere(query.binary(query.column(service::vpn::entities::VpnPeerEntity::columnName<"status">(), "peer"), Op::kEqual, query.value("active")))
        .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull, query.column(service::vpn::entities::VpnNetworkEntity::columnName<"deleted_at">(), "network")));
    const auto rows = co_await c.db().query(query);
    if (rows.empty()) {
        service::common::fail(21004, "Windows VPN 配置不存在或不属于当前用户", 404);
    }
    if (!detail::validManagedKey(detail::rowValue(rows.front(), 1)) || detail::rowValue(rows.front(), 2).empty()) {
        service::common::fail(21005, "VPN Hub 尚未配置完成", 503);
    }
    co_return detail::rowValue(rows.front(), 0);
}

template <typename Context>
ruvia::Task<void> VpnService::desktopDeletePeer(Context& c, std::string_view id) {
    using Op = ruvia::DbBinaryOperator;
    requireUuid(id, "VPN Peer ID 无效");
    ruvia::DbQuery update(c.pool());
    update
        .update(service::vpn::entities::VpnPeerEntity::tableName())
        .set(service::vpn::entities::VpnPeerEntity::columnName<"status">(), update.value("revoked"))
        .set(service::vpn::entities::VpnPeerEntity::columnName<"revoked_at">(), update.call("now"))
        .set(service::vpn::entities::VpnPeerEntity::columnName<"updated_at">(), update.call("now"))
        .where(update.binary(update.column(service::vpn::entities::VpnPeerEntity::columnName<"id">()), Op::kEqual, update.cast(update.value(id), ruvia::DbDataType::kUuid)))
        .andWhere(update.binary(update.column(service::vpn::entities::VpnPeerEntity::columnName<"user_id">()), Op::kEqual, update.cast(update.value(c.userId), ruvia::DbDataType::kUuid)))
        .andWhere(update.binary(update.column(service::vpn::entities::VpnPeerEntity::columnName<"peer_type">()), Op::kEqual, update.value("windows")))
        .andWhere(update.binary(update.column(service::vpn::entities::VpnPeerEntity::columnName<"client_managed">()), Op::kEqual, update.value(true)))
        .returning({ update.column(service::vpn::entities::VpnPeerEntity::columnName<"public_key">()) });
    const auto rows = co_await c.db().query(update);
    if (rows.empty()) {
        service::common::fail(21004, "Windows VPN 配置不存在或不属于当前用户", 404);
    }
    co_await audit(c, c.userId, "vpn.desktop.revoke", "vpn_peer", id, "success", "{}");
    (void)co_await reconcileHub(c);
}

template <typename Context>
ruvia::Task<void> VpnService::queueEdgeConfig(Context& c, std::string_view peerId) {
    (void)co_await control(c, "queue-edge-config", std::string(peerId) + "\n" + c.userId);
}

template <typename Context>
ruvia::Task<void> VpnService::audit(Context& c, std::string_view actor, std::string_view action, std::string_view resource, std::string_view resourceId, std::string_view outcome, std::string_view details) {
    const auto auditId = c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next();
    ruvia::DbQuery insert(c.pool());
    insert
        .insertInto(service::vpn::entities::SecurityAuditLogEntity::tableName(), { "id", "actor_user_id", "action", "resource_type", "resource_id", "outcome", "details" })
        .values({ insert.cast(insert.value(auditId), ruvia::DbDataType::kUuid), insert.cast(insert.value(actor), ruvia::DbDataType::kUuid), insert.value(action), insert.value(resource), insert.cast(insert.nullIf(insert.cast(insert.value(resourceId), ruvia::DbDataType::kText), insert.cast(insert.value(""), ruvia::DbDataType::kText)), ruvia::DbDataType::kUuid), insert.value(outcome), insert.cast(insert.value(details), ruvia::DbDataType::kJsonb) });
    (void)co_await c.db().execute(insert);
}

template <typename Context>
ruvia::Task<std::string> VpnService::control(Context& c, std::string_view operation, std::string payload) {
    co_return co_await service::rpc::call(c, "vpn", operation, std::move(payload));
}

template <typename Context>
ruvia::Task<void> VpnService::reconcileHub(Context& c) {
    (void)co_await control(c, "reconcile", std::string{});
}

inline VpnService& vpnService() {
    return VpnService::instance();
}

} // namespace service::vpn
