#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <charconv>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

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

// Parses RFC 3339 and the GB28181 date-time form without a zone. A missing
// zone is interpreted using defaultOffsetMinutes instead of the host timezone,
// so protocol input is deterministic on every deployment host.
inline std::optional<std::chrono::system_clock::time_point>
parseUtcTimestamp(std::string_view input, int defaultOffsetMinutes = 0) {
    if (input.size() < 19 || input[4] != '-' || input[7] != '-' ||
        (input[10] != 'T' && input[10] != ' ') || input[13] != ':' ||
        input[16] != ':')
        return std::nullopt;
    const auto number = [&](std::size_t offset,
                            std::size_t length) -> std::optional<int> {
        int value{};
        const auto parsed =
            std::from_chars(input.data() + offset,
                            input.data() + offset + length, value);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != input.data() + offset + length)
            return std::nullopt;
        return value;
    };
    const auto year = number(0, 4);
    const auto month = number(5, 2);
    const auto day = number(8, 2);
    const auto hour = number(11, 2);
    const auto minute = number(14, 2);
    const auto second = number(17, 2);
    if (!year || !month || !day || !hour || !minute || !second ||
        *month < 1 || *month > 12 || *day < 1 || *day > 31 ||
        *hour > 23 || *minute > 59 || *second > 59)
        return std::nullopt;

    std::size_t position = 19;
    if (position < input.size() && input[position] == '.') {
        ++position;
        const auto fractionalStart = position;
        while (position < input.size() && input[position] >= '0' &&
               input[position] <= '9')
            ++position;
        if (position == fractionalStart)
            return std::nullopt;
    }

    int offsetMinutes = defaultOffsetMinutes;
    if (position < input.size()) {
        if (input[position] == 'Z' || input[position] == 'z') {
            offsetMinutes = 0;
            ++position;
        } else if (input[position] == '+' || input[position] == '-') {
            const auto sign = input[position] == '+' ? 1 : -1;
            ++position;
            if (position + 2 > input.size())
                return std::nullopt;
            int zoneHour{};
            const auto parsedHour =
                std::from_chars(input.data() + position,
                                input.data() + position + 2, zoneHour);
            if (parsedHour.ec != std::errc{})
                return std::nullopt;
            position += 2;
            int zoneMinute{};
            if (position < input.size() && input[position] == ':')
                ++position;
            if (position + 2 <= input.size()) {
                const auto parsedMinute =
                    std::from_chars(input.data() + position,
                                    input.data() + position + 2, zoneMinute);
                if (parsedMinute.ec != std::errc{})
                    return std::nullopt;
                position += 2;
            }
            if (zoneHour > 23 || zoneMinute > 59)
                return std::nullopt;
            offsetMinutes = sign * (zoneHour * 60 + zoneMinute);
        } else {
            return std::nullopt;
        }
    }
    if (position != input.size())
        return std::nullopt;

    std::tm fields{};
    fields.tm_year = *year - 1900;
    fields.tm_mon = *month - 1;
    fields.tm_mday = *day;
    fields.tm_hour = *hour;
    fields.tm_min = *minute;
    fields.tm_sec = *second;
#ifdef _WIN32
    const auto seconds = _mkgmtime(&fields);
#else
    const auto seconds = timegm(&fields);
#endif
    if (seconds == static_cast<std::time_t>(-1))
        return std::nullopt;
    // mktime-family functions normalize invalid calendar input (for example
    // 2026-02-31). Reject it instead of silently changing the protocol fact.
    if (fields.tm_year != *year - 1900 || fields.tm_mon != *month - 1 ||
        fields.tm_mday != *day || fields.tm_hour != *hour ||
        fields.tm_min != *minute || fields.tm_sec != *second)
        return std::nullopt;
    return std::chrono::system_clock::from_time_t(seconds) -
           std::chrono::minutes(offsetMinutes);
}

inline std::string canonicalUtcTimestamp(std::string_view input,
                                         int defaultOffsetMinutes = 0) {
    const auto parsed = parseUtcTimestamp(input, defaultOffsetMinutes);
    return parsed ? utcTimestamp(*parsed) : std::string{};
}

} // namespace service::common
