#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>

#include <ruvia/web/Context.h>

#include "service/common/http.h"
#include "service/common/timestamp.h"

namespace service::gb28181 {

inline std::string requiredRoute(ruvia::Context& c, std::string_view name,
                                 std::string_view message) {
    const auto value = c.req().param(name);
    if (!value || value->empty() || value->size() > 128)
        service::common::fail(10001, std::string(message), 400);
    return std::string(*value);
}

inline std::string requiredQuery(ruvia::Context& c, std::string_view name,
                                 std::string_view message) {
    const auto value = c.req().query(name);
    if (!value || value->empty() || value->size() > 128)
        service::common::fail(10001, std::string(message), 400);
    return std::string(*value);
}

inline std::string requiredUtcQuery(ruvia::Context& c, std::string_view name,
                                    std::string_view message) {
    const auto value = requiredQuery(c, name, message);
    const auto canonical = service::common::canonicalUtcTimestamp(value);
    if (canonical.empty())
        service::common::fail(
            10001, std::string(name) + " 必须是有效的 RFC 3339 时间", 400);
    return canonical;
}

inline std::uint8_t ptzSpeed(ruvia::Context& c) {
    const auto input = c.req().query("speed");
    if (!input || input->empty())
        return 80;
    int value{};
    const auto [ptr, error] =
        std::from_chars(input->data(), input->data() + input->size(), value);
    if (error != std::errc{} || ptr != input->data() + input->size())
        service::common::fail(10001, "speed 必须是 0 - 255 的整数", 400);
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

inline double finiteQuery(ruvia::Context& c, std::string_view name, double minimum,
                          double maximum) {
    const auto input = requiredQuery(c, name, std::string(name) + " 不能为空");
    double value{};
    try {
        std::size_t consumed{};
        value = std::stod(input, &consumed);
        if (consumed != input.size())
            throw std::invalid_argument("trailing");
    } catch (...) {
        service::common::fail(10001, std::string(name) + " 必须是有效数字", 400);
    }
    if (!std::isfinite(value) || value < minimum || value > maximum)
        service::common::fail(10001, std::string(name) + " 超出允许范围", 400);
    return value;
}

inline void requirePtzAction(std::string_view action) {
    static const std::set<std::string_view> actions{"left", "right", "up",      "down",
                                                    "zoomin", "zoomout", "stop"};
    if (!actions.contains(action))
        service::common::fail(10001, "不支持的云台动作", 400);
}

} // namespace service::gb28181
