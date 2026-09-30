#pragma once

#include <array>
#include <charconv>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/web/ModelObject.h>

namespace service::utils {

inline bool isJsonContentType(std::string_view value) noexcept {
    value = value.substr(0, value.find(';'));
    const auto first = value.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return false;
    }
    value = value.substr(first, value.find_last_not_of(" \t") - first + 1);
    constexpr std::string_view expected = "application/json";
    if (value.size() != expected.size()) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char ch = value[index];
        const char lower = ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A')) : ch;
        if (lower != expected[index]) {
            return false;
        }
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

// 裸字段名表示顶层键；以 / 开始时使用 RFC 6901 JSON Pointer。
inline std::vector<std::string> jsonFieldSegments(std::string_view path) {
    if (path.empty()) {
        return {};
    }
    if (path.front() != '/') {
        return { std::string(path) };
    }
    std::vector<std::string> result;
    path.remove_prefix(1);
    for (;;) {
        const auto end = path.find('/');
        const auto part = path.substr(0, end);
        std::string key;
        for (std::size_t i = 0; i < part.size(); ++i) {
            if (part[i] != '~') {
                key += part[i];
                continue;
            }
            if (++i == part.size() || (part[i] != '0' && part[i] != '1')) {
                throw std::invalid_argument("invalid JSON Pointer escape");
            }
            key += part[i] == '0' ? '~' : '/';
        }
        result.push_back(std::move(key));
        if (result.size() > 32) {
            throw std::invalid_argument("JSON Pointer exceeds 32 levels");
        }
        if (end == std::string_view::npos) {
            return result;
        }
        path.remove_prefix(end + 1);
    }
}

inline std::optional<ruvia::JsonValue> jsonField(const ruvia::JsonValue& root, std::string_view path) {
    auto current = ruvia::JsonValue::parse(root.view());
    if (!current) {
        return std::nullopt;
    }
    for (const auto& key : jsonFieldSegments(path)) {
        if (current->isObject()) {
            current = current->get<ruvia::JsonValue>(key);
        } else if (current->isArray()) {
            std::size_t index = 0;
            const auto number = std::from_chars(key.data(), key.data() + key.size(), index);
            if (key.empty() || (key.size() > 1 && key.front() == '0') || number.ec != std::errc{} || number.ptr != key.data() + key.size()) {
                return std::nullopt;
            }
            std::optional<ruvia::JsonValue> selected;
            std::size_t position = 0;
            (void)current->forEachElement([&](const ruvia::JsonValue& item) {
                if (position++ == index) {
                    selected = ruvia::JsonValue::parse(item.view());
                    return false;
                }
                return true;
            });
            current = std::move(selected);
        } else {
            return std::nullopt;
        }
        if (!current) {
            return std::nullopt;
        }
    }
    return current;
}

inline std::string expandJsonTemplate(std::string_view input, const std::map<std::string, std::string>& values) {
    if (!ruvia::JsonValue::parse(input)) {
        throw std::invalid_argument("JSON 模板无效");
    }
    std::string result;
    for (std::size_t i = 0; i < input.size();) {
        if (input[i] != '"') {
            result += input[i++];
            continue;
        }
        const auto begin = i++;
        while (i < input.size()) {
            if (input[i] == '\\') {
                i += 2;
                continue;
            }
            if (input[i++] == '"') {
                break;
            }
        }
        const auto token = ruvia::JsonValue::parse(input.substr(begin, i - begin));
        const auto text = token ? token->get<ruvia::String>() : std::nullopt;
        const auto found = text ? values.find(std::string(text->view())) : values.end();
        result += found == values.end() ? std::string(input.substr(begin, i - begin)) : found->second;
    }
    if (!ruvia::JsonValue::parse(result)) {
        throw std::invalid_argument("模板占位符须位于有效的 JSON 值位置");
    }
    return result;
}

} // namespace service::utils
