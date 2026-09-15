#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace service::packet_log {

enum class Level { Trace, Debug, Info, Warn, Error, Off };

struct Context final {
    std::size_t workerIndex = std::numeric_limits<std::size_t>::max();
    std::string_view direction;
    std::string_view operation;
    std::string_view protocol;
    std::string_view linkId;
    std::string_view deviceId;
    std::string_view deviceCode;
    std::string_view connectionId;
    std::string_view remoteAddress;
    std::string_view messageId;
    std::string_view causationId;
    std::uint64_t sessionEpoch = 0;
};

} // namespace service::packet_log
