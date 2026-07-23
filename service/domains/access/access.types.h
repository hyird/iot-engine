#pragma once

#include <set>
#include <string>
#include <string_view>

namespace service::access {

struct AccessSession final {
    std::string id;
    std::string name;
    std::set<std::string, std::less<>> scopes;
    std::set<std::string, std::less<>> deviceIds;

    [[nodiscard]] bool allows(std::string_view scope) const { return scopes.contains(scope); }
    [[nodiscard]] bool allowsDevice(std::string_view id) const { return deviceIds.contains(id); }
};

} // namespace service::access
