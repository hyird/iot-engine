#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <string_view>

namespace service::common::packet_log {

enum class Level { Trace, Debug, Info, Warn, Error, Off };

struct Config final {
    Level level = Level::Debug;
    std::filesystem::path directory;
};

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

[[nodiscard]] Level parseLevel(std::string_view value, Level fallback = Level::Debug) noexcept;

void initialize(Config config);
void shutdown() noexcept;

void write(Level level, std::string_view event, const Context& context = {},
           std::span<const std::uint8_t> bytes = {}, std::string_view reason = {}) noexcept;

} // namespace service::common::packet_log
