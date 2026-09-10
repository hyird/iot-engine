#pragma once

#include <sstream>
#include <utility>

#include <spdlog/spdlog.h>

namespace service::common {

class LogLine final {
  public:
    explicit LogLine(spdlog::level::level_enum level) noexcept : level_(level) {}

    ~LogLine() {
        try {
            spdlog::log(level_, "{}", output_.str());
        } catch (...) {
        }
    }

    LogLine(const LogLine&) = delete;
    LogLine& operator=(const LogLine&) = delete;
    LogLine(LogLine&&) = delete;
    LogLine& operator=(LogLine&&) = delete;

    template <typename T> LogLine& operator<<(T&& value) {
        output_ << std::forward<T>(value);
        return *this;
    }

  private:
    spdlog::level::level_enum level_;
    std::ostringstream output_;
};

} // namespace service::common

#define LOG_TRACE ::service::common::LogLine(spdlog::level::trace)
#define LOG_DEBUG ::service::common::LogLine(spdlog::level::debug)
#define LOG_INFO ::service::common::LogLine(spdlog::level::info)
#define LOG_WARN ::service::common::LogLine(spdlog::level::warn)
#define LOG_ERROR ::service::common::LogLine(spdlog::level::err)

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
