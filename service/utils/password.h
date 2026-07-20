#pragma once

#include <array>
#include <charconv>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace service::utils::password_detail {

inline std::string hexEncode(const unsigned char* data, std::size_t size) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        result.push_back(digits[data[i] >> 4]);
        result.push_back(digits[data[i] & 0x0f]);
    }
    return result;
}

inline std::vector<unsigned char> hexDecode(std::string_view value) {
    auto nibble = [](char ch) -> int {
        if (ch >= '0' && ch <= '9')
            return ch - '0';
        if (ch >= 'a' && ch <= 'f')
            return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F')
            return ch - 'A' + 10;
        return -1;
    };
    if (value.size() % 2 != 0)
        return {};
    std::vector<unsigned char> result;
    result.reserve(value.size() / 2);
    for (std::size_t i = 0; i < value.size(); i += 2) {
        const int high = nibble(value[i]);
        const int low = nibble(value[i + 1]);
        if (high < 0 || low < 0)
            return {};
        result.push_back(static_cast<unsigned char>((high << 4) | low));
    }
    return result;
}

inline bool derive(std::string_view plain, const unsigned char* salt, std::size_t saltSize,
                   int iterations, unsigned char* output, std::size_t outputSize) {
    return PKCS5_PBKDF2_HMAC(plain.data(), static_cast<int>(plain.size()), salt,
                             static_cast<int>(saltSize), iterations, EVP_sha256(),
                             static_cast<int>(outputSize), output) == 1;
}

} // namespace service::utils::password_detail

namespace service::utils {

inline std::string hashPassword(std::string_view plain) {
    constexpr int iterations = 210000;
    std::array<unsigned char, 16> salt{};
    std::array<unsigned char, 32> key{};
    if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1 ||
        !password_detail::derive(plain, salt.data(), salt.size(), iterations, key.data(),
                                 key.size())) {
        throw std::runtime_error("password hashing failed");
    }
    return "pbkdf2_sha256$" + std::to_string(iterations) + "$" +
           password_detail::hexEncode(salt.data(), salt.size()) + "$" +
           password_detail::hexEncode(key.data(), key.size());
}

inline bool comparePassword(std::string_view plain, std::string_view encoded) {
    constexpr std::string_view prefix = "pbkdf2_sha256$";
    if (!encoded.starts_with(prefix))
        return false;
    auto rest = encoded.substr(prefix.size());
    const auto first = rest.find('$');
    const auto second = first == std::string_view::npos ? first : rest.find('$', first + 1);
    if (first == std::string_view::npos || second == std::string_view::npos)
        return false;

    int iterations{};
    const auto iterationText = rest.substr(0, first);
    const auto [ptr, ec] = std::from_chars(iterationText.data(),
                                           iterationText.data() + iterationText.size(), iterations);
    if (ec != std::errc{} || ptr != iterationText.data() + iterationText.size() ||
        iterations <= 0) {
        return false;
    }

    const auto salt = password_detail::hexDecode(rest.substr(first + 1, second - first - 1));
    const auto expected = password_detail::hexDecode(rest.substr(second + 1));
    if (salt.empty() || expected.empty())
        return false;
    std::vector<unsigned char> actual(expected.size());
    if (!password_detail::derive(plain, salt.data(), salt.size(), iterations, actual.data(),
                                 actual.size()))
        return false;
    return CRYPTO_memcmp(actual.data(), expected.data(), expected.size()) == 0;
}

} // namespace service::utils
