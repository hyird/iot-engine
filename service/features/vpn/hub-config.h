#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <ruvia/core/Task.h>

#include "service/common/http.h"
#include "service/features/vpn/wireguard.h"

namespace service::vpn::hub_config {

inline constexpr std::string_view kDefaultNetworkId{
    "00000000-0000-7000-8000-000000000004"};
inline constexpr std::string_view kDefaultNetworkName{"iot-server"};

inline std::string rowValue(const auto& row, std::size_t index) {
    return std::string(row[index].value().value_or(std::string_view{}));
}

inline int base64Value(char value) noexcept {
    if (value >= 'A' && value <= 'Z')
        return value - 'A';
    if (value >= 'a' && value <= 'z')
        return value - 'a' + 26;
    if (value >= '0' && value <= '9')
        return value - '0' + 52;
    if (value == '+')
        return 62;
    if (value == '/')
        return 63;
    return -1;
}

inline bool decodeKey(std::string_view input, std::array<unsigned char, 32>& output) noexcept {
    if (input.size() != 44 || input.back() != '=')
        return false;
    std::size_t outputIndex = 0;
    for (std::size_t index = 0; index < input.size(); index += 4) {
        const auto first = base64Value(input[index]);
        const auto second = base64Value(input[index + 1]);
        const auto third = input[index + 2] == '=' ? 0 : base64Value(input[index + 2]);
        const auto fourth = input[index + 3] == '=' ? 0 : base64Value(input[index + 3]);
        if (first < 0 || second < 0 || third < 0 || fourth < 0 ||
            (index + 2 == input.size() - 1 && input[index + 2] != '=') ||
            (index + 3 == input.size() - 1 && input[index + 3] != '='))
            return false;
        if (outputIndex < output.size())
            output[outputIndex++] = static_cast<unsigned char>((first << 2) | (second >> 4));
        if (index + 2 < input.size() - 1 && outputIndex < output.size())
            output[outputIndex++] = static_cast<unsigned char>((second << 4) | (third >> 2));
        if (index + 3 < input.size() - 1 && outputIndex < output.size())
            output[outputIndex++] = static_cast<unsigned char>((third << 6) | fourth);
    }
    return outputIndex == output.size();
}

inline std::string encodeKey(const std::array<unsigned char, 32>& input) {
    constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.resize(44, '=');
    std::size_t out = 0;
    for (std::size_t index = 0; index < input.size(); index += 3) {
        const auto left = input.size() - index;
        const auto value = (static_cast<unsigned>(input[index]) << 16) |
                           (left > 1 ? static_cast<unsigned>(input[index + 1]) << 8 : 0U) |
                           (left > 2 ? static_cast<unsigned>(input[index + 2]) : 0U);
        output[out++] = alphabet[(value >> 18) & 0x3fU];
        output[out++] = alphabet[(value >> 12) & 0x3fU];
        if (left > 1)
            output[out++] = alphabet[(value >> 6) & 0x3fU];
        if (left > 2)
            output[out++] = alphabet[value & 0x3fU];
    }
    return output;
}

inline bool derivePublicKey(std::string_view privateKey, std::string& publicKey) {
    std::array<unsigned char, 32> privateBytes{};
    if (!decodeKey(privateKey, privateBytes))
        return false;
    using KeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    KeyPtr key(EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, privateBytes.data(),
                                            privateBytes.size()),
              &EVP_PKEY_free);
    if (!key)
        return false;
    std::array<unsigned char, 32> publicBytes{};
    std::size_t publicSize = publicBytes.size();
    if (EVP_PKEY_get_raw_public_key(key.get(), publicBytes.data(), &publicSize) != 1 ||
        publicSize != publicBytes.size())
        return false;
    publicKey = encodeKey(publicBytes);
    return true;
}

inline bool generateKeyPair(std::string& privateKey, std::string& publicKey) {
    std::array<unsigned char, 32> privateBytes{};
    if (RAND_bytes(privateBytes.data(), static_cast<int>(privateBytes.size())) != 1)
        return false;
    privateKey = encodeKey(privateBytes);
    return derivePublicKey(privateKey, publicKey);
}

template <typename Context>
ruvia::Task<std::optional<wireguard::HubConfig>> loadOrInitialize(
    Context& context, const wireguard::HubConfig& fallback) {
    auto transaction = co_await context.db().beginTransaction();
    (void)co_await transaction.query(
        "SELECT pg_advisory_xact_lock(5282804697543808067::bigint)");
    const auto rows = co_await transaction.query(R"sql(
SELECT hub_private_key, hub_public_key, hub_endpoint, hub_listen_port
FROM vpn_network
WHERE id = '00000000-0000-7000-8000-000000000004'::uuid
  AND name = 'iot-server' AND status = 'enabled' AND deleted_at IS NULL
LIMIT 1
FOR UPDATE)sql");
    if (rows.empty()) {
        co_await transaction.commit();
        if (!wireguard::validKey(fallback.privateKey))
            co_return std::nullopt;
        co_return fallback;
    }

    auto config = fallback;
    config.interfaceName = "wg";
    config.address = "100.96.0.1/32";
    const auto storedPrivateKey = rowValue(rows.front(), 0);
    const auto storedPublicKey = rowValue(rows.front(), 1);
    const auto storedEndpoint = rowValue(rows.front(), 2);
    const auto storedPortText = rowValue(rows.front(), 3);
    const auto storedPort = service::common::parseInt64(
        std::optional<std::string_view>(storedPortText));
    config.endpoint = storedEndpoint.empty() ? fallback.endpoint : storedEndpoint;
    config.listenPort = storedPort && *storedPort > 0 && *storedPort <= 65535
                            ? static_cast<std::uint16_t>(*storedPort)
                            : fallback.listenPort;

    bool persist = false;
    if (storedPrivateKey.empty()) {
        if (wireguard::validKey(fallback.privateKey) &&
            derivePublicKey(fallback.privateKey, config.publicKey)) {
            config.privateKey = fallback.privateKey;
        } else if (!generateKeyPair(config.privateKey, config.publicKey)) {
            co_return std::nullopt;
        }
        persist = true;
    } else {
        if (!wireguard::validKey(storedPrivateKey) ||
            !derivePublicKey(storedPrivateKey, config.publicKey))
            co_return std::nullopt;
        config.privateKey = storedPrivateKey;
        persist = storedPublicKey != config.publicKey;
    }
    if (storedEndpoint.empty() && !fallback.endpoint.empty())
        persist = true;
    if (!storedPort || *storedPort != config.listenPort)
        persist = true;

    if (persist) {
        (void)co_await transaction.execute(R"sql(
UPDATE vpn_network
SET hub_private_key = $1, hub_public_key = $2, hub_endpoint = $3,
    hub_listen_port = $4, updated_at = NOW()
WHERE id = '00000000-0000-7000-8000-000000000004'::uuid)sql",
                                            service::common::dbParams(
                                                config.privateKey, config.publicKey,
                                                config.endpoint, static_cast<int>(config.listenPort)));
    }
    co_await transaction.commit();
    co_return config;
}

} // namespace service::vpn::hub_config
