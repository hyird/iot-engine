#pragma once
#include "service/utils/json.h"
#include "service/common/http.h"

#include "service/common/uuid.h"
#include "service/common/derived_point.h"

#include <optional>
#include <string>

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

// Configuration JSON stays exact until PostgreSQL jsonb normalization. Request
// fields have explicit types; an omitted remark differs from clearing it.
struct CreateProtocolBody {
    std::string protocol;
    std::string name;
    std::optional<std::string> remark;
    bool enabled{ true };
    std::optional<ruvia::JsonValue> config;
    static CreateProtocolBody parse(const ruvia::JsonValue& object);
};

struct UpdateProtocolBody {
    std::optional<std::string> protocol;
    std::optional<std::string> name;
    std::optional<std::string> remark;
    bool remarkPresent{};
    std::optional<bool> enabled;
    std::optional<ruvia::JsonValue> config;
    static UpdateProtocolBody parse(const ruvia::JsonValue& object);
};

RUVIA_REQUEST_MODEL(ProtocolListQuery, RUVIA_OPTIONAL_FIELD(page, ruvia::Int64, RUVIA_DEFAULT(1), RUVIA_MIN(1, "page 必须大于 0")), RUVIA_OPTIONAL_FIELD_NAME("pageSize", pageSize, ruvia::Int64, RUVIA_DEFAULT(10), RUVIA_MIN(1, "pageSize 必须在 1 - 1000 之间"), RUVIA_MAX(1000, "pageSize 必须在 1 - 1000 之间")), RUVIA_OPTIONAL_FIELD(protocol, ruvia::String, RUVIA_ONE_OF("协议无效", "SL651", "Modbus", "S7", "MC", "FINS", "DLT645")));

RUVIA_REQUEST_MODEL(ProtocolIdParams, RUVIA_REQUIRED_FIELD(id, ruvia::String, RUVIA_CUSTOM("id 必须是 UUID", service::common::isUuidField)));



namespace protocolRequest {
inline std::optional<std::string> text(const ruvia::JsonValue& object, std::string_view name, std::size_t maximum);
inline std::optional<std::string> remark(const ruvia::JsonValue& object);
inline std::optional<bool> enabled(const ruvia::JsonValue& object);

inline std::optional<std::string> text(const ruvia::JsonValue& object, std::string_view name, std::size_t maximum) {
        const auto raw = service::utils::jsonField(object, name);
        if (!raw) {
            return std::nullopt;
        }
        const auto value = object.get<ruvia::String>(name);
        if (!value) {
            service::common::fail(16002, std::string(name) + " 必须是字符串", 400);
        }
        if (value->view().size() > maximum) {
            service::common::fail(16002, std::string(name) + " 长度超出限制", 400);
        }
        return std::string(value->view());
    }

inline std::optional<std::string> remark(const ruvia::JsonValue& object) {
        const auto raw = service::utils::jsonField(object, "remark");
        if (!raw || raw->isNull()) {
            return std::nullopt;
        }
        if (!raw->isString()) {
            service::common::fail(16002, "remark 必须是字符串或 null", 400);
        }
        auto value = text(object, "remark", 500);
        return value && !value->empty() ? value : std::nullopt;
    }

inline std::optional<bool> enabled(const ruvia::JsonValue& object) {
        const auto raw = service::utils::jsonField(object, "enabled");
        if (!raw) {
            return std::nullopt;
        }
        if (!raw->isBoolean()) {
            service::common::fail(16004, "enabled 必须是布尔值", 400);
        }
        return static_cast<bool>(*object.get<ruvia::Bool>("enabled"));
    }
} // namespace protocolRequest

inline CreateProtocolBody CreateProtocolBody::parse(const ruvia::JsonValue& object) {
        if (!object.isObject()) {
            service::common::fail(16002, "请求体必须是对象", 400);
        }
        const auto protocol = protocolRequest::text(object, "protocol", 16);
        const auto name = protocolRequest::text(object, "name", 64);
        if (!protocol || protocol->empty()) {
            service::common::fail(16002, "协议不能为空", 400);
        }
        if (!name || name->empty()) {
            service::common::fail(16002, "配置名称不能为空", 400);
        }
        return CreateProtocolBody{ *protocol, *name, protocolRequest::remark(object), protocolRequest::enabled(object).value_or(true), service::utils::jsonField(object, "config") };
    }

inline UpdateProtocolBody UpdateProtocolBody::parse(const ruvia::JsonValue& object) {
        if (!object.isObject()) {
            service::common::fail(16002, "请求体必须是对象", 400);
        }

        return UpdateProtocolBody{ protocolRequest::text(object, "protocol", 16), protocolRequest::text(object, "name", 64), protocolRequest::remark(object), service::utils::jsonField(object, "remark").has_value(), protocolRequest::enabled(object), service::utils::jsonField(object, "config") };
    }

} // namespace service::protocol
