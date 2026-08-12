#pragma once

#include <cmath>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace service::utils {

inline bool decimalSyntax(std::string_view value) noexcept {
    if (value.empty())
        return false;
    std::size_t index = 0;
    if (value[index] == '+' || value[index] == '-')
        ++index;

    bool integralDigit = false;
    while (index < value.size() && value[index] >= '0' && value[index] <= '9') {
        integralDigit = true;
        ++index;
    }

    bool fractionalDigit = false;
    if (index < value.size() && value[index] == '.') {
        ++index;
        while (index < value.size() && value[index] >= '0' && value[index] <= '9') {
            fractionalDigit = true;
            ++index;
        }
    }
    if (!integralDigit && !fractionalDigit)
        return false;

    if (index < value.size() && (value[index] == 'e' || value[index] == 'E')) {
        ++index;
        if (index < value.size() && (value[index] == '+' || value[index] == '-'))
            ++index;
        bool exponentDigit = false;
        while (index < value.size() && value[index] >= '0' && value[index] <= '9') {
            exponentDigit = true;
            ++index;
        }
        if (!exponentDigit)
            return false;
    }
    return index == value.size();
}

inline std::optional<double> decimal(std::string_view value) noexcept {
    if (value.empty() || !decimalSyntax(value))
        return std::nullopt;

    try {
        std::istringstream input{std::string(value)};
        input.imbue(std::locale::classic());
        input >> std::noskipws;

        double result{};
        if (!(input >> result) || !input.eof() || !std::isfinite(result))
            return std::nullopt;
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace service::utils
