#pragma once

#include <chrono>
#include <cstdint>
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

template <typename Redis>
ruvia::Task<bool> claim(const Redis& redis, std::string_view nodeId, std::uint64_t epoch) {
    ruvia::RedisSetOptions options;
    options.expiration =
        ruvia::RedisSetExpiration::expiresAfter(std::chrono::seconds(90));
    co_await redis.set(key(nodeId), std::to_string(epoch), std::move(options));
    co_return true;
}

template <typename Redis>
ruvia::Task<bool> refresh(const Redis& redis, std::string_view nodeId, std::uint64_t epoch) {
    static constexpr std::string_view script = R"lua(
if redis.call('GET', KEYS[1]) ~= ARGV[1] then return 0 end
redis.call('EXPIRE', KEYS[1], ARGV[2])
return 1
)lua";
    const auto sessionKey = key(nodeId);
    const auto expected = std::to_string(epoch);
    const std::string ttl = "90";
    const std::string_view keys[]{sessionKey};
    const std::string_view arguments[]{expected, ttl};
    const auto reply = co_await redis.eval(script, keys, arguments);
    if (reply.kind() != ruvia::RedisValue::Kind::kInteger)
        service::message::redis::throwValue("refresh edge session", reply);
    co_return reply.integer() == 1;
}

template <typename Redis>
ruvia::Task<bool> release(const Redis& redis, std::string_view nodeId, std::uint64_t epoch) {
    static constexpr std::string_view script = R"lua(
if redis.call('GET', KEYS[1]) ~= ARGV[1] then return 0 end
redis.call('DEL', KEYS[1])
return 1
)lua";
    const auto sessionKey = key(nodeId);
    const auto expected = std::to_string(epoch);
    const std::string_view keys[]{sessionKey};
    const std::string_view arguments[]{expected};
    const auto reply = co_await redis.eval(script, keys, arguments);
    if (reply.kind() != ruvia::RedisValue::Kind::kInteger)
        service::message::redis::throwValue("release edge session", reply);
    co_return reply.integer() == 1;
}

} // namespace service::edge::session_state
