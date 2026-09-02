#include <cstdlib>
#include <iostream>
#include <string_view>

#include "service/features/vpn/cidr.h"
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
    return 0;
}
