#pragma once

#include <cstdint>
#include <memory_resource>
#include <string>

#include <ruvia/web/db/DbEntity.h>

namespace service::edge {

RUVIA_DB_ENTITY(
    EdgeNodeEntity, "edge_node",
    RUVIA_DB_COLUMN(id, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid,
                                           .primaryKey = true}),
    RUVIA_DB_COLUMN(platform_id, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid}),
    RUVIA_DB_COLUMN(imei, std::pmr::string), RUVIA_DB_COLUMN(name, std::pmr::string,
                                                              ruvia::DbColumnOptions{.nullable = true}),
    RUVIA_DB_COLUMN(model, std::pmr::string), RUVIA_DB_COLUMN(software_version, std::pmr::string),
    RUVIA_DB_COLUMN(hostname, std::pmr::string), RUVIA_DB_COLUMN(architecture, std::pmr::string),
    RUVIA_DB_COLUMN(openwrt_release, std::pmr::string),
    RUVIA_DB_COLUMN(enrollment_status, std::pmr::string),
    RUVIA_DB_COLUMN(group_id, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid,
                                           .nullable = true}),
    RUVIA_DB_COLUMN(status, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kJsonb}),
    RUVIA_DB_COLUMN(capability, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kJsonb}),
    RUVIA_DB_COLUMN(mobile, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kJsonb}),
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
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz}),
    RUVIA_DB_COLUMN(updated_at, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz}));

RUVIA_DB_ENTITY(
    EdgeNodeGroupEntity, "edge_node_group",
    RUVIA_DB_COLUMN(id, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid,
                                           .primaryKey = true}),
    RUVIA_DB_COLUMN(name, std::pmr::string),
    RUVIA_DB_COLUMN(parent_id, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid,
                                           .nullable = true}),
    RUVIA_DB_COLUMN(status, std::pmr::string), RUVIA_DB_COLUMN(sort_order, std::int32_t),
    RUVIA_DB_COLUMN(remark, std::pmr::string,
                    ruvia::DbColumnOptions{.nullable = true}),
    RUVIA_DB_COLUMN(created_by, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid}),
    RUVIA_DB_COLUMN(created_at, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz}),
    RUVIA_DB_COLUMN(updated_at, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz}),
    RUVIA_DB_COLUMN(deleted_at, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz,
                                           .nullable = true}));

RUVIA_DB_ENTITY(
    EdgeNodeInterfaceEntity, "edge_node_interface",
    RUVIA_DB_COLUMN(node_id, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid,
                                           .primaryKey = true}),
    RUVIA_DB_COLUMN(name, std::pmr::string, ruvia::DbColumnOptions{.primaryKey = true}),
    RUVIA_DB_COLUMN(display_name, std::pmr::string),
    RUVIA_DB_COLUMN(mac, std::pmr::string, ruvia::DbColumnOptions{.nullable = true}),
    RUVIA_DB_COLUMN(is_up, bool), RUVIA_DB_COLUMN(is_bridge, bool),
    RUVIA_DB_COLUMN(ipv4, std::pmr::string, ruvia::DbColumnOptions{.nullable = true}),
    RUVIA_DB_COLUMN(prefix_length, std::int32_t,
                    ruvia::DbColumnOptions{.nullable = true}),
    RUVIA_DB_COLUMN(gateway, std::pmr::string, ruvia::DbColumnOptions{.nullable = true}),
    RUVIA_DB_COLUMN(bridge_ports, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kJsonb}),
    RUVIA_DB_COLUMN(updated_at, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz}));

RUVIA_DB_ENTITY(
    EdgeNodeNetworkEntity, "edge_node_network",
    RUVIA_DB_COLUMN(node_id, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid,
                                           .primaryKey = true}),
    RUVIA_DB_COLUMN(name, std::pmr::string, ruvia::DbColumnOptions{.primaryKey = true}),
    RUVIA_DB_COLUMN(address_mode, std::pmr::string), RUVIA_DB_COLUMN(device, std::pmr::string),
    RUVIA_DB_COLUMN(is_up, bool), RUVIA_DB_COLUMN(is_bridge, bool),
    RUVIA_DB_COLUMN(ipv4, std::pmr::string, ruvia::DbColumnOptions{.nullable = true}),
    RUVIA_DB_COLUMN(prefix_length, std::int32_t,
                    ruvia::DbColumnOptions{.nullable = true}),
    RUVIA_DB_COLUMN(gateway, std::pmr::string, ruvia::DbColumnOptions{.nullable = true}),
    RUVIA_DB_COLUMN(bridge_ports, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kJsonb}),
    RUVIA_DB_COLUMN(updated_at, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz}));

RUVIA_DB_ENTITY(
    EdgeNodeSerialEntity, "edge_node_serial",
    RUVIA_DB_COLUMN(node_id, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid,
                                           .primaryKey = true}),
    RUVIA_DB_COLUMN(path, std::pmr::string, ruvia::DbColumnOptions{.primaryKey = true}),
    RUVIA_DB_COLUMN(display_name, std::pmr::string), RUVIA_DB_COLUMN(available, bool),
    RUVIA_DB_COLUMN(rs485, bool),
    RUVIA_DB_COLUMN(updated_at, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz}));

RUVIA_DB_ENTITY(
    EdgeFirmwareEntity, "edge_firmware",
    RUVIA_DB_COLUMN(id, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid,
                                           .primaryKey = true}),
    RUVIA_DB_COLUMN(version, std::pmr::string), RUVIA_DB_COLUMN(file_name, std::pmr::string),
    RUVIA_DB_COLUMN(storage_path, std::pmr::string), RUVIA_DB_COLUMN(sha256, std::pmr::string),
    RUVIA_DB_COLUMN(size_bytes, std::int64_t), RUVIA_DB_COLUMN(download_token, std::pmr::string),
    RUVIA_DB_COLUMN(created_by, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid}),
    RUVIA_DB_COLUMN(created_at, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz}));

RUVIA_DB_ENTITY(
    EdgeTaskEntity, "edge_task",
    RUVIA_DB_COLUMN(id, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid,
                                           .primaryKey = true}),
    RUVIA_DB_COLUMN(node_id, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid}),
    RUVIA_DB_COLUMN(task_type, std::pmr::string), RUVIA_DB_COLUMN(status, std::pmr::string),
    RUVIA_DB_COLUMN(request, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kJsonb}),
    RUVIA_DB_COLUMN(result, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kJsonb}),
    RUVIA_DB_COLUMN(created_by, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid}),
    RUVIA_DB_COLUMN(created_at, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz}),
    RUVIA_DB_COLUMN(updated_at, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz}),
    RUVIA_DB_COLUMN(completed_at, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz,
                                           .nullable = true}));

} // namespace service::edge
