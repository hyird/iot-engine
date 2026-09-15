#pragma once

#include <string>
#include <vector>

namespace service::vpn::firewall {

struct ClientAccess final {
    std::string assignedIpv4;
    std::vector<std::string> sourceRoutes;
    std::vector<std::string> allowedRoutes;
    std::vector<std::string> edgeAddresses;
};

struct Result final {
    bool configured{};
    std::string message;
};

} // namespace service::vpn::firewall
