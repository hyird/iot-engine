#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include "service/common/message.h"
#include "service/common/uuid.h"

namespace service::edge::session_state {

inline std::string key(std::string_view nodeId) {
    return "iot:edge:session:" + std::string(nodeId);
}

struct State final {
    std::uint64_t epoch{};
    std::uint32_t protocolVersion{};
    std::size_t workerIndex{};
    std::string instanceId;
};

inline std::string value(std::uint64_t epoch, std::uint32_t protocolVersion,
                         std::size_t workerIndex,
                         std::string_view instance = service::runtime::instanceId()) {
    return std::to_string(epoch) + "|" + std::to_string(protocolVersion) + "|" +
           std::to_string(workerIndex) + "|" + std::string(instance);
}

template <typename Integer>
bool parseInteger(std::string_view value, Integer& result) {
    if (value.empty())
        return false;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}

inline std::optional<State> parse(std::string_view state) {
    const auto firstSeparator = state.find('|');
    const auto secondSeparator = firstSeparator == std::string_view::npos
                                     ? std::string_view::npos
                                     : state.find('|', firstSeparator + 1);
    if (firstSeparator == std::string_view::npos || firstSeparator == 0 ||
        secondSeparator == std::string_view::npos ||
        secondSeparator == firstSeparator + 1 || secondSeparator + 1 >= state.size())
        return std::nullopt;
    const auto thirdSeparator = state.find('|',secondSeparator+1);
    if (thirdSeparator == std::string_view::npos || thirdSeparator+1 >= state.size())
        return std::nullopt;
    State result;
    result.instanceId = state.substr(thirdSeparator+1);
    if (!service::common::isUuid(result.instanceId)) return std::nullopt;
    if (!parseInteger(state.substr(0, firstSeparator), result.epoch) ||
        !parseInteger(state.substr(firstSeparator + 1,
                                   secondSeparator - firstSeparator - 1),
                      result.protocolVersion) ||
        !parseInteger(state.substr(secondSeparator + 1, thirdSeparator-secondSeparator-1), result.workerIndex))
        return std::nullopt;
    return result;
}

inline std::optional<std::uint32_t> protocolVersion(std::string_view state) {
    const auto parsed = parse(state);
    return parsed ? std::optional<std::uint32_t>(parsed->protocolVersion) : std::nullopt;
}

inline std::optional<std::size_t> workerIndex(std::string_view state) {
    const auto parsed = parse(state);
    return parsed ? std::optional<std::size_t>(parsed->workerIndex) : std::nullopt;
}

} // namespace service::edge::session_state
