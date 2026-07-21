#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <hiredis/hiredis.h>

#include "service/common/bridge/message.contract.h"

namespace service::bridge {

struct RedisEndpoint {
    std::string host = "127.0.0.1";
    std::uint16_t port = 6379;
    std::string username;
    std::string password;
    std::uint32_t database = 0;
    std::chrono::milliseconds connectTimeout{2000};
};

class RedisStreamConnection {
  public:
    explicit RedisStreamConnection(RedisEndpoint endpoint) : endpoint_(std::move(endpoint)) {}

    RedisStreamConnection(const RedisStreamConnection&) = delete;
    RedisStreamConnection& operator=(const RedisStreamConnection&) = delete;

    ~RedisStreamConnection() { disconnect(); }

    void connect() {
        disconnect();
        timeval timeout{};
        timeout.tv_sec = static_cast<long>(endpoint_.connectTimeout.count() / 1000);
        timeout.tv_usec = static_cast<long>((endpoint_.connectTimeout.count() % 1000) * 1000);
        context_ = redisConnectWithTimeout(endpoint_.host.c_str(), endpoint_.port, timeout);
        if (!context_)
            throw std::runtime_error("Redis connection allocation failed");
        if (context_->err != 0) {
            const std::string message = context_->errstr;
            disconnect();
            throw std::runtime_error("Redis connection failed: " + message);
        }
        if (!endpoint_.password.empty()) {
            std::vector<std::string> auth{"AUTH"};
            if (!endpoint_.username.empty())
                auth.push_back(endpoint_.username);
            auth.push_back(endpoint_.password);
            ensureStatus(command(auth), "AUTH");
        }
        if (endpoint_.database != 0)
            ensureStatus(command({"SELECT", std::to_string(endpoint_.database)}), "SELECT");
    }

    [[nodiscard]] bool connected() const noexcept {
        return context_ != nullptr && context_->err == 0;
    }

    std::string add(std::string_view stream, const std::vector<StreamField>& fields,
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
        const auto reply = command(args);
        if (reply->type != REDIS_REPLY_STRING)
            throwReply("XADD", reply.get());
        return std::string(reply->str, reply->len);
    }

    std::optional<std::string> addGroupedBounded(std::string_view stream,
                                                 const std::vector<StreamField>& fields,
                                                 std::size_t streamCapacity,
                                                 std::string_view depthKey,
                                                 std::size_t groupCapacity) {
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
        std::vector<std::string> args{"EVAL",
                                      std::string(script),
                                      "2",
                                      std::string(stream),
                                      std::string(depthKey),
                                      std::to_string(streamCapacity),
                                      std::to_string(groupCapacity)};
        args.reserve(args.size() + fields.size() * 2);
        for (const auto& field : fields) {
            args.push_back(field.name);
            args.push_back(field.value);
        }
        const auto reply = command(args, true);
        if (reply->type == REDIS_REPLY_NIL)
            return std::nullopt;
        if (reply->type != REDIS_REPLY_STRING)
            throwReply("grouped bounded XADD", reply.get());
        return std::string(reply->str, reply->len);
    }

    void acknowledgeGroupedAndDelete(std::string_view stream, std::string_view consumerGroup,
                                     std::string_view id, std::string_view depthKey) {
        static constexpr std::string_view script = R"lua(
local acknowledged = redis.call('XACK', KEYS[1], ARGV[1], ARGV[2])
redis.call('XDEL', KEYS[1], ARGV[2])
local depth = redis.call('DECR', KEYS[2])
if depth <= 0 then
  redis.call('DEL', KEYS[2])
end
return acknowledged
)lua";
        const auto reply =
            command({"EVAL", std::string(script), "2", std::string(stream), std::string(depthKey),
                     std::string(consumerGroup), std::string(id)});
        if (reply->type != REDIS_REPLY_INTEGER)
            throwReply("grouped XACK/XDEL", reply.get());
    }

    void ensureGroup(std::string_view stream, std::string_view group) {
        const auto reply = command(
            {"XGROUP", "CREATE", std::string(stream), std::string(group), "0", "MKSTREAM"}, true);
        if (reply->type == REDIS_REPLY_ERROR) {
            const std::string_view error(reply->str, reply->len);
            if (!error.starts_with("BUSYGROUP"))
                throwReply("XGROUP CREATE", reply.get());
        }
    }

    std::vector<StreamMessage> readGroup(std::string_view stream, std::string_view group,
                                         std::string_view consumer, std::string_view id,
                                         std::chrono::milliseconds block, std::size_t count = 100) {
        std::vector<std::string> args{"XREADGROUP",          "GROUP", std::string(group),
                                      std::string(consumer), "COUNT", std::to_string(count)};
        if (block.count() > 0) {
            args.push_back("BLOCK");
            args.push_back(std::to_string(block.count()));
        }
        args.insert(args.end(), {"STREAMS", std::string(stream), std::string(id)});
        auto reply = command(args, true);
        std::vector<StreamMessage> messages;
        if (reply->type == REDIS_REPLY_NIL)
            return messages;
        if (reply->type != REDIS_REPLY_ARRAY || reply->elements == 0)
            throwReply("XREADGROUP", reply.get());
        for (std::size_t streamIndex = 0; streamIndex < reply->elements; ++streamIndex) {
            const auto* streamReply = reply->element[streamIndex];
            if (!streamReply || streamReply->type != REDIS_REPLY_ARRAY ||
                streamReply->elements != 2)
                continue;
            const auto* entries = streamReply->element[1];
            if (!entries || entries->type != REDIS_REPLY_ARRAY)
                continue;
            for (std::size_t entryIndex = 0; entryIndex < entries->elements; ++entryIndex) {
                const auto* entry = entries->element[entryIndex];
                if (!entry || entry->type != REDIS_REPLY_ARRAY || entry->elements != 2)
                    continue;
                const auto* entryId = entry->element[0];
                const auto* fieldArray = entry->element[1];
                if (!entryId || entryId->type != REDIS_REPLY_STRING || !fieldArray ||
                    fieldArray->type != REDIS_REPLY_ARRAY)
                    continue;
                StreamMessage message;
                message.id.assign(entryId->str, entryId->len);
                for (std::size_t fieldIndex = 0; fieldIndex + 1 < fieldArray->elements;
                     fieldIndex += 2) {
                    const auto* name = fieldArray->element[fieldIndex];
                    const auto* value = fieldArray->element[fieldIndex + 1];
                    if (!name || !value || name->type != REDIS_REPLY_STRING ||
                        value->type != REDIS_REPLY_STRING)
                        continue;
                    message.fields.push_back({{name->str, name->len}, {value->str, value->len}});
                }
                messages.push_back(std::move(message));
            }
        }
        return messages;
    }

    void acknowledge(std::string_view stream, std::string_view group, std::string_view id) {
        const auto reply =
            command({"XACK", std::string(stream), std::string(group), std::string(id)});
        if (reply->type != REDIS_REPLY_INTEGER)
            throwReply("XACK", reply.get());
    }

    void acknowledgeAndDelete(std::string_view stream, std::string_view group,
                              std::string_view id) {
        acknowledge(stream, group, id);
        const auto reply = command({"XDEL", std::string(stream), std::string(id)});
        if (reply->type != REDIS_REPLY_INTEGER)
            throwReply("XDEL", reply.get());
    }

    void setHash(std::string_view key, const std::vector<StreamField>& fields) {
        if (fields.empty())
            return;
        std::vector<std::string> args{"HSET", std::string(key)};
        for (const auto& field : fields) {
            args.push_back(field.name);
            args.push_back(field.value);
        }
        const auto reply = command(args);
        if (reply->type != REDIS_REPLY_INTEGER)
            throwReply("HSET", reply.get());
    }

    void eraseHash(std::string_view key) {
        const auto reply = command({"DEL", std::string(key)});
        if (reply->type != REDIS_REPLY_INTEGER)
            throwReply("DEL", reply.get());
    }

    [[nodiscard]] std::vector<StreamField> hashEntries(std::string_view key) {
        const auto reply = command({"HGETALL", std::string(key)});
        if (reply->type != REDIS_REPLY_ARRAY)
            throwReply("HGETALL", reply.get());
        std::vector<StreamField> entries;
        entries.reserve(reply->elements / 2);
        for (std::size_t index = 0; index + 1 < reply->elements; index += 2) {
            const auto* name = reply->element[index];
            const auto* value = reply->element[index + 1];
            if (!name || !value || name->type != REDIS_REPLY_STRING ||
                value->type != REDIS_REPLY_STRING)
                continue;
            entries.push_back({{name->str, name->len}, {value->str, value->len}});
        }
        return entries;
    }

    [[nodiscard]] bool claimHash(std::string_view key, const std::vector<StreamField>& fields,
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
        std::vector<std::string> args{"EVAL", std::string(script), "1", std::string(key),
                                      std::to_string(ttl.count())};
        for (const auto& field : fields) {
            args.push_back(field.name);
            args.push_back(field.value);
        }
        const auto reply = command(args);
        if (reply->type != REDIS_REPLY_INTEGER)
            throwReply("claim state Hash", reply.get());
        return reply->integer == 1;
    }

    [[nodiscard]] bool eraseHashIfFieldValue(std::string_view key, std::string_view field,
                                             std::string_view expected) {
        static constexpr std::string_view script = R"lua(
if redis.call('HGET', KEYS[1], ARGV[1]) ~= ARGV[2] then return 0 end
return redis.call('DEL', KEYS[1])
)lua";
        const auto reply = command({"EVAL", std::string(script), "1", std::string(key),
                                    std::string(field), std::string(expected)});
        if (reply->type != REDIS_REPLY_INTEGER)
            throwReply("conditional state DEL", reply.get());
        return reply->integer == 1;
    }

    [[nodiscard]] bool completeInflightTask(std::string_view stream, std::string_view consumerGroup,
                                            std::string_view id, std::string_view depthKey,
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
        const auto reply =
            command({"EVAL", std::string(script), "3", std::string(stream), std::string(depthKey),
                     std::string(inflightKey), std::string(consumerGroup), std::string(id),
                     std::string(expectedToken)});
        if (reply->type != REDIS_REPLY_INTEGER)
            throwReply("complete inflight task", reply.get());
        return reply->integer == 1;
    }

    void eraseMatching(std::string_view pattern) {
        std::string cursor = "0";
        do {
            const auto reply =
                command({"SCAN", cursor, "MATCH", std::string(pattern), "COUNT", "100"});
            if (reply->type != REDIS_REPLY_ARRAY || reply->elements != 2 ||
                reply->element[0]->type != REDIS_REPLY_STRING ||
                reply->element[1]->type != REDIS_REPLY_ARRAY)
                throwReply("SCAN", reply.get());
            cursor.assign(reply->element[0]->str, reply->element[0]->len);
            const auto* keys = reply->element[1];
            for (std::size_t index = 0; index < keys->elements; ++index) {
                const auto* key = keys->element[index];
                if (key && key->type == REDIS_REPLY_STRING)
                    eraseHash(std::string_view(key->str, key->len));
            }
        } while (cursor != "0");
    }

    [[nodiscard]] std::vector<std::string> keysMatching(std::string_view pattern) {
        std::vector<std::string> result;
        std::string cursor = "0";
        do {
            const auto reply =
                command({"SCAN", cursor, "MATCH", std::string(pattern), "COUNT", "100"});
            if (reply->type != REDIS_REPLY_ARRAY || reply->elements != 2 ||
                reply->element[0]->type != REDIS_REPLY_STRING ||
                reply->element[1]->type != REDIS_REPLY_ARRAY)
                throwReply("SCAN", reply.get());
            cursor.assign(reply->element[0]->str, reply->element[0]->len);
            const auto* keys = reply->element[1];
            for (std::size_t index = 0; index < keys->elements; ++index) {
                const auto* key = keys->element[index];
                if (key && key->type == REDIS_REPLY_STRING)
                    result.emplace_back(key->str, key->len);
            }
        } while (cursor != "0");
        return result;
    }

    std::int64_t incrementWithExpiry(std::string_view key, std::chrono::milliseconds expiry) {
        const auto incremented = command({"INCR", std::string(key)});
        if (incremented->type != REDIS_REPLY_INTEGER)
            throwReply("INCR", incremented.get());
        const auto expires = command({"PEXPIRE", std::string(key), std::to_string(expiry.count())});
        if (expires->type != REDIS_REPLY_INTEGER)
            throwReply("PEXPIRE", expires.get());
        return incremented->integer;
    }

    void erase(std::string_view key) {
        const auto reply = command({"DEL", std::string(key)});
        if (reply->type != REDIS_REPLY_INTEGER)
            throwReply("DEL", reply.get());
    }

  private:
    struct ReplyDeleter {
        void operator()(redisReply* reply) const noexcept {
            if (reply)
                freeReplyObject(reply);
        }
    };
    using Reply = std::unique_ptr<redisReply, ReplyDeleter>;

    void disconnect() noexcept {
        if (context_)
            redisFree(context_);
        context_ = nullptr;
    }

    Reply command(const std::vector<std::string>& args, bool allowNil = false) {
        if (!connected())
            connect();
        std::vector<const char*> argv;
        std::vector<std::size_t> lengths;
        argv.reserve(args.size());
        lengths.reserve(args.size());
        for (const auto& arg : args) {
            argv.push_back(arg.data());
            lengths.push_back(arg.size());
        }
        auto* raw = static_cast<redisReply*>(
            redisCommandArgv(context_, static_cast<int>(argv.size()), argv.data(), lengths.data()));
        if (!raw) {
            const std::string message = context_ ? context_->errstr : "no context";
            disconnect();
            throw std::runtime_error("Redis command failed: " + message);
        }
        Reply reply(raw);
        if (reply->type == REDIS_REPLY_ERROR && !allowNil)
            throwReply(args.empty() ? "command" : args.front(), reply.get());
        return reply;
    }

    static void ensureStatus(const Reply& reply, std::string_view operation) {
        if (reply->type != REDIS_REPLY_STATUS || std::string_view(reply->str, reply->len) != "OK")
            throwReply(operation, reply.get());
    }

    [[noreturn]] static void throwReply(std::string_view operation, const redisReply* reply) {
        std::string message(operation);
        message += " failed";
        if (reply && reply->str && reply->len > 0) {
            message += ": ";
            message.append(reply->str, reply->len);
        }
        throw std::runtime_error(message);
    }

    RedisEndpoint endpoint_;
    redisContext* context_ = nullptr;
};

class RedisStreamProducer {
  public:
    explicit RedisStreamProducer(RedisEndpoint endpoint) : connection_(std::move(endpoint)) {}

    std::string publish(std::string_view stream, const std::vector<StreamField>& fields,
                        std::size_t maxLength = 0) {
        std::lock_guard lock(mutex_);
        if (maxLength == 0)
            maxLength = stream == kDeadLetterStream || stream == kLinkEventStream ? 1000 : 10000;
        return connection_.add(stream, fields, maxLength);
    }

    void updateSession(std::string_view connectionId, const std::vector<StreamField>& fields) {
        std::lock_guard lock(mutex_);
        const auto key = sessionStateKey(connectionId);
        connection_.eraseHash(key);
        connection_.setHash(key, fields);
    }

    void removeSession(std::string_view connectionId) {
        std::lock_guard lock(mutex_);
        connection_.eraseHash(sessionStateKey(connectionId));
    }

    void clearSessions() {
        std::lock_guard lock(mutex_);
        connection_.eraseMatching(std::string(kSessionStatePrefix) + '*');
    }

    void writeLinkStatus(std::string_view linkId, const std::vector<StreamField>& fields,
                         bool publishEvent = true) {
        std::lock_guard lock(mutex_);
        const auto id = std::string(linkId);
        const auto key = "iot:state:link:" + id;
        connection_.eraseHash(key);
        connection_.setHash(key, fields);
        if (publishEvent) {
            auto event = fields;
            event.insert(event.begin(), {"link_id", id});
            event.insert(event.begin(), {"message_id", nextMessageId()});
            connection_.add(kLinkEventStream, event, 500);
        }
    }

    void removeLinkStatus(std::string_view linkId) {
        std::lock_guard lock(mutex_);
        const auto id = std::string(linkId);
        connection_.eraseHash("iot:state:link:" + id);
        connection_.add(kLinkEventStream,
                        {{"message_id", nextMessageId()},
                         {"link_id", id},
                         {"state", "removed"},
                         {"connection_count", "0"},
                         {"updated_at_ms", std::to_string(utcNowMilliseconds())}},
                        500);
    }

  private:
    RedisStreamConnection connection_;
    std::mutex mutex_;
};

} // namespace service::bridge
