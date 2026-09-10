#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

namespace service::utils {

inline std::string trim(std::string_view input) {
    while (!input.empty() && std::isspace(static_cast<unsigned char>(input.front())))
        input.remove_prefix(1);
    while (!input.empty() && std::isspace(static_cast<unsigned char>(input.back())))
        input.remove_suffix(1);
    return std::string(input);
}

inline std::string sanitize(std::string_view value, std::size_t maximum = 1000) {
    std::string output;
    output.reserve(std::min(maximum, value.size()));
    for (const auto ch : value) {
        if (output.size() == maximum)
            break;
        output.push_back(ch == '\n' || ch == '\r' ? ' ' : ch);
    }
    return trim(output);
}

} // namespace service::utils
