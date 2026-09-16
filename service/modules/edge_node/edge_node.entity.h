#pragma once

#include <cstdint>
#include <memory_resource>
#include <string>

#include <ruvia/web/db/DbEntity.h>

namespace service::edge {

// GETDEL 一次性票据使用 Redis String；ORM Hash 映射不能表达该消费语义。
struct SerialDebugTicketRecord final {
    std::string nodeId;
    std::string nodeSession;
    std::string path;
    std::string encode() const { return nodeId + "\n" + nodeSession + "\n" + path; }
};

RUVIA_DB_ENTITY(
    EdgeNodeEntity, "edge_node",
    RUVIA_DB_COLUMN(id, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid,
                                           .primaryKey = true}),
    RUVIA_DB_COLUMN(platform_id, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid}),
    RUVIA_DB_COLUMN(imei, std::pmr::string, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 15}), RUVIA_DB_COLUMN(name, std::pmr::string,
                                                              ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .nullable = true, .length = 100}),
    RUVIA_DB_COLUMN(model, std::pmr::string, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 128, .defaultExpression = ruvia::FixedString{"''::character varying"}}), RUVIA_DB_COLUMN(software_version, std::pmr::string, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 32, .defaultExpression = ruvia::FixedString{"''::character varying"}}),
    RUVIA_DB_COLUMN(hostname, std::pmr::string, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 64, .defaultExpression = ruvia::FixedString{"''::character varying"}}), RUVIA_DB_COLUMN(architecture, std::pmr::string, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 32, .defaultExpression = ruvia::FixedString{"''::character varying"}}),
    RUVIA_DB_COLUMN(openwrt_release, std::pmr::string, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 64, .defaultExpression = ruvia::FixedString{"''::character varying"}}),
    RUVIA_DB_COLUMN(enrollment_status, std::pmr::string, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 20, .defaultExpression = ruvia::FixedString{"'pending'::character varying"}}),
    RUVIA_DB_COLUMN(group_id, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid,
                                           .nullable = true}),
    RUVIA_DB_COLUMN(status, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kJsonb, .defaultExpression = ruvia::FixedString{"'{\"log\": {\"level\": \"info\"}, \"config\": {\"state\": \"idle\", \"message\": \"\", \"activeVersion\": 0, \"desiredVersion\": 0}, \"outbox\": {\"bytes\": 0, \"records\": 0}}'::jsonb"}}),
    RUVIA_DB_COLUMN(capability, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kJsonb, .defaultExpression = ruvia::FixedString{"'{}'::jsonb"}}),
    RUVIA_DB_COLUMN(mobile, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kJsonb, .defaultExpression = ruvia::FixedString{"'{}'::jsonb"}}),
    RUVIA_DB_COLUMN(last_seen_at, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz,
                                           .nullable = true}),
    RUVIA_DB_COLUMN(approved_by, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid,
                                           .nullable = true}),
    RUVIA_DB_COLUMN(approved_at, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz,
                                           .nullable = true}),
    RUVIA_DB_COLUMN(created_at, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}),
    RUVIA_DB_COLUMN(updated_at, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}));

RUVIA_DB_ENTITY(
    EdgeNodeGroupEntity, "edge_node_group",
    RUVIA_DB_COLUMN(id, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid,
                                           .primaryKey = true}),
    RUVIA_DB_COLUMN(name, std::pmr::string, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 100}),
    RUVIA_DB_COLUMN(parent_id, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid,
                                           .nullable = true}),
    RUVIA_DB_COLUMN(status, std::pmr::string, ruvia::DbColumnOptions{.enumName = ruvia::FixedString{"status_enum"}, .defaultExpression = ruvia::FixedString{"'enabled'::status_enum"}}), RUVIA_DB_COLUMN(sort_order, std::int32_t, ruvia::DbColumnOptions{.defaultExpression = ruvia::FixedString{"0"}}),
    RUVIA_DB_COLUMN(remark, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .nullable = true, .length = 500}),
    RUVIA_DB_COLUMN(created_by, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid}),
    RUVIA_DB_COLUMN(created_at, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}),
    RUVIA_DB_COLUMN(updated_at, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}),
    RUVIA_DB_COLUMN(deleted_at, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz,
                                           .nullable = true}));

RUVIA_DB_ENTITY(
    EdgeNodeInterfaceEntity, "edge_node_interface",
    RUVIA_DB_COLUMN(node_id, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid,
                                           .primaryKey = true}),
    RUVIA_DB_COLUMN(name, std::pmr::string, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .primaryKey = true, .length = 32}),
    RUVIA_DB_COLUMN(display_name, std::pmr::string, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 64, .defaultExpression = ruvia::FixedString{"''::character varying"}}),
    RUVIA_DB_COLUMN(mac, std::pmr::string, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .nullable = true, .length = 17}),
    RUVIA_DB_COLUMN(is_up, bool, ruvia::DbColumnOptions{.defaultExpression = ruvia::FixedString{"false"}}), RUVIA_DB_COLUMN(is_bridge, bool, ruvia::DbColumnOptions{.defaultExpression = ruvia::FixedString{"false"}}),
    RUVIA_DB_COLUMN(ipv4, std::pmr::string, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .nullable = true, .length = 15}),
    RUVIA_DB_COLUMN(prefix_length, std::int32_t,
                    ruvia::DbColumnOptions{.nullable = true}),
    RUVIA_DB_COLUMN(gateway, std::pmr::string, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .nullable = true, .length = 15}),
    RUVIA_DB_COLUMN(bridge_ports, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kJsonb, .defaultExpression = ruvia::FixedString{"'[]'::jsonb"}}),
    RUVIA_DB_COLUMN(updated_at, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}));

RUVIA_DB_ENTITY(
    EdgeNodeNetworkEntity, "edge_node_network",
    RUVIA_DB_COLUMN(node_id, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid,
                                           .primaryKey = true}),
    RUVIA_DB_COLUMN(name, std::pmr::string, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .primaryKey = true, .length = 32}),
    RUVIA_DB_COLUMN(address_mode, std::pmr::string, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 12}), RUVIA_DB_COLUMN(device, std::pmr::string, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 32, .defaultExpression = ruvia::FixedString{"''::character varying"}}),
    RUVIA_DB_COLUMN(is_up, bool, ruvia::DbColumnOptions{.defaultExpression = ruvia::FixedString{"false"}}), RUVIA_DB_COLUMN(is_bridge, bool, ruvia::DbColumnOptions{.defaultExpression = ruvia::FixedString{"false"}}),
    RUVIA_DB_COLUMN(ipv4, std::pmr::string, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .nullable = true, .length = 15}),
    RUVIA_DB_COLUMN(prefix_length, std::int32_t,
                    ruvia::DbColumnOptions{.nullable = true}),
    RUVIA_DB_COLUMN(gateway, std::pmr::string, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .nullable = true, .length = 15}),
    RUVIA_DB_COLUMN(bridge_ports, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kJsonb, .defaultExpression = ruvia::FixedString{"'[]'::jsonb"}}),
    RUVIA_DB_COLUMN(updated_at, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}));

RUVIA_DB_ENTITY(
    EdgeNodeSerialEntity, "edge_node_serial",
    RUVIA_DB_COLUMN(node_id, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid,
                                           .primaryKey = true}),
    RUVIA_DB_COLUMN(path, std::pmr::string, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .primaryKey = true, .length = 96}),
    RUVIA_DB_COLUMN(display_name, std::pmr::string, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 64, .defaultExpression = ruvia::FixedString{"''::character varying"}}), RUVIA_DB_COLUMN(available, bool, ruvia::DbColumnOptions{.defaultExpression = ruvia::FixedString{"false"}}),
    RUVIA_DB_COLUMN(rs485, bool, ruvia::DbColumnOptions{.defaultExpression = ruvia::FixedString{"false"}}),
    RUVIA_DB_COLUMN(updated_at, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}));

RUVIA_DB_ENTITY(
    EdgeFirmwareEntity, "edge_firmware",
    RUVIA_DB_COLUMN(id, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid,
                                           .primaryKey = true}),
    RUVIA_DB_COLUMN(version, std::pmr::string, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 64}), RUVIA_DB_COLUMN(file_name, std::pmr::string, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 255}),
    RUVIA_DB_COLUMN(storage_path, std::pmr::string), RUVIA_DB_COLUMN(sha256, std::pmr::string, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 64}),
    RUVIA_DB_COLUMN(size_bytes, std::int64_t), RUVIA_DB_COLUMN(download_token, std::pmr::string, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 64}),
    RUVIA_DB_COLUMN(created_by, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid}),
    RUVIA_DB_COLUMN(created_at, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}));

RUVIA_DB_ENTITY(
    EdgeTaskEntity, "edge_task",
    RUVIA_DB_COLUMN(id, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid,
                                           .primaryKey = true}),
    RUVIA_DB_COLUMN(node_id, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid}),
    RUVIA_DB_COLUMN(task_type, std::pmr::string, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 30}), RUVIA_DB_COLUMN(status, std::pmr::string, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 20, .defaultExpression = ruvia::FixedString{"'pending'::character varying"}}),
    RUVIA_DB_COLUMN(request, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kJsonb, .defaultExpression = ruvia::FixedString{"'{}'::jsonb"}}),
    RUVIA_DB_COLUMN(result, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kJsonb, .defaultExpression = ruvia::FixedString{"'{}'::jsonb"}}),
    RUVIA_DB_COLUMN(created_by, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid}),
    RUVIA_DB_COLUMN(created_at, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}),
    RUVIA_DB_COLUMN(updated_at, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}),
    RUVIA_DB_COLUMN(completed_at, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz,
                                           .nullable = true}));

} // namespace service::edge

namespace service::edge_node::entities {

RUVIA_DB_ENTITY(VpnPeerEntity, "vpn_peer",
    RUVIA_DB_COLUMN(id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid, .primaryKey = true}),
    RUVIA_DB_COLUMN(network_id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid}),
    RUVIA_DB_COLUMN(peer_type, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 16}),
    RUVIA_DB_COLUMN(edge_node_id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid, .nullable = true}),
    RUVIA_DB_COLUMN(user_id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid, .nullable = true}),
    RUVIA_DB_COLUMN(name, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 100}),
    RUVIA_DB_COLUMN(public_key, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 64, .defaultExpression = ruvia::FixedString{"''::character varying"}}),
    RUVIA_DB_COLUMN(assigned_ipv4, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kInet}),
    RUVIA_DB_COLUMN(allowed_routes, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kJsonb, .defaultExpression = ruvia::FixedString{"'[]'::jsonb"}}),
    RUVIA_DB_COLUMN(status, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 16, .defaultExpression = ruvia::FixedString{"'active'::character varying"}}),
    RUVIA_DB_COLUMN(config_revision, std::int64_t,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kBigInt, .defaultExpression = ruvia::FixedString{"1"}}),
    RUVIA_DB_COLUMN(last_handshake_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .nullable = true}),
    RUVIA_DB_COLUMN(created_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}),
    RUVIA_DB_COLUMN(updated_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}),
    RUVIA_DB_COLUMN(revoked_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .nullable = true}),
    RUVIA_DB_COLUMN(client_private_key, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 44, .defaultExpression = ruvia::FixedString{"''::character varying"}}),
    RUVIA_DB_COLUMN(client_managed, bool,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kBoolean, .defaultExpression = ruvia::FixedString{"false"}}))

RUVIA_DB_ENTITY(VpnRouteEntity, "vpn_route",
    RUVIA_DB_COLUMN(id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid, .primaryKey = true}),
    RUVIA_DB_COLUMN(network_id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid}),
    RUVIA_DB_COLUMN(edge_peer_id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid}),
    RUVIA_DB_COLUMN(lan_interface, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 32}),
    RUVIA_DB_COLUMN(target_cidr, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 18}),
    RUVIA_DB_COLUMN(virtual_cidr, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 18}),
    RUVIA_DB_COLUMN(mode, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 16, .defaultExpression = ruvia::FixedString{"'nat'::character varying"}}),
    RUVIA_DB_COLUMN(nat_mode, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 16, .defaultExpression = ruvia::FixedString{"'masquerade'::character varying"}}),
    RUVIA_DB_COLUMN(status, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 16, .defaultExpression = ruvia::FixedString{"'active'::character varying"}}),
    RUVIA_DB_COLUMN(enabled, bool,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kBoolean, .defaultExpression = ruvia::FixedString{"true"}}),
    RUVIA_DB_COLUMN(last_error, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kText, .defaultExpression = ruvia::FixedString{"''::text"}}),
    RUVIA_DB_COLUMN(created_by, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid}),
    RUVIA_DB_COLUMN(created_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}),
    RUVIA_DB_COLUMN(updated_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}))

} // namespace service::edge_node::entities
