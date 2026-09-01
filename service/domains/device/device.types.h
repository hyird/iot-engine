#pragma once

#include <cctype>
#include <string_view>

#include <ruvia/web/Model.h>

namespace service::device {

// 设备时区格式校验：[+-]HH:MM，范围 -14:00 .. +14:00（空视为合法，可选字段）。
inline bool isDeviceTimezone(const ruvia::String& value) {
    const std::string_view v = value.view();
    if (v.empty())
        return true;
    if (v.size() != 6 || (v[0] != '+' && v[0] != '-') || v[3] != ':')
        return false;
    for (const std::size_t i : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{5}})
        if (!std::isdigit(static_cast<unsigned char>(v[i])))
            return false;
    const int hh = (v[1] - '0') * 10 + (v[2] - '0');
    const int mm = (v[4] - '0') * 10 + (v[5] - '0');
    if (hh == 14)
        return mm == 0;
    return hh <= 13 && mm <= 59;
}

RUVIA_REQUEST_MODEL(DeviceIdParams,
    RUVIA_OPTIONAL_FIELD(id, ruvia::String));

// ===== 设备分享 =====

RUVIA_REQUEST_MODEL(DeviceShareBodyItem,
    RUVIA_OPTIONAL_FIELD_NAME("subject_type", subjectType, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("subject_id", subjectId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("access_level", accessLevel, ruvia::String));

RUVIA_REQUEST_MODEL(ReplaceDeviceSharesBody,
    RUVIA_OPTIONAL_FIELD(shares, ruvia::Array<DeviceShareBodyItem>));

RUVIA_RESPONSE_MODEL(DeviceShareItemDto,
    RUVIA_OPTIONAL_FIELD(id, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("subject_type", subjectType, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("subject_id", subjectId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("subject_name", subjectName, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("access_level", accessLevel, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("source_type", sourceType, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("source_group_id", sourceGroupId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("source_group_name", sourceGroupName, ruvia::String),
    RUVIA_OPTIONAL_FIELD(inherited, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("created_at", createdAt, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("updated_at", updatedAt, ruvia::String));

RUVIA_RESPONSE_MODEL(DeviceShareTargetDto,
    RUVIA_OPTIONAL_FIELD_NAME("subject_type", subjectType, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("subject_id", subjectId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("subject_name", subjectName, ruvia::String));

RUVIA_RESPONSE_MODEL(DeviceSharesResponse,
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(message, ruvia::String),
    RUVIA_OPTIONAL_FIELD(data, ruvia::BoxedArray<DeviceShareItemDto>));

RUVIA_RESPONSE_MODEL(DeviceShareTargetsResponse,
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(message, ruvia::String),
    RUVIA_OPTIONAL_FIELD(data, ruvia::BoxedArray<DeviceShareTargetDto>));

// ===== 设备写侧 Body =====

RUVIA_REQUEST_MODEL(DevicePacketBody,
    RUVIA_OPTIONAL_FIELD(mode, ruvia::String),
    RUVIA_OPTIONAL_FIELD(content, ruvia::String));

RUVIA_REQUEST_MODEL(SaveDeviceBody,
    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("device_code", deviceCode, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("link_id", linkId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("edge_node_id", edgeNodeId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("edge_transport", edgeTransport, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("edge_interface", edgeInterface, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("edge_mode", edgeMode, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("edge_ip", edgeIp, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("edge_port", edgePort, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("serial_baud_rate", serialBaudRate, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("serial_data_bits", serialDataBits, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("serial_stop_bits", serialStopBits, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("serial_parity", serialParity, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("serial_rs485", serialRs485, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("target_id", targetId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("protocol_config_id", protocolConfigId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("group_id", groupId, ruvia::String),
    RUVIA_OPTIONAL_FIELD(status, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("online_timeout", onlineTimeout, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("remote_control", remoteControl, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("modbus_mode", modbusMode, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("slave_id", slaveId, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(timezone, ruvia::String),
    RUVIA_OPTIONAL_FIELD(heartbeat, DevicePacketBody),
    RUVIA_OPTIONAL_FIELD(registration, DevicePacketBody),
    RUVIA_OPTIONAL_FIELD(remark, ruvia::String));

// ===== 设备读侧 DTO =====

// 心跳包 / 注册包（透传 DB jsonb 的 mode + 可选 content）
RUVIA_RESPONSE_MODEL(DevicePacketDto,
    RUVIA_OPTIONAL_FIELD(mode, ruvia::String),
    RUVIA_OPTIONAL_FIELD(content, ruvia::String));

RUVIA_REQUEST_MODEL(DeviceCommandElementBody,
    RUVIA_OPTIONAL_FIELD_NAME("elementId", elementId, ruvia::String),
    RUVIA_OPTIONAL_FIELD(value, ruvia::String));

RUVIA_REQUEST_MODEL(DeviceCommandBody,
    RUVIA_OPTIONAL_FIELD_NAME("deviceId", deviceId, ruvia::String),
    RUVIA_OPTIONAL_FIELD(elements, ruvia::Array<DeviceCommandElementBody>));

RUVIA_REQUEST_MODEL(DeviceCommandWaitBody,
    RUVIA_OPTIONAL_FIELD_NAME("command_ids", commandIds, ruvia::Array<ruvia::String>),
    RUVIA_OPTIONAL_FIELD_NAME("timeout_ms", timeoutMs, ruvia::Int64));

RUVIA_RESPONSE_MODEL(DeviceCommandCreateDto,
    RUVIA_OPTIONAL_FIELD_NAME("command_ids", commandIds, ruvia::BoxedArray<ruvia::String>),
    RUVIA_OPTIONAL_FIELD(status, ruvia::String));

RUVIA_RESPONSE_MODEL(DeviceCommandStatusDto,
    RUVIA_OPTIONAL_FIELD_NAME("command_id", commandId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("device_id", deviceId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("device_code", deviceCode, ruvia::String),
    RUVIA_OPTIONAL_FIELD(protocol, ruvia::String),
    RUVIA_OPTIONAL_FIELD(status, ruvia::String),
    RUVIA_OPTIONAL_FIELD(reason, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("created_at_ms", createdAtMs, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("completed_at_ms", completedAtMs, ruvia::Int64));

RUVIA_RESPONSE_MODEL(DeviceCommandWaitDto,
    RUVIA_OPTIONAL_FIELD(complete, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD(statuses, ruvia::BoxedArray<DeviceCommandStatusDto>));

RUVIA_RESPONSE_MODEL(DeviceElementDto,
    RUVIA_OPTIONAL_FIELD(id, ruvia::String),
    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD(value, ruvia::String),
    RUVIA_OPTIONAL_FIELD(unit, ruvia::String),
    RUVIA_OPTIONAL_FIELD(scale, ruvia::Double),
    RUVIA_OPTIONAL_FIELD(decimals, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(group, ruvia::String),
    RUVIA_OPTIONAL_FIELD(encode, ruvia::String));

RUVIA_RESPONSE_MODEL(DeviceCommandOptionDto,
    RUVIA_OPTIONAL_FIELD(label, ruvia::String),
    RUVIA_OPTIONAL_FIELD(value, ruvia::String));

RUVIA_RESPONSE_MODEL(DeviceCommandOperationElementDto,
    RUVIA_OPTIONAL_FIELD_NAME("elementId", elementId, ruvia::String),
    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD(value, ruvia::String),
    RUVIA_OPTIONAL_FIELD(unit, ruvia::String),
    RUVIA_OPTIONAL_FIELD(options, ruvia::BoxedArray<DeviceCommandOptionDto>),
    RUVIA_OPTIONAL_FIELD_NAME("registerType", registerType, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("dataType", dataType, ruvia::String),
    RUVIA_OPTIONAL_FIELD(size, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(encode, ruvia::String),
    RUVIA_OPTIONAL_FIELD(length, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(digits, ruvia::Int64));

RUVIA_RESPONSE_MODEL(DeviceCommandOperationDto,
    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD(elements, ruvia::BoxedArray<DeviceCommandOperationElementDto>));

RUVIA_RESPONSE_MODEL(EdgeStatusDto,
    RUVIA_OPTIONAL_FIELD(state, ruvia::String),
    RUVIA_OPTIONAL_FIELD(reason, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("clientCount", clientCount, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("lastActivityAt", lastActivityAt, ruvia::String));

RUVIA_RESPONSE_MODEL(DeviceItemDto,
    RUVIA_OPTIONAL_FIELD(id, ruvia::String),
    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("device_code", deviceCode, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("link_id", linkId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("edge_node_id", edgeNodeId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("edge_node_name", edgeNodeName, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("edge_node_imei", edgeNodeImei, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("edge_transport", edgeTransport, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("edge_interface", edgeInterface, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("edge_mode", edgeMode, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("edge_ip", edgeIp, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("edge_port", edgePort, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("serial_baud_rate", serialBaudRate, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("serial_data_bits", serialDataBits, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("serial_stop_bits", serialStopBits, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("serial_parity", serialParity, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("serial_rs485", serialRs485, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("target_id", targetId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("protocol_config_id", protocolConfigId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("group_id", groupId, ruvia::String),
    RUVIA_OPTIONAL_FIELD(status, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("online_timeout", onlineTimeout, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("remote_control", remoteControl, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("modbus_mode", modbusMode, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("slave_id", slaveId, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(timezone, ruvia::String),
    RUVIA_OPTIONAL_FIELD(heartbeat, DevicePacketDto),
    RUVIA_OPTIONAL_FIELD(registration, DevicePacketDto),
    RUVIA_OPTIONAL_FIELD(remark, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("created_by", createdBy, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("created_at", createdAt, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("updated_at", updatedAt, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("link_name", linkName, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("link_mode", linkMode, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("link_protocol", linkProtocol, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("protocol_name", protocolName, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("protocol_type", protocolType, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("read_interval", readInterval, ruvia::Double),
    RUVIA_OPTIONAL_FIELD_NAME("storage_interval", storageInterval, ruvia::Double),
    RUVIA_OPTIONAL_FIELD_NAME("element_count", elementCount, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(connected, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("connectionState", connectionState, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("edgeStatus", edgeStatus, EdgeStatusDto),
    RUVIA_OPTIONAL_FIELD_NAME("reportTime", reportTime, ruvia::String),
    RUVIA_OPTIONAL_FIELD(elements, ruvia::BoxedArray<DeviceElementDto>),
    RUVIA_OPTIONAL_FIELD_NAME("commandOperations", commandOperations, ruvia::BoxedArray<DeviceCommandOperationDto>),
    RUVIA_OPTIONAL_FIELD_NAME("can_edit", canEdit, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("can_delete", canDelete, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("can_share", canShare, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("can_command", canCommand, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("access_level", accessLevel, ruvia::String));

RUVIA_RESPONSE_MODEL(DeviceOptionDto,
    RUVIA_OPTIONAL_FIELD(id, ruvia::String),
    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("device_code", deviceCode, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("can_edit", canEdit, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("can_delete", canDelete, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("can_share", canShare, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("can_command", canCommand, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("access_level", accessLevel, ruvia::String));

RUVIA_RESPONSE_MODEL(DeviceRealtimeDto,
    RUVIA_OPTIONAL_FIELD(id, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("device_code", deviceCode, ruvia::String),
    RUVIA_OPTIONAL_FIELD(connected, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("connectionState", connectionState, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("reportTime", reportTime, ruvia::String),
    RUVIA_OPTIONAL_FIELD(elements, ruvia::BoxedArray<DeviceElementDto>),
    RUVIA_OPTIONAL_FIELD_NAME("edge_node_id", edgeNodeId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("edge_transport", edgeTransport, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("edgeStatus", edgeStatus, EdgeStatusDto),
    RUVIA_OPTIONAL_FIELD_NAME("can_edit", canEdit, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("can_delete", canDelete, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("can_share", canShare, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("can_command", canCommand, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("access_level", accessLevel, ruvia::String));

RUVIA_RESPONSE_MODEL(DevicePageDataDto,
    RUVIA_OPTIONAL_FIELD(list, ruvia::BoxedArray<DeviceItemDto>),
    RUVIA_OPTIONAL_FIELD(total, ruvia::Int64));
RUVIA_RESPONSE_MODEL(DeviceRealtimePageDto,
    RUVIA_OPTIONAL_FIELD(list, ruvia::BoxedArray<DeviceRealtimeDto>),
    RUVIA_OPTIONAL_FIELD(total, ruvia::Int64));

RUVIA_RESPONSE_MODEL(DevicePageResponse,
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(message, ruvia::String),
    RUVIA_OPTIONAL_FIELD(data, DevicePageDataDto));
RUVIA_RESPONSE_MODEL(DeviceDetailResponse,
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(message, ruvia::String),
    RUVIA_OPTIONAL_FIELD(data, DeviceItemDto));
RUVIA_RESPONSE_MODEL(DeviceCommandCreateResponse,
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(message, ruvia::String),
    RUVIA_OPTIONAL_FIELD(data, DeviceCommandCreateDto));
RUVIA_RESPONSE_MODEL(DeviceCommandStatusResponse,
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(message, ruvia::String),
    RUVIA_OPTIONAL_FIELD(data, DeviceCommandStatusDto));
RUVIA_RESPONSE_MODEL(DeviceCommandWaitResponse,
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(message, ruvia::String),
    RUVIA_OPTIONAL_FIELD(data, DeviceCommandWaitDto));
RUVIA_RESPONSE_MODEL(DeviceOptionsResponse,
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(message, ruvia::String),
    RUVIA_OPTIONAL_FIELD(data, ruvia::BoxedArray<DeviceOptionDto>));
RUVIA_RESPONSE_MODEL(DeviceRealtimeResponse,
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(message, ruvia::String),
    RUVIA_OPTIONAL_FIELD(data, DeviceRealtimePageDto));

// ===== 设备分组（合并入 device 模块，子资源）=====

RUVIA_REQUEST_MODEL(SaveDeviceGroupBody,
    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("parent_id", parentId, ruvia::String),
    RUVIA_OPTIONAL_FIELD(status, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("sort_order", sortOrder, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(remark, ruvia::String));

RUVIA_RESPONSE_MODEL(DeviceGroupItemDto,
    RUVIA_OPTIONAL_FIELD(id, ruvia::String),
    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("parent_id", parentId, ruvia::String),
    RUVIA_OPTIONAL_FIELD(status, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("sort_order", sortOrder, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(remark, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("deviceCount", deviceCount, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("created_at", createdAt, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("updated_at", updatedAt, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("can_share", canShare, ruvia::Bool));

RUVIA_RESPONSE_MODEL(DeviceGroupListResponse,
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(message, ruvia::String),
    RUVIA_OPTIONAL_FIELD(data, ruvia::BoxedArray<DeviceGroupItemDto>));
RUVIA_RESPONSE_MODEL(DeviceGroupDetailResponse,
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(message, ruvia::String),
    RUVIA_OPTIONAL_FIELD(data, DeviceGroupItemDto));

} // namespace service::device
