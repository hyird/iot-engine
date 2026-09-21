#pragma once

#include <array>
#include <string>
#include <string_view>

#include <ruvia/web/ModelObject.h>

namespace service::utils {

inline bool isJsonContentType(std::string_view value) noexcept {
    value = value.substr(0, value.find(';'));
    const auto first = value.find_first_not_of(" \t");
    if (first == std::string_view::npos) return false;
    value = value.substr(first, value.find_last_not_of(" \t") - first + 1);
    constexpr std::string_view expected = "application/json";
    if (value.size() != expected.size()) return false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char ch = value[index];
        const char lower = ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A')) : ch;
        if (lower != expected[index]) return false;
    }
    return true;
}

inline std::string jsonEscape(std::string_view value) {
    static constexpr std::array<char, 16> hex{ '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' };
    std::string output;
    output.reserve(value.size() + 8);
    for (const auto byte : value) {
        const auto ch = static_cast<unsigned char>(byte);
        switch (ch) {
            case '"':
                output += "\\\"";
                break;
            case '\\':
                output += "\\\\";
                break;
            case '\b':
                output += "\\b";
                break;
            case '\f':
                output += "\\f";
                break;
            case '\n':
                output += "\\n";
                break;
            case '\r':
                output += "\\r";
                break;
            case '\t':
                output += "\\t";
                break;
            default:
                if (ch < 0x20) {
                    output += "\\u00";
                    output.push_back(hex[ch >> 4U]);
                    output.push_back(hex[ch & 0x0FU]);
                } else {
                    output.push_back(static_cast<char>(ch));
                }
        }
    }
    return output;
}

inline std::string jsonQuoted(std::string_view value) {
    return "\"" + jsonEscape(value) + "\"";
}

} // namespace service::utils
