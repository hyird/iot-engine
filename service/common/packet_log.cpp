#include "service/common/packet_log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <spdlog/async.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/spdlog.h>

namespace service::common::packet_log {
namespace {

struct State final {
    std::shared_ptr<spdlog::async_logger> logger;
};

std::shared_ptr<State> state;
inline constexpr std::size_t kQueueSize = 65536;
inline constexpr std::size_t kHexLimitBytes = 64U * 1024U;
inline constexpr std::uint16_t kRetentionDays = 14;

[[nodiscard]] std::string lower(std::string_view value) {
    std::string result(value);
    std::ranges::transform(result, result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

[[nodiscard]] spdlog::level::level_enum spdLevel(Level level) noexcept {
    switch (level) {
    case Level::Trace:
        return spdlog::level::trace;
    case Level::Debug:
        return spdlog::level::debug;
    case Level::Info:
        return spdlog::level::info;
    case Level::Warn:
        return spdlog::level::warn;
    case Level::Error:
        return spdlog::level::err;
    case Level::Off:
        return spdlog::level::off;
    }
    return spdlog::level::debug;
}

void appendEscaped(std::string& output, std::string_view value) {
    output.push_back('"');
    for (const unsigned char character : value) {
        switch (character) {
        case '\\':
            output += "\\\\";
            break;
        case '"':
            output += "\\\"";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            output.push_back(character < 0x20 ? '?' : static_cast<char>(character));
            break;
        }
    }
    output.push_back('"');
}

void appendField(std::string& output, std::string_view name, std::string_view value) {
    if (value.empty())
        return;
    output.push_back(' ');
    output.append(name);
    output.push_back('=');
    appendEscaped(output, value);
}

void appendInteger(std::string& output, std::string_view name, std::uint64_t value) {
    output.push_back(' ');
    output.append(name);
    output.push_back('=');
    output += std::to_string(value);
}

[[nodiscard]] std::string hex(std::span<const std::uint8_t> bytes, std::size_t limit) {
    static constexpr char digits[] = "0123456789ABCDEF";
    const auto count = std::min(bytes.size(), limit);
    std::string result;
    result.reserve(count == 0 ? 0 : count * 3 - 1);
    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0)
            result.push_back(' ');
        result.push_back(digits[bytes[index] >> 4U]);
        result.push_back(digits[bytes[index] & 0x0FU]);
    }
    return result;
}

} // namespace

Level parseLevel(std::string_view value, Level fallback) noexcept {
    const auto normalized = lower(value);
    if (normalized == "trace")
        return Level::Trace;
    if (normalized == "debug")
        return Level::Debug;
    if (normalized == "info")
        return Level::Info;
    if (normalized == "warn" || normalized == "warning")
        return Level::Warn;
    if (normalized == "error")
        return Level::Error;
    if (normalized == "off")
        return Level::Off;
    return fallback;
}

void initialize(Config config) {
    shutdown();
    if (config.level == Level::Off)
        return;
    if (config.directory.empty())
        throw std::invalid_argument("packet log directory is empty");

    std::filesystem::create_directories(config.directory);
    spdlog::init_thread_pool(kQueueSize, 1);
    auto sink = std::make_shared<spdlog::sinks::daily_file_sink_mt>(
        (config.directory / "packet.log").string(), 0, 0, false, kRetentionDays);
    auto logger = std::make_shared<spdlog::async_logger>(
        "iot-engine-packet", std::move(sink), spdlog::thread_pool(),
        spdlog::async_overflow_policy::overrun_oldest);
    logger->set_level(spdLevel(config.level));
    logger->flush_on(spdlog::level::warn);
    logger->set_formatter(std::make_unique<spdlog::pattern_formatter>(
        "%Y-%m-%dT%H:%M:%S.%eZ [%l] %v", spdlog::pattern_time_type::utc));
    spdlog::register_logger(logger);
    spdlog::flush_every(std::chrono::seconds(1));

    auto next = std::make_shared<State>();
    next->logger = std::move(logger);
    std::atomic_store_explicit(&state, std::move(next), std::memory_order_release);

    write(Level::Info, "PACKET_LOG_STARTED", {}, {}, "async_daily_14d_overrun_oldest");
}

void shutdown() noexcept {
    try {
        auto current = std::atomic_exchange_explicit(&state, std::shared_ptr<State>{},
                                                     std::memory_order_acq_rel);
        if (current && current->logger) {
            current->logger->flush();
            spdlog::drop(current->logger->name());
        }
        spdlog::shutdown();
    } catch (...) {
    }
}

void write(Level level, std::string_view event, const Context& context,
           std::span<const std::uint8_t> bytes, std::string_view reason) noexcept {
    try {
        const auto current = std::atomic_load_explicit(&state, std::memory_order_acquire);
        if (!current || !current->logger || !current->logger->should_log(spdLevel(level)))
            return;

        std::string line("event=");
        appendEscaped(line, event);
        appendField(line, "direction", context.direction);
        appendField(line, "operation", context.operation);
        appendField(line, "protocol", context.protocol);
        if (context.workerIndex != std::numeric_limits<std::size_t>::max())
            appendInteger(line, "worker_id", context.workerIndex);
        appendField(line, "link_id", context.linkId);
        appendField(line, "device_id", context.deviceId);
        appendField(line, "device_code", context.deviceCode);
        appendField(line, "connection_id", context.connectionId);
        if (context.sessionEpoch != 0)
            appendInteger(line, "session_epoch", context.sessionEpoch);
        appendField(line, "remote", context.remoteAddress);
        appendField(line, "message_id", context.messageId);
        appendField(line, "causation_id", context.causationId);
        appendField(line, "reason", reason);
        if (!bytes.empty()) {
            appendInteger(line, "bytes", bytes.size());
            const auto encoded = hex(bytes, kHexLimitBytes);
            appendField(line, "hex", encoded);
            if (bytes.size() > kHexLimitBytes)
                appendInteger(line, "truncated_bytes", bytes.size() - kHexLimitBytes);
        }
        current->logger->log(spdLevel(level), "{}", line);
    } catch (...) {
    }
}

} // namespace service::common::packet_log
