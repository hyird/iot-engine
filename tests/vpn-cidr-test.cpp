#include <cstdlib>
#include <iostream>
#include <string_view>

#include "service/features/vpn/vpn.service.h"
#include "service/modules/vpn/vpn.types.h"
#include "service/features/vpn/firewall/firewall.transport.h"
#include "service/features/vpn/wireguard/wireguard.transport.h"

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
    require(service::vpn::kVirtualLanPool.text() == "172.16.0.0/12",
            "virtual LAN pool size");
    require(virtualLan && service::vpn::kVirtualLanPool.contains(virtualLan->network),
            "virtual LAN pool membership");
    require(realLan && service::vpn::isPrivateIpv4(*realLan), "private LAN detection");
    const auto mapped = realLan ? service::vpn::mappedVirtualCidr(*realLan) : std::nullopt;
    require(mapped && mapped->text() == "172.24.1.0/24", "automatic network mapping");
    require(mapped && service::vpn::hostAddress(*mapped, 50).value() == 0xac180132U &&
                         service::vpn::hostAddress(*realLan, 50).value() == 0xc0a80132U,
            "host offset is preserved");
    const auto smallerReal = service::vpn::parseCidr("192.168.1.128/25", 1, 30);
    const auto smallerMapped = smallerReal ? service::vpn::mappedVirtualCidr(*smallerReal)
                                           : std::nullopt;
    require(smallerMapped && smallerMapped->text() == "172.24.1.128/25",
            "automatic mapping preserves subnet bits");
    const auto expandedReal = service::vpn::parseCidr("192.168.0.0/16", 1, 30);
    const auto expandedMapped = expandedReal ? service::vpn::mappedVirtualCidr(*expandedReal)
                                             : std::nullopt;
    require(expandedMapped && expandedMapped->text() == "172.24.0.0/16",
            "automatic mapping follows an expanded bridge prefix");
    const auto outsideVirtualPool = service::vpn::parseCidr("172.168.1.0/24", 1, 30);
    require(outsideVirtualPool &&
                !service::vpn::kVirtualLanPool.contains(outsideVirtualPool->network),
            "public 172/8 space is excluded from the virtual LAN pool");
    require(overlay && service::vpn::parseCidr("100.96.0.1/24", 16, 30) == std::nullopt,
            "non-canonical CIDR rejection");
    require(virtualLan && realLan && virtualLan->prefix == realLan->prefix,
            "equal-prefix route contract");
    require(virtualLan && service::vpn::hostAddress(*virtualLan, 2).value() == 0xac1f0102U,
            "host address allocation");
    const auto moduleReal = service::vpn::module::parseCidr("192.168.1.0/24", 1, 30);
    const auto moduleVirtual = service::vpn::module::parseCidr("172.31.1.0/24", 1, 30);
    const auto otherVirtual = service::vpn::module::parseCidr("172.31.2.0/24", 1, 30);
    require(moduleReal && !service::vpn::module::realTargetConflictsVirtual(*moduleReal, std::nullopt),
            "duplicate real LANs remain independent across Edge peers");
    require(moduleVirtual && moduleReal &&
                service::vpn::module::virtualMappingConflicts(*moduleVirtual, moduleReal, moduleVirtual) &&
                !service::vpn::module::virtualMappingConflicts(*moduleVirtual, moduleReal, otherVirtual),
            "virtual mapping conflicts are checked independently");

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
    require(clientConfig.find(
                "AllowedIPs = 172.31.1.0/24, 172.31.2.0/24") !=
                std::string::npos,
            "client configuration includes only accessible virtual routes");
    require(clientConfig.find("AllowedIPs = 100.96.") == std::string::npos,
            "client configuration excludes VPN overlay routes");
    require(clientConfig.find("<client-private-key>") == std::string::npos,
            "generated client configuration has no private-key placeholder");

    std::string firewallScript;
    const auto firewallResult = service::vpn::firewall::render(
        "wg0",
        {{.assignedIpv4 = "100.96.0.2",
          .sourceRoutes = {"172.24.1.0/24"},
          .allowedRoutes = {"172.24.1.0/24", "172.24.2.0/24"}},
         {.assignedIpv4 = "100.96.0.3",
          .allowedRoutes = {"172.24.1.0/24"},
          .edgeAddresses = {"100.96.0.2"}}},
        firewallScript);
    require(firewallResult.configured, "site-to-site firewall rendering");
    require(firewallScript.find(
                "ip saddr { 100.96.0.2/32, 172.24.1.0/24 } ip daddr { "
                "172.24.1.0/24, 172.24.2.0/24 } accept") != std::string::npos,
            "Edge overlay and virtual LAN sources can reach same-network virtual routes");
    require(firewallScript.find(
                "ip saddr 100.96.0.3/32 ip daddr { 172.24.1.0/24, "
                "100.96.0.2/32 } accept") != std::string::npos,
            "Windows clients retain virtual route and Edge address access");
    require(firewallScript.find(
                "ip saddr { 100.96.0.0/11, 172.16.0.0/12 } drop") !=
                std::string::npos,
            "unapproved overlay and virtual LAN forwarding is denied");
    require(firewallScript.find("ct state established,related ip daddr") != std::string::npos,
            "permitted reverse established traffic is accepted");
    require(firewallScript.find(
                "ct state established,related ip daddr 100.96.0.3/32 ip saddr { 172.24.1.0/24, 100.96.0.2/32 } accept") !=
                std::string::npos,
            "reverse traffic is narrowed to the selected route");
    require(firewallScript.find(
                "iifname \"wg0\" oifname \"wg0\" ct state established,related accept\n") ==
                std::string::npos,
            "global established traffic acceptance is absent");
    require(service::vpn::firewall::render("wg0", {service::vpn::firewall::ClientAccess{.assignedIpv4 = "100.96.0.3"}},
                             firewallScript).configured,
            "empty client selection renders");
    require(firewallScript.find(" accept\n") == std::string::npos &&
                firewallScript.find("ct state established,related") == std::string::npos,
            "clearing a selection removes both forward and established reverse grants");
    return 0;
}
