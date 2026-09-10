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



} // namespace service::access
