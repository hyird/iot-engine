#pragma once
#include "service/utils/text.h"
#include "service/utils/json.h"
#include "service/common/http.h"
#include <ruvia/web/ModelObject.h>
#include <utility>
#include <string_view>
#include <set>
#include <optional>
#include <initializer_list>
#include <charconv>
#include <algorithm>

#include "service/common/uuid.h"

#include <cstdint>
#include <string>
#include <vector>

#include <ruvia/web/Validation.h>

namespace service::alert {

struct ApplyTemplateInput final {
    std::string templateId;
    std::vector<std::string> deviceIds;
};

RUVIA_REQUEST_MODEL(AlertIdParams, RUVIA_REQUIRED_FIELD(id, ruvia::String, RUVIA_CUSTOM("id 必须是 UUID", service::common::isUuidField)));

RUVIA_REQUEST_MODEL(AlertListQuery, RUVIA_OPTIONAL_FIELD(page, ruvia::Int64, RUVIA_DEFAULT(1), RUVIA_MIN(1, "page 必须大于 0")), RUVIA_OPTIONAL_FIELD_NAME("pageSize", pageSize, ruvia::Int64, RUVIA_DEFAULT(20), RUVIA_MIN(1, "pageSize 必须在 1 - 100 之间"), RUVIA_MAX(100, "pageSize 必须在 1 - 100 之间")), RUVIA_OPTIONAL_FIELD(keyword, ruvia::String), RUVIA_OPTIONAL_FIELD(deviceId, ruvia::String), RUVIA_OPTIONAL_FIELD(ruleId, ruvia::String), RUVIA_OPTIONAL_FIELD(status, ruvia::String), RUVIA_OPTIONAL_FIELD(severity, ruvia::String), RUVIA_OPTIONAL_FIELD(category, ruvia::String));

struct RuleInput final {
    std::string name;
    std::string deviceId;
    std::string severity;
    std::string conditions;
    std::string logic;
    std::int64_t silenceDuration{};
    std::string recoveryCondition;
    std::int64_t recoveryWaitSeconds{};
    std::string status;
    std::string remark;
};

struct TemplateInput final {
    std::string name;
    std::string category;
    std::string description;
    std::string severity;
    std::string conditions;
    std::string logic;
    std::int64_t silenceDuration{};
    std::string recoveryCondition;
    std::int64_t recoveryWaitSeconds{};
    std::string applicableProtocols;
    std::string protocolConfigId;
};

RUVIA_REQUEST_MODEL(AlertGroupedQuery, RUVIA_OPTIONAL_FIELD(days, ruvia::Int64, RUVIA_DEFAULT(7), RUVIA_MIN(1, "days 必须在 1 - 365 之间"), RUVIA_MAX(365, "days 必须在 1 - 365 之间")));


namespace alertRequest {
inline ApplyTemplateInput applyTemplateInput(const ruvia::JsonValue& payload);
inline RuleInput ruleInput(const ruvia::JsonValue& payload);
inline TemplateInput templateInput(const ruvia::JsonValue& payload);
inline std::string requiredString(const ruvia::JsonValue& payload, std::string_view field, std::string_view message, std::size_t maximum);
inline std::optional<std::string> optionalString(const ruvia::JsonValue& payload, std::string_view field, std::size_t maximum);
inline std::string requiredUuid(const ruvia::JsonValue& payload, std::string_view field, std::string_view message);
inline std::string optionalUuid(const ruvia::JsonValue& payload, std::string_view field);
inline std::vector<std::string> requiredUuids(const ruvia::JsonValue& payload, std::string_view field, std::string_view message);
inline std::string enumString(const ruvia::JsonValue& payload, std::string_view field, std::string_view fallback, std::initializer_list<std::string_view> allowed);
inline std::int64_t integer(const ruvia::JsonValue& payload, std::string_view field, std::int64_t fallback, std::int64_t minimum, std::int64_t maximum);
inline std::string array(const ruvia::JsonValue& payload, std::string_view field, std::string_view fallback);
inline std::string requiredArray(const ruvia::JsonValue& payload, std::string_view field, std::string_view message);
inline std::string requiredConditions(const ruvia::JsonValue& payload, std::string_view field, std::string_view message);
inline void validateConditions(std::string_view raw);
inline void validateCondition(std::string_view raw);
inline bool oneOf(std::string_view value, std::initializer_list<std::string_view> allowed);
inline std::string requiredConditionString(const ruvia::JsonValue& object, std::string_view field, std::string_view message, std::size_t maximum);
inline std::optional<std::string> optionalConditionString(const ruvia::JsonValue& object, std::string_view field, std::size_t maximum);
inline std::optional<std::string> optionalScalarText(const ruvia::JsonValue& object, std::string_view field);
inline std::optional<std::int64_t> optionalIntegerField(const ruvia::JsonValue& object, std::string_view field);
inline bool decimalText(std::string_view raw, bool allowNegative);
inline void validateOptionalBitIndex(const ruvia::JsonValue& object);

inline ApplyTemplateInput applyTemplateInput(const ruvia::JsonValue& payload) {
        return { requiredUuid(payload, "template_id", "请选择告警模板"),
                 requiredUuids(payload, "device_ids", "请选择目标设备") };
    }

inline RuleInput ruleInput(const ruvia::JsonValue& payload) {
        RuleInput result;
        result.name = requiredString(payload, "name", "规则名称不能为空", 128);
        result.deviceId = requiredUuid(payload, "device_id", "请选择关联设备");
        result.severity = enumString(payload, "severity", "warning", { "critical", "warning", "info" });
        result.conditions = requiredConditions(payload, "conditions", "至少配置一个告警条件");
        result.logic = enumString(payload, "logic", "and", { "and", "or" });
        result.silenceDuration = integer(payload, "silence_duration", 300, 0, 86400);
        result.recoveryCondition =
            optionalString(payload, "recovery_condition", 32).value_or("reverse");
        result.recoveryWaitSeconds =
            integer(payload, "recovery_wait_seconds", 60, 0, 86400);
        result.status = enumString(payload, "status", "enabled", { "enabled", "disabled" });
        result.remark = optionalString(payload, "remark", 500).value_or("");
        return result;
    }

inline TemplateInput templateInput(const ruvia::JsonValue& payload) {
        TemplateInput result;
        result.name = requiredString(payload, "name", "模板名称不能为空", 128);
        result.category = optionalString(payload, "category", 64).value_or("");
        result.description = optionalString(payload, "description", 500).value_or("");
        result.severity = enumString(payload, "severity", "warning", { "critical", "warning", "info" });
        result.conditions = requiredConditions(payload, "conditions", "至少配置一个告警条件");
        result.logic = enumString(payload, "logic", "and", { "and", "or" });
        result.silenceDuration = integer(payload, "silence_duration", 300, 0, 86400);
        result.recoveryCondition =
            optionalString(payload, "recovery_condition", 32).value_or("reverse");
        result.recoveryWaitSeconds =
            integer(payload, "recovery_wait_seconds", 60, 0, 86400);
        result.applicableProtocols = array(payload, "applicable_protocols", "[]");
        result.protocolConfigId = optionalUuid(payload, "protocol_config_id");
        return result;
    }

inline std::string requiredString(const ruvia::JsonValue& payload, std::string_view field, std::string_view message, std::size_t maximum) {
        const auto value = payload.get<ruvia::String>(field);
        if (!value) {
            service::common::fail(17002, std::string(message), 400);
        }
        auto result = service::utils::trim(value->view());
        if (result.empty() || result.size() > maximum) {
            service::common::fail(17002, std::string(message), 400);
        }
        return result;
    }

inline std::optional<std::string> optionalString(const ruvia::JsonValue& payload, std::string_view field, std::size_t maximum) {
        const auto raw = service::utils::jsonField(payload, field);
        if (!raw) {
            return std::nullopt;
        }
        const auto value = payload.get<ruvia::String>(field);
        if (!value) {
            service::common::fail(17002, std::string(field) + " 必须是字符串", 400);
        }
        auto result = service::utils::trim(value->view());
        if (result.size() > maximum) {
            service::common::fail(17002, std::string(field) + " 长度超出限制", 400);
        }
        return result;
    }

inline std::string requiredUuid(const ruvia::JsonValue& payload, std::string_view field, std::string_view message) {
        const auto value = payload.get<ruvia::String>(field);
        if (!value) {
            service::common::fail(17002, std::string(message), 400);
        }
        service::common::requireUuid(19002, value->view(), message);
        return std::string(value->view());
    }

inline std::string optionalUuid(const ruvia::JsonValue& payload, std::string_view field) {
        const auto value = payload.get<ruvia::String>(field);
        if (!value || value->view().empty()) {
            return {};
        }
        service::common::requireUuid(19002, value->view(), std::string(field) + " 无效");
        return std::string(value->view());
    }

inline std::vector<std::string> requiredUuids(const ruvia::JsonValue& payload, std::string_view field, std::string_view message) {
        const auto values = payload.get<ruvia::Array<ruvia::String>>(field);
        if (!values || values->empty() || values->size() > 1000) {
            service::common::fail(17002, std::string(message), 400);
        }
        std::set<std::string, std::less<>> unique;
        for (const auto& value : *values) {
            service::common::requireUuid(19002, value.view(), std::string(field) + " 包含无效 UUID");
            unique.emplace(value.view());
        }
        return { unique.begin(), unique.end() };
    }

inline std::string enumString(const ruvia::JsonValue& payload, std::string_view field, std::string_view fallback, std::initializer_list<std::string_view> allowed) {
        const auto value = optionalString(payload, field, 32).value_or(std::string(fallback));
        if (std::find(allowed.begin(), allowed.end(), value) == allowed.end()) {
            service::common::fail(17002, std::string(field) + " 无效", 400);
        }
        return value;
    }

inline std::int64_t integer(const ruvia::JsonValue& payload, std::string_view field, std::int64_t fallback, std::int64_t minimum, std::int64_t maximum) {
        const auto raw = service::utils::jsonField(payload, field);
        if (!raw) {
            return fallback;
        }
        const auto value = payload.get<ruvia::Int64>(field);
        if (!value) {
            service::common::fail(17002, std::string(field) + " 必须是整数", 400);
        }
        const auto result = static_cast<std::int64_t>(*value);
        if (result < minimum || result > maximum) {
            service::common::fail(17002, std::string(field) + " 超出允许范围", 400);
        }
        return result;
    }

inline std::string array(const ruvia::JsonValue& payload, std::string_view field, std::string_view fallback) {
        const auto value = service::utils::jsonField(payload, field);
        if (!value) {
            return std::string(fallback);
        }
        if (!value->isArray()) {
            service::common::fail(17002, std::string(field) + " 必须是数组", 400);
        }
        return std::string(value->view());
    }

inline std::string requiredArray(const ruvia::JsonValue& payload, std::string_view field, std::string_view message) {
        const auto result = array(payload, field, "[]");
        if (result == "[]") {
            service::common::fail(17002, std::string(message), 400);
        }
        return result;
    }

inline std::string requiredConditions(const ruvia::JsonValue& payload, std::string_view field, std::string_view message) {
        auto result = requiredArray(payload, field, message);
        validateConditions(result);
        return result;
    }

inline void validateConditions(std::string_view raw) {
        std::size_t count = 0;
        const auto parsed = ruvia::JsonValue::parse(raw);
        const auto valid = parsed && service::utils::visitJsonArray(*parsed, [&](const ruvia::JsonValue& item) {
                               ++count;
                               validateCondition(item.view());
                               return true;
                           });
        if (!valid || count == 0) {
            service::common::fail(17002, "至少配置一个告警条件", 400);
        }
    }

inline void validateCondition(std::string_view raw) {
        const auto parsed = ruvia::JsonValue::parse(raw);
        if (!parsed || !parsed->isObject()) {
            service::common::fail(17002, "告警条件必须是对象", 400);
        }
        const auto type = requiredConditionString(*parsed, "type", "告警条件类型不能为空", 32);
        if (type == "offline") {
            if (const auto duration = optionalIntegerField(*parsed, "duration");
                duration && (*duration < 1 || *duration > 86400)) {
                service::common::fail(17002, "离线检测时长超出允许范围", 400);
            }
            return;
        }
        if (type == "threshold") {
            (void)requiredConditionString(*parsed, "elementKey", "请选择告警要素", 128);
            const auto op = requiredConditionString(*parsed, "operator", "告警比较符不能为空", 8);
            if (!oneOf(op, { ">", ">=", "<", "<=", "==", "!=" })) {
                service::common::fail(17002, "告警比较符无效", 400);
            }
            const auto value = optionalScalarText(*parsed, "value");
            if (!value || value->empty()) {
                service::common::fail(17002, "告警阈值不能为空", 400);
            }
            if (oneOf(op, { ">", ">=", "<", "<=" }) && !decimalText(*value, true)) {
                service::common::fail(17002, "告警阈值必须是数字", 400);
            }
            validateOptionalBitIndex(*parsed);
            return;
        }
        if (type == "rate_of_change") {
            (void)requiredConditionString(*parsed, "elementKey", "请选择告警要素", 128);
            const auto direction =
                optionalConditionString(*parsed, "changeDirection", 16).value_or("any");
            if (!oneOf(direction, { "any", "rise", "fall" })) {
                service::common::fail(17002, "变化方向无效", 400);
            }
            const auto rate = optionalScalarText(*parsed, "changeRate");
            if (rate && !rate->empty() && !decimalText(*rate, false)) {
                service::common::fail(17002, "变化率必须是非负数字", 400);
            }
            validateOptionalBitIndex(*parsed);
            return;
        }
        service::common::fail(17002, "告警条件类型无效", 400);
    }

inline bool oneOf(std::string_view value, std::initializer_list<std::string_view> allowed) {
        return std::find(allowed.begin(), allowed.end(), value) != allowed.end();
    }

inline std::string requiredConditionString(const ruvia::JsonValue& object, std::string_view field, std::string_view message, std::size_t maximum) {
        const auto value = optionalConditionString(object, field, maximum);
        if (!value || value->empty()) {
            service::common::fail(17002, std::string(message), 400);
        }
        return *value;
    }

inline std::optional<std::string> optionalConditionString(const ruvia::JsonValue& object, std::string_view field, std::size_t maximum) {
        const auto raw = service::utils::jsonField(object, field);
        if (!raw) {
            return std::nullopt;
        }
        const auto value = object.get<ruvia::String>(field);
        if (!value) {
            service::common::fail(17002, std::string(field) + " 必须是字符串", 400);
        }
        auto result = service::utils::trim(value->view());
        if (result.size() > maximum) {
            service::common::fail(17002, std::string(field) + " 长度超出限制", 400);
        }
        return result;
    }

inline std::optional<std::string> optionalScalarText(const ruvia::JsonValue& object, std::string_view field) {
        const auto value = service::utils::jsonField(object, field);
        if (!value || value->isNull()) {
            return std::nullopt;
        }
        if (value->isString()) {
            return optionalConditionString(object, field, 128);
        }
        if (value->isNumber() || value->isBoolean()) {
            return service::utils::trim(value->view());
        }
        service::common::fail(17002, std::string(field) + " 必须是标量", 400);
        return std::nullopt;
    }

inline std::optional<std::int64_t> optionalIntegerField(const ruvia::JsonValue& object, std::string_view field) {
        const auto text = optionalScalarText(object, field);
        if (!text || text->empty()) {
            return std::nullopt;
        }
        std::int64_t result = 0;
        const auto first = text->data();
        const auto last = first + text->size();
        const auto [end, error] = std::from_chars(first, last, result);
        if (error != std::errc{} || end != last) {
            service::common::fail(17002, std::string(field) + " 必须是整数", 400);
        }
        return result;
    }

inline bool decimalText(std::string_view raw, bool allowNegative) {
        const auto value = service::utils::trim(raw);
        if (value.empty() || value.size() > 64) {
            return false;
        }
        std::size_t index = 0;
        if (allowNegative && value[index] == '-') {
            ++index;
            if (index == value.size()) {
                return false;
            }
        }
        bool beforeDot = false;
        bool afterDot = false;
        bool dot = false;
        for (; index < value.size(); ++index) {
            const char c = value[index];
            if (c >= '0' && c <= '9') {
                if (dot) {
                    afterDot = true;
                } else {
                    beforeDot = true;
                }
                continue;
            }
            if (c == '.' && !dot) {
                dot = true;
                continue;
            }
            return false;
        }
        return beforeDot && (!dot || afterDot);
    }

inline void validateOptionalBitIndex(const ruvia::JsonValue& object) {
        const auto bitIndex = optionalIntegerField(object, "bitIndex");
        if (bitIndex && (*bitIndex < 0 || *bitIndex > 62)) {
            service::common::fail(17002, "位索引超出允许范围", 400);
        }
    }
} // namespace alertRequest

} // namespace service::alert
