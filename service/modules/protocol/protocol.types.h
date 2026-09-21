#pragma once

#include "service/common/uuid.h"
#include <ruvia/web/Validation.h>
#include <ruvia/web/ModelObject.h>

namespace service::protocol {

RUVIA_REQUEST_MODEL(ExpressionTestInput,
    RUVIA_REQUIRED_FIELD(alias, ruvia::String, RUVIA_MIN(1, "变量名不能为空"), RUVIA_MAX(32, "变量名过长")),
    RUVIA_REQUIRED_FIELD(value, ruvia::Double));
RUVIA_REQUEST_MODEL(ExpressionTestUnitRule,
    RUVIA_REQUIRED_FIELD(condition, ruvia::String, RUVIA_MIN(1, "单位条件不能为空"), RUVIA_MAX(512, "单位条件过长")),
    RUVIA_REQUIRED_FIELD(unit, ruvia::String, RUVIA_MAX(32, "单位过长")));
RUVIA_REQUEST_MODEL(ExpressionTestBody,
    RUVIA_REQUIRED_FIELD(expression, ruvia::String, RUVIA_MIN(1, "公式不能为空"), RUVIA_MAX(512, "公式过长")),
    RUVIA_REQUIRED_FIELD(inputs, ruvia::Array<ExpressionTestInput>, RUVIA_MAX(16, "最多 16 个变量")),
    RUVIA_REQUIRED_FIELD(unit, ruvia::String, RUVIA_MAX(32, "单位过长")),
    RUVIA_REQUIRED_FIELD(unitRules, ruvia::Array<ExpressionTestUnitRule>, RUVIA_MAX(8, "最多 8 条单位条件")));
RUVIA_RESPONSE_MODEL(ExpressionTestResult,
    RUVIA_REQUIRED_FIELD(value, ruvia::Double),
    RUVIA_REQUIRED_FIELD(unit, ruvia::String),
    RUVIA_REQUIRED_FIELD(matchedRule, ruvia::Int64));
RUVIA_RESPONSE_MODEL(ExpressionTestResponse,
    RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::String),
    RUVIA_REQUIRED_FIELD(data, ExpressionTestResult));

// config 保留各协议扩展字段和原始 JSON；跨字段配置规则由 Service 校验。
RUVIA_REQUEST_MODEL(CreateProtocolBody,
    RUVIA_REQUIRED_FIELD(protocol, ruvia::String, RUVIA_MIN(1, "协议不能为空"), RUVIA_MAX(16, "协议过长")),
    RUVIA_REQUIRED_FIELD(name, ruvia::String, RUVIA_MIN(1, "配置名称不能为空"), RUVIA_MAX(64, "配置名称过长")),
    RUVIA_OPTIONAL_FIELD(remark, ruvia::String, RUVIA_NULLABLE, RUVIA_MAX(500, "remark 长度超出限制")),
    RUVIA_OPTIONAL_FIELD(enabled, ruvia::Bool, RUVIA_NULLABLE, RUVIA_DEFAULT(true)),
    RUVIA_OPTIONAL_FIELD(config, ruvia::JsonValue, RUVIA_NULLABLE));
RUVIA_REQUEST_MODEL(UpdateProtocolBody,
    RUVIA_OPTIONAL_FIELD(protocol, ruvia::String, RUVIA_NULLABLE, RUVIA_MAX(16, "协议过长")),
    RUVIA_OPTIONAL_FIELD(name, ruvia::String, RUVIA_NULLABLE, RUVIA_MAX(64, "配置名称过长")),
    RUVIA_OPTIONAL_FIELD(remark, ruvia::String, RUVIA_NULLABLE, RUVIA_MAX(500, "remark 长度超出限制")),
    RUVIA_OPTIONAL_FIELD(enabled, ruvia::Bool, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(config, ruvia::JsonValue, RUVIA_NULLABLE));

RUVIA_REQUEST_MODEL(ProtocolListQuery,
    RUVIA_OPTIONAL_FIELD(page, ruvia::Int64, RUVIA_DEFAULT(1), RUVIA_MIN(1, "page 必须大于 0")),
    RUVIA_OPTIONAL_FIELD(pageSize, ruvia::Int64, RUVIA_DEFAULT(10), RUVIA_MIN(1, "pageSize 必须在 1 - 1000 之间"), RUVIA_MAX(1000, "pageSize 必须在 1 - 1000 之间")),
    RUVIA_OPTIONAL_FIELD(protocol, ruvia::String, RUVIA_ONE_OF("协议无效", "SL651", "Modbus", "S7", "MC", "FINS", "DLT645")));
RUVIA_REQUEST_MODEL(ProtocolIdParams,
    RUVIA_REQUIRED_FIELD(id, ruvia::String, RUVIA_CUSTOM("id 必须是 UUID", service::common::isUuidField)));

} // namespace service::protocol
