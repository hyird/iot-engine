#pragma once

#include <array>
#include <memory_resource>
#include <optional>
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

template <typename Visitor>
bool visitJsonArray(const ruvia::JsonValue& value, Visitor&& visitor) {
    if (!value.isArray()) {
        return false;
    }
    // JsonValue has no public raw array iterator. Scan already validated JSON
    // without converting number tokens to floating point.
    const auto wire = value.view();
    auto position = wire.find('[') + 1;
    while (position < wire.size()) {
        position = wire.find_first_not_of(" \t\r\n", position);
        if (position == std::string_view::npos) {
            return false;
        }
        if (wire[position] == ']') {
            return true;
        }
        const auto start = position;
        std::size_t depth = 0;
        bool quoted = false;
        for (; position < wire.size(); ++position) {
            const char ch = wire[position];
            if (quoted) {
                if (ch == '\\') {
                    ++position;
                } else if (ch == '"') {
                    quoted = false;
                }
            } else if (ch == '"') {
                quoted = true;
            } else if (ch == '[' || ch == '{') {
                ++depth;
            } else if (ch == ']' || ch == '}') {
                if (!depth) {
                    break;
                }
                --depth;
            } else if (ch == ',' && !depth) {
                break;
            }
        }
        const auto item = ruvia::JsonValue::parse(wire.substr(start, position - start));
        if (!item || !visitor(*item) || position == wire.size()) {
            return false;
        }
        if (wire[position] == ']') {
            return true;
        }
        ++position;
    }
    return false;
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

// Ruvia exposes typed field reads but not raw object-field iteration. Slice
// already validated JSON here to preserve numeric and duplicate-key semantics
// without depending on the framework's private parser interfaces.
template <typename Visitor>
bool visitJsonFields(const ruvia::JsonValue& object, Visitor&& visitor) {
    if (!object.isObject()) {
        return false;
    }
    const auto wire = object.view();
    std::size_t position = wire.find('{') + 1;
    const auto whitespace = [&] {
        while (position < wire.size() && (wire[position] == ' ' || wire[position] == '\t' || wire[position] == '\r' || wire[position] == '\n')) {
            ++position;
        }
    };
    whitespace();
    while (position < wire.size() && wire[position] != '}') {
        const auto keyStart = position++;
        while (position < wire.size()) {
            const auto c = wire[position++];
            if (c == '\\') {
                ++position;
            } else if (c == '"') {
                break;
            }
        }
        const auto keyWire = "{\"key\":" + std::string(wire.substr(keyStart, position - keyStart)) + "}";
        const auto keyObject = ruvia::JsonObject::parse(keyWire);
        const auto key = keyObject ? keyObject->get<ruvia::String>("key") : std::nullopt;
        if (!key) {
            return false;
        }
        whitespace();
        if (position >= wire.size() || wire[position++] != ':') {
            return false;
        }
        whitespace();
        const auto valueStart = position;
        std::size_t depth = 0;
        bool quoted = false;
        for (; position < wire.size(); ++position) {
            const auto c = wire[position];
            if (quoted) {
                if (c == '\\') {
                    ++position;
                } else if (c == '"') {
                    quoted = false;
                }
            } else if (c == '"') {
                quoted = true;
            } else if (c == '{' || c == '[') {
                ++depth;
            } else if (c == '}' || c == ']') {
                if (!depth) {
                    break;
                }
                --depth;
            } else if (c == ',' && !depth) {
                break;
            }
        }
        const auto raw = wire.substr(valueStart, position - valueStart);
        if (!visitor(key->view(), raw)) {
            return false;
        }
        whitespace();
        if (position < wire.size() && wire[position] == ',') {
            ++position;
            whitespace();
        } else {
            break;
        }
    }
    return position < wire.size() && wire[position] == '}';
}

inline std::optional<ruvia::JsonValue> jsonField(const ruvia::JsonValue& object, std::string_view field) {
    if (!object.isObject()) {
        return std::nullopt;
    }
    std::optional<ruvia::JsonValue> result;
    const auto valid = visitJsonFields(object, [&](std::string_view key, std::string_view value) {
        if (key == field) {
            result = ruvia::JsonValue::parse(value);
        }
        return true;
    });
    if (!valid) return std::nullopt;
    return result;
}

} // namespace service::utils
