#pragma once

#include <array>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include <ruvia/web/ModelObject.h>

namespace service::utils {

inline std::string jsonEscape(std::string_view value) {
    static constexpr std::array<char, 16> hex{'0', '1', '2', '3', '4', '5', '6', '7',
                                              '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
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

inline std::string jsonQuoted(std::string_view value) { return "\"" + jsonEscape(value) + "\""; }

inline std::optional<ruvia::JsonValue> jsonField(const ruvia::JsonValue& object,
                                                 std::string_view field) {
    if (!object.isObject())
        return std::nullopt;
    std::optional<ruvia::JsonValue> result;
    const auto valid = ruvia::detail::visitJsonObjectFields(
        ruvia::detail::ResolvedPmrResourceTag{}, object.view(), std::pmr::get_default_resource(),
        [&](std::string_view key, std::string_view value) {
            if (key == field)
                result = ruvia::JsonValue::parse(value);
            return true;
        });
    return valid ? result : std::nullopt;
}

} // namespace service::utils
