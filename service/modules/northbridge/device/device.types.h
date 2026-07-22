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

struct DeviceIdParams final {
    RUVIA_OPTIONAL_FIELD(id, ruvia::String);
    RUVIA_MODEL(DeviceIdParams, id);
};

// ===== 设备分享 =====

struct DeviceShareBodyItem final {
    RUVIA_OPTIONAL_FIELD_NAME("subject_type", subjectType, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("subject_id", subjectId, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("access_level", accessLevel, ruvia::String);
    RUVIA_MODEL(DeviceShareBodyItem, subjectType, subjectId, accessLevel);
};

struct ReplaceDeviceSharesBody final {
    RUVIA_OPTIONAL_FIELD(shares, ruvia::Array<DeviceShareBodyItem>);
    RUVIA_MODEL(ReplaceDeviceSharesBody, shares);
};

struct DeviceShareItemDto final {
    RUVIA_OPTIONAL_FIELD(id, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("subject_type", subjectType, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("subject_id", subjectId, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("subject_name", subjectName, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("access_level", accessLevel, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("source_type", sourceType, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("source_group_id", sourceGroupId, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("source_group_name", sourceGroupName, ruvia::String);
    RUVIA_OPTIONAL_FIELD(inherited, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("created_at", createdAt, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("updated_at", updatedAt, ruvia::String);
    RUVIA_MODEL(DeviceShareItemDto, id, subjectType, subjectId, subjectName, accessLevel, sourceType,
                sourceGroupId, sourceGroupName, inherited, createdAt, updatedAt);
};

struct DeviceShareTargetDto final {
    RUVIA_OPTIONAL_FIELD_NAME("subject_type", subjectType, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("subject_id", subjectId, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("subject_name", subjectName, ruvia::String);
    RUVIA_MODEL(DeviceShareTargetDto, subjectType, subjectId, subjectName);
};

struct DeviceSharesResponse final {
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(message, ruvia::String);
    RUVIA_OPTIONAL_FIELD(data, ruvia::List<DeviceShareItemDto>);
    RUVIA_MODEL(DeviceSharesResponse, code, message, data);
};

struct DeviceShareTargetsResponse final {
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(message, ruvia::String);
    RUVIA_OPTIONAL_FIELD(data, ruvia::List<DeviceShareTargetDto>);
    RUVIA_MODEL(DeviceShareTargetsResponse, code, message, data);
};

// ===== 设备写侧 Body =====

struct DevicePacketBody final {
    RUVIA_OPTIONAL_FIELD(mode, ruvia::String);
    RUVIA_OPTIONAL_FIELD(content, ruvia::String);
    RUVIA_MODEL(DevicePacketBody, mode, content);
};

struct SaveDeviceBody final {
    RUVIA_OPTIONAL_FIELD(name, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("device_code", deviceCode, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("link_id", linkId, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("target_id", targetId, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("protocol_config_id", protocolConfigId, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("group_id", groupId, ruvia::String);
    RUVIA_OPTIONAL_FIELD(status, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("online_timeout", onlineTimeout, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD_NAME("remote_control", remoteControl, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("modbus_mode", modbusMode, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("slave_id", slaveId, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(timezone, ruvia::String);
    RUVIA_OPTIONAL_FIELD(heartbeat, DevicePacketBody);
    RUVIA_OPTIONAL_FIELD(registration, DevicePacketBody);
    RUVIA_OPTIONAL_FIELD(remark, ruvia::String);
    RUVIA_MODEL(SaveDeviceBody, name, deviceCode, linkId, targetId, protocolConfigId, groupId,
                status, onlineTimeout, remoteControl, modbusMode, slaveId, timezone, heartbeat,
                registration, remark);
};

// ===== 设备读侧 DTO =====

// 心跳包 / 注册包（透传 DB jsonb 的 mode + 可选 content）
struct DevicePacketDto final {
    RUVIA_OPTIONAL_FIELD(mode, ruvia::String);
    RUVIA_OPTIONAL_FIELD(content, ruvia::String);
    RUVIA_MODEL(DevicePacketDto, mode, content);
};

struct DeviceElementDto final {
    RUVIA_OPTIONAL_FIELD(id, ruvia::String);
    RUVIA_OPTIONAL_FIELD(name, ruvia::String);
    RUVIA_OPTIONAL_FIELD(value, ruvia::String);
    RUVIA_OPTIONAL_FIELD(unit, ruvia::String);
    RUVIA_OPTIONAL_FIELD(scale, ruvia::Double);
    RUVIA_OPTIONAL_FIELD(decimals, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(group, ruvia::String);
    RUVIA_OPTIONAL_FIELD(encode, ruvia::String);
    RUVIA_MODEL(DeviceElementDto, id, name, value, unit, scale, decimals, group, encode);
};

struct DeviceItemDto final {
    RUVIA_OPTIONAL_FIELD(id, ruvia::String);
    RUVIA_OPTIONAL_FIELD(name, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("device_code", deviceCode, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("link_id", linkId, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("target_id", targetId, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("protocol_config_id", protocolConfigId, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("group_id", groupId, ruvia::String);
    RUVIA_OPTIONAL_FIELD(status, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("online_timeout", onlineTimeout, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD_NAME("remote_control", remoteControl, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("modbus_mode", modbusMode, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("slave_id", slaveId, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(timezone, ruvia::String);
    RUVIA_OPTIONAL_FIELD(heartbeat, DevicePacketDto);
    RUVIA_OPTIONAL_FIELD(registration, DevicePacketDto);
    RUVIA_OPTIONAL_FIELD(remark, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("created_by", createdBy, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("created_at", createdAt, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("updated_at", updatedAt, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("link_name", linkName, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("link_mode", linkMode, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("link_protocol", linkProtocol, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("protocol_name", protocolName, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("protocol_type", protocolType, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("read_interval", readInterval, ruvia::Double);
    RUVIA_OPTIONAL_FIELD_NAME("storage_interval", storageInterval, ruvia::Double);
    RUVIA_OPTIONAL_FIELD_NAME("element_count", elementCount, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(connected, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("connectionState", connectionState, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("reportTime", reportTime, ruvia::String);
    RUVIA_OPTIONAL_FIELD(elements, ruvia::List<DeviceElementDto>);
    RUVIA_OPTIONAL_FIELD_NAME("can_edit", canEdit, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("can_delete", canDelete, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("can_share", canShare, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("can_command", canCommand, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("access_level", accessLevel, ruvia::String);
    RUVIA_MODEL(DeviceItemDto, id, name, deviceCode, linkId, targetId, protocolConfigId, groupId,
                status, onlineTimeout, remoteControl, modbusMode, slaveId, timezone, heartbeat,
                registration, remark, createdBy, createdAt, updatedAt, linkName, linkMode,
                linkProtocol, protocolName, protocolType, readInterval, storageInterval,
                elementCount, connected, connectionState, reportTime, elements, canEdit,
                canDelete, canShare, canCommand, accessLevel);
};

struct DeviceOptionDto final {
    RUVIA_OPTIONAL_FIELD(id, ruvia::String);
    RUVIA_OPTIONAL_FIELD(name, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("device_code", deviceCode, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("can_edit", canEdit, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("can_delete", canDelete, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("can_share", canShare, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("can_command", canCommand, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("access_level", accessLevel, ruvia::String);
    RUVIA_MODEL(DeviceOptionDto, id, name, deviceCode, canEdit, canDelete, canShare, canCommand, accessLevel);
};

struct DeviceRealtimeDto final {
    RUVIA_OPTIONAL_FIELD(id, ruvia::String);
    RUVIA_OPTIONAL_FIELD(connected, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("connectionState", connectionState, ruvia::String);
    RUVIA_OPTIONAL_FIELD(elements, ruvia::List<ruvia::String>);
    RUVIA_OPTIONAL_FIELD_NAME("can_edit", canEdit, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("can_delete", canDelete, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("can_share", canShare, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("can_command", canCommand, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD_NAME("access_level", accessLevel, ruvia::String);
    RUVIA_MODEL(DeviceRealtimeDto, id, connected, connectionState, elements, canEdit, canDelete,
                canShare, canCommand, accessLevel);
};

struct DevicePageDataDto final {
    RUVIA_OPTIONAL_FIELD(list, ruvia::List<DeviceItemDto>);
    RUVIA_OPTIONAL_FIELD(total, ruvia::Int64);
    RUVIA_MODEL(DevicePageDataDto, list, total);
};
struct DeviceRealtimePageDto final {
    RUVIA_OPTIONAL_FIELD(list, ruvia::List<DeviceRealtimeDto>);
    RUVIA_OPTIONAL_FIELD(total, ruvia::Int64);
    RUVIA_MODEL(DeviceRealtimePageDto, list, total);
};

struct DevicePageResponse final {
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(message, ruvia::String);
    RUVIA_OPTIONAL_FIELD(data, DevicePageDataDto);
    RUVIA_MODEL(DevicePageResponse, code, message, data);
};
struct DeviceDetailResponse final {
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(message, ruvia::String);
    RUVIA_OPTIONAL_FIELD(data, DeviceItemDto);
    RUVIA_MODEL(DeviceDetailResponse, code, message, data);
};
struct DeviceOptionsResponse final {
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(message, ruvia::String);
    RUVIA_OPTIONAL_FIELD(data, ruvia::List<DeviceOptionDto>);
    RUVIA_MODEL(DeviceOptionsResponse, code, message, data);
};
struct DeviceRealtimeResponse final {
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(message, ruvia::String);
    RUVIA_OPTIONAL_FIELD(data, DeviceRealtimePageDto);
    RUVIA_MODEL(DeviceRealtimeResponse, code, message, data);
};

// ===== 设备分组（合并入 device 模块，子资源）=====

struct SaveDeviceGroupBody final {
    RUVIA_OPTIONAL_FIELD(name, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("parent_id", parentId, ruvia::String);
    RUVIA_OPTIONAL_FIELD(status, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("sort_order", sortOrder, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(remark, ruvia::String);
    RUVIA_MODEL(SaveDeviceGroupBody, name, parentId, status, sortOrder, remark);
};

struct DeviceGroupItemDto final {
    RUVIA_OPTIONAL_FIELD(id, ruvia::String);
    RUVIA_OPTIONAL_FIELD(name, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("parent_id", parentId, ruvia::String);
    RUVIA_OPTIONAL_FIELD(status, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("sort_order", sortOrder, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(remark, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("deviceCount", deviceCount, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD_NAME("created_at", createdAt, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("updated_at", updatedAt, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("can_share", canShare, ruvia::Bool);
    RUVIA_MODEL(DeviceGroupItemDto, id, name, parentId, status, sortOrder, remark, deviceCount,
                createdAt, updatedAt, canShare);
};

struct DeviceGroupListResponse final {
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(message, ruvia::String);
    RUVIA_OPTIONAL_FIELD(data, ruvia::List<DeviceGroupItemDto>);
    RUVIA_MODEL(DeviceGroupListResponse, code, message, data);
};
struct DeviceGroupDetailResponse final {
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(message, ruvia::String);
    RUVIA_OPTIONAL_FIELD(data, DeviceGroupItemDto);
    RUVIA_MODEL(DeviceGroupDetailResponse, code, message, data);
};

} // namespace service::device
