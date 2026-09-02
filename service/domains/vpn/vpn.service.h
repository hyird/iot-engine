#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdint>
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
#include "service/features/vpn/edge-config.h"
#include "service/features/vpn/wireguard.h"
#include "service/middleware/auth.h"
#include "service/domains/vpn/vpn.types.h"

namespace service::vpn {

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
    static ruvia::Task<void> audit(ruvia::Context&, std::string_view, std::string_view,
                                   std::string_view, std::string_view, std::string_view,
                                   std::string_view);
    static ruvia::Task<void> queueEdgeConfig(ruvia::Context&, std::string_view);
    static ruvia::Task<std::string> clientConfigJson(ruvia::Context&, std::string_view,
                                                     std::string_view);
    static ruvia::Task<std::optional<std::uint32_t>> allocateAddress(
        ruvia::Context&, std::string_view, const Ipv4Cidr&);
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
        std::vector<ruvia::DbValue> params;
        std::string where{" WHERE deleted_at IS NULL"};
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
                                           const ruvia::JsonValue& payload) {
        const auto name = detail::requiredText(payload, "name", 100, "VPN 网络名称不能为空");
        const auto overlayText = detail::optionalText(payload, "overlayCidr")
                                     .value_or("100.96.0.0/24");
        const auto overlay = parseCidr(overlayText, 16, 30);
        if (!overlay || !kOverlayPool.contains(overlay->network) ||
            !kOverlayPool.contains(overlay->network + overlay->size() - 1U))
            service::common::fail(21001, "Overlay CIDR 必须位于 100.96.0.0/11 且前缀为 /16-/30", 400);
        const auto existing = co_await c.db().query(
            "SELECT overlay_cidr FROM vpn_network WHERE deleted_at IS NULL");
        for (const auto& row : existing) {
            const auto current = parseCidr(detail::rowValue(row, 0), 1, 32);
            if (current && current->overlaps(*overlay))
                service::common::fail(21002, "Overlay CIDR 与现有 VPN 网络重叠", 409);
        }
        const auto hub = hubConfig(c);
        const auto port = detail::optionalInteger(payload, "hubListenPort").value_or(hub.listenPort);
        if (port < 1 || port > 65535)
            service::common::fail(21001, "WireGuard 监听端口必须在 1 - 65535", 400);
        if (port != hub.listenPort)
            service::common::fail(21003, "VPN 网络监听端口必须与 Hub 监听端口一致", 409);
        const auto endpoint = detail::optionalText(payload, "hubEndpoint").value_or(hub.endpoint);
        const auto principal = service::middleware::requireAuth(c);
        const auto id = service::common::nextUuidV7();
        const auto overlayCidr = overlay->text();
        (void)co_await c.db().execute(R"sql(
INSERT INTO vpn_network(id, name, overlay_cidr, hub_public_key, hub_endpoint, hub_listen_port, created_by)
VALUES ($1::uuid, $2, $3, $4, $5, $6, $7::uuid))sql",
                                      service::common::dbParams(id, name, overlayCidr, hub.publicKey,
                                                                endpoint, port, principal.userId));
        co_await audit(c, principal.userId, "vpn.network.create", "vpn_network", id,
                       "success", "{}");
        (void)co_await reconcileHub(c);
        co_return id;
    }

    ruvia::Task<void> updateNetwork(ruvia::Context& c, std::string_view id,
                                    const ruvia::JsonValue& payload) {
        requireUuid(id, "VPN 网络 ID 无效");
        const auto current = co_await c.db().query(
            "SELECT id, overlay_cidr FROM vpn_network WHERE id = $1::uuid AND deleted_at IS NULL",
            service::common::dbParams(id));
        if (current.empty())
            service::common::fail(21004, "VPN 网络不存在", 404);
        const auto hub = hubConfig(c);
        const auto edgePeers = co_await c.db().query(
            "SELECT id::text FROM vpn_peer WHERE network_id = $1::uuid AND peer_type = 'edge'",
            service::common::dbParams(id));
        const auto activeKeys = co_await c.db().query(
            "SELECT public_key FROM vpn_peer WHERE network_id = $1::uuid AND status = 'active' "
            "AND public_key <> ''",
            service::common::dbParams(id));
        if (detail::optionalText(payload, "overlayCidr"))
            service::common::fail(21003, "已创建的 VPN 网络不允许修改 Overlay CIDR", 409);
        const auto name = detail::optionalText(payload, "name");
        const auto status = detail::optionalText(payload, "status");
        const auto endpoint = detail::optionalText(payload, "hubEndpoint");
        const auto port = detail::optionalInteger(payload, "hubListenPort");
        if (status && *status != "enabled" && *status != "disabled")
            service::common::fail(21001, "VPN 网络状态无效", 400);
        if (port && (*port < 1 || *port > 65535))
            service::common::fail(21001, "WireGuard 监听端口必须在 1 - 65535", 400);
        if (port && *port != hub.listenPort)
            service::common::fail(21003, "VPN 网络监听端口必须与 Hub 监听端口一致", 409);
        if (!name && !status && !endpoint && !port)
            service::common::fail(21001, "没有可更新的 VPN 网络字段", 400);
        std::string set;
        std::vector<ruvia::DbValue> params;
        const auto append = [&](std::string_view expression, const auto& value) {
            if (!set.empty())
                set += ", ";
            params.emplace_back(value);
            set += std::string(expression) + " $" + std::to_string(params.size());
        };
        if (name) append("name =", *name);
        if (status) append("status =", *status);
        if (endpoint) append("hub_endpoint =", *endpoint);
        if (port) append("hub_listen_port =", *port);
        set += ", updated_at = NOW()";
        params.emplace_back(id);
        (void)co_await c.db().execute("UPDATE vpn_network SET " + set +
                                          " WHERE id = $" + std::to_string(params.size()) +
                                          "::uuid AND deleted_at IS NULL", params);
        const auto principal = service::middleware::requireAuth(c);
        co_await audit(c, principal.userId, "vpn.network.update", "vpn_network", id,
                       "success", "{}");
        for (const auto& row : edgePeers)
            co_await service::vpn::queueEdgeConfig(
                c, detail::rowValue(row, 0), principal.userId);
        if (status && *status == "disabled") {
            for (const auto& row : activeKeys) {
                const auto key = detail::rowValue(row, 0);
                if (detail::validKey(key))
                    (void)wireguard::controller().removePeer(hub, key);
            }
        }
        (void)co_await reconcileHub(c);
    }

    ruvia::Task<void> removeNetwork(ruvia::Context& c, std::string_view id) {
        requireUuid(id, "VPN 网络 ID 无效");
        const auto keys = co_await c.db().query(
            "SELECT public_key FROM vpn_peer WHERE network_id = $1::uuid AND public_key <> ''",
            service::common::dbParams(id));
        const auto removed = co_await c.db().query(
            "DELETE FROM vpn_network WHERE id = $1::uuid RETURNING id",
            service::common::dbParams(id));
        if (removed.empty())
            service::common::fail(21004, "VPN 网络不存在", 404);
        const auto config = hubConfig(c);
        for (const auto& row : keys) {
            const auto key = detail::rowValue(row, 0);
            if (detail::validKey(key))
                (void)wireguard::controller().removePeer(config, key);
        }
        const auto principal = service::middleware::requireAuth(c);
        co_await audit(c, principal.userId, "vpn.network.delete", "vpn_network", id,
                       "success", "{}");
        (void)co_await reconcileHub(c);
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
        const auto networkId = detail::requiredUuid(payload, "networkId", "VPN 网络 ID 无效");
        const auto edgePeerId = detail::requiredUuid(payload, "edgePeerId", "Edge Peer ID 无效");
        const auto lanInterface = detail::requiredText(payload, "lanInterface", 32,
                                                        "LAN 接口不能为空");
        if (!detail::validInterface(lanInterface))
            service::common::fail(21001, "LAN 接口名称包含非法字符", 400);
        const auto targetText = detail::requiredText(payload, "targetCidr", 18,
                                                     "真实 LAN CIDR 不能为空");
        const auto virtualText = detail::requiredText(payload, "virtualCidr", 18,
                                                      "虚拟 LAN CIDR 不能为空");
        const auto target = parseCidr(targetText, 1, 30);
        const auto virtualNetwork = parseCidr(virtualText, 1, 30);
        if (!target || !virtualNetwork || target->prefix != virtualNetwork->prefix ||
            !isPrivateIpv4(*target) || !kVirtualLanPool.contains(virtualNetwork->network) ||
            !kVirtualLanPool.contains(virtualNetwork->network + virtualNetwork->size() - 1U))
            service::common::fail(21001, "真实 LAN 与虚拟 LAN 必须是等长 IPv4 私网 CIDR，虚拟网段必须位于 172.31.0.0/16", 400);
        const auto mode = detail::optionalText(payload, "mode").value_or("nat");
        if (mode != "nat" && mode != "routed")
            service::common::fail(21001, "VPN 路由模式只支持 nat 或 routed", 400);
        const auto natMode = mode == "nat" ? "masquerade" : "none";
        if (detail::optionalText(payload, "natMode") &&
            *detail::optionalText(payload, "natMode") != natMode)
            service::common::fail(21001, "NAT 模式与路由模式不匹配", 400);
        const auto enabled = detail::optionalBoolean(payload, "enabled").value_or(true);
        const auto edge = co_await c.db().query(R"sql(
SELECT p.network_id, p.peer_type, p.edge_node_id, n.overlay_cidr
FROM vpn_peer p JOIN vpn_network n ON n.id = p.network_id
WHERE p.id = $1::uuid AND p.status <> 'revoked' AND n.status = 'enabled'
LIMIT 1)sql", service::common::dbParams(edgePeerId));
        if (edge.empty() || detail::rowValue(edge.front(), 1) != "edge" ||
            detail::rowValue(edge.front(), 0) != networkId)
            service::common::fail(21004, "Edge Peer 不属于指定 VPN 网络", 404);
        const auto conflicts = co_await c.db().query(
            "SELECT virtual_cidr FROM vpn_route WHERE network_id = $1::uuid AND enabled",
            service::common::dbParams(networkId));
        for (const auto& row : conflicts) {
            const auto current = parseCidr(detail::rowValue(row, 0), 1, 32);
            if (current && current->overlaps(*virtualNetwork))
                service::common::fail(21002, "虚拟 LAN CIDR 与现有路由重叠", 409);
        }
        const auto id = service::common::nextUuidV7();
        const auto principal = service::middleware::requireAuth(c);
        const auto targetCidr = target->text();
        const auto virtualCidr = virtualNetwork->text();
        (void)co_await c.db().execute(R"sql(
INSERT INTO vpn_route(id, network_id, edge_peer_id, lan_interface, target_cidr, virtual_cidr,
                      mode, nat_mode, enabled, created_by)
VALUES ($1::uuid, $2::uuid, $3::uuid, $4, $5, $6, $7, $8, $9, $10::uuid))sql",
                                      service::common::dbParams(id, networkId, edgePeerId,
                                                                lanInterface, targetCidr,
                                                                virtualCidr, mode, natMode,
                                                                enabled, principal.userId));
        co_await audit(c, principal.userId, "vpn.route.create", "vpn_route", id,
                       "success", "{}");
        co_await queueEdgeConfig(c, edgePeerId);
        (void)co_await reconcileHub(c);
        co_return id;
    }

    ruvia::Task<void> updateRoute(ruvia::Context& c, std::string_view id,
                                  const ruvia::JsonValue& payload) {
        requireUuid(id, "VPN 路由 ID 无效");
        const auto rows = co_await c.db().query(
            "SELECT edge_peer_id, network_id, target_cidr, virtual_cidr, mode, enabled "
            "FROM vpn_route WHERE id = $1::uuid", service::common::dbParams(id));
        if (rows.empty())
            service::common::fail(21004, "VPN 路由不存在", 404);
        const auto edgePeerId = detail::rowValue(rows.front(), 0);
        const auto networkId = detail::rowValue(rows.front(), 1);
        const auto currentTarget = detail::rowValue(rows.front(), 2);
        const auto currentVirtual = detail::rowValue(rows.front(), 3);
        const auto targetText = detail::optionalText(payload, "targetCidr").value_or(currentTarget);
        const auto virtualText = detail::optionalText(payload, "virtualCidr").value_or(currentVirtual);
        const auto target = parseCidr(targetText, 1, 30);
        const auto virtualNetwork = parseCidr(virtualText, 1, 30);
        if (!target || !virtualNetwork || target->prefix != virtualNetwork->prefix ||
            !isPrivateIpv4(*target) || !kVirtualLanPool.contains(virtualNetwork->network) ||
            !kVirtualLanPool.contains(virtualNetwork->network + virtualNetwork->size() - 1U))
            service::common::fail(21001, "VPN 路由 CIDR 无效或不是等长私网映射", 400);
        const auto mode = detail::optionalText(payload, "mode").value_or(detail::rowValue(rows.front(), 4));
        if (mode != "nat" && mode != "routed")
            service::common::fail(21001, "VPN 路由模式无效", 400);
        const auto enabled = detail::optionalBoolean(payload, "enabled").value_or(
            detail::rowValue(rows.front(), 5) == "t");
        const auto interface = detail::optionalText(payload, "lanInterface");
        if (interface && !detail::validInterface(*interface))
            service::common::fail(21001, "LAN 接口名称包含非法字符", 400);
        const auto natMode = mode == "nat" ? "masquerade" : "none";
        if (enabled) {
            const auto conflicts = co_await c.db().query(
                "SELECT virtual_cidr FROM vpn_route WHERE network_id = $1::uuid "
                "AND enabled AND id <> $2::uuid",
                service::common::dbParams(networkId, id));
            for (const auto& row : conflicts) {
                const auto current = parseCidr(detail::rowValue(row, 0), 1, 32);
                if (current && current->overlaps(*virtualNetwork))
                    service::common::fail(21002, "虚拟 LAN CIDR 与现有路由重叠", 409);
            }
        }
        const auto interfaceName = interface.value_or("");
        const auto targetCidr = target->text();
        const auto virtualCidr = virtualNetwork->text();
        (void)co_await c.db().execute(R"sql(
UPDATE vpn_route SET lan_interface = COALESCE(NULLIF($2, ''), lan_interface), target_cidr = $3,
    virtual_cidr = $4, mode = $5, nat_mode = $6, enabled = $7,
    status = CASE WHEN $7 THEN 'active' ELSE 'disabled' END, last_error = '', updated_at = NOW()
WHERE id = $1::uuid)sql",
                                      service::common::dbParams(id, interfaceName,
                                                                targetCidr, virtualCidr,
                                                                mode, natMode, enabled));
        const auto principal = service::middleware::requireAuth(c);
        co_await audit(c, principal.userId, "vpn.route.update", "vpn_route", id,
                       "success", "{}");
        co_await queueEdgeConfig(c, edgePeerId);
        (void)co_await reconcileHub(c);
    }

    ruvia::Task<void> removeRoute(ruvia::Context& c, std::string_view id) {
        requireUuid(id, "VPN 路由 ID 无效");
        const auto rows = co_await c.db().query(
            "DELETE FROM vpn_route WHERE id = $1::uuid RETURNING edge_peer_id",
            service::common::dbParams(id));
        if (rows.empty())
            service::common::fail(21004, "VPN 路由不存在", 404);
        const auto principal = service::middleware::requireAuth(c);
        co_await audit(c, principal.userId, "vpn.route.delete", "vpn_route", id,
                       "success", "{}");
        co_await queueEdgeConfig(c, detail::rowValue(rows.front(), 0));
        (void)co_await reconcileHub(c);
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
        const auto networkId = detail::requiredUuid(payload, "networkId", "VPN 网络 ID 无效");
        const auto peerType = detail::requiredText(payload, "peerType", 16, "Peer 类型不能为空");
        if (peerType != "windows" && peerType != "edge")
            service::common::fail(21001, "Peer 类型只支持 windows 或 edge", 400);
        const auto name = detail::requiredText(payload, "name", 100, "Peer 名称不能为空");
        const auto publicKey = detail::optionalText(payload, "publicKey").value_or("");
        if (!publicKey.empty() && !detail::validKey(publicKey))
            service::common::fail(21001, "WireGuard 公钥格式无效", 400);
        const auto principal = service::middleware::requireAuth(c);
        std::string edgeNodeId;
        std::string userId = principal.userId;
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
                "SELECT id FROM edge_node WHERE id = $1::uuid AND enrollment_status = 'approved'",
                service::common::dbParams(edgeNodeId));
            if (edge.empty())
                service::common::fail(21004, "Edge 节点不存在或尚未批准", 404);
            const auto duplicate = co_await c.db().query(
                "SELECT id FROM vpn_peer WHERE network_id = $1::uuid AND edge_node_id = $2::uuid "
                "AND status <> 'revoked'", service::common::dbParams(networkId, edgeNodeId));
            if (!duplicate.empty())
                service::common::fail(21002, "该 Edge 节点已经加入 VPN 网络", 409);
            userId.clear();
        }
        const auto overlay = parseCidr(detail::rowValue(network.front(), 0), 16, 30);
        if (!overlay)
            service::common::fail(21005, "VPN 网络 Overlay 配置损坏", 500);
        const auto assigned = co_await allocateAddress(c, networkId, *overlay);
        const auto allowedRoutes = detail::textArray(payload, "allowedRoutes");
        if (allowedRoutes.size() > 64)
            service::common::fail(21001, "Peer 最多授权 64 条路由", 400);
        co_await validateAllowedRoutes(c, networkId, allowedRoutes);
        const auto id = service::common::nextUuidV7();
        const auto status = publicKey.empty() ? "pending" : "active";
        const auto assignedText = detail::hostText(*assigned);
        const auto allowedRoutesJson = detail::jsonArray(allowedRoutes);
        (void)co_await c.db().execute(R"sql(
INSERT INTO vpn_peer(id, network_id, peer_type, edge_node_id, user_id, name, public_key,
                     assigned_ipv4, allowed_routes, status)
VALUES ($1::uuid, $2::uuid, $3, NULLIF($4, '')::uuid, NULLIF($5, '')::uuid,
        $6, $7, $8::inet, $9::jsonb, $10))sql",
                                      service::common::dbParams(id, networkId, peerType, edgeNodeId,
                                                                userId, name, publicKey,
                                                                assignedText, allowedRoutesJson, status));
        co_await audit(c, principal.userId, "vpn.peer.create", "vpn_peer", id,
                       "success", "{}");
        if (peerType == "edge")
            co_await queueEdgeConfig(c, id);
        (void)co_await reconcileHub(c);
        co_return id;
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
            "SELECT edge_node_id, peer_type, status FROM vpn_peer WHERE id = $1::uuid",
            service::common::dbParams(id));
        if (rows.empty())
            service::common::fail(21004, "VPN Peer 不存在", 404);
        if (detail::rowValue(rows.front(), 1) != "edge")
            service::common::fail(21001, "只有 Edge Peer 支持重新下发", 400);
        if (detail::rowValue(rows.front(), 2) == "revoked")
            service::common::fail(21003, "VPN Peer 已撤销", 409);
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
        const auto networkId = detail::requiredUuid(payload, "networkId", "VPN 网络 ID 无效");
        const auto network = co_await c.db().query(
            "SELECT id FROM vpn_network WHERE id = $1::uuid AND status = 'enabled' AND deleted_at IS NULL",
            service::common::dbParams(networkId));
        if (network.empty())
            service::common::fail(21004, "VPN 网络不存在或已停用", 404);
        const auto routes = detail::textArray(payload, "allowedRoutes");
        if (routes.empty() || routes.size() > 64)
            service::common::fail(21001, "Enrollment 至少需要授权一条路由，最多 64 条", 400);
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
        co_return co_await clientConfigJson(c, peerId, principal.userId);
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
        const auto config = hubConfig(c);
        const auto runtime = wireguard::controller().status(config);
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
    config.interfaceName = std::string(c.env().get("VPN_HUB_INTERFACE").value_or("wg-iot"));
    config.privateKey = std::string(c.env().get("VPN_HUB_PRIVATE_KEY").value_or(""));
    config.publicKey = std::string(c.env().get("VPN_HUB_PUBLIC_KEY").value_or(""));
    config.endpoint = std::string(c.env().get("VPN_HUB_ENDPOINT").value_or(""));
    config.listenPort = c.env().get<std::uint16_t>("VPN_HUB_LISTEN_PORT").value_or(51820);
    return config;
}

inline ruvia::Task<std::optional<std::uint32_t>> VpnService::allocateAddress(
    ruvia::Context& c, std::string_view networkId, const Ipv4Cidr& overlay) {
    const auto rows = co_await c.db().query(
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
                                                             std::string_view userId) {
    requireUuid(peerId, "VPN Peer ID 无效");
    const auto rows = co_await c.db().query(R"sql(
SELECT p.id, p.name, host(p.assigned_ipv4), p.allowed_routes::text,
       n.hub_public_key, n.hub_endpoint, n.hub_listen_port, n.status, p.status, p.user_id,
       COALESCE((SELECT jsonb_agg(r.virtual_cidr ORDER BY r.virtual_cidr)
                 FROM vpn_route r WHERE r.network_id = n.id AND r.enabled), '[]'::jsonb)::text
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
    auto allowed = detail::rowValue(row, 3);
    if (allowed == "[]" || allowed.empty())
        allowed = detail::rowValue(row, 10);
    const auto allowedValues = detail::textArrayJson(allowed);
    const auto endpoint = detail::rowValue(row, 5);
    const auto port = detail::rowValue(row, 6);
    const auto endpointText = endpoint.empty() ? std::string{} : endpoint + ":" + port;
    std::string config{"[Interface]\nPrivateKey = <client-private-key>\nAddress = " +
                       detail::rowValue(row, 2) + "/32\nMTU = 1280\n\n[Peer]\nPublicKey = " +
                       hubKey + "\nEndpoint = " + endpointText + "\nAllowedIPs = "};
    for (std::size_t index = 0; index < allowedValues.size(); ++index) {
        if (index != 0)
            config += ", ";
        config += allowedValues[index];
    }
    config += "\nPersistentKeepalive = 25\n";
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
    const auto config = hubConfig(c);
    auto& controller = wireguard::controller();
    auto result = controller.configure(config);
    if (!result.configured)
        co_return result;
    const auto rows = co_await c.db().query(R"sql(
SELECT p.public_key, host(p.assigned_ipv4), p.peer_type,
       COALESCE((SELECT jsonb_agg(r.virtual_cidr ORDER BY r.virtual_cidr)
                 FROM vpn_route r WHERE r.edge_peer_id = p.id AND r.enabled), '[]'::jsonb)::text
FROM vpn_peer p JOIN vpn_network n ON n.id = p.network_id
WHERE p.status = 'active' AND n.status = 'enabled' AND p.public_key IS NOT NULL
ORDER BY p.id)sql");
    std::size_t configuredPeers = 0;
    std::unordered_set<std::string> expectedKeys;
    for (const auto& row : rows) {
        const auto publicKey = detail::rowValue(row, 0);
        const auto assigned = detail::rowValue(row, 1);
        if (!wireguard::validKey(publicKey) || !parseIpv4(assigned))
            continue;
        wireguard::Peer peer;
        peer.publicKey = publicKey;
        peer.allowedIps.emplace_back(assigned + "/32");
        for (const auto& route : detail::textArrayJson(detail::rowValue(row, 3)))
            peer.allowedIps.push_back(route);
        const auto peerResult = controller.upsertPeer(config, peer);
        if (!peerResult.configured)
            co_return peerResult;
        expectedKeys.insert(publicKey);
        ++configuredPeers;
    }
    if (const auto currentPeers = controller.peerKeys(config)) {
        for (const auto& publicKey : *currentPeers)
            if (!expectedKeys.contains(publicKey))
                (void)controller.removePeer(config, publicKey);
    }
    result.peerCount = configuredPeers;
    result.message = "WireGuard hub is configured";
    co_return result;
}

inline VpnService& vpnService() { return VpnService::instance(); }

} // namespace service::vpn
