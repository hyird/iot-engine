#pragma once
#include "service/utils/text.h"
#include "service/utils/json.h"
#include "service/common/http.h"
#include <ruvia/web/Validation.h>
#include <map>
#include <limits>
#include <array>
#include <algorithm>

#include "service/common/uuid.h"

#include <charconv>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/web/ModelObject.h>

#include "service/common/message.h"

namespace service::access {

inline constexpr std::string_view kScopeRealtime = "device:realtime";
inline constexpr std::string_view kScopeHistory = "device:history";
inline constexpr std::string_view kScopeCommand = "device:command";
inline constexpr std::string_view kScopeAlert = "alert:read";

inline bool supportedScope(std::string_view value) {
    return value == kScopeRealtime || value == kScopeHistory || value == kScopeCommand ||
        value == kScopeAlert;
}

struct AccessSession final {
    std::string id;
    std::string name;
    std::set<std::string, std::less<>> scopes;
    std::set<std::string, std::less<>> deviceIds;

    [[nodiscard]] bool allows(std::string_view scope) const { return scopes.contains(scope); }

    [[nodiscard]] bool allowsDevice(std::string_view id) const { return deviceIds.contains(id); }
};

struct AccessKeyInput final {
    std::optional<std::string> name;
    std::optional<std::string> status;
    std::optional<std::set<std::string, std::less<>>> scopes;
    std::optional<std::vector<std::string>> deviceIds;
    bool expiresAtPresent{ false };
    std::optional<std::string> expiresAt;
    bool remarkPresent{ false };
    std::optional<std::string> remark;
};

struct WebhookInput final {
    std::optional<std::string> accessKeyId;
    std::optional<std::string> name;
    std::optional<std::string> url;
    std::optional<std::string> status;
    std::optional<std::int64_t> timeoutSeconds;
    std::optional<bool> skipTlsVerify;
    std::optional<std::string> headers;
    std::optional<std::set<std::string, std::less<>>> eventTypes;
    bool secretPresent{ false };
    std::optional<std::string> secret;
};

struct WebhookQuery final {
    std::optional<std::string> accessKeyId;
};

struct AccessLogQuery final {
    std::int64_t page{ 1 };
    std::int64_t pageSize{ 20 };
    std::optional<std::string> accessKeyId, webhookId, deviceId;
    std::optional<std::string> direction, action, status, eventType;
};

RUVIA_REQUEST_MODEL(AccessIdParams, RUVIA_REQUIRED_FIELD(id, ruvia::String, RUVIA_CUSTOM("ID 必须是 UUID", service::common::isUuidField)));
RUVIA_REQUEST_MODEL(WebhookQueryParams, RUVIA_OPTIONAL_FIELD(accessKeyId, ruvia::String, RUVIA_CUSTOM("accessKeyId 必须是 UUID", service::common::isUuidField)));
RUVIA_REQUEST_MODEL(AccessLogParams, RUVIA_OPTIONAL_FIELD(page, ruvia::Int64, RUVIA_DEFAULT(1), RUVIA_MIN(1, "page 必须大于 0")), RUVIA_OPTIONAL_FIELD(pageSize, ruvia::Int64, RUVIA_DEFAULT(20), RUVIA_MIN(1, "pageSize 必须在 1 - 100 之间"), RUVIA_MAX(100, "pageSize 必须在 1 - 100 之间")), RUVIA_OPTIONAL_FIELD(accessKeyId, ruvia::String, RUVIA_CUSTOM("accessKeyId 必须是 UUID", service::common::isUuidField)), RUVIA_OPTIONAL_FIELD(webhookId, ruvia::String, RUVIA_CUSTOM("webhookId 必须是 UUID", service::common::isUuidField)), RUVIA_OPTIONAL_FIELD(deviceId, ruvia::String, RUVIA_CUSTOM("deviceId 必须是 UUID", service::common::isUuidField)), RUVIA_OPTIONAL_FIELD(direction, ruvia::String, RUVIA_MAX(128, "direction 长度超出限制")), RUVIA_OPTIONAL_FIELD(action, ruvia::String, RUVIA_MAX(128, "action 长度超出限制")), RUVIA_OPTIONAL_FIELD(status, ruvia::String, RUVIA_MAX(128, "status 长度超出限制")), RUVIA_OPTIONAL_FIELD(eventType, ruvia::String, RUVIA_MAX(128, "eventType 长度超出限制")));



namespace accessRequest {
inline AccessKeyInput keyInput(const ruvia::JsonValue& payload, bool creating);
inline WebhookInput webhookInput(const ruvia::JsonValue& payload, bool creating);
inline std::string requiredString(const ruvia::JsonValue& payload, std::string_view field, std::string_view message, std::size_t maximum);
inline std::optional<std::string> optionalString(const ruvia::JsonValue& payload, std::string_view field, std::size_t maximum);
inline std::optional<std::string> optionalNullableString(const ruvia::JsonValue& payload, std::string_view field, std::size_t maximum);
inline std::string optionalStatus(const ruvia::JsonValue& payload, std::string_view fallback);
inline std::int64_t optionalInteger(const ruvia::JsonValue& payload, std::string_view field, std::int64_t fallback, std::int64_t minimum, std::int64_t maximum);
inline bool optionalBoolean(const ruvia::JsonValue& payload, std::string_view field, bool fallback);
inline std::set<std::string, std::less<>> requiredScopes(const ruvia::JsonValue& payload);
inline std::vector<std::string> requiredUuids(const ruvia::JsonValue& payload, std::string_view field, std::string_view message, std::size_t maximum);
inline std::string requiredUuid(const ruvia::JsonValue& payload, std::string_view field, std::string_view message);
inline std::string objectJson(const ruvia::JsonValue& payload, std::string_view field, std::string_view fallback);
inline std::set<std::string, std::less<>> eventTypes(const ruvia::JsonValue& payload);
inline void validateHeaders(std::string_view headers);
inline void validateWebhookUrl(std::string_view url);

inline AccessKeyInput keyInput(const ruvia::JsonValue& payload, bool creating) {
        AccessKeyInput result;
        if (creating || service::utils::jsonField(payload, "name")) {
            result.name = requiredString(payload, "name", "调用配置名称不能为空", 64);
        }
        if (creating || service::utils::jsonField(payload, "status")) {
            result.status = optionalStatus(payload, "enabled");
        }
        if (creating || service::utils::jsonField(payload, "scopes")) {
            result.scopes = requiredScopes(payload);
        }
        if (creating || service::utils::jsonField(payload, "deviceIds")) {
            result.deviceIds = requiredUuids(payload, "deviceIds", "至少选择一个设备", 10000);
        }
        result.expiresAtPresent = service::utils::jsonField(payload, "expiresAt").has_value();
        result.expiresAt = optionalNullableString(payload, "expiresAt", 64);
        result.remarkPresent = service::utils::jsonField(payload, "remark").has_value();
        result.remark = optionalNullableString(payload, "remark", 200);
        return result;
    }

inline WebhookInput webhookInput(const ruvia::JsonValue& payload, bool creating) {
        WebhookInput result;
        if (creating || service::utils::jsonField(payload, "accessKeyId")) {
            result.accessKeyId = requiredUuid(payload, "accessKeyId", "请选择调用配置");
        }
        if (creating || service::utils::jsonField(payload, "name")) {
            result.name = requiredString(payload, "name", "Webhook 名称不能为空", 64);
        }
        if (creating || service::utils::jsonField(payload, "url")) {
            result.url = requiredString(payload, "url", "Webhook 地址不能为空", 2048);
            validateWebhookUrl(*result.url);
        }
        if (creating || service::utils::jsonField(payload, "status")) {
            result.status = optionalStatus(payload, "enabled");
        }
        if (creating || service::utils::jsonField(payload, "timeoutSeconds")) {
            result.timeoutSeconds = optionalInteger(payload, "timeoutSeconds", 5, 1, 30);
        }
        if (creating || service::utils::jsonField(payload, "skipTlsVerify")) {
            result.skipTlsVerify = optionalBoolean(payload, "skipTlsVerify", false);
        }
        if (creating || service::utils::jsonField(payload, "headers")) {
            result.headers = objectJson(payload, "headers", "{}");
            validateHeaders(*result.headers);
        }
        if (creating || service::utils::jsonField(payload, "eventTypes")) {
            result.eventTypes = eventTypes(payload);
        }
        result.secretPresent = service::utils::jsonField(payload, "secret").has_value();
        result.secret = optionalNullableString(payload, "secret", 255);
        return result;
    }

inline std::string requiredString(const ruvia::JsonValue& payload, std::string_view field, std::string_view message, std::size_t maximum) {
        const auto value = payload.get<ruvia::String>(field);
        if (!value) {
            service::common::fail(19002, std::string(message), 400);
        }
        auto result = service::utils::trim(value->view());
        if (result.empty() || result.size() > maximum) {
            service::common::fail(19002, std::string(message), 400);
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
            service::common::fail(19002, std::string(field) + " 必须是字符串", 400);
        }
        auto result = service::utils::trim(value->view());
        if (result.size() > maximum) {
            service::common::fail(19002, std::string(field) + " 长度超出限制", 400);
        }
        return result;
    }

inline std::optional<std::string> optionalNullableString(const ruvia::JsonValue& payload, std::string_view field, std::size_t maximum) {
        const auto raw = service::utils::jsonField(payload, field);
        if (!raw || raw->isNull()) {
            return std::nullopt;
        }
        const auto value = payload.get<ruvia::String>(field);
        if (!value) {
            service::common::fail(19002, std::string(field) + " 必须是字符串或 null", 400);
        }
        auto result = service::utils::trim(value->view());
        if (result.size() > maximum) {
            service::common::fail(19002, std::string(field) + " 长度超出限制", 400);
        }
        return result.empty() ? std::nullopt : std::optional<std::string>(std::move(result));
    }

inline std::string optionalStatus(const ruvia::JsonValue& payload, std::string_view fallback) {
        const auto status = optionalString(payload, "status", 16).value_or(std::string(fallback));
        if (status != "enabled" && status != "disabled") {
            service::common::fail(19002, "status 只能为 enabled 或 disabled", 400);
        }
        return status;
    }

inline std::int64_t optionalInteger(const ruvia::JsonValue& payload, std::string_view field, std::int64_t fallback, std::int64_t minimum, std::int64_t maximum) {
        const auto raw = service::utils::jsonField(payload, field);
        if (!raw) {
            return fallback;
        }
        const auto value = payload.get<ruvia::Int64>(field);
        if (!value) {
            service::common::fail(19002, std::string(field) + " 必须是整数", 400);
        }
        const auto result = static_cast<std::int64_t>(*value);
        if (result < minimum || result > maximum) {
            service::common::fail(19002, std::string(field) + " 超出允许范围", 400);
        }
        return result;
    }

inline bool optionalBoolean(const ruvia::JsonValue& payload, std::string_view field, bool fallback) {
        const auto raw = service::utils::jsonField(payload, field);
        if (!raw) {
            return fallback;
        }
        const auto value = payload.get<ruvia::Bool>(field);
        if (!value) {
            service::common::fail(19002, std::string(field) + " 必须是布尔值", 400);
        }
        return static_cast<bool>(*value);
    }

inline std::set<std::string, std::less<>> requiredScopes(const ruvia::JsonValue& payload) {
        const auto values = payload.get<ruvia::Array<ruvia::String>>("scopes");
        if (!values || values->empty()) {
            service::common::fail(19002, "至少选择一个开放权限", 400);
        }
        std::set<std::string, std::less<>> result;
        for (const auto& value : *values) {
            if (!supportedScope(value.view())) {
                service::common::fail(19002, "包含不支持的开放权限", 400);
            }
            result.emplace(value.view());
        }
        return result;
    }

inline std::vector<std::string> requiredUuids(const ruvia::JsonValue& payload, std::string_view field, std::string_view message, std::size_t maximum) {
        const auto values = payload.get<ruvia::Array<ruvia::String>>(field);
        if (!values || values->empty() || values->size() > maximum) {
            service::common::fail(19002, std::string(message), 400);
        }
        std::set<std::string, std::less<>> unique;
        for (const auto& value : *values) {
            service::common::requireUuid(19002, value.view(), std::string(field) + " 包含无效 UUID");
            unique.emplace(value.view());
        }
        return { unique.begin(), unique.end() };
    }

inline std::string requiredUuid(const ruvia::JsonValue& payload, std::string_view field, std::string_view message) {
        const auto value = payload.get<ruvia::String>(field);
        if (!value) {
            service::common::fail(19002, std::string(message), 400);
        }
        service::common::requireUuid(19002, value->view(), message);
        return std::string(value->view());
    }

inline std::string objectJson(const ruvia::JsonValue& payload, std::string_view field, std::string_view fallback) {
        const auto value = service::utils::jsonField(payload, field);
        if (!value) {
            return std::string(fallback);
        }
        if (!value->isObject()) {
            service::common::fail(19002, std::string(field) + " 必须是对象", 400);
        }
        return std::string(value->view());
    }

inline std::set<std::string, std::less<>> eventTypes(const ruvia::JsonValue& payload) {
        const auto raw = service::utils::jsonField(payload, "eventTypes");
        if (!raw) {
            return { "device.data.reported" };
        }
        const auto values = payload.get<ruvia::Array<ruvia::String>>("eventTypes");
        if (!values || values->empty()) {
            service::common::fail(19002, "eventTypes 必须是非空字符串数组", 400);
        }
        std::set<std::string, std::less<>> result;
        for (const auto& value : *values) {
            if (!service::message::supportedEvent(value.view())) {
                service::common::fail(19002, "包含不支持的 Webhook 事件", 400);
            }
            result.emplace(value.view());
        }
        return result;
    }

inline void validateHeaders(std::string_view headers) {
        // jsonb retains the final value for each decoded, case-sensitive key.
        std::map<std::string, std::optional<std::string>, std::less<>> fields;
        const auto object = ruvia::JsonValue::parse(headers);
        const auto valid = object && service::utils::visitJsonFields(*object, [&](std::string_view key, std::string_view raw) {
                               const auto fieldJson = "{\"value\":" + std::string(raw) + "}";
                               const auto field = ruvia::JsonValue::parse(fieldJson);
                               const auto value = field ? field->get<ruvia::String>("value") : std::nullopt;
                               fields.insert_or_assign(std::string(key), value ? std::optional<std::string>(value->view()) : std::nullopt);
                               return true;
                           });
        if (!valid) {
            service::common::fail(19002, "自定义 Header 包含非法或保留字段", 400);
        }
        constexpr std::array<std::string_view, 15> reserved{
            "host",
            "content-length",
            "connection",
            "x-iot-event",
            "x-iot-timestamp",
            "x-iot-delivery",
            "x-iot-signature",
            "content-type",
            "user-agent",
            "transfer-encoding",
            "trailer",
            "te",
            "upgrade",
            "expect",
            "proxy-connection"
        };
        for (const auto& [key, value] : fields) {
            const auto token = !key.empty() && std::all_of(key.begin(), key.end(), [](char ch) {
                return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
                    (ch >= 'a' && ch <= 'z') ||
                    std::string_view("!#$%&'*+.^_`|~-").find(ch) != std::string_view::npos;
            });
            std::string lower(key);
            for (auto& ch : lower) {
                if (ch >= 'A' && ch <= 'Z') {
                    ch = static_cast<char>(ch + ('a' - 'A'));
                }
            }
            if (!token || !value || value->find_first_of("\r\n") != std::string::npos ||
                std::find(reserved.begin(), reserved.end(), lower) != reserved.end()) {
                service::common::fail(19002, "自定义 Header 包含非法或保留字段", 400);
            }
        }
    }

inline void validateWebhookUrl(std::string_view url) {
        if ((!url.starts_with("http://") && !url.starts_with("https://")) ||
            url.find('\r') != std::string_view::npos || url.find('\n') != std::string_view::npos ||
            url.find('@') != std::string_view::npos || url.find('#') != std::string_view::npos) {
            service::common::fail(19002, "Webhook 地址必须是有效的 HTTP(S) URL", 400);
        }
        const auto authority = url.find("://") + 3;
        if (authority >= url.size() || url[authority] == '/' || url[authority] == '?' ||
            url[authority] == '#') {
            service::common::fail(19002, "Webhook 地址主机无效", 400);
        }
    }
} // namespace accessRequest

inline AccessLogQuery accessLogFilters(const AccessLogParams& parameters) {
        AccessLogQuery result;
        result.page = static_cast<std::int64_t>(*parameters.get<"page">());
        result.pageSize = static_cast<std::int64_t>(*parameters.get<"pageSize">());
        if (result.page - 1 > std::numeric_limits<std::int64_t>::max() / result.pageSize) {
            service::common::fail(19002, "分页超出允许范围", 400);
        }
        if (const auto& value = parameters.get<"accessKeyId">()) {
            result.accessKeyId = service::utils::trim(value->view());
        }
        if (const auto& value = parameters.get<"webhookId">()) {
            result.webhookId = service::utils::trim(value->view());
        }
        if (const auto& value = parameters.get<"deviceId">()) {
            result.deviceId = service::utils::trim(value->view());
        }
        if (const auto& value = parameters.get<"direction">()) {
            result.direction = service::utils::trim(value->view());
        }
        if (const auto& value = parameters.get<"action">()) {
            result.action = service::utils::trim(value->view());
        }
        if (const auto& value = parameters.get<"status">()) {
            result.status = service::utils::trim(value->view());
        }
        if (const auto& value = parameters.get<"eventType">()) {
            result.eventType = service::utils::trim(value->view());
        }
        return result;
    }

} // namespace service::access
