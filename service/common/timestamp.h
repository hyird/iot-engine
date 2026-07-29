#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

namespace service::common {

// Internal pipelines keep time as time_point, epoch milliseconds, or TIMESTAMPTZ.
// Convert to the public RFC 3339 contract only at an API or webhook boundary.
inline std::string utcTimestamp(std::chrono::system_clock::time_point value) {
    const auto seconds = std::chrono::floor<std::chrono::seconds>(value);
    const auto raw = std::chrono::system_clock::to_time_t(seconds);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &raw);
#else
    gmtime_r(&raw, &utc);
#endif
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

inline std::string utcTimestampFromMilliseconds(std::int64_t milliseconds) {
    return utcTimestamp(std::chrono::system_clock::time_point{
        std::chrono::milliseconds{milliseconds}});
}

inline std::string utcTimestampNow() {
    return utcTimestamp(std::chrono::system_clock::now());
}

} // namespace service::common
