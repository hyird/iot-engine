#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <openssl/rand.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/ModelObject.h>

#include "service/common/http.h"
#include "service/common/uuid.h"
#include "service/features/access/contract.h"
#include "service/features/edge/dispatch.h"
#include "service/features/edge/protocol.h"
#include "service/features/vpn/cidr.h"
#include "service/features/vpn/client-config.h"
#include "service/features/vpn/edge-config.h"
#include "service/features/vpn/firewall.h"
#include "service/features/vpn/hub-config.h"
#include "service/features/vpn/route-sync.h"
#include "service/features/vpn/wireguard.h"
#include "service/middleware/auth.h"
#include "service/domains/vpn/vpn.types.h"

namespace service::vpn {

inline constexpr std::string_view kDefaultNetworkId{
    "00000000-0000-7000-8000-000000000004"};
inline constexpr std::string_view kDefaultNetworkName{"iot-server"};
inline constexpr std::string_view kDefaultOverlayCidr{"100.96.0.0/16"};

namespace detail {

inline std::string text(const ruvia::JsonValue& object, std::string_view field) {
    const auto value = object.get<ruvia::String>(field);
    return value ? std::string(value->view()) : std::string{};
}

inline std::optional<std::string> optionalText(const ruvia::JsonValue& object,
                                               std::string_view field) {
    const auto value = object.get<ruvia::String>(field);
    return value ? std::optional<std::string>(std::string(value->view())) : std::nullopt;
}

inline std::optional<std::int64_t> optionalInteger(const ruvia::JsonValue& object,
                                                   std::string_view field) {
    const auto value = object.get<ruvia::Int64>(field);
    return value ? std::optional<std::int64_t>(static_cast<std::int64_t>(*value)) : std::nullopt;
}

inline std::optional<bool> optionalBoolean(const ruvia::JsonValue& object,
                                           std::string_view field) {
    const auto value = object.get<ruvia::Bool>(field);
    return value ? std::optional<bool>(*value) : std::nullopt;
}

inline std::vector<std::string> textArray(const ruvia::JsonValue& object,
                                          std::string_view field) {
    const auto values = object.get<ruvia::Array<ruvia::String>>(field);
    if (!values)
        return {};
    std::vector<std::string> result;
    result.reserve(values->size());
    for (const auto& value : *values)
        result.emplace_back(value.view());
    return result;
}

inline std::vector<std::string> textArrayJson(std::string_view raw) {
    auto input = raw;
    auto value = ruvia::detail::parseJsonValue<ruvia::Array<ruvia::String>>(
        input, std::pmr::get_default_resource());
    ruvia::detail::skipJsonWhitespace(input);
    if (!value || !input.empty())
        return {};
    std::vector<std::string> result;
    result.reserve(value->size());
    for (const auto& item : *value)
        result.emplace_back(item.view());
    return result;
}

inline std::string jsonArray(const std::vector<std::string>& values) {
    std::string result{"["};
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0)
            result.push_back(',');
        result += service::access::jsonQuoted(values[index]);
    }
    result.push_back(']');
    return result;
}

inline std::string requiredText(const ruvia::JsonValue& object, std::string_view field,
                                std::size_t maximum, std::string_view message) {
    auto value = text(object, field);
    if (value.empty() || value.size() > maximum)
        service::common::fail(21001, std::string(message), 400);
    return value;
}

inline std::string requiredUuid(const ruvia::JsonValue& object, std::string_view field,
                                std::string_view message) {
    auto value = requiredText(object, field, 36, message);
    if (!service::common::isUuid(value))
        service::common::fail(21001, std::string(message), 400);
    return value;
}

inline bool validKey(std::string_view value) noexcept {
    if (value.size() != 44 || value.back() != '=')
        return false;
    for (const auto character : value)
        if (!((character >= 'A' && character <= 'Z') ||
              (character >= 'a' && character <= 'z') ||
              (character >= '0' && character <= '9') || character == '+' || character == '/' ||
              character == '='))
            return false;
    return true;
}

inline std::string randomToken() {
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        throw std::runtime_error("VPN enrollment token generation failed");
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
    if (value.empty() || value.size() > 32)
        return false;
    for (const auto character : value)
        if (!std::isalnum(static_cast<unsigned char>(character)) && character != '_' &&
            character != '-' && character != '.' && character != ':')
            return false;
    return true;
}

inline std::string rowValue(const auto& row, std::size_t index) {
    return std::string(row[index].value().value_or(std::string_view{}));
}

} // namespace detail

class VpnService final {
  private:
    static std::int64_t integer(std::string_view, std::int64_t = 0);
    static void requireUuid(std::string_view, std::string_view);
    static wireguard::HubConfig hubConfig(ruvia::Context&);
    static ruvia::Task<std::string> ensureDefaultNetwork(ruvia::Context&);
    static ruvia::Task<void> audit(ruvia::Context&, std::string_view, std::string_view,
                                   std::string_view, std::string_view, std::string_view,
                                   std::string_view);
    static ruvia::Task<void> queueEdgeConfig(ruvia::Context&, std::string_view);
    static ruvia::Task<std::string> clientConfigJson(ruvia::Context&, std::string_view,
                                                     std::string_view,
                                                     std::string_view =
                                                         "<client-private-key>");
    static ruvia::Task<std::optional<std::uint32_t>> allocateAddress(
        ruvia::Context&, std::string_view, const Ipv4Cidr&);
    template <typename Db>
    static ruvia::Task<std::optional<std::uint32_t>> allocateAddressFromDb(
        Db&, std::string_view, const Ipv4Cidr&);
    static ruvia::Task<void> validateAllowedRoutes(ruvia::Context&, std::string_view,
                                                   const std::vector<std::string>&);
    static ruvia::Task<wireguard::RuntimeStatus> reconcileHub(ruvia::Context&);

  public:
    static VpnService& instance() {
        static VpnService value;
        return value;
    }

    ruvia::Task<std::string> networks(ruvia::Context& c, std::int64_t page,
                                      std::int64_t pageSize,
                                      std::optional<std::string> keyword,
                                      std::optional<std::string> status) {
        page = std::max<std::int64_t>(1, page);
        pageSize = std::clamp<std::int64_t>(pageSize, 1, 100);
        const auto defaultNetworkId = co_await ensureDefaultNetwork(c);
        std::vector<ruvia::DbValue> params;
        params.emplace_back(defaultNetworkId);
        std::string where{" WHERE deleted_at IS NULL AND id = $1::uuid"};
        if (keyword && !keyword->empty()) {
            const auto pattern = "%" + *keyword + "%";
            params.emplace_back(std::string_view(pattern));
            where += " AND name ILIKE $" + std::to_string(params.size());
        }
        if (status && !status->empty()) {
            params.emplace_back(*status);
            where += " AND status = $" + std::to_string(params.size()) + "::status_enum";
        }
        const auto countRows = co_await c.db().query("SELECT COUNT(*) FROM vpn_network" + where,
                                                     params);
        const auto total = integer(detail::rowValue(countRows.front(), 0));
        auto listParams = params;
        listParams.emplace_back(pageSize);
        const auto limit = listParams.size();
        listParams.emplace_back((page - 1) * pageSize);
        const auto offset = listParams.size();
        const auto rows = co_await c.db().query(R"sql(
SELECT jsonb_build_object(
  'id', id, 'name', name, 'overlayCidr', overlay_cidr,
  'hubPublicKey', hub_public_key, 'hubEndpoint', hub_endpoint,
  'hubListenPort', hub_listen_port, 'status', status,
  'peerCount', (SELECT COUNT(*) FROM vpn_peer p WHERE p.network_id = vpn_network.id AND p.status <> 'revoked'),
  'routeCount', (SELECT COUNT(*) FROM vpn_route r WHERE r.network_id = vpn_network.id AND r.enabled),
  'createdAt', iot_utc_timestamp(created_at), 'updatedAt', iot_utc_timestamp(updated_at))
FROM vpn_network)sql" + where + " ORDER BY created_at DESC, id DESC LIMIT $" +
                                              std::to_string(limit) + " OFFSET $" +
                                              std::to_string(offset),
                                          listParams);
        std::string list{"["};
        for (std::size_t index = 0; index < rows.size(); ++index) {
            if (index != 0)
                list.push_back(',');
            list += detail::rowValue(rows[index], 0);
        }
        list.push_back(']');
        co_return "{\"list\":" + list + ",\"total\":" + std::to_string(total) +
                  ",\"page\":" + std::to_string(page) + ",\"pageSize\":" +
                  std::to_string(pageSize) + ",\"totalPages\":" +
                  std::to_string(total == 0 ? 0 : (total + pageSize - 1) / pageSize) + "}";
    }

    ruvia::Task<std::string> network(ruvia::Context& c, std::string_view id) {
        requireUuid(id, "VPN 网络 ID 无效");
        const auto rows = co_await c.db().query(R"sql(
SELECT jsonb_build_object(
  'id', n.id, 'name', n.name, 'overlayCidr', n.overlay_cidr,
  'hubPublicKey', n.hub_public_key, 'hubEndpoint', n.hub_endpoint,
  'hubListenPort', n.hub_listen_port, 'status', n.status,
  'createdAt', iot_utc_timestamp(n.created_at), 'updatedAt', iot_utc_timestamp(n.updated_at),
  'peers', COALESCE((SELECT jsonb_agg(jsonb_build_object(
      'id', p.id, 'peerType', p.peer_type, 'edgeNodeId', p.edge_node_id,
      'userId', p.user_id, 'name', p.name, 'publicKey', p.public_key,
      'assignedIpv4', host(p.assigned_ipv4), 'allowedRoutes', p.allowed_routes,
      'status', p.status, 'configRevision', p.config_revision,
      'lastHandshakeAt', iot_utc_timestamp(p.last_handshake_at)) ORDER BY p.created_at), '[]'::jsonb),
  'routes', COALESCE((SELECT jsonb_agg(jsonb_build_object(
      'id', r.id, 'edgePeerId', r.edge_peer_id, 'lanInterface', r.lan_interface,
      'targetCidr', r.target_cidr, 'virtualCidr', r.virtual_cidr, 'mode', r.mode,
      'natMode', r.nat_mode, 'status', r.status, 'enabled', r.enabled,
      'lastError', r.last_error) ORDER BY r.created_at), '[]'::jsonb))::text
FROM vpn_network n WHERE n.id = $1::uuid AND n.deleted_at IS NULL)sql",
                                                service::common::dbParams(id));
        if (rows.empty())
            service::common::fail(21004, "VPN 网络不存在", 404);
        co_return detail::rowValue(rows.front(), 0);
    }

    ruvia::Task<std::string> createNetwork(ruvia::Context& c,
                                           const ruvia::JsonValue&) {
        (void)co_await ensureDefaultNetwork(c);
        service::common::fail(21003, "VPN 使用默认 iot-server，无需新建 VPN 网络", 409);
        co_return std::string{};
    }

    ruvia::Task<void> updateNetwork(ruvia::Context& c, std::string_view id,
                                    const ruvia::JsonValue&) {
        requireUuid(id, "VPN 网络 ID 无效");
        (void)co_await ensureDefaultNetwork(c);
        service::common::fail(21003, "VPN 使用 iot-server，网络配置不可修改", 409);
    }

    ruvia::Task<void> removeNetwork(ruvia::Context& c, std::string_view id) {
        requireUuid(id, "VPN 网络 ID 无效");
        (void)co_await ensureDefaultNetwork(c);
        service::common::fail(21003, "默认 iot-server VPN 网络不能删除", 409);
    }

    ruvia::Task<std::string> routes(ruvia::Context& c,
                                    std::optional<std::string> networkId,
                                    std::optional<std::string> edgeNodeId = std::nullopt) {
        std::vector<ruvia::DbValue> params;
        std::string where;
        if (networkId && !networkId->empty()) {
            requireUuid(*networkId, "VPN 网络 ID 无效");
            params.emplace_back(*networkId);
            where = " WHERE r.network_id = $1::uuid";
        }
        if (edgeNodeId && !edgeNodeId->empty()) {
            requireUuid(*edgeNodeId, "Edge 节点 ID 无效");
            const auto parameter = params.size() + 1;
            where += std::string(where.empty() ? " WHERE " : " AND ") +
                     "p.edge_node_id = $" + std::to_string(parameter) + "::uuid";
            params.emplace_back(*edgeNodeId);
        }
        const auto rows = co_await c.db().query(R"sql(
SELECT jsonb_build_object(
  'id', r.id, 'networkId', r.network_id, 'edgePeerId', r.edge_peer_id,
  'edgeNodeId', p.edge_node_id, 'lanInterface', r.lan_interface,
  'targetCidr', r.target_cidr, 'virtualCidr', r.virtual_cidr, 'mode', r.mode,
  'natMode', r.nat_mode, 'status', r.status, 'enabled', r.enabled,
  'lastError', r.last_error, 'createdAt', iot_utc_timestamp(r.created_at),
  'updatedAt', iot_utc_timestamp(r.updated_at))
FROM vpn_route r JOIN vpn_peer p ON p.id = r.edge_peer_id)sql" + where +
                                          " ORDER BY r.created_at DESC LIMIT 1000",
                                      params);
        std::string result{"["};
        for (std::size_t index = 0; index < rows.size(); ++index) {
            if (index != 0)
                result.push_back(',');
            result += detail::rowValue(rows[index], 0);
        }
        result.push_back(']');
        co_return result;
    }

    ruvia::Task<std::string> createRoute(ruvia::Context& c,
                                         const ruvia::JsonValue& payload) {
        const auto defaultNetworkId = co_await ensureDefaultNetwork(c);
        const auto networkId = detail::requiredUuid(payload, "networkId", "VPN 网络 ID 无效");
        if (networkId != defaultNetworkId)
            service::common::fail(21003, "VPN 仅使用默认 iot-server 网络", 409);
        const auto edgePeerId = detail::requiredUuid(payload, "edgePeerId", "Edge Peer ID 无效");
        const auto targetText = detail::requiredText(payload, "targetCidr", 18,
                                                     "真实 LAN CIDR 不能为空");
        const auto target = parseCidr(targetText, 1, 30);
        if (!target || !isPrivateIpv4(*target))
            service::common::fail(21001, "真实 LAN 必须是有效的私有 IPv4 网段", 400);
        const auto edge = co_await c.db().query(R"sql(
SELECT p.network_id, p.peer_type, p.edge_node_id
FROM vpn_peer p JOIN vpn_network n ON n.id = p.network_id
WHERE p.id = $1::uuid AND p.status <> 'revoked' AND n.status = 'enabled'
LIMIT 1)sql", service::common::dbParams(edgePeerId));
        if (edge.empty() || detail::rowValue(edge.front(), 1) != "edge" ||
            detail::rowValue(edge.front(), 0) != networkId)
            service::common::fail(21004, "Edge Peer 不属于指定 VPN 网络", 404);
        const auto principal = service::middleware::requireAuth(c);
        const auto targetCidr = target->text();
        auto transaction = co_await c.db().beginTransaction();
        (void)co_await transaction.query(
            "SELECT pg_advisory_xact_lock(5282804697543808068::bigint)");
        co_await syncEdgeBridgeRoutes(transaction, edgePeerId, networkId,
                                      detail::rowValue(edge.front(), 2), principal.userId);
        co_await transaction.commit();
        const auto mapped = co_await c.db().query(
            "SELECT id::text, virtual_cidr FROM vpn_route "
            "WHERE edge_peer_id = $1::uuid AND target_cidr = $2 LIMIT 1",
            service::common::dbParams(edgePeerId, targetCidr));
        if (mapped.empty())
            service::common::fail(21004, "真实 LAN 不是 EdgeNode 的桥接网段", 409);
        const auto requestedVirtual = detail::optionalText(payload, "virtualCidr");
        if (requestedVirtual && *requestedVirtual != detail::rowValue(mapped.front(), 1))
            service::common::fail(21003, "虚拟网段由平台自动生成，请使用编辑修改", 409);
        const auto id = detail::rowValue(mapped.front(), 0);
        co_await audit(c, principal.userId, "vpn.route.ensure", "vpn_route", id,
                       "success", "{}");
        co_await queueEdgeConfig(c, edgePeerId);
        (void)co_await reconcileHub(c);
        co_return id;
    }

    ruvia::Task<void> updateRoute(ruvia::Context& c, std::string_view id,
                                  const ruvia::JsonValue& payload) {
        requireUuid(id, "VPN 路由 ID 无效");
        const auto rows = co_await c.db().query(
            "SELECT edge_peer_id, network_id, lan_interface, target_cidr, virtual_cidr, mode, enabled "
            "FROM vpn_route WHERE id = $1::uuid", service::common::dbParams(id));
        if (rows.empty())
            service::common::fail(21004, "VPN 路由不存在", 404);
        const auto edgePeerId = detail::rowValue(rows.front(), 0);
        const auto currentInterface = detail::rowValue(rows.front(), 2);
        const auto currentTarget = detail::rowValue(rows.front(), 3);
        const auto currentVirtual = detail::rowValue(rows.front(), 4);
        const auto currentMode = detail::rowValue(rows.front(), 5);
        const auto currentEnabled = detail::rowValue(rows.front(), 6) == "t";
        const auto requestedTarget = detail::optionalText(payload, "targetCidr");
        const auto requestedInterface = detail::optionalText(payload, "lanInterface");
        const auto requestedMode = detail::optionalText(payload, "mode");
        const auto requestedEnabled = detail::optionalBoolean(payload, "enabled");
        if ((requestedTarget && *requestedTarget != currentTarget) ||
            (requestedInterface && *requestedInterface != currentInterface) ||
            (requestedMode && *requestedMode != currentMode) ||
            (requestedEnabled && *requestedEnabled != currentEnabled))
            service::common::fail(21003, "VPN 映射中只有虚拟网段可以修改", 409);
        const auto targetText = currentTarget;
        const auto virtualText = detail::optionalText(payload, "virtualCidr").value_or(currentVirtual);
        const auto target = parseCidr(targetText, 1, 30);
        const auto virtualNetwork = parseCidr(virtualText, 1, 30);
        if (!target || !virtualNetwork || target->prefix != virtualNetwork->prefix ||
            !isPrivateIpv4(*target) || !kVirtualLanPool.contains(virtualNetwork->network) ||
            !kVirtualLanPool.contains(virtualNetwork->network + virtualNetwork->size() - 1U) ||
            virtualNetwork->overlaps(*target))
            service::common::fail(21001, "VPN 路由 CIDR 无效或不是等长私网映射", 400);
        if (currentMode != "nat" || !currentEnabled)
            service::common::fail(21003, "VPN 自动映射只能保持启用的 NAT 模式", 409);
        auto transaction = co_await c.db().beginTransaction();
        (void)co_await transaction.query(
            "SELECT pg_advisory_xact_lock(5282804697543808068::bigint)");
        const auto conflicts = co_await transaction.query(
            "SELECT target_cidr, virtual_cidr FROM vpn_route WHERE id <> $1::uuid",
            service::common::dbParams(id));
        for (const auto& row : conflicts) {
            const auto otherTarget = parseCidr(detail::rowValue(row, 0), 1, 30);
            const auto otherVirtual = parseCidr(detail::rowValue(row, 1), 1, 30);
            if ((otherTarget && virtualNetwork->overlaps(*otherTarget)) ||
                (otherVirtual && virtualNetwork->overlaps(*otherVirtual)))
                service::common::fail(21002, "虚拟网段与全局已有网段重叠", 409);
        }
        const auto virtualCidr = virtualNetwork->text();
        (void)co_await transaction.execute(
            "UPDATE vpn_route SET virtual_cidr = $2, last_error = '', "
            "updated_at = NOW() WHERE id = $1::uuid",
            service::common::dbParams(id, virtualCidr));
        co_await transaction.commit();
        const auto principal = service::middleware::requireAuth(c);
        co_await audit(c, principal.userId, "vpn.route.update", "vpn_route", id,
                       "success", "{}");
        co_await queueEdgeConfig(c, edgePeerId);
        (void)co_await reconcileHub(c);
    }

    ruvia::Task<void> removeRoute(ruvia::Context& c, std::string_view id) {
        requireUuid(id, "VPN 路由 ID 无效");
        service::common::fail(21003, "VPN 桥接网段映射不能删除，请撤销 Edge Peer", 409);
    }

    ruvia::Task<std::string> peers(ruvia::Context& c,
                                   std::optional<std::string> networkId,
                                   std::optional<std::string> edgeNodeId = std::nullopt) {
        std::vector<ruvia::DbValue> params;
        std::string where;
        if (networkId && !networkId->empty()) {
            requireUuid(*networkId, "VPN 网络 ID 无效");
            params.emplace_back(*networkId);
            where = " WHERE p.network_id = $1::uuid";
        }
        if (edgeNodeId && !edgeNodeId->empty()) {
            requireUuid(*edgeNodeId, "Edge 节点 ID 无效");
            const auto parameter = params.size() + 1;
            where += std::string(where.empty() ? " WHERE " : " AND ") +
                     "p.edge_node_id = $" + std::to_string(parameter) + "::uuid";
            params.emplace_back(*edgeNodeId);
        }
        const auto rows = co_await c.db().query(R"sql(
SELECT jsonb_build_object(
  'id', p.id, 'networkId', p.network_id, 'peerType', p.peer_type,
  'edgeNodeId', p.edge_node_id, 'userId', p.user_id, 'name', p.name,
  'publicKey', p.public_key, 'assignedIpv4', host(p.assigned_ipv4),
  'allowedRoutes', p.allowed_routes, 'status', p.status,
  'configRevision', p.config_revision, 'lastHandshakeAt', iot_utc_timestamp(p.last_handshake_at),
  'createdAt', iot_utc_timestamp(p.created_at))
FROM vpn_peer p)sql" + where + " ORDER BY p.created_at DESC LIMIT 1000", params);
        std::string result{"["};
        for (std::size_t index = 0; index < rows.size(); ++index) {
            if (index != 0)
                result.push_back(',');
            result += detail::rowValue(rows[index], 0);
        }
        result.push_back(']');
        co_return result;
    }

    ruvia::Task<std::string> createPeer(ruvia::Context& c,
                                        const ruvia::JsonValue& payload) {
        const auto defaultNetworkId = co_await ensureDefaultNetwork(c);
        const auto requestedNetworkId = detail::optionalText(payload, "networkId");
        if (requestedNetworkId && !service::common::isUuid(*requestedNetworkId))
            service::common::fail(21001, "VPN 网络 ID 无效", 400);
        if (requestedNetworkId && *requestedNetworkId != defaultNetworkId)
            service::common::fail(21003, "VPN 仅使用默认 iot-server 网络", 409);
        const auto networkId = defaultNetworkId;
        const auto peerType = detail::requiredText(payload, "peerType", 16, "Peer 类型不能为空");
        if (peerType != "windows" && peerType != "edge")
            service::common::fail(21001, "Peer 类型只支持 windows 或 edge", 400);
        const auto name = detail::requiredText(payload, "name", 100, "Peer 名称不能为空");
        auto publicKey = detail::optionalText(payload, "publicKey").value_or("");
        if (!publicKey.empty() && !detail::validKey(publicKey))
            service::common::fail(21001, "WireGuard 公钥格式无效", 400);
        const auto principal = service::middleware::requireAuth(c);
        std::string edgeNodeId;
        std::string userId = principal.userId;
        std::string reusableEdgePeerId;
        if (peerType == "edge")
            edgeNodeId = detail::requiredUuid(payload, "edgeNodeId", "Edge 节点 ID 无效");
        else if (publicKey.empty())
            service::common::fail(21001, "Windows Peer 必须提供公钥", 400);
        const auto network = co_await c.db().query(
            "SELECT overlay_cidr, status FROM vpn_network WHERE id = $1::uuid AND deleted_at IS NULL",
            service::common::dbParams(networkId));
        if (network.empty())
            service::common::fail(21004, "VPN 网络不存在", 404);
        if (detail::rowValue(network.front(), 1) != "enabled")
            service::common::fail(21003, "VPN 网络已停用", 409);
        if (peerType == "edge") {
            const auto edge = co_await c.db().query(
                "SELECT COALESCE(capability->'vpn'->>'publicKey', '') FROM edge_node "
                "WHERE id = $1::uuid AND enrollment_status = 'approved'",
                service::common::dbParams(edgeNodeId));
            if (edge.empty())
                service::common::fail(21004, "Edge 节点不存在或尚未批准", 404);
            const auto reportedKey = detail::rowValue(edge.front(), 0);
            publicKey = detail::validKey(reportedKey) ? reportedKey : std::string{};
            const auto existing = co_await c.db().query(R"sql(
SELECT id::text, status
FROM vpn_peer
WHERE network_id = $1::uuid AND edge_node_id = $2::uuid
ORDER BY CASE WHEN status = 'revoked' THEN 1 ELSE 0 END, created_at DESC
LIMIT 1)sql", service::common::dbParams(networkId, edgeNodeId));
            if (!existing.empty()) {
                if (detail::rowValue(existing.front(), 1) != "revoked")
                    service::common::fail(21002, "该 Edge 节点已经加入 VPN 网络", 409);
                reusableEdgePeerId = detail::rowValue(existing.front(), 0);
            }
            userId.clear();
        }
        const auto overlay = parseCidr(detail::rowValue(network.front(), 0), 16, 30);
        if (!overlay)
            service::common::fail(21005, "VPN 网络 Overlay 配置损坏", 500);
        const auto allowedRoutes = detail::textArray(payload, "allowedRoutes");
        if (allowedRoutes.size() > 64)
            service::common::fail(21001, "Peer 最多授权 64 条路由", 400);
        co_await validateAllowedRoutes(c, networkId, allowedRoutes);
        const auto id = reusableEdgePeerId.empty() ? service::common::nextUuidV7()
                                                   : reusableEdgePeerId;
        const auto status = publicKey.empty() ? "pending" : "active";
        const auto allowedRoutesJson = detail::jsonArray(allowedRoutes);
        auto transaction = co_await c.db().beginTransaction();
        (void)co_await transaction.query(
            "SELECT pg_advisory_xact_lock(5282804697543808068::bigint)");
        if (reusableEdgePeerId.empty()) {
            const auto assigned = co_await allocateAddressFromDb(transaction, networkId, *overlay);
            const auto assignedText = detail::hostText(*assigned);
            (void)co_await transaction.execute(R"sql(
INSERT INTO vpn_peer(id, network_id, peer_type, edge_node_id, user_id, name, public_key,
                     assigned_ipv4, allowed_routes, status)
VALUES ($1::uuid, $2::uuid, $3, NULLIF($4, '')::uuid, NULLIF($5, '')::uuid,
        $6, $7, $8::inet, $9::jsonb, $10))sql",
                                                  service::common::dbParams(
                                                      id, networkId, peerType, edgeNodeId,
                                                      userId, name, publicKey, assignedText,
                                                      allowedRoutesJson, status));
        } else {
            const auto reactivated = co_await transaction.query(R"sql(
UPDATE vpn_peer
SET name = $2, public_key = $3, allowed_routes = $4::jsonb, status = $5,
    revoked_at = NULL, updated_at = NOW()
WHERE id = $1::uuid AND peer_type = 'edge' AND status = 'revoked'
RETURNING id)sql", service::common::dbParams(
                           id, name, publicKey, allowedRoutesJson, status));
            if (reactivated.empty())
                service::common::fail(21002, "该 Edge 节点已经加入 VPN 网络", 409);
        }
        if (peerType == "edge")
            co_await syncEdgeBridgeRoutes(transaction, id, networkId, edgeNodeId,
                                          principal.userId);
        co_await transaction.commit();
        co_await audit(c, principal.userId,
                       reusableEdgePeerId.empty() ? "vpn.peer.create"
                                                  : "vpn.peer.reactivate",
                       "vpn_peer", id,
                       "success", "{}");
        if (peerType == "edge")
            co_await queueEdgeConfig(c, id);
        (void)co_await reconcileHub(c);
        co_return id;
    }

    ruvia::Task<std::string> createClientConfig(ruvia::Context& c,
                                                const ruvia::JsonValue& payload) {
        const auto networkId = co_await ensureDefaultNetwork(c);
        const auto name = detail::requiredText(payload, "name", 100,
                                               "Windows 设备名称不能为空");
        const auto principal = service::middleware::requireAuth(c);
        const auto duplicate = co_await c.db().query(R"sql(
SELECT id
FROM vpn_peer
WHERE peer_type = 'windows' AND user_id = $1::uuid AND status = 'active'
  AND lower(name) = lower($2)
LIMIT 1)sql", service::common::dbParams(principal.userId, name));
        if (!duplicate.empty())
            service::common::fail(21002, "该客户端设备已有 VPN 配置，请先删除旧配置", 409);
        const auto network = co_await c.db().query(
            "SELECT overlay_cidr FROM vpn_network WHERE id = $1::uuid "
            "AND status = 'enabled' AND deleted_at IS NULL",
            service::common::dbParams(networkId));
        if (network.empty())
            service::common::fail(21004, "VPN 网络不存在或已停用", 404);
        const auto overlay = parseCidr(detail::rowValue(network.front(), 0), 16, 30);
        if (!overlay)
            service::common::fail(21005, "VPN 网络 Overlay 配置损坏", 500);
        const auto routeRows = co_await c.db().query(R"sql(
SELECT r.virtual_cidr
FROM vpn_route r
JOIN vpn_peer p ON p.id = r.edge_peer_id AND p.peer_type = 'edge'
JOIN edge_node e ON e.id = p.edge_node_id
WHERE r.network_id = $1::uuid AND r.enabled AND r.status = 'active'
  AND p.status = 'active' AND e.enrollment_status = 'approved'
ORDER BY r.virtual_cidr)sql", service::common::dbParams(networkId));
        std::vector<std::string> routes;
        routes.reserve(routeRows.size());
        for (const auto& row : routeRows)
            routes.push_back(detail::rowValue(row, 0));
        if (routes.empty())
            service::common::fail(21003, "当前账户没有可访问的 VPN 设备", 403);
        std::string privateKey;
        std::string publicKey;
        if (!hub_config::generateKeyPair(privateKey, publicKey))
            service::common::fail(21005, "Windows WireGuard 密钥生成失败", 500);

        const auto id = service::common::nextUuidV7();
        const auto routesJson = detail::jsonArray(routes);
        auto transaction = co_await c.db().beginTransaction();
        (void)co_await transaction.query(
            "SELECT pg_advisory_xact_lock(5282804697543808068::bigint)");
        const auto assigned = co_await allocateAddressFromDb(transaction, networkId, *overlay);
        const auto assignedAddress = detail::hostText(*assigned);
        (void)co_await transaction.execute(R"sql(
INSERT INTO vpn_peer(id, network_id, peer_type, user_id, name, public_key,
                     client_private_key, assigned_ipv4, allowed_routes, status)
VALUES ($1::uuid, $2::uuid, 'windows', $3::uuid, $4, $5, $6, $7::inet,
        $8::jsonb, 'active'))sql",
                                           service::common::dbParams(
                                               id, networkId, principal.userId, name,
                                               publicKey, privateKey, assignedAddress,
                                               routesJson));
        co_await transaction.commit();
        co_await audit(c, principal.userId, "vpn.client_config.create", "vpn_peer", id,
                       "success", "{}");
        std::exception_ptr reconcileFailure;
        try {
            (void)co_await reconcileHub(c);
        } catch (...) {
            reconcileFailure = std::current_exception();
        }
        if (reconcileFailure) {
            (void)co_await c.db().execute(
                "DELETE FROM vpn_peer WHERE id = $1::uuid",
                service::common::dbParams(id));
            try {
                (void)co_await reconcileHub(c);
            } catch (...) {
                // The periodic reconciler will remove any partially applied peer.
            }
            std::rethrow_exception(reconcileFailure);
        }
        co_return co_await clientConfigJson(c, id, principal.userId, privateKey);
    }

    ruvia::Task<std::string> clientConfigs(ruvia::Context& c) {
        const auto principal = service::middleware::requireAuth(c);
        const auto rows = co_await c.db().query(R"sql(
SELECT jsonb_build_object(
  'id', p.id, 'name', p.name, 'assignedIpv4', host(p.assigned_ipv4),
  'allowedRoutes', COALESCE((
      SELECT jsonb_agg(r.virtual_cidr ORDER BY r.virtual_cidr)
      FROM vpn_route r
      JOIN vpn_peer edge_peer ON edge_peer.id = r.edge_peer_id
      JOIN edge_node e ON e.id = edge_peer.edge_node_id
      WHERE r.network_id = p.network_id AND r.enabled AND r.status = 'active'
        AND edge_peer.status = 'active' AND e.enrollment_status = 'approved'
  ), '[]'::jsonb), 'status', p.status,
  'lastHandshakeAt', iot_utc_timestamp(p.last_handshake_at),
  'createdAt', iot_utc_timestamp(p.created_at))
FROM vpn_peer p
WHERE p.peer_type = 'windows' AND p.user_id = $1::uuid AND p.status = 'active'
ORDER BY p.created_at DESC, p.id DESC
LIMIT 1000)sql", service::common::dbParams(principal.userId));
        std::string result{"["};
        for (std::size_t index = 0; index < rows.size(); ++index) {
            if (index != 0)
                result.push_back(',');
            result += detail::rowValue(rows[index], 0);
        }
        result.push_back(']');
        co_return result;
    }

    ruvia::Task<void> removeClientConfig(ruvia::Context& c, std::string_view id) {
        requireUuid(id, "VPN 配置 ID 无效");
        const auto principal = service::middleware::requireAuth(c);
        const auto rows = co_await c.db().query(R"sql(
DELETE FROM vpn_peer
WHERE id = $1::uuid AND peer_type = 'windows' AND user_id = $2::uuid
RETURNING public_key)sql", service::common::dbParams(id, principal.userId));
        if (rows.empty())
            service::common::fail(21004, "VPN 配置不存在或不属于当前用户", 404);
        const auto key = detail::rowValue(rows.front(), 0);
        if (detail::validKey(key))
            (void)wireguard::controller().removePeer(hubConfig(c), key);
        co_await audit(c, principal.userId, "vpn.client_config.delete", "vpn_peer", id,
                       "success", "{}");
        (void)co_await reconcileHub(c);
    }

    ruvia::Task<void> revokePeer(ruvia::Context& c, std::string_view id) {
        requireUuid(id, "VPN Peer ID 无效");
        const auto rows = co_await c.db().query(
            "UPDATE vpn_peer SET status = 'revoked', revoked_at = NOW(), updated_at = NOW() "
            "WHERE id = $1::uuid AND status <> 'revoked' RETURNING public_key, edge_node_id",
            service::common::dbParams(id));
        if (rows.empty())
            service::common::fail(21004, "VPN Peer 不存在或已撤销", 404);
        const auto key = detail::rowValue(rows.front(), 0);
        const auto config = hubConfig(c);
        if (detail::validKey(key))
            (void)wireguard::controller().removePeer(config, key);
        const auto edgeNode = detail::rowValue(rows.front(), 1);
        if (!edgeNode.empty())
            co_await queueEdgeConfig(c, id);
        const auto principal = service::middleware::requireAuth(c);
        co_await audit(c, principal.userId, "vpn.peer.revoke", "vpn_peer", id,
                       "success", "{}");
        (void)co_await reconcileHub(c);
    }

    ruvia::Task<void> syncPeer(ruvia::Context& c, std::string_view id) {
        requireUuid(id, "VPN Peer ID 无效");
        const auto rows = co_await c.db().query(
            "SELECT edge_node_id, peer_type, status, network_id::text FROM vpn_peer WHERE id = $1::uuid",
            service::common::dbParams(id));
        if (rows.empty())
            service::common::fail(21004, "VPN Peer 不存在", 404);
        if (detail::rowValue(rows.front(), 1) != "edge")
            service::common::fail(21001, "只有 Edge Peer 支持重新下发", 400);
        if (detail::rowValue(rows.front(), 2) == "revoked")
            service::common::fail(21003, "VPN Peer 已撤销", 409);
        const auto edgeNodeId = detail::rowValue(rows.front(), 0);
        const auto networkId = detail::rowValue(rows.front(), 3);
        const auto principal = service::middleware::requireAuth(c);
        auto transaction = co_await c.db().beginTransaction();
        (void)co_await transaction.query(
            "SELECT pg_advisory_xact_lock(5282804697543808068::bigint)");
        co_await syncEdgeBridgeRoutes(transaction, id, networkId, edgeNodeId,
                                      principal.userId);
        co_await transaction.commit();
        co_await queueEdgeConfig(c, id);
        (void)co_await reconcileHub(c);
    }

    ruvia::Task<void> rotatePeerKey(ruvia::Context& c, std::string_view id,
                                    const ruvia::JsonValue& payload) {
        requireUuid(id, "VPN Peer ID 无效");
        const auto publicKey = detail::requiredText(payload, "publicKey", 64, "WireGuard 公钥不能为空");
        if (!detail::validKey(publicKey))
            service::common::fail(21001, "WireGuard 公钥格式无效", 400);
        const auto current = co_await c.db().query(
            "SELECT public_key, edge_node_id FROM vpn_peer WHERE id = $1::uuid AND status <> 'revoked'",
            service::common::dbParams(id));
        if (current.empty())
            service::common::fail(21004, "VPN Peer 不存在或已撤销", 404);
        const auto oldKey = detail::rowValue(current.front(), 0);
        const auto edgeNodeId = detail::rowValue(current.front(), 1);
        const auto updated = co_await c.db().query(
            "UPDATE vpn_peer SET public_key = $2, status = 'active', revoked_at = NULL, "
            "config_revision = config_revision + 1, updated_at = NOW() "
            "WHERE id = $1::uuid AND status <> 'revoked' RETURNING public_key, edge_node_id",
            service::common::dbParams(id, publicKey));
        if (updated.empty())
            service::common::fail(21004, "VPN Peer 不存在或已撤销", 404);
        const auto config = hubConfig(c);
        if (detail::validKey(oldKey) && oldKey != publicKey)
            (void)wireguard::controller().removePeer(config, oldKey);
        const auto principal = service::middleware::requireAuth(c);
        co_await audit(c, principal.userId, "vpn.peer.rotate_key", "vpn_peer", id,
                       "success", "{}");
        co_await reconcileHub(c);
        if (!edgeNodeId.empty())
            co_await queueEdgeConfig(c, id);
    }

    ruvia::Task<std::string> createEnrollment(ruvia::Context& c,
                                              const ruvia::JsonValue& payload) {
        const auto defaultNetworkId = co_await ensureDefaultNetwork(c);
        const auto requestedNetworkId = detail::optionalText(payload, "networkId");
        if (requestedNetworkId && !service::common::isUuid(*requestedNetworkId))
            service::common::fail(21001, "VPN 网络 ID 无效", 400);
        if (requestedNetworkId && *requestedNetworkId != defaultNetworkId)
            service::common::fail(21003, "VPN 仅使用默认 iot-server 网络", 409);
        const auto networkId = defaultNetworkId;
        const auto network = co_await c.db().query(
            "SELECT id FROM vpn_network WHERE id = $1::uuid AND status = 'enabled' AND deleted_at IS NULL",
            service::common::dbParams(networkId));
        if (network.empty())
            service::common::fail(21004, "VPN 网络不存在或已停用", 404);
        const auto routeRows = co_await c.db().query(R"sql(
SELECT r.virtual_cidr
FROM vpn_route r
JOIN vpn_peer p ON p.id = r.edge_peer_id AND p.peer_type = 'edge'
JOIN edge_node e ON e.id = p.edge_node_id
WHERE r.network_id = $1::uuid AND r.enabled AND r.status = 'active'
  AND p.status = 'active' AND e.enrollment_status = 'approved'
ORDER BY r.virtual_cidr)sql", service::common::dbParams(networkId));
        std::vector<std::string> routes;
        routes.reserve(routeRows.size());
        for (const auto& row : routeRows)
            routes.push_back(detail::rowValue(row, 0));
        if (routes.empty())
            service::common::fail(21003, "当前账户没有可访问的 VPN 设备", 403);
        if (routes.size() > 64)
            service::common::fail(21001, "当前账户可访问的 VPN 路由超过 64 条", 409);
        co_await validateAllowedRoutes(c, networkId, routes);
        const auto seconds = detail::optionalInteger(payload, "expiresInSec").value_or(600);
        if (seconds < 60 || seconds > 3600)
            service::common::fail(21001, "Enrollment 有效期必须在 60 - 3600 秒之间", 400);
        const auto token = detail::randomToken();
        const auto hash = service::access::sha256(token);
        const auto routesJson = detail::jsonArray(routes);
        const auto id = service::common::nextUuidV7();
        const auto principal = service::middleware::requireAuth(c);
        (void)co_await c.db().execute(R"sql(
INSERT INTO vpn_enrollment(id, token_hash, network_id, allowed_routes, expires_at, created_by)
VALUES ($1::uuid, $2, $3::uuid, $4::jsonb, NOW() + ($5::bigint * INTERVAL '1 second'), $6::uuid))sql",
                                      service::common::dbParams(id, hash, networkId,
                                                                routesJson, seconds,
                                                                principal.userId));
        co_await audit(c, principal.userId, "vpn.enrollment.create", "vpn_enrollment", id,
                       "success", "{}");
        co_return "{\"id\":" + service::access::jsonQuoted(id) +",\"token\":" +
                  service::access::jsonQuoted(token) + ",\"expiresInSec\":" +
                  std::to_string(seconds) + "}";
    }

    ruvia::Task<std::string> enrollClient(ruvia::Context& c,
                                          const ruvia::JsonValue& payload) {
        const auto token = detail::requiredText(payload, "token", 128, "Enrollment token 不能为空");
        const auto publicKey = detail::requiredText(payload, "publicKey", 64, "WireGuard 公钥不能为空");
        if (!detail::validKey(publicKey))
            service::common::fail(21001, "WireGuard 公钥格式无效", 400);
        const auto tokenHash = service::access::sha256(token);
        const auto enrollment = co_await c.db().query(R"sql(
UPDATE vpn_enrollment SET used_at = NOW()
WHERE token_hash = $1 AND used_at IS NULL AND expires_at > NOW()
RETURNING id, network_id, allowed_routes, created_by)sql",
                                                      service::common::dbParams(tokenHash));
        if (enrollment.empty())
            service::common::fail(21006, "Enrollment token 无效、已使用或已过期", 401);
        const auto networkId = detail::rowValue(enrollment.front(), 1);
        const auto network = co_await c.db().query(
            "SELECT overlay_cidr FROM vpn_network WHERE id = $1::uuid AND status = 'enabled' "
            "AND deleted_at IS NULL", service::common::dbParams(networkId));
        if (network.empty())
            service::common::fail(21004, "VPN 网络不存在或已停用", 409);
        const auto overlay = parseCidr(detail::rowValue(network.front(), 0), 16, 30);
        if (!overlay)
            service::common::fail(21005, "VPN 网络 Overlay 配置损坏", 500);
        const auto assigned = co_await allocateAddress(c, networkId, *overlay);
        const auto name = detail::optionalText(payload, "name").value_or("Windows client");
        if (name.empty() || name.size() > 100)
            service::common::fail(21001, "Peer 名称长度无效", 400);
        const auto id = service::common::nextUuidV7();
        const auto creatorId = detail::rowValue(enrollment.front(), 3);
        const auto assignedAddress = detail::hostText(*assigned);
        const auto allowedRoutesJson = detail::rowValue(enrollment.front(), 2);
        (void)co_await c.db().execute(R"sql(
INSERT INTO vpn_peer(id, network_id, peer_type, user_id, name, public_key, assigned_ipv4,
                     allowed_routes, status)
VALUES ($1::uuid, $2::uuid, 'windows', $3::uuid, $4, $5, $6::inet, $7::jsonb, 'active'))sql",
                                      service::common::dbParams(id, networkId,
                                                                creatorId,
                                                                name, publicKey,
                                                                assignedAddress, allowedRoutesJson));
        (void)co_await reconcileHub(c);
        co_return co_await clientConfigJson(c, id, detail::rowValue(enrollment.front(), 3));
    }

    ruvia::Task<std::string> clientConfig(ruvia::Context& c, std::string_view peerId) {
        requireUuid(peerId, "VPN Peer ID 无效");
        const auto principal = service::middleware::requireAuth(c);
        auto transaction = co_await c.db().beginTransaction();
        const auto rows = co_await transaction.query(R"sql(
SELECT public_key, client_private_key
FROM vpn_peer
WHERE id = $1::uuid AND peer_type = 'windows' AND user_id = $2::uuid
  AND status = 'active'
FOR UPDATE)sql", service::common::dbParams(peerId, principal.userId));
        if (rows.empty())
            service::common::fail(21004, "Windows VPN 配置不存在或不属于当前用户", 404);

        const auto oldPublicKey = detail::rowValue(rows.front(), 0);
        auto privateKey = detail::rowValue(rows.front(), 1);
        std::string derivedPublicKey;
        bool rekeyed = !hub_config::derivePublicKey(privateKey, derivedPublicKey) ||
                       derivedPublicKey != oldPublicKey;
        if (rekeyed) {
            if (!hub_config::generateKeyPair(privateKey, derivedPublicKey))
                service::common::fail(21005, "Windows WireGuard 密钥生成失败", 500);
            (void)co_await transaction.execute(R"sql(
UPDATE vpn_peer
SET public_key = $2, client_private_key = $3,
    config_revision = config_revision + 1, updated_at = NOW()
WHERE id = $1::uuid)sql",
                                               service::common::dbParams(
                                                   peerId, derivedPublicKey, privateKey));
        }
        co_await transaction.commit();

        if (rekeyed) {
            const auto config = hubConfig(c);
            if (detail::validKey(oldPublicKey) && oldPublicKey != derivedPublicKey)
                (void)wireguard::controller().removePeer(config, oldPublicKey);
            (void)co_await reconcileHub(c);
        }
        co_await audit(c, principal.userId,
                       rekeyed ? "vpn.client_config.rekey_download"
                               : "vpn.client_config.download",
                       "vpn_peer", peerId, "success", "{}");
        co_return co_await clientConfigJson(c, peerId, principal.userId, privateKey);
    }

    ruvia::Task<std::string> sessions(ruvia::Context& c) {
        const auto rows = co_await c.db().query(R"sql(
SELECT jsonb_build_object(
  'peerId', p.id, 'networkId', p.network_id, 'name', p.name,
  'peerType', p.peer_type, 'assignedIpv4', host(p.assigned_ipv4),
  'status', p.status, 'lastHandshakeAt', iot_utc_timestamp(p.last_handshake_at),
  'online', CASE WHEN p.last_handshake_at > NOW() - INTERVAL '3 minutes' THEN true ELSE false END)
FROM vpn_peer p WHERE p.status = 'active' ORDER BY p.updated_at DESC LIMIT 1000)sql");
        std::string result{"["};
        for (std::size_t index = 0; index < rows.size(); ++index) {
            if (index != 0)
                result.push_back(',');
            result += detail::rowValue(rows[index], 0);
        }
        result.push_back(']');
        co_return result;
    }

    ruvia::Task<std::string> diagnostics(ruvia::Context& c) {
        const auto config = co_await hub_config::loadOrInitialize(c, hubConfig(c));
        const auto runtime = config
                                 ? wireguard::controller().status(*config)
                                 : wireguard::RuntimeStatus{
                                       .supported = true,
                                       .configured = false,
                                       .code = "hub_config_missing",
                                       .message = "WireGuard Hub 配置尚未初始化",
                                       .peerCount = 0};
        const auto peers = co_await c.db().query(
            "SELECT COUNT(*) FILTER (WHERE status = 'active'), "
            "COUNT(*) FILTER (WHERE status = 'revoked') FROM vpn_peer");
        const auto routes = co_await c.db().query(
            "SELECT COUNT(*) FILTER (WHERE enabled), COUNT(*) FILTER (WHERE status = 'error') FROM vpn_route");
        co_return "{\"platformSupported\":" + std::string(runtime.supported ? "true" : "false") +
                  ",\"configured\":" + (runtime.configured ? "true" : "false") +
                  ",\"code\":" + service::access::jsonQuoted(runtime.code) +
                  ",\"message\":" + service::access::jsonQuoted(runtime.message) +
                  ",\"runtimePeerCount\":" + std::to_string(runtime.peerCount) +
                  ",\"activePeerCount\":" + detail::rowValue(peers.front(), 0) +
                  ",\"revokedPeerCount\":" + detail::rowValue(peers.front(), 1) +
                  ",\"enabledRouteCount\":" + detail::rowValue(routes.front(), 0) +
                  ",\"errorRouteCount\":" + detail::rowValue(routes.front(), 1) + "}";
    }

  private:
};

inline std::int64_t VpnService::integer(std::string_view value, std::int64_t fallback) {
    std::int64_t result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    return error == std::errc{} && end == value.data() + value.size() ? result : fallback;
}

inline void VpnService::requireUuid(std::string_view id, std::string_view message) {
    if (!service::common::isUuid(id))
        service::common::fail(21001, std::string(message), 400);
}

inline wireguard::HubConfig VpnService::hubConfig(ruvia::Context& c) {
    wireguard::HubConfig config;
    config.interfaceName = std::string(c.env().get("VPN_HUB_INTERFACE").value_or("wg"));
    config.privateKey = std::string(c.env().get("VPN_HUB_PRIVATE_KEY").value_or(""));
    config.publicKey = std::string(c.env().get("VPN_HUB_PUBLIC_KEY").value_or(""));
    config.endpoint = std::string(c.env().get("VPN_HUB_ENDPOINT").value_or(""));
    config.listenPort = c.env().get<std::uint16_t>("VPN_HUB_LISTEN_PORT").value_or(51820);
    return config;
}

inline ruvia::Task<std::string> VpnService::ensureDefaultNetwork(ruvia::Context& c) {
    const auto principal = service::middleware::requireAuth(c);
    const auto hub = hubConfig(c);
    auto transaction = co_await c.db().beginTransaction();
    (void)co_await transaction.query(
        "SELECT pg_advisory_xact_lock(5282804697543808067::bigint)");
    const auto existing = co_await transaction.query(
        "SELECT id::text FROM vpn_network WHERE name = $1 AND deleted_at IS NULL LIMIT 1",
        service::common::dbParams(kDefaultNetworkName));
    std::string networkId = std::string(kDefaultNetworkId);
    if (!existing.empty()) {
        networkId = detail::rowValue(existing.front(), 0);
    } else {
        const auto inserted = co_await transaction.query(R"sql(
INSERT INTO vpn_network(id, name, overlay_cidr, hub_public_key, hub_endpoint,
                        hub_listen_port, created_by)
VALUES ($1::uuid, $2, $3, $4, $5, $6, $7::uuid)
ON CONFLICT (id) DO UPDATE SET
    name = EXCLUDED.name, overlay_cidr = EXCLUDED.overlay_cidr,
    deleted_at = NULL, updated_at = NOW()
RETURNING id::text)sql",
                                                        service::common::dbParams(
                                                            kDefaultNetworkId, kDefaultNetworkName,
                                                            kDefaultOverlayCidr, hub.publicKey,
                                                            hub.endpoint, static_cast<int>(hub.listenPort),
                                                            principal.userId));
        if (inserted.empty())
            service::common::fail(21005, "默认 iot-server VPN 网络创建失败", 500);
        networkId = detail::rowValue(inserted.front(), 0);
    }
    (void)co_await transaction.execute(R"sql(
UPDATE vpn_network
SET name = $2, overlay_cidr = $3, status = 'enabled', deleted_at = NULL, updated_at = NOW()
WHERE id = $1::uuid)sql",
                                        service::common::dbParams(
                                            networkId, kDefaultNetworkName, kDefaultOverlayCidr));
    co_await transaction.commit();
    co_return networkId;
}

inline ruvia::Task<std::optional<std::uint32_t>> VpnService::allocateAddress(
    ruvia::Context& c, std::string_view networkId, const Ipv4Cidr& overlay) {
    auto database = c.db();
    co_return co_await allocateAddressFromDb(database, networkId, overlay);
}

template <typename Db>
inline ruvia::Task<std::optional<std::uint32_t>> VpnService::allocateAddressFromDb(
    Db& db, std::string_view networkId, const Ipv4Cidr& overlay) {
    const auto rows = co_await db.query(
        "SELECT host(assigned_ipv4) FROM vpn_peer WHERE network_id = $1::uuid",
        service::common::dbParams(networkId));
    std::unordered_set<std::uint32_t> used;
    for (const auto& row : rows)
        if (const auto value = parseIpv4(detail::rowValue(row, 0)))
            used.emplace(*value);
    for (std::uint32_t offset = 2; offset + 1 < overlay.size(); ++offset) {
        const auto candidate = *hostAddress(overlay, offset);
        if (!used.contains(candidate))
            co_return candidate;
    }
    service::common::fail(21007, "VPN Overlay 地址池已耗尽", 409);
}

inline ruvia::Task<void> VpnService::validateAllowedRoutes(
    ruvia::Context& c, std::string_view networkId, const std::vector<std::string>& routes) {
    if (routes.empty())
        co_return;
    for (const auto& route : routes) {
        if (!parseCidr(route, 1, 30))
            service::common::fail(21001, "Peer 授权路由 CIDR 无效", 400);
    }
    const auto rows = co_await c.db().query(
        "SELECT virtual_cidr FROM vpn_route WHERE network_id = $1::uuid AND enabled",
        service::common::dbParams(networkId));
    std::unordered_set<std::string> allowed;
    for (const auto& row : rows)
        allowed.emplace(detail::rowValue(row, 0));
    for (const auto& route : routes)
        if (!allowed.contains(route))
            service::common::fail(21003, "Peer 授权路由不属于该 VPN 网络", 403);
}

inline ruvia::Task<std::string> VpnService::clientConfigJson(ruvia::Context& c,
                                                             std::string_view peerId,
                                                             std::string_view userId,
                                                             std::string_view privateKey) {
    requireUuid(peerId, "VPN Peer ID 无效");
    const auto rows = co_await c.db().query(R"sql(
SELECT p.id, p.name, host(p.assigned_ipv4), p.allowed_routes::text,
       n.hub_public_key, n.hub_endpoint, n.hub_listen_port, n.status, p.status, p.user_id,
       COALESCE((SELECT jsonb_agg(r.virtual_cidr ORDER BY r.virtual_cidr)
                 FROM vpn_route r
                 JOIN vpn_peer edge_peer ON edge_peer.id = r.edge_peer_id
                 JOIN edge_node e ON e.id = edge_peer.edge_node_id
                 WHERE r.network_id = n.id AND r.enabled AND r.status = 'active'
                   AND edge_peer.status = 'active'
                   AND e.enrollment_status = 'approved'), '[]'::jsonb)::text
FROM vpn_peer p JOIN vpn_network n ON n.id = p.network_id
WHERE p.id = $1::uuid AND p.peer_type = 'windows' AND p.user_id = $2::uuid
LIMIT 1)sql", service::common::dbParams(peerId, userId));
    if (rows.empty())
        service::common::fail(21004, "Windows VPN Peer 不存在或不属于当前用户", 404);
    const auto& row = rows.front();
    if (detail::rowValue(row, 7) != "enabled" || detail::rowValue(row, 8) != "active")
        service::common::fail(21003, "VPN Peer 当前不可用", 409);
    const auto hubKey = detail::rowValue(row, 4);
    if (!wireguard::validKey(hubKey))
        service::common::fail(21005, "Hub 公钥尚未配置", 503);
    const auto allowed = detail::rowValue(row, 10);
    auto allowedValues = detail::textArrayJson(allowed);
    if (allowedValues.empty())
        service::common::fail(21008, "VPN 当前没有可用虚拟网段", 409);
    const auto endpoint = detail::rowValue(row, 5);
    const auto port = detail::rowValue(row, 6);
    const auto portValue = integer(port);
    if (endpoint.empty() || portValue < 1 || portValue > 65535)
        service::common::fail(21005, "Hub 公网端点尚未配置", 503);
    const auto config = client_config::render(
        privateKey, detail::rowValue(row, 2), hubKey, endpoint,
        static_cast<std::uint16_t>(portValue), allowedValues);
    co_return "{\"peerId\":" + service::access::jsonQuoted(detail::rowValue(row, 0)) +
              ",\"name\":" + service::access::jsonQuoted(detail::rowValue(row, 1)) +
              ",\"assignedIpv4\":" + service::access::jsonQuoted(detail::rowValue(row, 2)) +
              ",\"allowedRoutes\":" + detail::jsonArray(allowedValues) +
              ",\"config\":" + service::access::jsonQuoted(config) + "}";
}

inline ruvia::Task<void> VpnService::queueEdgeConfig(ruvia::Context& c,
                                                     std::string_view peerId) {
    const auto principal = service::middleware::requireAuth(c);
    co_await service::vpn::queueEdgeConfig(c, peerId, principal.userId);
}

inline ruvia::Task<void> VpnService::audit(ruvia::Context& c, std::string_view actor,
                                           std::string_view action, std::string_view resource,
                                           std::string_view resourceId,
                                           std::string_view outcome,
                                           std::string_view details) {
    const auto auditId = service::common::nextUuidV7();
    (void)co_await c.db().execute(R"sql(
INSERT INTO security_audit_log(id, actor_user_id, action, resource_type, resource_id, outcome, details)
VALUES ($1::uuid, $2::uuid, $3, $4, NULLIF($5, '')::uuid, $6, $7::jsonb))sql",
                                  service::common::dbParams(auditId, actor,
                                                            action, resource, resourceId, outcome,
                                                            details));
}

inline ruvia::Task<wireguard::RuntimeStatus> VpnService::reconcileHub(ruvia::Context& c) {
    const auto config = co_await hub_config::loadOrInitialize(c, hubConfig(c));
    if (!config)
        co_return wireguard::RuntimeStatus{
            .supported = true,
            .configured = false,
            .code = "hub_config_missing",
            .message = "WireGuard Hub 配置尚未初始化",
            .peerCount = 0};
    auto& controller = wireguard::controller();
    auto result = controller.configure(*config);
    if (!result.configured)
        co_return result;
    const auto rows = co_await c.db().query(R"sql(
SELECT p.public_key, host(p.assigned_ipv4), p.peer_type,
       COALESCE((SELECT jsonb_agg(r.virtual_cidr ORDER BY r.virtual_cidr)
                 FROM vpn_route r WHERE r.edge_peer_id = p.id AND r.enabled), '[]'::jsonb)::text,
       COALESCE((SELECT jsonb_agg(client_route.virtual_cidr
                                  ORDER BY client_route.virtual_cidr)
                 FROM vpn_route client_route
                 JOIN vpn_peer edge_peer ON edge_peer.id = client_route.edge_peer_id
                 JOIN edge_node edge ON edge.id = edge_peer.edge_node_id
                 WHERE client_route.network_id = p.network_id
                   AND client_route.enabled AND client_route.status = 'active'
                   AND edge_peer.status = 'active'
                   AND edge.enrollment_status = 'approved'), '[]'::jsonb)::text,
       COALESCE((SELECT jsonb_agg(host(edge_peer.assigned_ipv4)
                                  ORDER BY host(edge_peer.assigned_ipv4))
                 FROM vpn_peer edge_peer
                 JOIN edge_node edge ON edge.id = edge_peer.edge_node_id
                 WHERE edge_peer.network_id = p.network_id
                   AND edge_peer.peer_type = 'edge' AND edge_peer.status = 'active'
                   AND edge.enrollment_status = 'approved'), '[]'::jsonb)::text
FROM vpn_peer p JOIN vpn_network n ON n.id = p.network_id
WHERE p.status = 'active' AND n.status = 'enabled' AND p.public_key IS NOT NULL
ORDER BY p.id)sql");
    std::size_t configuredPeers = 0;
    std::unordered_set<std::string> expectedKeys;
    std::vector<std::string> expectedRoutes;
    std::vector<firewall::ClientAccess> clients;
    for (const auto& row : rows) {
        const auto publicKey = detail::rowValue(row, 0);
        const auto assigned = detail::rowValue(row, 1);
        const auto peerType = detail::rowValue(row, 2);
        if (!wireguard::validKey(publicKey) || !parseIpv4(assigned))
            continue;
        wireguard::Peer peer;
        peer.publicKey = publicKey;
        peer.allowedIps.emplace_back(assigned + "/32");
        if (peerType == "edge") {
            for (const auto& route : detail::textArrayJson(detail::rowValue(row, 3)))
                peer.allowedIps.push_back(route);
        } else if (peerType == "windows") {
            clients.push_back(firewall::ClientAccess{
                .assignedIpv4 = assigned,
                .allowedRoutes = detail::textArrayJson(detail::rowValue(row, 4)),
                .edgeAddresses = detail::textArrayJson(detail::rowValue(row, 5)),
            });
        }
        const auto peerResult = controller.upsertPeer(*config, peer);
        if (!peerResult.configured)
            co_return peerResult;
        expectedRoutes.insert(expectedRoutes.end(), peer.allowedIps.begin(),
                              peer.allowedIps.end());
        expectedKeys.insert(publicKey);
        ++configuredPeers;
    }
    if (const auto currentPeers = controller.peerKeys(*config)) {
        for (const auto& publicKey : *currentPeers)
            if (!expectedKeys.contains(publicKey))
                (void)controller.removePeer(*config, publicKey);
    }
    const auto routeResult = controller.reconcileRoutes(*config, expectedRoutes);
    if (!routeResult.configured)
        co_return routeResult;
    const auto firewallResult = firewall::apply(config->interfaceName, clients);
    if (!firewallResult.configured)
        co_return wireguard::RuntimeStatus{
            .supported = true,
            .configured = false,
            .code = "firewall_configure_failed",
            .message = firewallResult.message,
            .peerCount = configuredPeers,
        };
    result.peerCount = configuredPeers;
    result.message = "WireGuard hub is configured";
    co_return result;
}

inline VpnService& vpnService() { return VpnService::instance(); }

} // namespace service::vpn
