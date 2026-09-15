#pragma once

#include <cstdint>
#include <string>

namespace service::vpn::wireguard {

struct HubConfig final {
    std::string interfaceName{"wg"};
    std::string privateKey;
    std::string publicKey;
    std::string endpoint;
    std::uint16_t listenPort{51820};
    std::string address{"100.96.0.1/32"};
};

} // namespace service::vpn::wireguard
