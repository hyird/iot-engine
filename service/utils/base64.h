#pragma once

#include <algorithm>
#include <limits>
#include <openssl/evp.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace service::utils {

inline std::string encodeBase64(std::string_view bytes) {
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) / 4 * 3) {
        throw std::length_error("Base64 input exceeds codec limit");
    }
    std::string result((bytes.size() + 2) / 3 * 4 + 1, '\0');
    const auto length = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(result.data()), reinterpret_cast<const unsigned char*>(bytes.data()), static_cast<int>(bytes.size()));
    result.resize(static_cast<std::size_t>(length));
    return result;
}

inline std::optional<std::string> decodeBase64(std::string_view encoded, std::size_t limit) {
    if (encoded.empty() || encoded.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) || encoded.size() / 4 > (limit + 2) / 3 || encoded.size() % 4) {
        return std::nullopt;
    }
    const std::size_t padding = encoded.ends_with("==") ? 2 : encoded.ends_with('=') ? 1
                                                                                     : 0;
    const auto content = encoded.substr(0, encoded.size() - padding);
    if (!std::ranges::all_of(content, [](unsigned char value) {
            return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
                (value >= '0' && value <= '9') || value == '+' || value == '/';
        })) {
        return std::nullopt;
    }
    std::string bytes(encoded.size() / 4 * 3, '\0');
    const int length = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(bytes.data()), reinterpret_cast<const unsigned char*>(encoded.data()), static_cast<int>(encoded.size()));
    if (length < 0 || static_cast<std::size_t>(length) < padding) {
        return std::nullopt;
    }
    bytes.resize(static_cast<std::size_t>(length) - padding);
    if (bytes.empty() || bytes.size() > limit || encodeBase64(bytes) != encoded) {
        return std::nullopt;
    }
    return bytes;
}

} // namespace service::utils
