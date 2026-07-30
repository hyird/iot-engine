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
