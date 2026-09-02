#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace service::vpn {

struct Ipv4Cidr final {
    std::uint32_t network{};
    std::uint8_t prefix{};

    [[nodiscard]] std::uint32_t size() const noexcept {
        return prefix == 0 ? 0U : (1U << (32U - prefix));
    }

    [[nodiscard]] bool contains(std::uint32_t address) const noexcept {
        if (prefix == 0)
            return true;
        const auto mask = 0xffffffffU << (32U - prefix);
        return (address & mask) == network;
    }

    [[nodiscard]] bool overlaps(const Ipv4Cidr& other) const noexcept {
        return contains(other.network) || other.contains(network);
    }

    [[nodiscard]] std::string text() const {
        return std::to_string((network >> 24U) & 0xffU) + "." +
               std::to_string((network >> 16U) & 0xffU) + "." +
               std::to_string((network >> 8U) & 0xffU) + "." +
               std::to_string(network & 0xffU) + "/" + std::to_string(prefix);
    }
};

inline std::optional<std::uint32_t> parseIpv4(std::string_view input) noexcept {
    std::uint32_t result{};
    for (int part = 0; part < 4; ++part) {
        const auto dot = input.find('.');
        const auto token = input.substr(0, dot);
        unsigned value{};
        const auto [end, error] =
            std::from_chars(token.data(), token.data() + token.size(), value);
        if (token.empty() || error != std::errc{} || end != token.data() + token.size() ||
            value > 255U || (token.size() > 1 && token.front() == '0'))
            return std::nullopt;
        result = (result << 8U) | value;
        if (part == 3)
            return dot == std::string_view::npos ? std::optional(result) : std::nullopt;
        if (dot == std::string_view::npos)
            return std::nullopt;
        input.remove_prefix(dot + 1);
    }
    return std::nullopt;
}

inline std::optional<Ipv4Cidr> parseCidr(std::string_view input,
                                         std::uint8_t minPrefix = 1,
                                         std::uint8_t maxPrefix = 32) noexcept {
    const auto slash = input.find('/');
    if (slash == std::string_view::npos || input.find('/', slash + 1) != std::string_view::npos)
        return std::nullopt;
    const auto address = parseIpv4(input.substr(0, slash));
    if (!address)
        return std::nullopt;
    unsigned prefix{};
    const auto prefixText = input.substr(slash + 1);
    const auto [end, error] = std::from_chars(prefixText.data(), prefixText.data() + prefixText.size(), prefix);
    if (prefixText.empty() || error != std::errc{} || end != prefixText.data() + prefixText.size() ||
        prefix < minPrefix || prefix > maxPrefix)
        return std::nullopt;
    const auto mask = prefix == 0 ? 0U : (0xffffffffU << (32U - prefix));
    if ((*address & mask) != *address)
        return std::nullopt;
    return Ipv4Cidr{*address, static_cast<std::uint8_t>(prefix)};
}

inline std::optional<Ipv4Cidr> networkCidr(std::string_view address,
                                           std::uint8_t prefix) noexcept {
    if (prefix == 0 || prefix > 30)
        return std::nullopt;
    const auto parsed = parseIpv4(address);
    if (!parsed)
        return std::nullopt;
    const auto mask = 0xffffffffU << (32U - prefix);
    return Ipv4Cidr{*parsed & mask, prefix};
}

inline std::optional<std::uint32_t> hostAddress(const Ipv4Cidr& network,
                                                std::uint32_t offset) noexcept {
    if (offset >= network.size())
        return std::nullopt;
    return network.network + offset;
}

inline bool isPrivateIpv4(const Ipv4Cidr& cidr) noexcept {
    const auto private10 = Ipv4Cidr{0x0a000000U, 8};
    const auto private172 = Ipv4Cidr{0xac100000U, 12};
    const auto private192 = Ipv4Cidr{0xc0a80000U, 16};
    return private10.contains(cidr.network) && private10.contains(cidr.network + cidr.size() - 1U) ||
           private172.contains(cidr.network) && private172.contains(cidr.network + cidr.size() - 1U) ||
           private192.contains(cidr.network) && private192.contains(cidr.network + cidr.size() - 1U);
}

inline constexpr Ipv4Cidr kOverlayPool{0x64600000U, 11}; // 100.96.0.0/11
inline constexpr Ipv4Cidr kVirtualLanPool{0xac1f0000U, 16}; // 172.31.0.0/16

// Keep the host portion unchanged by assigning the virtual network number from
// the real network number. If that slot is already occupied, the caller may
// choose another free slot while preserving the same prefix length.
inline std::optional<Ipv4Cidr> mappedVirtualCidr(const Ipv4Cidr& real) noexcept {
    if (real.prefix < kVirtualLanPool.prefix)
        return std::nullopt;
    const auto slotBits = static_cast<unsigned>(real.prefix - kVirtualLanPool.prefix);
    const auto slotCount = std::uint64_t{1} << slotBits;
    const auto slot = (static_cast<std::uint64_t>(real.network) >> (32U - real.prefix)) &
                      (slotCount - 1U);
    const auto network = static_cast<std::uint64_t>(kVirtualLanPool.network) +
                         slot * static_cast<std::uint64_t>(real.size());
    return network <= 0xffffffffU
               ? std::optional<Ipv4Cidr>(Ipv4Cidr{static_cast<std::uint32_t>(network), real.prefix})
               : std::nullopt;
}

} // namespace service::vpn
