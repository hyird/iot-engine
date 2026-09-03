#include <cstdlib>
#include <iostream>
#include <string_view>

#include "service/features/vpn/cidr.h"
#include "service/features/vpn/client-config.h"
#include "service/features/vpn/hub-config.h"
#include "service/features/vpn/wireguard.h"

namespace {

void require(bool condition, std::string_view message) {
    if (condition)
        return;
    std::cerr << "vpn test failed: " << message << '\n';
    std::exit(1);
}

} // namespace

int main() {
    const auto overlay = service::vpn::parseCidr("100.96.0.0/24", 16, 30);
    const auto virtualLan = service::vpn::parseCidr("172.31.1.0/24", 1, 30);
    const auto realLan = service::vpn::parseCidr("192.168.1.0/24", 1, 30);
    require(overlay && overlay->text() == "100.96.0.0/24", "overlay CIDR parsing");
    require(virtualLan && service::vpn::kVirtualLanPool.contains(virtualLan->network),
            "virtual LAN pool membership");
    require(realLan && service::vpn::isPrivateIpv4(*realLan), "private LAN detection");
    const auto mapped = realLan ? service::vpn::mappedVirtualCidr(*realLan) : std::nullopt;
    require(mapped && mapped->text() == "172.31.1.0/24", "automatic network mapping");
    require(mapped && service::vpn::hostAddress(*mapped, 50).value() == 0xac1f0132U &&
                         service::vpn::hostAddress(*realLan, 50).value() == 0xc0a80132U,
            "host offset is preserved");
    const auto smallerReal = service::vpn::parseCidr("192.168.1.128/25", 1, 30);
    const auto smallerMapped = smallerReal ? service::vpn::mappedVirtualCidr(*smallerReal)
                                           : std::nullopt;
    require(smallerMapped && smallerMapped->text() == "172.31.1.128/25",
            "automatic mapping preserves subnet bits");
    require(overlay && service::vpn::parseCidr("100.96.0.1/24", 16, 30) == std::nullopt,
            "non-canonical CIDR rejection");
    require(virtualLan && realLan && virtualLan->prefix == realLan->prefix,
            "equal-prefix route contract");
    require(virtualLan && service::vpn::hostAddress(*virtualLan, 2).value() == 0xac1f0102U,
            "host address allocation");

    constexpr std::string_view key =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq=";
    require(service::vpn::wireguard::validKey(key), "WireGuard public key validation");
    require(!service::vpn::wireguard::validKey("invalid"), "invalid WireGuard key rejection");

    std::string privateKey;
    std::string publicKey;
    require(service::vpn::hub_config::generateKeyPair(privateKey, publicKey),
            "Hub key pair generation");
    std::string derivedPublicKey;
    require(service::vpn::hub_config::derivePublicKey(privateKey, derivedPublicKey) &&
                derivedPublicKey == publicKey,
            "Hub public key derivation");
    const auto clientConfig = service::vpn::client_config::render(
        privateKey, "100.96.0.3", publicKey, "vpn.example.com", 51820,
        {"172.31.1.0/24", "172.31.2.0/24"});
    require(clientConfig.find("PrivateKey = " + privateKey) != std::string::npos,
            "client configuration includes its one-time private key");
    require(clientConfig.find("Address = 100.96.0.3/32") != std::string::npos,
            "client configuration uses a unique overlay host address");
    require(clientConfig.find("AllowedIPs = 172.31.1.0/24, 172.31.2.0/24") !=
                std::string::npos,
            "client configuration includes every accessible virtual route");
    require(clientConfig.find("<client-private-key>") == std::string::npos,
            "generated client configuration has no private-key placeholder");
    return 0;
}
