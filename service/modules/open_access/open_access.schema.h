#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <memory_resource>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/web/Controller.h>
#include <ruvia/web/ModelObject.h>

#include "service/common/http.h"
#include "service/common/message.h"
#include "service/modules/open_access/open_access.types.h"
#include "service/utils/json.h"
#include "service/utils/text.h"

namespace service::access {

class AccessPayloadValidator final {
  public:
    static AccessKeyInput keyInput(const ruvia::JsonValue& payload, bool creating) {
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

    static WebhookInput webhookInput(const ruvia::JsonValue& payload, bool creating) {
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

    static std::string requiredString(const ruvia::JsonValue& payload, std::string_view field, std::string_view message, std::size_t maximum) {
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

    static std::optional<std::string> optionalString(const ruvia::JsonValue& payload, std::string_view field, std::size_t maximum) {
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

    static std::optional<std::string> optionalNullableString(const ruvia::JsonValue& payload, std::string_view field, std::size_t maximum) {
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

    static std::string optionalStatus(const ruvia::JsonValue& payload, std::string_view fallback) {
        const auto status = optionalString(payload, "status", 16).value_or(std::string(fallback));
        if (status != "enabled" && status != "disabled") {
            service::common::fail(19002, "status 只能为 enabled 或 disabled", 400);
        }
        return status;
    }

    static std::int64_t optionalInteger(const ruvia::JsonValue& payload, std::string_view field, std::int64_t fallback, std::int64_t minimum, std::int64_t maximum) {
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

    static bool optionalBoolean(const ruvia::JsonValue& payload, std::string_view field, bool fallback) {
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

    static std::set<std::string, std::less<>> requiredScopes(const ruvia::JsonValue& payload) {
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

    static std::vector<std::string> requiredUuids(const ruvia::JsonValue& payload, std::string_view field, std::string_view message, std::size_t maximum) {
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

    static std::string requiredUuid(const ruvia::JsonValue& payload, std::string_view field, std::string_view message) {
        const auto value = payload.get<ruvia::String>(field);
        if (!value) {
            service::common::fail(19002, std::string(message), 400);
        }
        service::common::requireUuid(19002, value->view(), message);
        return std::string(value->view());
    }

    static std::string objectJson(const ruvia::JsonValue& payload, std::string_view field, std::string_view fallback) {
        const auto value = service::utils::jsonField(payload, field);
        if (!value) {
            return std::string(fallback);
        }
        if (!value->isObject()) {
            service::common::fail(19002, std::string(field) + " 必须是对象", 400);
        }
        return std::string(value->view());
    }

    static std::set<std::string, std::less<>> eventTypes(const ruvia::JsonValue& payload) {
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

    static void validateHeaders(std::string_view headers) {
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

    static void validateWebhookUrl(std::string_view url) {
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
};

class AccessIdValidator final : public ruvia::Middleware<AccessIdValidator> {
  public:
    RUVIA_VALIDATE_PARAM(AccessIdParams, RUVIA_RULE(id, RUVIA_REQUIRED("ID 不能为空"), RUVIA_CUSTOM("ID 必须是 UUID", service::common::isUuidField)))
};

class WebhookQueryValidator final : public ruvia::Middleware<WebhookQueryValidator> {
  public:
    RUVIA_VALIDATE_QUERY(WebhookQueryParams, RUVIA_RULE(accessKeyId, RUVIA_CUSTOM("accessKeyId 必须是 UUID", service::common::isUuidField)))
};

class AccessLogValidator final : public ruvia::Middleware<AccessLogValidator> {
  public:
    RUVIA_VALIDATE_QUERY(AccessLogParams, RUVIA_RULE(page, RUVIA_MIN(1, "page 必须大于 0")), RUVIA_RULE(pageSize, RUVIA_MIN(1, "pageSize 必须在 1 - 100 之间"), RUVIA_MAX(100, "pageSize 必须在 1 - 100 之间")), RUVIA_RULE(accessKeyId, RUVIA_CUSTOM("accessKeyId 必须是 UUID", service::common::isUuidField)), RUVIA_RULE(webhookId, RUVIA_CUSTOM("webhookId 必须是 UUID", service::common::isUuidField)), RUVIA_RULE(deviceId, RUVIA_CUSTOM("deviceId 必须是 UUID", service::common::isUuidField)), RUVIA_RULE(direction, RUVIA_MAX(128, "direction 长度超出限制")), RUVIA_RULE(action, RUVIA_MAX(128, "action 长度超出限制")), RUVIA_RULE(status, RUVIA_MAX(128, "status 长度超出限制")), RUVIA_RULE(eventType, RUVIA_MAX(128, "eventType 长度超出限制")))

    static AccessLogQuery filters(const AccessLogParams& parameters) {
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
};
} // namespace service::access
