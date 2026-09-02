#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <ruvia/core/Task.h>

#include "service/common/http.h"
#include "service/common/uuid.h"
#include "service/features/edge/dispatch.h"
#include "service/features/edge/protocol.h"

namespace service::vpn {

namespace detail {

inline std::string edgeConfigRowValue(const auto& row, std::size_t index) {
    return std::string(row[index].value().value_or(std::string_view{}));
}

} // namespace detail

// Queue a complete, versioned VPN configuration for an Edge peer. The actor is
// optional for background projection: in that case the VPN network owner is
// used so edge_task.created_by remains a valid audit subject.
template <typename Context>
ruvia::Task<void> queueEdgeConfig(Context& c, std::string_view peerId,
                                  std::string_view actorId = {}) {
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
    auto* request = envelope.mutable_vpn_config_request();
    request->set_request_id(
        service::edge::protocol::bytes(requestBytes, sizeof(requestBytes)));
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
INSERT INTO edge_task(id, node_id, task_type, request, created_by)
VALUES ($1::uuid, $2::uuid, 'vpn',
        jsonb_build_object('peerId', $3, 'configVersion', $4::bigint, 'enabled', $6), $5::uuid))sql",
                                  service::common::dbParams(requestId, nodeId, peerId,
                                                            nextVersion, createdBy,
                                                            request->enabled()));
    const auto key = "iot:edge:egress:" + nodeId;
    (void)co_await c.redis().rpush(key, wire);
    (void)co_await c.redis().ltrim(key, -100, -1);
    co_await service::edge::dispatch::notifyNode(c.redis(), nodeId);
    (void)co_await c.db().execute(
        "UPDATE vpn_peer SET config_revision = $2, updated_at = NOW() WHERE id = $1::uuid",
        service::common::dbParams(peerId, nextVersion));
}

} // namespace service::vpn
