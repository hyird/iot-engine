#pragma once

#include <ruvia/web/Controller.h>

#include "service/modules/northbridge/device/device.types.h"
#include "service/common/id.validation.h"

namespace service::device {

class DeviceIdParamsValidator final : public ruvia::Middleware<DeviceIdParamsValidator> {
  public:
    RUVIA_VALIDATE_PARAM(DeviceIdParams,
                         RUVIA_RULE(id, RUVIA_REQUIRED("id 不能为空"),
                                    RUVIA_CUSTOM("id 必须是 UUID", service::common::isUuidField)))
};

class ReplaceDeviceSharesValidator final : public ruvia::Middleware<ReplaceDeviceSharesValidator> {
  public:
    RUVIA_VALIDATE_JSON(ReplaceDeviceSharesBody,
                        RUVIA_RULE(shares, RUVIA_REQUIRED("分享列表不能为空"),
                                   RUVIA_MAX(500, "单次最多设置 500 个分享对象")))
};

class DeviceCommandValidator final : public ruvia::Middleware<DeviceCommandValidator> {
  public:
    RUVIA_VALIDATE_JSON(DeviceCommandBody, RUVIA_RULE(elements, RUVIA_REQUIRED("下发要素不能为空"),
                                                      RUVIA_MIN(1, "请至少选择一个下发要素"),
                                                      RUVIA_MAX(256, "单次最多下发 256 个要素")))
};

// ===== 设备（写侧扁平校验；跨字段/协议相关校验在 service）=====

class CreateDeviceValidator final : public ruvia::Middleware<CreateDeviceValidator> {
  public:
    RUVIA_VALIDATE_JSON(
        SaveDeviceBody,
        RUVIA_RULE(name, RUVIA_REQUIRED("设备名称不能为空"),
                   RUVIA_MIN(1, "设备名称长度必须在 1 - 100 之间"),
                   RUVIA_MAX(100, "设备名称长度必须在 1 - 100 之间")),
        RUVIA_RULE_NAME("device_code", deviceCode,
                        RUVIA_REQUIRED("设备编码长度必须在 1 - 100 之间"),
                        RUVIA_MIN(1, "设备编码长度必须在 1 - 100 之间"),
                        RUVIA_MAX(100, "设备编码长度必须在 1 - 100 之间")),
        RUVIA_RULE_NAME("link_id", linkId, RUVIA_REQUIRED("链路和设备类型不能为空"),
                        RUVIA_CUSTOM("链路和设备类型必须是 UUID", service::common::isUuidField)),
        RUVIA_RULE_NAME("protocol_config_id", protocolConfigId,
                        RUVIA_REQUIRED("链路和设备类型不能为空"),
                        RUVIA_CUSTOM("链路和设备类型必须是 UUID", service::common::isUuidField)),
        RUVIA_RULE_NAME("group_id", groupId,
                        RUVIA_CUSTOM("设备分组必须是 UUID", service::common::isOptionalUuidField)),
        RUVIA_RULE(status, RUVIA_ONE_OF("设备参数无效", "enabled", "disabled")),
        RUVIA_RULE_NAME("online_timeout", onlineTimeout, RUVIA_MIN(1, "设备参数无效"),
                        RUVIA_MAX(86400, "设备参数无效")),
        RUVIA_RULE_NAME("modbus_mode", modbusMode, RUVIA_ONE_OF("设备参数无效", "TCP", "RTU")),
        RUVIA_RULE_NAME("slave_id", slaveId, RUVIA_MIN(1, "设备参数无效"),
                        RUVIA_MAX(247, "设备参数无效")),
        RUVIA_RULE(timezone, RUVIA_CUSTOM("设备参数无效", isDeviceTimezone)))
};

class UpdateDeviceValidator final : public ruvia::Middleware<UpdateDeviceValidator> {
  public:
    RUVIA_VALIDATE_JSON(
        SaveDeviceBody,
        RUVIA_RULE(name, RUVIA_MIN(1, "设备名称长度必须在 1 - 100 之间"),
                   RUVIA_MAX(100, "设备名称长度必须在 1 - 100 之间")),
        RUVIA_RULE_NAME("device_code", deviceCode, RUVIA_MIN(1, "设备编码长度必须在 1 - 100 之间"),
                        RUVIA_MAX(100, "设备编码长度必须在 1 - 100 之间")),
        RUVIA_RULE_NAME("link_id", linkId,
                        RUVIA_CUSTOM("链路和设备类型必须是 UUID",
                                     service::common::isOptionalUuidField)),
        RUVIA_RULE_NAME("protocol_config_id", protocolConfigId,
                        RUVIA_CUSTOM("链路和设备类型必须是 UUID",
                                     service::common::isOptionalUuidField)),
        RUVIA_RULE_NAME("group_id", groupId,
                        RUVIA_CUSTOM("设备分组必须是 UUID", service::common::isOptionalUuidField)),
        RUVIA_RULE(status, RUVIA_ONE_OF("设备参数无效", "enabled", "disabled")),
        RUVIA_RULE_NAME("online_timeout", onlineTimeout, RUVIA_MIN(1, "设备参数无效"),
                        RUVIA_MAX(86400, "设备参数无效")),
        RUVIA_RULE_NAME("modbus_mode", modbusMode, RUVIA_ONE_OF("设备参数无效", "TCP", "RTU")),
        RUVIA_RULE_NAME("slave_id", slaveId, RUVIA_MIN(1, "设备参数无效"),
                        RUVIA_MAX(247, "设备参数无效")),
        RUVIA_RULE(timezone, RUVIA_CUSTOM("设备参数无效", isDeviceTimezone)))
};

// ===== 设备分组（合并入 device 模块）=====

class CreateDeviceGroupValidator final : public ruvia::Middleware<CreateDeviceGroupValidator> {
  public:
    RUVIA_VALIDATE_JSON(SaveDeviceGroupBody,
                        RUVIA_RULE(name, RUVIA_REQUIRED("分组名称不能为空"),
                                   RUVIA_MIN(1, "分组名称长度必须在 1 - 100 之间"),
                                   RUVIA_MAX(100, "分组名称长度必须在 1 - 100 之间")),
                        RUVIA_RULE(status, RUVIA_ONE_OF("设备分组参数无效", "enabled", "disabled")),
                        RUVIA_RULE_NAME("sort_order", sortOrder, RUVIA_MIN(0, "设备分组参数无效")))
};

class UpdateDeviceGroupValidator final : public ruvia::Middleware<UpdateDeviceGroupValidator> {
  public:
    RUVIA_VALIDATE_JSON(SaveDeviceGroupBody,
                        RUVIA_RULE(name, RUVIA_MIN(1, "分组名称长度必须在 1 - 100 之间"),
                                   RUVIA_MAX(100, "分组名称长度必须在 1 - 100 之间")),
                        RUVIA_RULE(status, RUVIA_ONE_OF("设备分组参数无效", "enabled", "disabled")),
                        RUVIA_RULE_NAME("sort_order", sortOrder, RUVIA_MIN(0, "设备分组参数无效")))
};

} // namespace service::device
