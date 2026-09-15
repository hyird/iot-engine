#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include "service/features/packet_log/packet_log.types.h"

namespace service::packet_log {

struct Config final {
    Level level = Level::Debug;
    std::filesystem::path directory;
};

inline Level parseLevel(std::string_view value, Level fallback = Level::Debug) noexcept {
    std::string normalized(value);
    std::ranges::transform(normalized, normalized.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (normalized == "trace") {
        return Level::Trace;
    }
    if (normalized == "debug") {
        return Level::Debug;
    }
    if (normalized == "info") {
        return Level::Info;
    }
    if (normalized == "warn" || normalized == "warning") {
        return Level::Warn;
    }
    if (normalized == "error") {
        return Level::Error;
    }
    if (normalized == "off") {
        return Level::Off;
    }
    return fallback;
}

} // namespace service::packet_log
