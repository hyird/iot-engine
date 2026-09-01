#pragma once

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <ruvia/core/Task.h>
#include <ruvia/web/redis/RedisTypes.h>

#include "service/features/collector/stream.h"

namespace service::edge::session_state {

inline std::string key(std::string_view nodeId) {
    return "iot:edge:session:" + std::string(nodeId);
}

struct State final {
    std::uint64_t epoch{};
    std::uint32_t protocolVersion{};
    std::size_t workerIndex{};
};

inline std::string value(std::uint64_t epoch, std::uint32_t protocolVersion,
                         std::size_t workerIndex) {
    return std::to_string(epoch) + "|" + std::to_string(protocolVersion) + "|" +
           std::to_string(workerIndex);
}

template <typename Integer>
inline bool parseInteger(std::string_view value, Integer& result) {
    if (value.empty())
        return false;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}

inline std::optional<State> parse(std::string_view state) {
    const auto firstSeparator = state.find('|');
    const auto secondSeparator = firstSeparator == std::string_view::npos
                                     ? std::string_view::npos
                                     : state.find('|', firstSeparator + 1);
    if (firstSeparator == std::string_view::npos || firstSeparator == 0 ||
        secondSeparator == std::string_view::npos ||
        secondSeparator == firstSeparator + 1 || secondSeparator + 1 >= state.size())
        return std::nullopt;
    State result;
    if (!parseInteger(state.substr(0, firstSeparator), result.epoch) ||
        !parseInteger(state.substr(firstSeparator + 1,
                                   secondSeparator - firstSeparator - 1),
                      result.protocolVersion) ||
        !parseInteger(state.substr(secondSeparator + 1), result.workerIndex))
        return std::nullopt;
    return result;
}

inline std::optional<std::uint32_t> protocolVersion(std::string_view state) {
    const auto parsed = parse(state);
    return parsed ? std::optional<std::uint32_t>(parsed->protocolVersion) : std::nullopt;
}

inline std::optional<std::size_t> workerIndex(std::string_view state) {
    const auto parsed = parse(state);
    return parsed ? std::optional<std::size_t>(parsed->workerIndex) : std::nullopt;
}

template <typename Redis>
ruvia::Task<bool> claim(const Redis& redis, std::string_view nodeId, std::uint64_t epoch,
                        std::uint32_t protocolVersion, std::size_t workerIndex) {
    ruvia::RedisSetOptions options;
    options.expiration =
        ruvia::RedisSetExpiration::expiresAfter(std::chrono::seconds(90));
    co_await redis.set(key(nodeId), value(epoch, protocolVersion, workerIndex),
                       std::move(options));
    co_return true;
}

template <typename Redis>
ruvia::Task<bool> refresh(const Redis& redis, std::string_view nodeId, std::uint64_t epoch,
                          std::uint32_t protocolVersion, std::size_t workerIndex) {
    static constexpr std::string_view script = R"lua(
if redis.call('GET', KEYS[1]) ~= ARGV[1] then return 0 end
redis.call('EXPIRE', KEYS[1], ARGV[2])
return 1
)lua";
    const auto sessionKey = key(nodeId);
    const auto expected = value(epoch, protocolVersion, workerIndex);
    const std::string ttl = "90";
    const std::string_view keys[]{sessionKey};
    const std::string_view arguments[]{expected, ttl};
    const auto reply = co_await redis.eval(script, keys, arguments);
    if (reply.kind() != ruvia::RedisValue::Kind::kInteger)
        service::message::redis::throwValue("refresh edge session", reply);
    co_return reply.integer() == 1;
}

template <typename Redis>
ruvia::Task<bool> release(const Redis& redis, std::string_view nodeId, std::uint64_t epoch,
                          std::uint32_t protocolVersion, std::size_t workerIndex) {
    static constexpr std::string_view script = R"lua(
if redis.call('GET', KEYS[1]) ~= ARGV[1] then return 0 end
redis.call('DEL', KEYS[1])
return 1
)lua";
    const auto sessionKey = key(nodeId);
    const auto expected = value(epoch, protocolVersion, workerIndex);
    const std::string_view keys[]{sessionKey};
    const std::string_view arguments[]{expected};
    const auto reply = co_await redis.eval(script, keys, arguments);
    if (reply.kind() != ruvia::RedisValue::Kind::kInteger)
        service::message::redis::throwValue("release edge session", reply);
    co_return reply.integer() == 1;
}

} // namespace service::edge::session_state
