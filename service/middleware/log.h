#pragma once

#include <sstream>
#include <utility>

#include <spdlog/spdlog.h>

namespace service::middleware {

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

} // namespace service::middleware

#define LOG_TRACE ::service::middleware::LogLine(spdlog::level::trace)
#define LOG_DEBUG ::service::middleware::LogLine(spdlog::level::debug)
#define LOG_INFO ::service::middleware::LogLine(spdlog::level::info)
#define LOG_WARN ::service::middleware::LogLine(spdlog::level::warn)
#define LOG_ERROR ::service::middleware::LogLine(spdlog::level::err)
