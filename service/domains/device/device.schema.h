#pragma once

#include <ruvia/web/Controller.h>

#include "service/domains/device/device.types.h"
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
        RUVIA_RULE_NAME("link_id", linkId,
                        RUVIA_CUSTOM("链路必须是 UUID", service::common::isOptionalUuidField)),
        RUVIA_RULE_NAME("edge_node_id", edgeNodeId,
                        RUVIA_CUSTOM("边缘节点必须是 UUID",
                                     service::common::isOptionalUuidField)),
        RUVIA_RULE_NAME("edge_transport", edgeTransport,
                        RUVIA_ONE_OF("边缘传输方式无效", "serial", "tcp")),
        RUVIA_RULE_NAME("edge_interface", edgeInterface,
                        RUVIA_MAX(96, "边缘接口长度不能超过 96")),
        RUVIA_RULE_NAME("edge_mode", edgeMode,
                        RUVIA_ONE_OF("边缘连接模式无效", "TCP Client", "TCP Server")),
        RUVIA_RULE_NAME("edge_ip", edgeIp, RUVIA_MAX(64, "边缘目标地址长度不能超过 64")),
        RUVIA_RULE_NAME("edge_port", edgePort, RUVIA_MIN(1, "边缘端口必须在 1 - 65535"),
                        RUVIA_MAX(65535, "边缘端口必须在 1 - 65535")),
        RUVIA_RULE_NAME("serial_baud_rate", serialBaudRate,
                        RUVIA_MIN(300, "串口波特率必须在 300 - 4000000"),
                        RUVIA_MAX(4000000, "串口波特率必须在 300 - 4000000")),
        RUVIA_RULE_NAME("serial_data_bits", serialDataBits,
                        RUVIA_MIN(5, "串口数据位必须在 5 - 8"),
                        RUVIA_MAX(8, "串口数据位必须在 5 - 8")),
        RUVIA_RULE_NAME("serial_stop_bits", serialStopBits,
                        RUVIA_MIN(1, "串口停止位必须为 1 或 2"),
                        RUVIA_MAX(2, "串口停止位必须为 1 或 2")),
        RUVIA_RULE_NAME("serial_parity", serialParity,
                        RUVIA_ONE_OF("串口校验位无效", "none", "even", "odd")),
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
                        RUVIA_CUSTOM("链路必须是 UUID",
                                     service::common::isOptionalUuidField)),
        RUVIA_RULE_NAME("edge_node_id", edgeNodeId,
                        RUVIA_CUSTOM("边缘节点必须是 UUID",
                                     service::common::isOptionalUuidField)),
        RUVIA_RULE_NAME("edge_transport", edgeTransport,
                        RUVIA_ONE_OF("边缘传输方式无效", "serial", "tcp")),
        RUVIA_RULE_NAME("edge_interface", edgeInterface,
                        RUVIA_MAX(96, "边缘接口长度不能超过 96")),
        RUVIA_RULE_NAME("edge_mode", edgeMode,
                        RUVIA_ONE_OF("边缘连接模式无效", "TCP Client", "TCP Server")),
        RUVIA_RULE_NAME("edge_ip", edgeIp, RUVIA_MAX(64, "边缘目标地址长度不能超过 64")),
        RUVIA_RULE_NAME("edge_port", edgePort, RUVIA_MIN(1, "边缘端口必须在 1 - 65535"),
                        RUVIA_MAX(65535, "边缘端口必须在 1 - 65535")),
        RUVIA_RULE_NAME("serial_baud_rate", serialBaudRate,
                        RUVIA_MIN(300, "串口波特率必须在 300 - 4000000"),
                        RUVIA_MAX(4000000, "串口波特率必须在 300 - 4000000")),
        RUVIA_RULE_NAME("serial_data_bits", serialDataBits,
                        RUVIA_MIN(5, "串口数据位必须在 5 - 8"),
                        RUVIA_MAX(8, "串口数据位必须在 5 - 8")),
        RUVIA_RULE_NAME("serial_stop_bits", serialStopBits,
                        RUVIA_MIN(1, "串口停止位必须为 1 或 2"),
                        RUVIA_MAX(2, "串口停止位必须为 1 或 2")),
        RUVIA_RULE_NAME("serial_parity", serialParity,
                        RUVIA_ONE_OF("串口校验位无效", "none", "even", "odd")),
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
