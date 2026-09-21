#pragma once

#include "service/common/uuid.h"
#include <ruvia/web/Validation.h>
#include <ruvia/web/ModelObject.h>

namespace service::protocol {

RUVIA_MODEL(ExpressionTestInput,
    RUVIA_REQUIRED_FIELD(alias, ruvia::String, RUVIA_MIN(1, "变量名不能为空"), RUVIA_MAX(32, "变量名过长")),
    RUVIA_REQUIRED_FIELD(value, ruvia::Double));
RUVIA_MODEL(ExpressionTestUnitRule,
    RUVIA_REQUIRED_FIELD(condition, ruvia::String, RUVIA_MIN(1, "单位条件不能为空"), RUVIA_MAX(512, "单位条件过长")),
    RUVIA_REQUIRED_FIELD(unit, ruvia::String, RUVIA_MAX(32, "单位过长")));
RUVIA_MODEL(ExpressionTestBody,
    RUVIA_REQUIRED_FIELD(expression, ruvia::String, RUVIA_MIN(1, "公式不能为空"), RUVIA_MAX(512, "公式过长")),
    RUVIA_REQUIRED_FIELD(inputs, ruvia::Array<ExpressionTestInput>, RUVIA_MAX(16, "最多 16 个变量")),
    RUVIA_REQUIRED_FIELD(unit, ruvia::String, RUVIA_MAX(32, "单位过长")),
    RUVIA_REQUIRED_FIELD(unitRules, ruvia::Array<ExpressionTestUnitRule>, RUVIA_MAX(8, "最多 8 条单位条件")));
RUVIA_MODEL(ExpressionTestResult,
    RUVIA_REQUIRED_FIELD(value, ruvia::Double),
    RUVIA_REQUIRED_FIELD(unit, ruvia::String),
    RUVIA_REQUIRED_FIELD(matchedRule, ruvia::Int64));
RUVIA_MODEL(ExpressionTestResponse,
    RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::String),
    RUVIA_REQUIRED_FIELD(data, ExpressionTestResult));

RUVIA_MODEL(Sl651Element,
    RUVIA_OPTIONAL_FIELD(positionMode, ruvia::String),
    RUVIA_OPTIONAL_FIELD(byteOffset, ruvia::JsonValue),
    RUVIA_OPTIONAL_FIELD(length, ruvia::JsonValue),
    RUVIA_OPTIONAL_FIELD(digits, ruvia::JsonValue),
    RUVIA_OPTIONAL_FIELD(encode, ruvia::String),
    RUVIA_OPTIONAL_FIELD(guideHex, ruvia::String));
RUVIA_MODEL(Sl651Func,
    RUVIA_OPTIONAL_FIELD(funcCode, ruvia::String),
    RUVIA_OPTIONAL_FIELD(dir, ruvia::String),
    RUVIA_OPTIONAL_FIELD(elements, ruvia::Array<Sl651Element>),
    RUVIA_OPTIONAL_FIELD(responseElements, ruvia::Array<Sl651Element>));
RUVIA_MODEL(ModbusPacket,
    RUVIA_OPTIONAL_FIELD(mergeGap, ruvia::JsonValue),
    RUVIA_OPTIONAL_FIELD(maxQuantity, ruvia::JsonValue));
RUVIA_MODEL(ModbusRegister,
    RUVIA_OPTIONAL_FIELD(id, ruvia::String),
    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD(registerType, ruvia::String),
    RUVIA_OPTIONAL_FIELD(dataType, ruvia::String),
    RUVIA_OPTIONAL_FIELD(address, ruvia::JsonValue),
    RUVIA_OPTIONAL_FIELD(quantity, ruvia::JsonValue),
    RUVIA_OPTIONAL_FIELD(byteOrder, ruvia::String),
    RUVIA_OPTIONAL_FIELD(scale, ruvia::JsonValue),
    RUVIA_OPTIONAL_FIELD(decimals, ruvia::JsonValue),
    RUVIA_OPTIONAL_FIELD(writable, ruvia::Bool));
RUVIA_MODEL(S7Connection,
    RUVIA_OPTIONAL_FIELD(probeMode, ruvia::String),
    RUVIA_OPTIONAL_FIELD(handshakeTimeout, ruvia::JsonValue),
    RUVIA_OPTIONAL_FIELD(directProbeTimeout, ruvia::JsonValue));
RUVIA_MODEL(S7Area,
    RUVIA_OPTIONAL_FIELD(id, ruvia::String),
    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD(area, ruvia::String),
    RUVIA_OPTIONAL_FIELD(dataType, ruvia::String),
    RUVIA_OPTIONAL_FIELD(start, ruvia::JsonValue),
    RUVIA_OPTIONAL_FIELD(size, ruvia::JsonValue),
    RUVIA_OPTIONAL_FIELD(startBit, ruvia::JsonValue),
    RUVIA_OPTIONAL_FIELD(decimals, ruvia::JsonValue),
    RUVIA_OPTIONAL_FIELD(writable, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD(dbNumber, ruvia::JsonValue));
RUVIA_MODEL(IndustrialConnection,
    RUVIA_OPTIONAL_FIELD(frame, ruvia::String),
    RUVIA_OPTIONAL_FIELD(network, ruvia::JsonValue),
    RUVIA_OPTIONAL_FIELD(station, ruvia::JsonValue),
    RUVIA_OPTIONAL_FIELD(moduleIo, ruvia::JsonValue),
    RUVIA_OPTIONAL_FIELD(multidrop, ruvia::JsonValue),
    RUVIA_OPTIONAL_FIELD(monitoringTimer, ruvia::JsonValue),
    RUVIA_OPTIONAL_FIELD(destinationNetwork, ruvia::JsonValue),
    RUVIA_OPTIONAL_FIELD(sourceNetwork, ruvia::JsonValue),
    RUVIA_OPTIONAL_FIELD(destinationNode, ruvia::JsonValue),
    RUVIA_OPTIONAL_FIELD(sourceNode, ruvia::JsonValue),
    RUVIA_OPTIONAL_FIELD(destinationUnit, ruvia::JsonValue),
    RUVIA_OPTIONAL_FIELD(sourceUnit, ruvia::JsonValue),
    RUVIA_OPTIONAL_FIELD(version, ruvia::String),
    RUVIA_OPTIONAL_FIELD(wakeupBytes, ruvia::JsonValue),
    RUVIA_OPTIONAL_FIELD(writePassword, ruvia::String),
    RUVIA_OPTIONAL_FIELD(operatorCode, ruvia::String));
RUVIA_MODEL(IndustrialPoint,
    RUVIA_OPTIONAL_FIELD(id, ruvia::String),
    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD(dataType, ruvia::String),
    RUVIA_OPTIONAL_FIELD(unit, ruvia::String),
    RUVIA_OPTIONAL_FIELD(writable, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD(identifier, ruvia::String),
    RUVIA_OPTIONAL_FIELD(length, ruvia::JsonValue),
    RUVIA_OPTIONAL_FIELD(digits, ruvia::JsonValue),
    RUVIA_OPTIONAL_FIELD(area, ruvia::String),
    RUVIA_OPTIONAL_FIELD(byteOrder, ruvia::String),
    RUVIA_OPTIONAL_FIELD(scale, ruvia::JsonValue),
    RUVIA_OPTIONAL_FIELD(decimals, ruvia::JsonValue),
    RUVIA_OPTIONAL_FIELD(address, ruvia::JsonValue),
    RUVIA_OPTIONAL_FIELD(bit, ruvia::JsonValue));

// 顶层 config 保留未知键、精确数字 token 和 PATCH 合并；各协议固定嵌套结构用 Model。
RUVIA_MODEL(CreateProtocolBody,
    RUVIA_REQUIRED_FIELD(protocol, ruvia::String, RUVIA_MIN(1, "协议不能为空"), RUVIA_MAX(16, "协议过长")),
    RUVIA_REQUIRED_FIELD(name, ruvia::String, RUVIA_MIN(1, "配置名称不能为空"), RUVIA_MAX(64, "配置名称过长")),
    RUVIA_OPTIONAL_FIELD(remark, ruvia::String, RUVIA_NULLABLE, RUVIA_MAX(500, "remark 长度超出限制")),
    RUVIA_OPTIONAL_FIELD(enabled, ruvia::Bool, RUVIA_NULLABLE, RUVIA_DEFAULT(true)),
    RUVIA_OPTIONAL_FIELD(config, ruvia::JsonValue, RUVIA_NULLABLE));
RUVIA_MODEL(UpdateProtocolBody,
    RUVIA_OPTIONAL_FIELD(protocol, ruvia::String, RUVIA_NULLABLE, RUVIA_MAX(16, "协议过长")),
    RUVIA_OPTIONAL_FIELD(name, ruvia::String, RUVIA_NULLABLE, RUVIA_MAX(64, "配置名称过长")),
    RUVIA_OPTIONAL_FIELD(remark, ruvia::String, RUVIA_NULLABLE, RUVIA_MAX(500, "remark 长度超出限制")),
    RUVIA_OPTIONAL_FIELD(enabled, ruvia::Bool, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(config, ruvia::JsonValue, RUVIA_NULLABLE));

RUVIA_MODEL(ProtocolListQuery,
    RUVIA_OPTIONAL_FIELD(page, ruvia::Int64, RUVIA_DEFAULT(1), RUVIA_MIN(1, "page 必须大于 0")),
    RUVIA_OPTIONAL_FIELD(pageSize, ruvia::Int64, RUVIA_DEFAULT(10), RUVIA_MIN(1, "pageSize 必须在 1 - 1000 之间"), RUVIA_MAX(1000, "pageSize 必须在 1 - 1000 之间")),
    RUVIA_OPTIONAL_FIELD(protocol, ruvia::String, RUVIA_ONE_OF("协议无效", "SL651", "Modbus", "S7", "MC", "FINS", "DLT645")));
RUVIA_MODEL(ProtocolIdParams,
    RUVIA_REQUIRED_FIELD(id, ruvia::String, RUVIA_CUSTOM("id 必须是 UUID", service::common::isUuidField)));

} // namespace service::protocol
