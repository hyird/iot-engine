#pragma once

#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/modules/device/device.types.h"

namespace service::device {

class DeviceEventsValidator final : public ruvia::Middleware<DeviceEventsValidator> {
  public:
    RUVIA_VALIDATE_QUERY(DeviceEventsQuery, RUVIA_RULE_NAME("debugDeviceId", debugDeviceId, RUVIA_CUSTOM("调试设备 ID 必须是 UUID", service::common::isUuidField)), RUVIA_RULE_NAME("commandIds", commandIds, RUVIA_MIN(1, "至少指定一条指令"), RUVIA_MAX(9471, "最多指定 256 条指令")))
};

class DeviceCommandStatusesValidator final : public ruvia::Middleware<DeviceCommandStatusesValidator> {
  public:
    RUVIA_VALIDATE_QUERY(DeviceCommandStatusesQuery, RUVIA_RULE(ids, RUVIA_MIN(1, "至少指定一条指令"), RUVIA_MAX(9471, "最多指定 256 条指令")))
};

class DeviceDebugValidator final : public ruvia::Middleware<DeviceDebugValidator> {
  public:
    RUVIA_VALIDATE_JSON(DeviceDebugBody, RUVIA_RULE(enabled, RUVIA_REQUIRED("必须指定调试开关")))
};

class DeviceIdParamsValidator final : public ruvia::Middleware<DeviceIdParamsValidator> {
  public:
    RUVIA_VALIDATE_PARAM(DeviceIdParams, RUVIA_RULE(id, RUVIA_REQUIRED("id 不能为空"), RUVIA_CUSTOM("id 必须是 UUID", service::common::isUuidField)))
};

class ReplaceDeviceSharesValidator final : public ruvia::Middleware<ReplaceDeviceSharesValidator> {
  public:
    RUVIA_VALIDATE_JSON(ReplaceDeviceSharesBody, RUVIA_RULE(shares, RUVIA_REQUIRED("分享列表不能为空"), RUVIA_MAX(500, "单次最多设置 500 个分享对象")))
};

// ===== 设备（写侧扁平校验；跨字段/协议相关校验在 service）=====

class CreateDeviceValidator final : public ruvia::Middleware<CreateDeviceValidator> {
  public:
    RUVIA_VALIDATE_JSON(
        SaveDeviceBody,
        RUVIA_RULE(name, RUVIA_REQUIRED("设备名称不能为空"), RUVIA_MIN(1, "设备名称长度必须在 1 - 100 之间"), RUVIA_MAX(100, "设备名称长度必须在 1 - 100 之间")),
        RUVIA_RULE_NAME("device_code", deviceCode, RUVIA_REQUIRED("设备编码长度必须在 1 - 100 之间"), RUVIA_MIN(1, "设备编码长度必须在 1 - 100 之间"), RUVIA_MAX(100, "设备编码长度必须在 1 - 100 之间")),
        RUVIA_RULE_NAME("link_id", linkId, RUVIA_CUSTOM("链路必须是 UUID", service::common::isOptionalUuidField)),
        RUVIA_RULE_NAME("protocol_config_id", protocolConfigId, RUVIA_REQUIRED("链路和设备类型不能为空"), RUVIA_CUSTOM("链路和设备类型必须是 UUID", service::common::isUuidField)),
        RUVIA_RULE_NAME("group_id", groupId, RUVIA_CUSTOM("设备分组必须是 UUID", service::common::isOptionalUuidField)),
        RUVIA_RULE(status, RUVIA_ONE_OF("设备参数无效", "enabled", "disabled")),
        RUVIA_RULE_NAME("online_timeout", onlineTimeout, RUVIA_MIN(1, "设备参数无效"), RUVIA_MAX(86400, "设备参数无效")),
        RUVIA_RULE_NAME("modbus_mode", modbusMode, RUVIA_ONE_OF("设备参数无效", "TCP", "RTU")),
        RUVIA_RULE_NAME("slave_id", slaveId, RUVIA_MIN(1, "设备参数无效"), RUVIA_MAX(247, "设备参数无效")),
        RUVIA_RULE(timezone, RUVIA_CUSTOM("设备参数无效", isDeviceTimezone))
    )
};

class UpdateDeviceValidator final : public ruvia::Middleware<UpdateDeviceValidator> {
  public:
    RUVIA_VALIDATE_JSON(
        SaveDeviceBody,
        RUVIA_RULE(name, RUVIA_MIN(1, "设备名称长度必须在 1 - 100 之间"), RUVIA_MAX(100, "设备名称长度必须在 1 - 100 之间")),
        RUVIA_RULE_NAME("device_code", deviceCode, RUVIA_MIN(1, "设备编码长度必须在 1 - 100 之间"), RUVIA_MAX(100, "设备编码长度必须在 1 - 100 之间")),
        RUVIA_RULE_NAME("link_id", linkId, RUVIA_CUSTOM("链路必须是 UUID", service::common::isOptionalUuidField)),
        RUVIA_RULE_NAME("protocol_config_id", protocolConfigId, RUVIA_CUSTOM("链路和设备类型必须是 UUID", service::common::isOptionalUuidField)),
        RUVIA_RULE_NAME("group_id", groupId, RUVIA_CUSTOM("设备分组必须是 UUID", service::common::isOptionalUuidField)),
        RUVIA_RULE(status, RUVIA_ONE_OF("设备参数无效", "enabled", "disabled")),
        RUVIA_RULE_NAME("online_timeout", onlineTimeout, RUVIA_MIN(1, "设备参数无效"), RUVIA_MAX(86400, "设备参数无效")),
        RUVIA_RULE_NAME("modbus_mode", modbusMode, RUVIA_ONE_OF("设备参数无效", "TCP", "RTU")),
        RUVIA_RULE_NAME("slave_id", slaveId, RUVIA_MIN(1, "设备参数无效"), RUVIA_MAX(247, "设备参数无效")),
        RUVIA_RULE(timezone, RUVIA_CUSTOM("设备参数无效", isDeviceTimezone))
    )
};

// ===== 设备分组（合并入 device 模块）=====

class CreateDeviceGroupValidator final : public ruvia::Middleware<CreateDeviceGroupValidator> {
  public:
    RUVIA_VALIDATE_JSON(SaveDeviceGroupBody, RUVIA_RULE(name, RUVIA_REQUIRED("分组名称不能为空"), RUVIA_MIN(1, "分组名称长度必须在 1 - 100 之间"), RUVIA_MAX(100, "分组名称长度必须在 1 - 100 之间")), RUVIA_RULE(status, RUVIA_ONE_OF("设备分组参数无效", "enabled", "disabled")), RUVIA_RULE_NAME("sort_order", sortOrder, RUVIA_MIN(0, "设备分组参数无效")))
};

class UpdateDeviceGroupValidator final : public ruvia::Middleware<UpdateDeviceGroupValidator> {
  public:
    RUVIA_VALIDATE_JSON(SaveDeviceGroupBody, RUVIA_RULE(name, RUVIA_MIN(1, "分组名称长度必须在 1 - 100 之间"), RUVIA_MAX(100, "分组名称长度必须在 1 - 100 之间")), RUVIA_RULE(status, RUVIA_ONE_OF("设备分组参数无效", "enabled", "disabled")), RUVIA_RULE_NAME("sort_order", sortOrder, RUVIA_MIN(0, "设备分组参数无效")))
};

class DeviceHistoryValidator final : public ruvia::Middleware<DeviceHistoryValidator> {
  public:
    RUVIA_VALIDATE_QUERY(DeviceHistoryQuery, RUVIA_RULE(startTime, RUVIA_MIN(1, "startTime 不能为空")), RUVIA_RULE(endTime, RUVIA_MIN(1, "endTime 不能为空")), RUVIA_RULE(page, RUVIA_MIN(1, "page 必须大于 0")), RUVIA_RULE(pageSize, RUVIA_MIN(1, "pageSize 必须在 1 - 100 之间"), RUVIA_MAX(100, "pageSize 必须在 1 - 100 之间")))
};
} // namespace service::device
