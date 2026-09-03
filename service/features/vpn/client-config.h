#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace service::vpn::client_config {

inline std::string render(std::string_view privateKey, std::string_view address,
                          std::string_view hubPublicKey, std::string_view hubEndpoint,
                          std::uint16_t hubPort,
                          const std::vector<std::string>& allowedRoutes) {
    std::string config{"[Interface]\nPrivateKey = "};
    config += privateKey;
    config += "\nAddress = ";
    config += address;
    config += "/32\nMTU = 1280\n\n[Peer]\nPublicKey = ";
    config += hubPublicKey;
    config += "\nEndpoint = ";
    config += hubEndpoint;
    config += ":" + std::to_string(hubPort);
    config += "\nAllowedIPs = ";
    for (std::size_t index = 0; index < allowedRoutes.size(); ++index) {
        if (index != 0)
            config += ", ";
        config += allowedRoutes[index];
    }
    config += "\nPersistentKeepalive = 25\n";
    return config;
}

} // namespace service::vpn::client_config
