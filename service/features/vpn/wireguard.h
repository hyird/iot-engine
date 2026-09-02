#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "service/features/vpn/cidr.h"

namespace service::vpn::wireguard {

struct Peer final {
    std::string publicKey;
    std::vector<std::string> allowedIps;
};

struct HubConfig final {
    std::string interfaceName{"wg-iot"};
    std::string privateKey;
    std::string publicKey;
    std::string endpoint;
    std::uint16_t listenPort{51820};
    std::string address{"100.96.0.1/32"};
};

struct RuntimeStatus final {
    bool supported{};
    bool configured{};
    std::string code;
    std::string message;
    std::size_t peerCount{};
};

inline bool validKey(std::string_view value) noexcept {
    if (value.size() != 44 || value.back() != '=')
        return false;
    for (const auto character : value)
        if (!((character >= 'A' && character <= 'Z') ||
              (character >= 'a' && character <= 'z') ||
              (character >= '0' && character <= '9') || character == '+' || character == '/' ||
              character == '='))
            return false;
    return true;
}

class IWireGuardController {
  public:
    virtual ~IWireGuardController() = default;
    virtual RuntimeStatus configure(const HubConfig& config) = 0;
    virtual RuntimeStatus upsertPeer(const HubConfig& config, const Peer& peer) = 0;
    virtual RuntimeStatus removePeer(const HubConfig& config, std::string_view publicKey) = 0;
    virtual RuntimeStatus status(const HubConfig& config) = 0;
    virtual std::optional<std::vector<std::string>> peerKeys(const HubConfig& config) = 0;
    virtual std::optional<std::unordered_map<std::string, std::uint64_t>> peerHandshakes(
        const HubConfig& config) = 0;
};

class UnsupportedController final : public IWireGuardController {
  public:
    RuntimeStatus configure(const HubConfig&) override { return unsupported(); }
    RuntimeStatus upsertPeer(const HubConfig&, const Peer&) override { return unsupported(); }
    RuntimeStatus removePeer(const HubConfig&, std::string_view) override { return unsupported(); }
    RuntimeStatus status(const HubConfig&) override { return unsupported(); }
    std::optional<std::vector<std::string>> peerKeys(const HubConfig&) override { return std::nullopt; }
    std::optional<std::unordered_map<std::string, std::uint64_t>>
    peerHandshakes(const HubConfig&) override {
        return std::nullopt;
    }

  private:
    static RuntimeStatus unsupported() {
        return {.supported = false,
                .configured = false,
                .code = "unsupported_platform",
                .message = "WireGuard hub is only supported on Linux",
                .peerCount = 0};
    }
};

} // namespace service::vpn::wireguard

#ifdef __linux__
#include "service/features/vpn/wireguard-linux.h"
namespace service::vpn::wireguard {
using LinuxController = linux_detail::Controller;
}
#endif

namespace service::vpn::wireguard {
inline IWireGuardController& controller() {
#ifdef __linux__
    static LinuxController value;
#else
    static UnsupportedController value;
#endif
    return value;
}

} // namespace service::vpn::wireguard
