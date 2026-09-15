#pragma once

#include <algorithm>
#include <array>
#include <map>
#include <memory_resource>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/web/ModelObject.h>

#include "service/common/http.h"
#include "service/common/message.h"
#include "service/modules/open_access/open_access.types.h"
#include "service/utils/json.h"
#include "service/utils/text.h"

namespace service::access {

class AccessPayloadValidator final {
  public:
    static std::string requiredString(const ruvia::JsonValue& payload, std::string_view field,
                                      std::string_view message, std::size_t maximum) {
        const auto value = payload.get<ruvia::String>(field);
        if (!value)
            service::common::fail(19002, std::string(message), 400);
        auto result = service::utils::trim(value->view());
        if (result.empty() || result.size() > maximum)
            service::common::fail(19002, std::string(message), 400);
        return result;
    }

    static std::optional<std::string> optionalString(const ruvia::JsonValue& payload,
                                                     std::string_view field, std::size_t maximum) {
        const auto raw = service::utils::jsonField(payload, field);
        if (!raw)
            return std::nullopt;
        const auto value = payload.get<ruvia::String>(field);
        if (!value)
            service::common::fail(19002, std::string(field) + " 必须是字符串", 400);
        auto result = service::utils::trim(value->view());
        if (result.size() > maximum)
            service::common::fail(19002, std::string(field) + " 长度超出限制", 400);
        return result;
    }

    static std::optional<std::string> optionalNullableString(const ruvia::JsonValue& payload,
                                                             std::string_view field,
                                                             std::size_t maximum) {
        const auto raw = service::utils::jsonField(payload, field);
        if (!raw || raw->isNull())
            return std::nullopt;
        const auto value = payload.get<ruvia::String>(field);
        if (!value)
            service::common::fail(19002, std::string(field) + " 必须是字符串或 null", 400);
        auto result = service::utils::trim(value->view());
        if (result.size() > maximum)
            service::common::fail(19002, std::string(field) + " 长度超出限制", 400);
        return result.empty() ? std::nullopt : std::optional<std::string>(std::move(result));
    }

    static std::string optionalStatus(const ruvia::JsonValue& payload, std::string_view fallback) {
        const auto status = optionalString(payload, "status", 16).value_or(std::string(fallback));
        if (status != "enabled" && status != "disabled")
            service::common::fail(19002, "status 只能为 enabled 或 disabled", 400);
        return status;
    }

    static std::int64_t optionalInteger(const ruvia::JsonValue& payload, std::string_view field,
                                        std::int64_t fallback, std::int64_t minimum,
                                        std::int64_t maximum) {
        const auto raw = service::utils::jsonField(payload, field);
        if (!raw)
            return fallback;
        const auto value = payload.get<ruvia::Int64>(field);
        if (!value)
            service::common::fail(19002, std::string(field) + " 必须是整数", 400);
        const auto result = static_cast<std::int64_t>(*value);
        if (result < minimum || result > maximum)
            service::common::fail(19002, std::string(field) + " 超出允许范围", 400);
        return result;
    }

    static bool optionalBoolean(const ruvia::JsonValue& payload, std::string_view field,
                                bool fallback) {
        const auto raw = service::utils::jsonField(payload, field);
        if (!raw)
            return fallback;
        const auto value = payload.get<ruvia::Bool>(field);
        if (!value)
            service::common::fail(19002, std::string(field) + " 必须是布尔值", 400);
        return static_cast<bool>(*value);
    }

    static std::set<std::string, std::less<>> requiredScopes(const ruvia::JsonValue& payload) {
        const auto values = payload.get<ruvia::Array<ruvia::String>>("scopes");
        if (!values || values->empty())
            service::common::fail(19002, "至少选择一个开放权限", 400);
        std::set<std::string, std::less<>> result;
        for (const auto& value : *values) {
            if (!supportedScope(value.view()))
                service::common::fail(19002, "包含不支持的开放权限", 400);
            result.emplace(value.view());
        }
        return result;
    }

    static std::vector<std::string> requiredUuids(const ruvia::JsonValue& payload,
                                                  std::string_view field, std::string_view message,
                                                  std::size_t maximum) {
        const auto values = payload.get<ruvia::Array<ruvia::String>>(field);
        if (!values || values->empty() || values->size() > maximum)
            service::common::fail(19002, std::string(message), 400);
        std::set<std::string, std::less<>> unique;
        for (const auto& value : *values) {
            service::common::requireUuid(19002, value.view(), std::string(field) + " 包含无效 UUID");
            unique.emplace(value.view());
        }
        return {unique.begin(), unique.end()};
    }

    static std::string requiredUuid(const ruvia::JsonValue& payload, std::string_view field,
                                    std::string_view message) {
        const auto value = payload.get<ruvia::String>(field);
        if (!value)
            service::common::fail(19002, std::string(message), 400);
        service::common::requireUuid(19002, value->view(), message);
        return std::string(value->view());
    }

    static std::string objectJson(const ruvia::JsonValue& payload, std::string_view field,
                                  std::string_view fallback) {
        const auto value = service::utils::jsonField(payload, field);
        if (!value)
            return std::string(fallback);
        if (!value->isObject())
            service::common::fail(19002, std::string(field) + " 必须是对象", 400);
        return std::string(value->view());
    }

    static std::set<std::string, std::less<>> eventTypes(const ruvia::JsonValue& payload) {
        const auto raw = service::utils::jsonField(payload, "eventTypes");
        if (!raw)
            return {"device.data.reported"};
        const auto values = payload.get<ruvia::Array<ruvia::String>>("eventTypes");
        if (!values || values->empty())
            service::common::fail(19002, "eventTypes 必须是非空字符串数组", 400);
        std::set<std::string, std::less<>> result;
        for (const auto& value : *values) {
            if (!service::message::supportedEvent(value.view()))
                service::common::fail(19002, "包含不支持的 Webhook 事件", 400);
            result.emplace(value.view());
        }
        return result;
    }

    static void validateHeaders(std::string_view headers) {
        // jsonb retains the final value for each decoded, case-sensitive key.
        std::map<std::string, std::optional<std::string>, std::less<>> fields;
        const auto valid = ruvia::detail::visitJsonObjectFields(
            ruvia::detail::ResolvedPmrResourceTag{}, headers, std::pmr::get_default_resource(),
            [&](std::string_view key, std::string_view raw) {
                auto remaining = raw;
                const auto value = ruvia::detail::parseJsonValue<ruvia::String>(
                    remaining, std::pmr::get_default_resource());
                fields.insert_or_assign(std::string(key), value
                    ? std::optional<std::string>(value->view()) : std::nullopt);
                return true;
            });
        if (!valid)
            service::common::fail(19002, "自定义 Header 包含非法或保留字段", 400);
        constexpr std::array<std::string_view, 15> reserved{
            "host", "content-length", "connection", "x-iot-event", "x-iot-timestamp",
            "x-iot-delivery", "x-iot-signature", "content-type", "user-agent",
            "transfer-encoding", "trailer", "te", "upgrade", "expect", "proxy-connection"};
        for (const auto& [key, value] : fields) {
            const auto token = !key.empty() && std::all_of(key.begin(), key.end(), [](char ch) {
                return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
                       (ch >= 'a' && ch <= 'z') ||
                       std::string_view("!#$%&'*+.^_`|~-").find(ch) != std::string_view::npos;
            });
            std::string lower(key);
            for (auto& ch : lower)
                if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
            if (!token || !value || value->find_first_of("\r\n") != std::string::npos ||
                std::find(reserved.begin(), reserved.end(), lower) != reserved.end())
                service::common::fail(19002, "自定义 Header 包含非法或保留字段", 400);
        }
    }

    static void validateWebhookUrl(std::string_view url) {
        if ((!url.starts_with("http://") && !url.starts_with("https://")) ||
            url.find('\r') != std::string_view::npos || url.find('\n') != std::string_view::npos ||
            url.find('@') != std::string_view::npos || url.find('#') != std::string_view::npos)
            service::common::fail(19002, "Webhook 地址必须是有效的 HTTP(S) URL", 400);
        const auto authority = url.find("://") + 3;
        if (authority >= url.size() || url[authority] == '/' || url[authority] == '?' ||
            url[authority] == '#')
            service::common::fail(19002, "Webhook 地址主机无效", 400);
    }

};

} // namespace service::access
