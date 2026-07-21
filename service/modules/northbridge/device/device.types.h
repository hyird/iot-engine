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

RUVIA_REQUEST_MODEL(DeviceIdParams, RUVIA_FIELD(id, ruvia::String));

// ===== 设备写侧 Body =====

RUVIA_REQUEST_MODEL(DevicePacketBody, RUVIA_FIELD(mode, ruvia::String),
                    RUVIA_FIELD(content, ruvia::String));

RUVIA_REQUEST_MODEL(SaveDeviceBody, RUVIA_FIELD(name, ruvia::String),
                    RUVIA_FIELD_NAME("device_code", deviceCode, ruvia::String),
                    RUVIA_FIELD_NAME("link_id", linkId, ruvia::String),
                    RUVIA_FIELD_NAME("target_id", targetId, ruvia::String),
                    RUVIA_FIELD_NAME("protocol_config_id", protocolConfigId, ruvia::String),
                    RUVIA_FIELD_NAME("group_id", groupId, ruvia::String),
                    RUVIA_FIELD(status, ruvia::String),
                    RUVIA_FIELD_NAME("online_timeout", onlineTimeout, ruvia::Int64),
                    RUVIA_FIELD_NAME("remote_control", remoteControl, ruvia::Bool),
                    RUVIA_FIELD_NAME("modbus_mode", modbusMode, ruvia::String),
                    RUVIA_FIELD_NAME("slave_id", slaveId, ruvia::Int64),
                    RUVIA_FIELD(timezone, ruvia::String),
                    RUVIA_FIELD(heartbeat, DevicePacketBody),
                    RUVIA_FIELD(registration, DevicePacketBody),
                    RUVIA_FIELD(remark, ruvia::String));

// ===== 设备读侧 DTO =====

// 心跳包 / 注册包（透传 DB jsonb 的 mode + 可选 content）
RUVIA_RESPONSE_MODEL(DevicePacketDto, RUVIA_FIELD(mode, ruvia::String),
                     RUVIA_FIELD(content, ruvia::String));

RUVIA_RESPONSE_MODEL(
    DeviceItemDto, RUVIA_FIELD(id, ruvia::String), RUVIA_FIELD(name, ruvia::String),
    RUVIA_FIELD_NAME("device_code", deviceCode, ruvia::String),
    RUVIA_FIELD_NAME("link_id", linkId, ruvia::String),
    RUVIA_FIELD_NAME("target_id", targetId, ruvia::String),
    RUVIA_FIELD_NAME("protocol_config_id", protocolConfigId, ruvia::String),
    RUVIA_FIELD_NAME("group_id", groupId, ruvia::String), RUVIA_FIELD(status, ruvia::String),
    RUVIA_FIELD_NAME("online_timeout", onlineTimeout, ruvia::Int64),
    RUVIA_FIELD_NAME("remote_control", remoteControl, ruvia::Bool),
    RUVIA_FIELD_NAME("modbus_mode", modbusMode, ruvia::String),
    RUVIA_FIELD_NAME("slave_id", slaveId, ruvia::Int64), RUVIA_FIELD(timezone, ruvia::String),
    RUVIA_FIELD(heartbeat, DevicePacketDto), RUVIA_FIELD(registration, DevicePacketDto),
    RUVIA_FIELD(remark, ruvia::String), RUVIA_FIELD_NAME("created_by", createdBy, ruvia::String),
    RUVIA_FIELD_NAME("created_at", createdAt, ruvia::String),
    RUVIA_FIELD_NAME("updated_at", updatedAt, ruvia::String),
    RUVIA_FIELD_NAME("link_name", linkName, ruvia::String),
    RUVIA_FIELD_NAME("link_mode", linkMode, ruvia::String),
    RUVIA_FIELD_NAME("link_protocol", linkProtocol, ruvia::String),
    RUVIA_FIELD_NAME("protocol_name", protocolName, ruvia::String),
    RUVIA_FIELD_NAME("protocol_type", protocolType, ruvia::String),
    RUVIA_FIELD_NAME("read_interval", readInterval, ruvia::Double),
    RUVIA_FIELD_NAME("storage_interval", storageInterval, ruvia::Double),
    RUVIA_FIELD_NAME("element_count", elementCount, ruvia::Int64),
    RUVIA_FIELD(connected, ruvia::Bool),
    RUVIA_FIELD_NAME("connectionState", connectionState, ruvia::String),
    RUVIA_FIELD(elements, ruvia::List<ruvia::String>),
    RUVIA_FIELD_NAME("can_edit", canEdit, ruvia::Bool),
    RUVIA_FIELD_NAME("can_delete", canDelete, ruvia::Bool),
    RUVIA_FIELD_NAME("can_share", canShare, ruvia::Bool),
    RUVIA_FIELD_NAME("can_command", canCommand, ruvia::Bool),
    RUVIA_FIELD_NAME("access_level", accessLevel, ruvia::String));

RUVIA_RESPONSE_MODEL(DeviceOptionDto, RUVIA_FIELD(id, ruvia::String),
                     RUVIA_FIELD(name, ruvia::String),
                     RUVIA_FIELD_NAME("device_code", deviceCode, ruvia::String),
                     RUVIA_FIELD_NAME("can_edit", canEdit, ruvia::Bool),
                     RUVIA_FIELD_NAME("can_delete", canDelete, ruvia::Bool),
                     RUVIA_FIELD_NAME("can_share", canShare, ruvia::Bool),
                     RUVIA_FIELD_NAME("can_command", canCommand, ruvia::Bool),
                     RUVIA_FIELD_NAME("access_level", accessLevel, ruvia::String));

RUVIA_RESPONSE_MODEL(DeviceRealtimeDto, RUVIA_FIELD(id, ruvia::String),
                     RUVIA_FIELD(connected, ruvia::Bool),
                     RUVIA_FIELD_NAME("connectionState", connectionState, ruvia::String),
                     RUVIA_FIELD(elements, ruvia::List<ruvia::String>),
                     RUVIA_FIELD_NAME("can_edit", canEdit, ruvia::Bool),
                     RUVIA_FIELD_NAME("can_delete", canDelete, ruvia::Bool),
                     RUVIA_FIELD_NAME("can_share", canShare, ruvia::Bool),
                     RUVIA_FIELD_NAME("can_command", canCommand, ruvia::Bool),
                     RUVIA_FIELD_NAME("access_level", accessLevel, ruvia::String));

RUVIA_RESPONSE_MODEL(DevicePageDataDto, RUVIA_FIELD(list, ruvia::List<DeviceItemDto>),
                     RUVIA_FIELD(total, ruvia::Int64));
RUVIA_RESPONSE_MODEL(DeviceRealtimePageDto, RUVIA_FIELD(list, ruvia::List<DeviceRealtimeDto>),
                     RUVIA_FIELD(total, ruvia::Int64));

RUVIA_RESPONSE_MODEL(DevicePageResponse, RUVIA_FIELD(code, ruvia::Int64),
                     RUVIA_FIELD(message, ruvia::String), RUVIA_FIELD(data, DevicePageDataDto));
RUVIA_RESPONSE_MODEL(DeviceDetailResponse, RUVIA_FIELD(code, ruvia::Int64),
                     RUVIA_FIELD(message, ruvia::String), RUVIA_FIELD(data, DeviceItemDto));
RUVIA_RESPONSE_MODEL(DeviceOptionsResponse, RUVIA_FIELD(code, ruvia::Int64),
                     RUVIA_FIELD(message, ruvia::String),
                     RUVIA_FIELD(data, ruvia::List<DeviceOptionDto>));
RUVIA_RESPONSE_MODEL(DeviceRealtimeResponse, RUVIA_FIELD(code, ruvia::Int64),
                     RUVIA_FIELD(message, ruvia::String), RUVIA_FIELD(data, DeviceRealtimePageDto));

// ===== 设备分组（合并入 device 模块，子资源）=====

RUVIA_REQUEST_MODEL(SaveDeviceGroupBody, RUVIA_FIELD(name, ruvia::String),
                    RUVIA_FIELD_NAME("parent_id", parentId, ruvia::String),
                    RUVIA_FIELD(status, ruvia::String),
                    RUVIA_FIELD_NAME("sort_order", sortOrder, ruvia::Int64),
                    RUVIA_FIELD(remark, ruvia::String));

RUVIA_RESPONSE_MODEL(DeviceGroupItemDto, RUVIA_FIELD(id, ruvia::String),
                     RUVIA_FIELD(name, ruvia::String),
                     RUVIA_FIELD_NAME("parent_id", parentId, ruvia::String),
                     RUVIA_FIELD(status, ruvia::String),
                     RUVIA_FIELD_NAME("sort_order", sortOrder, ruvia::Int64),
                     RUVIA_FIELD(remark, ruvia::String),
                     RUVIA_FIELD_NAME("deviceCount", deviceCount, ruvia::Int64),
                     RUVIA_FIELD_NAME("created_at", createdAt, ruvia::String),
                     RUVIA_FIELD_NAME("updated_at", updatedAt, ruvia::String));

RUVIA_RESPONSE_MODEL(DeviceGroupListResponse, RUVIA_FIELD(code, ruvia::Int64),
                     RUVIA_FIELD(message, ruvia::String),
                     RUVIA_FIELD(data, ruvia::List<DeviceGroupItemDto>));
RUVIA_RESPONSE_MODEL(DeviceGroupDetailResponse, RUVIA_FIELD(code, ruvia::Int64),
                     RUVIA_FIELD(message, ruvia::String), RUVIA_FIELD(data, DeviceGroupItemDto));

} // namespace service::device
