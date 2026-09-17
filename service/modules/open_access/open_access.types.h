#pragma once

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

RUVIA_REQUEST_MODEL(AccessIdParams, RUVIA_OPTIONAL_FIELD(id, ruvia::String));
RUVIA_REQUEST_MODEL(WebhookQueryParams, RUVIA_OPTIONAL_FIELD(accessKeyId, ruvia::String));
RUVIA_REQUEST_MODEL(AccessLogParams, RUVIA_OPTIONAL_FIELD(page, ruvia::Int64, RUVIA_DEFAULT(1)), RUVIA_OPTIONAL_FIELD(pageSize, ruvia::Int64, RUVIA_DEFAULT(20)), RUVIA_OPTIONAL_FIELD(accessKeyId, ruvia::String), RUVIA_OPTIONAL_FIELD(webhookId, ruvia::String), RUVIA_OPTIONAL_FIELD(deviceId, ruvia::String), RUVIA_OPTIONAL_FIELD(direction, ruvia::String), RUVIA_OPTIONAL_FIELD(action, ruvia::String), RUVIA_OPTIONAL_FIELD(status, ruvia::String), RUVIA_OPTIONAL_FIELD(eventType, ruvia::String));

} // namespace service::access
