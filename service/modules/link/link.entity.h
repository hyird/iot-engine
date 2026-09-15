#pragma once

#include <memory_resource>
#include <string>

#include <ruvia/web/db/DbEntity.h>

namespace service::link {
// 调试报文使用有界 Redis Stream；固定 Hash ORM 无法表达动态原始报文字段。
struct LinkDebugStream {
    static std::string key(std::string_view id) { return "iot:debug:packets:link:" + std::string(id); }
};


RUVIA_DB_ENTITY(
    LinkEntity, "link",
    RUVIA_DB_COLUMN(debug_enabled, bool, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kBoolean, .defaultExpression = ruvia::FixedString{"false"}}),
    RUVIA_DB_COLUMN(id, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid,
                                           .primaryKey = true}),
    RUVIA_DB_COLUMN(name, std::pmr::string, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 100}), RUVIA_DB_COLUMN(protocol, std::pmr::string, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 20}),
    RUVIA_DB_COLUMN(endpoint, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kJsonb}),
    RUVIA_DB_COLUMN(execution, std::pmr::string, ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 16}),
    RUVIA_DB_COLUMN(edge_node_id, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid,
                                           .nullable = true}),
    RUVIA_DB_COLUMN(status, std::pmr::string, ruvia::DbColumnOptions{.enumName = ruvia::FixedString{"status_enum"}, .defaultExpression = ruvia::FixedString{"'enabled'::status_enum"}}),
    RUVIA_DB_COLUMN(created_by, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid}),
    RUVIA_DB_COLUMN(created_at, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}),
    RUVIA_DB_COLUMN(updated_at, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}),
    RUVIA_DB_COLUMN(deleted_at, std::pmr::string,
                    ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz,
                                           .nullable = true}))

} // namespace service::link

namespace service::link::entities {

RUVIA_DB_ENTITY(DeviceEntity, "device",
    RUVIA_DB_COLUMN(id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid, .primaryKey = true}),
    RUVIA_DB_COLUMN(name, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 100}),
    RUVIA_DB_COLUMN(link_id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid}),
    RUVIA_DB_COLUMN(protocol_config_id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid}),
    RUVIA_DB_COLUMN(group_id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid, .nullable = true}),
    RUVIA_DB_COLUMN(status, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kText, .enumName = ruvia::FixedString{"status_enum"}, .defaultExpression = ruvia::FixedString{"'enabled'::status_enum"}}),
    RUVIA_DB_COLUMN(protocol_params, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kJsonb, .defaultExpression = ruvia::FixedString{"'{}'::jsonb"}}),
    RUVIA_DB_COLUMN(remark, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kText, .nullable = true}),
    RUVIA_DB_COLUMN(created_by, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid}),
    RUVIA_DB_COLUMN(created_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}),
    RUVIA_DB_COLUMN(updated_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}),
    RUVIA_DB_COLUMN(deleted_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .nullable = true}),
    RUVIA_DB_COLUMN(protocol_address, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kText}))

RUVIA_DB_ENTITY(EdgeNodeEntity, "edge_node",
    RUVIA_DB_COLUMN(id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid, .primaryKey = true}),
    RUVIA_DB_COLUMN(platform_id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid}),
    RUVIA_DB_COLUMN(imei, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 15}),
    RUVIA_DB_COLUMN(name, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .nullable = true, .length = 100}),
    RUVIA_DB_COLUMN(model, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 128, .defaultExpression = ruvia::FixedString{"''::character varying"}}),
    RUVIA_DB_COLUMN(software_version, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 32, .defaultExpression = ruvia::FixedString{"''::character varying"}}),
    RUVIA_DB_COLUMN(hostname, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 64, .defaultExpression = ruvia::FixedString{"''::character varying"}}),
    RUVIA_DB_COLUMN(architecture, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 32, .defaultExpression = ruvia::FixedString{"''::character varying"}}),
    RUVIA_DB_COLUMN(openwrt_release, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 64, .defaultExpression = ruvia::FixedString{"''::character varying"}}),
    RUVIA_DB_COLUMN(enrollment_status, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 20, .defaultExpression = ruvia::FixedString{"'pending'::character varying"}}),
    RUVIA_DB_COLUMN(last_seen_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .nullable = true}),
    RUVIA_DB_COLUMN(approved_by, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid, .nullable = true}),
    RUVIA_DB_COLUMN(approved_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .nullable = true}),
    RUVIA_DB_COLUMN(created_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}),
    RUVIA_DB_COLUMN(updated_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}),
    RUVIA_DB_COLUMN(network_config_version, std::int32_t,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kInteger, .defaultExpression = ruvia::FixedString{"0"}}),
    RUVIA_DB_COLUMN(status, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kJsonb, .defaultExpression = ruvia::FixedString{"'{\"log\": {\"level\": \"info\"}, \"config\": {\"state\": \"idle\", \"message\": \"\", \"activeVersion\": 0, \"desiredVersion\": 0}, \"outbox\": {\"bytes\": 0, \"records\": 0}}'::jsonb"}}),
    RUVIA_DB_COLUMN(capability, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kJsonb, .defaultExpression = ruvia::FixedString{"'{}'::jsonb"}}),
    RUVIA_DB_COLUMN(mobile, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kJsonb, .defaultExpression = ruvia::FixedString{"'{}'::jsonb"}}),
    RUVIA_DB_COLUMN(group_id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid, .nullable = true}))

RUVIA_DB_ENTITY(EdgeNodeInterfaceEntity, "edge_node_interface",
    RUVIA_DB_COLUMN(node_id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid, .primaryKey = true}),
    RUVIA_DB_COLUMN(name, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .primaryKey = true, .length = 32}),
    RUVIA_DB_COLUMN(display_name, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 64, .defaultExpression = ruvia::FixedString{"''::character varying"}}),
    RUVIA_DB_COLUMN(mac, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .nullable = true, .length = 17}),
    RUVIA_DB_COLUMN(is_up, bool,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kBoolean, .defaultExpression = ruvia::FixedString{"false"}}),
    RUVIA_DB_COLUMN(is_bridge, bool,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kBoolean, .defaultExpression = ruvia::FixedString{"false"}}),
    RUVIA_DB_COLUMN(ipv4, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .nullable = true, .length = 15}),
    RUVIA_DB_COLUMN(prefix_length, std::int32_t,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kInteger, .nullable = true}),
    RUVIA_DB_COLUMN(gateway, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .nullable = true, .length = 15}),
    RUVIA_DB_COLUMN(bridge_ports, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kJsonb, .defaultExpression = ruvia::FixedString{"'[]'::jsonb"}}),
    RUVIA_DB_COLUMN(updated_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}))

RUVIA_DB_ENTITY(EdgeNodeSerialEntity, "edge_node_serial",
    RUVIA_DB_COLUMN(node_id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid, .primaryKey = true}),
    RUVIA_DB_COLUMN(path, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .primaryKey = true, .length = 96}),
    RUVIA_DB_COLUMN(display_name, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 64, .defaultExpression = ruvia::FixedString{"''::character varying"}}),
    RUVIA_DB_COLUMN(available, bool,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kBoolean, .defaultExpression = ruvia::FixedString{"false"}}),
    RUVIA_DB_COLUMN(rs485, bool,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kBoolean, .defaultExpression = ruvia::FixedString{"false"}}),
    RUVIA_DB_COLUMN(updated_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}))

} // namespace service::link::entities
