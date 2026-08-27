#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string_view>

class SipUnsupportedRequestGuard final {
public:
    using Clock = std::chrono::steady_clock;

    struct Decision {
        bool allowed{true};
        std::uint64_t suppressedToReport{0};
    };

    explicit SipUnsupportedRequestGuard(
        double burst = 8.0, double refillPerSecond = 1.0,
        std::chrono::seconds reportInterval = std::chrono::seconds(30))
        : burst_(std::max(1.0, burst)),
          refillPerSecond_(std::max(0.0, refillPerSecond)),
          reportInterval_(std::max(std::chrono::seconds(1), reportInterval)),
          tokens_(burst_) {}

    [[nodiscard]] Decision inspect(std::string_view packet,
                                   Clock::time_point now = Clock::now()) {
        if (!requiresGuard(packet))
            return {};

        if (!initialized_) {
            initialized_ = true;
            lastRefill_ = now;
            lastReport_ = now;
        } else if (now > lastRefill_) {
            const auto elapsed = std::chrono::duration<double>(now - lastRefill_).count();
            tokens_ = std::min(burst_, tokens_ + elapsed * refillPerSecond_);
            lastRefill_ = now;
        }

        if (tokens_ >= 1.0) {
            tokens_ -= 1.0;
            return {};
        }

        ++suppressed_;
        if (now - lastReport_ < reportInterval_)
            return {.allowed = false};

        const auto report = suppressed_;
        suppressed_ = 0;
        lastReport_ = now;
        return {.allowed = false, .suppressedToReport = report};
    }

    [[nodiscard]] static bool requiresGuard(std::string_view packet) noexcept {
        const auto tokenEnd = packet.find_first_of(" \r\n\t");
        if (tokenEnd == std::string_view::npos || tokenEnd == 0)
            return true;
        const auto token = packet.substr(0, tokenEnd);
        return token != "REGISTER" && token != "MESSAGE" && token != "SIP/2.0";
    }

private:
    double burst_;
    double refillPerSecond_;
    std::chrono::seconds reportInterval_;
    double tokens_;
    bool initialized_{false};
    Clock::time_point lastRefill_{};
    Clock::time_point lastReport_{};
    std::uint64_t suppressed_{0};
};
