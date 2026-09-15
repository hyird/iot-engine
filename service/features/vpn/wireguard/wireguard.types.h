#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace service::vpn::wireguard {

struct Peer final {
    std::string publicKey;
    std::vector<std::string> allowedIps;
};

struct RuntimeStatus final {
    bool supported{};
    bool configured{};
    std::string code;
    std::string message;
    std::size_t peerCount{};
};

} // namespace service::vpn::wireguard
