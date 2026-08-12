#pragma once

#include <cmath>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace service::utils {

inline std::optional<double> decimal(std::string_view value) noexcept {
    if (value.empty())
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
