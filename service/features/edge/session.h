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
#include "service/common/instance.h"

namespace service::edge::session_state {

inline std::string key(std::string_view nodeId) {
    return "iot:edge:session:" + std::string(nodeId);
}

struct State final {
    std::uint64_t epoch{};
    std::uint32_t protocolVersion{};
    std::size_t workerIndex{};
    std::string instanceId;
};

inline std::string value(std::uint64_t epoch, std::uint32_t protocolVersion,
                         std::size_t workerIndex,
                         std::string_view instance = service::runtime::instanceId()) {
    return std::to_string(epoch) + "|" + std::to_string(protocolVersion) + "|" +
           std::to_string(workerIndex) + "|" + std::string(instance);
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
    const auto thirdSeparator = state.find('|',secondSeparator+1);
    if (thirdSeparator == std::string_view::npos || thirdSeparator+1 >= state.size())
        return std::nullopt;
    State result;
    result.instanceId = state.substr(thirdSeparator+1);
    if (!service::common::isUuid(result.instanceId)) return std::nullopt;
    if (!parseInteger(state.substr(0, firstSeparator), result.epoch) ||
        !parseInteger(state.substr(firstSeparator + 1,
                                   secondSeparator - firstSeparator - 1),
                      result.protocolVersion) ||
        !parseInteger(state.substr(secondSeparator + 1, thirdSeparator-secondSeparator-1), result.workerIndex))
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

inline constexpr std::string_view kDeadlines = "iot:edge:session-deadlines";
inline constexpr std::string_view kChanges = "iot:live:changes";
inline constexpr std::string_view kClaimScript = R"lua(
local now=redis.call('TIME')
redis.call('SET',KEYS[1],ARGV[1],'EX',90)
redis.call('ZADD',KEYS[2],now[1]*1000+math.floor(now[2]/1000)+90000,KEYS[1])
redis.call('XADD',KEYS[3],'MAXLEN','~',100000,'*','topic','edge')
return 1
)lua";
inline constexpr std::string_view kRefreshScript = R"lua(
if redis.call('GET',KEYS[1]) ~= ARGV[1] then return 0 end
local now=redis.call('TIME')
redis.call('EXPIRE',KEYS[1],90)
redis.call('ZADD',KEYS[2],now[1]*1000+math.floor(now[2]/1000)+90000,KEYS[1])
return 1
)lua";
inline constexpr std::string_view kReleaseScript = R"lua(
if redis.call('GET',KEYS[1]) ~= ARGV[1] then return 0 end
redis.call('DEL',KEYS[1])
redis.call('ZREM',KEYS[2],KEYS[1])
redis.call('XADD',KEYS[3],'MAXLEN','~',100000,'*','topic','edge')
return 1
)lua";
// Expiration is a state transition too, even when the owning API process died.
// All instances may service this deadline queue; the atomic removal emits once.
inline constexpr std::string_view kExpireScript = R"lua(
local time=redis.call('TIME')
local now=time[1]*1000+math.floor(time[2]/1000)
local due=redis.call('ZRANGEBYSCORE',KEYS[1],'-inf',now,'LIMIT',0,256)
local changed=0
for _,key in ipairs(due) do
 local ttl=redis.call('PTTL',key)
 if ttl == -2 then
  redis.call('ZREM',KEYS[1],key)
  changed=changed+1
 elseif ttl >= 0 then
  redis.call('ZADD',KEYS[1],now+ttl+1,key)
 else
  redis.call('ZREM',KEYS[1],key)
 end
end
if changed > 0 then redis.call('XADD',KEYS[2],'MAXLEN','~',100000,'*','topic','edge') end
return changed
)lua";
template <typename Redis>
ruvia::Task<bool> mutate(const Redis& redis, std::string_view script,
    std::string_view nodeId, std::uint64_t epoch, std::uint32_t protocolVersion,
    std::size_t workerIndex) {
    const auto sessionKey=key(nodeId);
    const auto expected=value(epoch,protocolVersion,workerIndex);
    const std::string_view keys[]{sessionKey,kDeadlines,kChanges};
    const std::string_view args[]{expected};
    const auto reply=co_await redis.eval(script,keys,args);
    if(reply.kind()!=ruvia::RedisValue::Kind::kInteger)
        service::message::redis::throwValue("edge session transition",reply);
    co_return reply.integer()==1;
}
template <typename Redis>
ruvia::Task<bool> claim(const Redis& redis, std::string_view nodeId, std::uint64_t epoch,
                        std::uint32_t protocolVersion, std::size_t workerIndex) {
    co_return co_await mutate(redis,kClaimScript,nodeId,epoch,protocolVersion,workerIndex);
}
template <typename Redis>
ruvia::Task<bool> refresh(const Redis& redis, std::string_view nodeId, std::uint64_t epoch,
                          std::uint32_t protocolVersion, std::size_t workerIndex) {
    co_return co_await mutate(redis,kRefreshScript,nodeId,epoch,protocolVersion,workerIndex);
}
template <typename Redis>
ruvia::Task<bool> release(const Redis& redis, std::string_view nodeId, std::uint64_t epoch,
                          std::uint32_t protocolVersion, std::size_t workerIndex) {
    co_return co_await mutate(redis,kReleaseScript,nodeId,epoch,protocolVersion,workerIndex);
}

} // namespace service::edge::session_state
