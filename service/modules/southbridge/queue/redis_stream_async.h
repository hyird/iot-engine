#pragma once

// 基于 ruvia RedisHandle 的南桥 Redis Stream helper。消费者和生产者都在同一个
// worker 上通过 co_await 调用，不创建额外同步连接。

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/web/redis/RedisHandle.h>
#include <ruvia/web/redis/RedisTypes.h>

#include "service/common/bridge/message.contract.h"

namespace service::bridge::redis_async {

using ruvia::RedisHandle;
using ruvia::RedisValue;

// ---- 通用命令封装：把 std::string 参数列表转成 string_view span 后 co_await ----

template <typename Redis>
inline ruvia::Task<RedisValue> command(const Redis& redis,
                                       const std::vector<std::string>& args) {
    std::vector<std::string_view> views;
    views.reserve(args.size());
    for (const auto& arg : args)
        views.emplace_back(arg);
    co_return co_await redis.command(std::span<const std::string_view>(views));
}

[[noreturn]] inline void throwValue(std::string_view operation, const RedisValue& value) {
    std::string message(operation);
    message += " failed";
    if (value.kind() == RedisValue::Kind::kError) {
        message += ": ";
        message.append(value.string());
    }
    throw std::runtime_error(message);
}

// ---- Stream 写入 ----

template <typename Redis>
inline ruvia::Task<std::string> add(const Redis& redis, std::string_view stream,
                                    const std::vector<StreamField>& fields,
                                    std::size_t maxLength = 100000) {
    if (fields.empty())
        throw std::invalid_argument("Redis Stream message must contain fields");
    std::vector<std::string> args{"XADD", std::string(stream),       "MAXLEN",
                                  "~",    std::to_string(maxLength), "*"};
    args.reserve(args.size() + fields.size() * 2);
    for (const auto& field : fields) {
        args.push_back(field.name);
        args.push_back(field.value);
    }
    const auto reply = co_await command(redis, args);
    if (reply.kind() != RedisValue::Kind::kString)
        throwValue("XADD", reply);
    co_return std::string(reply.string());
}

template <typename Redis>
inline ruvia::Task<std::optional<std::string>>
addGroupedBounded(const Redis& redis, std::string_view stream,
                  const std::vector<StreamField>& fields, std::size_t streamCapacity,
                  std::string_view depthKey, std::size_t groupCapacity) {
    if (fields.empty() || depthKey.empty())
        throw std::invalid_argument("Redis grouped Stream message is incomplete");
    static constexpr std::string_view script = R"lua(
if redis.call('XLEN', KEYS[1]) >= tonumber(ARGV[1]) then
  return false
end
local depth = tonumber(redis.call('GET', KEYS[2]) or '0')
if depth >= tonumber(ARGV[2]) then
  return false
end
local args = {'*'}
for index = 3, #ARGV do
  args[#args + 1] = ARGV[index]
end
local id = redis.call('XADD', KEYS[1], unpack(args))
redis.call('INCR', KEYS[2])
return id
)lua";
    std::vector<std::string> keyStore{std::string(stream), std::string(depthKey)};
    std::vector<std::string> argStore{std::to_string(streamCapacity),
                                      std::to_string(groupCapacity)};
    for (const auto& field : fields) {
        argStore.push_back(field.name);
        argStore.push_back(field.value);
    }
    std::vector<std::string_view> keys(keyStore.begin(), keyStore.end());
    std::vector<std::string_view> argv(argStore.begin(), argStore.end());
    const auto reply = co_await redis.eval(script, std::span<const std::string_view>(keys),
                                           std::span<const std::string_view>(argv));
    if (reply.null())
        co_return std::nullopt;
    if (reply.kind() != RedisValue::Kind::kString)
        throwValue("grouped bounded XADD", reply);
    co_return std::optional<std::string>(std::string(reply.string()));
}

// ---- 消费组 / 读取 ----

template <typename Redis>
inline ruvia::Task<void> ensureGroup(const Redis& redis, std::string_view stream,
                                     std::string_view group) {
    const auto reply = co_await command(
        redis, {"XGROUP", "CREATE", std::string(stream), std::string(group), "0", "MKSTREAM"});
    if (reply.kind() == RedisValue::Kind::kError && !reply.string().starts_with("BUSYGROUP"))
        throwValue("XGROUP CREATE", reply);
}

template <typename Redis>
inline ruvia::Task<std::vector<StreamMessage>>
readGroup(const Redis& redis, std::string_view stream, std::string_view group,
          std::string_view consumer, std::string_view id, std::chrono::milliseconds block,
          std::size_t count = 100) {
    std::vector<std::string> args{"XREADGROUP",          "GROUP", std::string(group),
                                  std::string(consumer), "COUNT", std::to_string(count)};
    if (block.count() > 0) {
        args.emplace_back("BLOCK");
        args.push_back(std::to_string(block.count()));
    }
    args.insert(args.end(), {"STREAMS", std::string(stream), std::string(id)});
    const auto reply = co_await command(redis, args);
    std::vector<StreamMessage> messages;
    if (reply.null())
        co_return messages;
    if (reply.kind() != RedisValue::Kind::kArray)
        throwValue("XREADGROUP", reply);
    for (const auto& streamReply : reply.array()) {
        if (streamReply.kind() != RedisValue::Kind::kArray || streamReply.array().size() != 2)
            continue;
        const auto& entries = streamReply.array()[1];
        if (entries.kind() != RedisValue::Kind::kArray)
            continue;
        for (const auto& entry : entries.array()) {
            if (entry.kind() != RedisValue::Kind::kArray || entry.array().size() != 2)
                continue;
            const auto& entryId = entry.array()[0];
            const auto& fieldArray = entry.array()[1];
            if (entryId.kind() != RedisValue::Kind::kString ||
                fieldArray.kind() != RedisValue::Kind::kArray)
                continue;
            StreamMessage message;
            message.id.assign(entryId.string());
            const auto fieldValues = fieldArray.array();
            for (std::size_t index = 0; index + 1 < fieldValues.size(); index += 2) {
                const auto& name = fieldValues[index];
                const auto& value = fieldValues[index + 1];
                if (name.kind() != RedisValue::Kind::kString ||
                    value.kind() != RedisValue::Kind::kString)
                    continue;
                message.fields.push_back({std::string(name.string()), std::string(value.string())});
            }
            messages.push_back(std::move(message));
        }
    }
    co_return messages;
}

// ---- ACK / 删除 ----

template <typename Redis>
inline ruvia::Task<void> acknowledge(const Redis& redis, std::string_view stream,
                                     std::string_view group, std::string_view id) {
    const auto reply =
        co_await command(redis, {"XACK", std::string(stream), std::string(group), std::string(id)});
    if (reply.kind() != RedisValue::Kind::kInteger)
        throwValue("XACK", reply);
}

template <typename Redis>
inline ruvia::Task<void> acknowledgeAndDelete(const Redis& redis, std::string_view stream,
                                              std::string_view group, std::string_view id) {
    co_await acknowledge(redis, stream, group, id);
    const auto reply = co_await command(redis, {"XDEL", std::string(stream), std::string(id)});
    if (reply.kind() != RedisValue::Kind::kInteger)
        throwValue("XDEL", reply);
}

template <typename Redis>
inline ruvia::Task<void> acknowledgeGroupedAndDelete(const Redis& redis,
                                                     std::string_view stream,
                                                     std::string_view consumerGroup,
                                                     std::string_view id,
                                                     std::string_view depthKey) {
    static constexpr std::string_view script = R"lua(
local acknowledged = redis.call('XACK', KEYS[1], ARGV[1], ARGV[2])
redis.call('XDEL', KEYS[1], ARGV[2])
local depth = redis.call('DECR', KEYS[2])
if depth <= 0 then
  redis.call('DEL', KEYS[2])
end
return acknowledged
)lua";
    const std::string streamStr(stream);
    const std::string depthStr(depthKey);
    const std::string groupStr(consumerGroup);
    const std::string idStr(id);
    const std::string_view keys[]{streamStr, depthStr};
    const std::string_view argv[]{groupStr, idStr};
    const auto reply = co_await redis.eval(script, std::span<const std::string_view>(keys),
                                           std::span<const std::string_view>(argv));
    if (reply.kind() != RedisValue::Kind::kInteger)
        throwValue("grouped XACK/XDEL", reply);
}

// ---- Hash 状态 ----

template <typename Redis>
inline ruvia::Task<void> setHash(const Redis& redis, std::string_view key,
                                 const std::vector<StreamField>& fields) {
    if (fields.empty())
        co_return;
    std::vector<std::string> args{"HSET", std::string(key)};
    for (const auto& field : fields) {
        args.push_back(field.name);
        args.push_back(field.value);
    }
    const auto reply = co_await command(redis, args);
    if (reply.kind() != RedisValue::Kind::kInteger)
        throwValue("HSET", reply);
}

template <typename Redis>
inline ruvia::Task<void> eraseHash(const Redis& redis, std::string_view key) {
    const auto reply = co_await command(redis, {"DEL", std::string(key)});
    if (reply.kind() != RedisValue::Kind::kInteger)
        throwValue("DEL Hash", reply);
}

template <typename Redis>
inline ruvia::Task<std::vector<StreamField>> hashEntries(const Redis& redis,
                                                         std::string_view key) {
    const auto entries = co_await command(redis, {"HGETALL", std::string(key)});
    if (entries.kind() != RedisValue::Kind::kArray)
        throwValue("HGETALL", entries);
    std::vector<StreamField> result;
    const auto values = entries.array();
    result.reserve(values.size() / 2);
    for (std::size_t index = 0; index + 1 < values.size(); index += 2) {
        if (values[index].kind() != RedisValue::Kind::kString ||
            values[index + 1].kind() != RedisValue::Kind::kString)
            throwValue("HGETALL fields", entries);
        result.push_back(
            {std::string(values[index].string()), std::string(values[index + 1].string())});
    }
    co_return result;
}

template <typename Redis>
inline ruvia::Task<bool> claimHash(const Redis& redis, std::string_view key,
                                   const std::vector<StreamField>& fields,
                                   std::chrono::milliseconds ttl) {
    if (fields.empty())
        throw std::invalid_argument("Redis state Hash fields are empty");
    static constexpr std::string_view script = R"lua(
if redis.call('EXISTS', KEYS[1]) ~= 0 then return 0 end
local args = {}
for index = 2, #ARGV do args[#args + 1] = ARGV[index] end
redis.call('HSET', KEYS[1], unpack(args))
redis.call('PEXPIRE', KEYS[1], ARGV[1])
return 1
)lua";
    const std::string keyStr(key);
    std::vector<std::string> argStore{std::to_string(ttl.count())};
    for (const auto& field : fields) {
        argStore.push_back(field.name);
        argStore.push_back(field.value);
    }
    const std::string_view keys[]{keyStr};
    std::vector<std::string_view> argv(argStore.begin(), argStore.end());
    const auto reply = co_await redis.eval(script, std::span<const std::string_view>(keys),
                                           std::span<const std::string_view>(argv));
    if (reply.kind() != RedisValue::Kind::kInteger)
        throwValue("claim state Hash", reply);
    co_return reply.integer() == 1;
}

template <typename Redis>
inline ruvia::Task<bool> eraseHashIfFieldValue(const Redis& redis, std::string_view key,
                                               std::string_view field, std::string_view expected) {
    static constexpr std::string_view script = R"lua(
if redis.call('HGET', KEYS[1], ARGV[1]) ~= ARGV[2] then return 0 end
return redis.call('DEL', KEYS[1])
)lua";
    const std::string keyStr(key);
    const std::string fieldStr(field);
    const std::string expectedStr(expected);
    const std::string_view keys[]{keyStr};
    const std::string_view argv[]{fieldStr, expectedStr};
    const auto reply = co_await redis.eval(script, std::span<const std::string_view>(keys),
                                           std::span<const std::string_view>(argv));
    if (reply.kind() != RedisValue::Kind::kInteger)
        throwValue("conditional state DEL", reply);
    co_return reply.integer() == 1;
}

template <typename Redis>
inline ruvia::Task<bool> completeInflightTask(const Redis& redis, std::string_view stream,
                                              std::string_view consumerGroup, std::string_view id,
                                              std::string_view depthKey,
                                              std::string_view inflightKey,
                                              std::string_view expectedToken) {
    static constexpr std::string_view script = R"lua(
if redis.call('HGET', KEYS[3], 'token') ~= ARGV[3] then
  return 0
end
redis.call('XACK', KEYS[1], ARGV[1], ARGV[2])
redis.call('XDEL', KEYS[1], ARGV[2])
redis.call('DEL', KEYS[3])
local depth = redis.call('DECR', KEYS[2])
if depth <= 0 then
  redis.call('DEL', KEYS[2])
end
return 1
)lua";
    const std::string streamStr(stream);
    const std::string depthStr(depthKey);
    const std::string inflightStr(inflightKey);
    const std::string groupStr(consumerGroup);
    const std::string idStr(id);
    const std::string tokenStr(expectedToken);
    const std::string_view keys[]{streamStr, depthStr, inflightStr};
    const std::string_view argv[]{groupStr, idStr, tokenStr};
    const auto reply = co_await redis.eval(script, std::span<const std::string_view>(keys),
                                           std::span<const std::string_view>(argv));
    if (reply.kind() != RedisValue::Kind::kInteger)
        throwValue("complete inflight task", reply);
    co_return reply.integer() == 1;
}

// ---- SCAN ----

template <typename Redis>
inline ruvia::Task<std::vector<std::string>> keysMatching(const Redis& redis,
                                                          std::string_view pattern) {
    std::vector<std::string> result;
    std::string cursor = "0";
    do {
        const auto page = co_await command(
            redis, {"SCAN", cursor, "MATCH", std::string(pattern), "COUNT", "100"});
        if (page.kind() != RedisValue::Kind::kArray || page.array().size() != 2 ||
            page.array()[0].kind() != RedisValue::Kind::kString ||
            page.array()[1].kind() != RedisValue::Kind::kArray)
            throwValue("SCAN", page);
        cursor.assign(page.array()[0].string());
        for (const auto& value : page.array()[1].array()) {
            if (value.kind() != RedisValue::Kind::kString)
                throwValue("SCAN key", page);
            result.emplace_back(value.string());
        }
    } while (cursor != "0");
    co_return result;
}

template <typename Redis>
inline ruvia::Task<void> eraseMatching(const Redis& redis, std::string_view pattern) {
    for (const auto& key : co_await keysMatching(redis, pattern))
        (void)co_await command(redis, {"DEL", key});
}

template <typename Redis>
inline ruvia::Task<void> eraseMatchingIfFieldValue(const Redis& redis,
                                                   std::string_view pattern,
                                                   std::string_view field,
                                                   std::string_view expected) {
    for (const auto& key : co_await keysMatching(redis, pattern))
        (void)co_await eraseHashIfFieldValue(redis, key, field, expected);
}

// ---- 计数 / 删除 ----

template <typename Redis>
inline ruvia::Task<std::int64_t> incrementWithExpiry(const Redis& redis, std::string_view key,
                                                     std::chrono::milliseconds expiry) {
    const auto incrementReply = co_await command(redis, {"INCR", std::string(key)});
    if (incrementReply.kind() != RedisValue::Kind::kInteger)
        throwValue("INCR", incrementReply);
    const auto reply =
        co_await command(redis, {"PEXPIRE", std::string(key), std::to_string(expiry.count())});
    if (reply.kind() != RedisValue::Kind::kInteger)
        throwValue("PEXPIRE", reply);
    co_return incrementReply.integer();
}

template <typename Redis>
inline ruvia::Task<void> erase(const Redis& redis, std::string_view key) {
    const auto reply = co_await command(redis, {"DEL", std::string(key)});
    if (reply.kind() != RedisValue::Kind::kInteger)
        throwValue("DEL", reply);
}

// ---- 生产者语义 ----

template <typename Redis>
inline ruvia::Task<std::string> publish(const Redis& redis, std::string_view stream,
                                        const std::vector<StreamField>& fields,
                                        std::size_t maxLength = 0) {
    if (maxLength == 0)
        maxLength = 10000;
    co_return co_await add(redis, stream, fields, maxLength);
}

template <typename Redis>
inline ruvia::Task<void> updateSession(const Redis& redis, std::string_view connectionId,
                                       const std::vector<StreamField>& fields) {
    const auto key = sessionStateKey(connectionId);
    co_await eraseHash(redis, key);
    co_await setHash(redis, key, fields);
}

template <typename Redis>
inline ruvia::Task<void> removeSession(const Redis& redis, std::string_view connectionId) {
    co_await eraseHash(redis, sessionStateKey(connectionId));
}

template <typename Redis>
inline ruvia::Task<void> clearSessions(const Redis& redis) {
    co_await eraseMatching(redis, std::string(kSessionStatePrefix) + '*');
}

template <typename Redis>
inline ruvia::Task<void> writeLinkStatus(const Redis& redis, std::size_t workerIndex,
                                         std::string_view linkId,
                                         const std::vector<StreamField>& fields,
                                         bool publishEvent = true) {
    const auto id = std::string(linkId);
    const auto key =
        "iot:state:link:" + id + ":worker:" + std::to_string(workerIndex);
    co_await eraseHash(redis, key);
    co_await setHash(redis, key, fields);
    if (publishEvent) {
        auto event = fields;
        event.insert(event.begin(), {"link_id", id});
        event.insert(event.begin(), {"message_id", nextMessageId()});
        event.push_back({"worker_id", std::to_string(workerIndex)});
        (void)co_await add(redis, linkEventStream(workerIndex), event, 500);
    }
}

template <typename Redis>
inline ruvia::Task<void> removeLinkStatus(const Redis& redis, std::size_t workerIndex,
                                          std::string_view linkId) {
    const auto id = std::string(linkId);
    co_await eraseHash(redis, "iot:state:link:" + id +
                                  ":worker:" + std::to_string(workerIndex));
    (void)co_await add(redis, linkEventStream(workerIndex),
                       {{"message_id", nextMessageId()},
                        {"link_id", id},
                        {"worker_id", std::to_string(workerIndex)},
                        {"state", "removed"},
                        {"connection_count", "0"},
                        {"updated_at_ms", std::to_string(utcNowMilliseconds())}},
                       500);
}

} // namespace service::bridge::redis_async
