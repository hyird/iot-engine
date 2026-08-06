#pragma once

#include <chrono>
#include <cstdint>
#include <string_view>

namespace service::collector {

// std::hash is not required to stay stable between builds. Keep the polling phase derived from
// the persisted device id so configuration reloads do not collapse every device onto one tick.
[[nodiscard]] inline std::uint32_t stablePollHash(std::string_view deviceId) noexcept {
    std::uint32_t value = 2166136261U;
    for (const auto byte : deviceId) {
        value ^= static_cast<std::uint8_t>(byte);
        value *= 16777619U;
    }
    value ^= value >> 16U;
    value *= 0x7feb352dU;
    value ^= value >> 15U;
    value *= 0x846ca68bU;
    value ^= value >> 16U;
    return value;
}

[[nodiscard]] inline std::chrono::milliseconds staggeredPollDelay(
    std::string_view deviceId, std::chrono::seconds interval,
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) noexcept {
    const auto intervalSeconds = interval.count();
    if (intervalSeconds <= 1)
        return std::chrono::seconds(1);

    const auto epochSeconds =
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    const auto currentPhase =
        ((epochSeconds % intervalSeconds) + intervalSeconds) % intervalSeconds;
    const auto targetPhase = static_cast<std::int64_t>(
        stablePollHash(deviceId) % static_cast<std::uint64_t>(intervalSeconds));
    auto delaySeconds = targetPhase - currentPhase;
    if (delaySeconds <= 0)
        delaySeconds += intervalSeconds;
    return std::chrono::seconds(delaySeconds);
}

} // namespace service::collector
