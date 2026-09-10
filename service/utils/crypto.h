#pragma once

#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace service::utils {

inline std::string hexEncode(const unsigned char* data, std::size_t size) {
    static constexpr std::array<char, 16> digits{'0', '1', '2', '3', '4', '5', '6', '7',
                                                 '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string output;
    output.reserve(size * 2);
    for (std::size_t index = 0; index < size; ++index) {
        output.push_back(digits[data[index] >> 4U]);
        output.push_back(digits[data[index] & 0x0FU]);
    }
    return output;
}

inline std::string sha256(std::string_view input) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    if (EVP_Digest(input.data(), input.size(), digest.data(), &length, EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("sha256 failed");
    return hexEncode(digest.data(), length);
}

inline std::string hmacSha256(std::string_view secret, std::string_view input) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    if (HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
             reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest.data(),
             &length) == nullptr)
        throw std::runtime_error("hmac-sha256 failed");
    return hexEncode(digest.data(), length);
}

} // namespace service::utils
