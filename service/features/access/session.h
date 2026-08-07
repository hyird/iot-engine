#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/web/ModelObject.h>

#include "service/common/uuid.h"
#include "service/features/access/contract.h"
#include "service/features/collector/stream.h"

namespace service::access::session {

inline constexpr std::string_view kActiveVersionKey{"iot:open-access:session:active"};
inline constexpr std::string_view kVersionPrefix{"iot:open-access:session:version:"};

struct Entry final {
    std::string id;
    std::string name;
    std::string status;
    std::int64_t expiresAtMs{0};
    std::set<std::string, std::less<>> scopes;
    std::set<std::string, std::less<>> deviceIds;
};

inline std::vector<std::string> parseStringArray(std::string_view json) {
    std::vector<std::string> result;
    auto remaining = json;
    const auto parsed = ruvia::detail::parseJsonValue<ruvia::Array<ruvia::String>>(
        remaining, std::pmr::get_default_resource());
    if (!parsed)
        throw std::runtime_error("invalid projected access-session array");
    result.reserve(parsed->size());
    for (const auto& item : *parsed)
        result.emplace_back(item.view());
    return result;
}

inline std::string encode(std::string_view id, std::string_view name,
                          std::string_view status, std::string_view expiresAtMs,
                          std::string_view scopes, std::string_view deviceIds) {
    std::string result;
    result.reserve(id.size() + name.size() + status.size() + expiresAtMs.size() +
                   scopes.size() + deviceIds.size() + 5);
    for (const auto field : {id, name, status, expiresAtMs, scopes, deviceIds}) {
        if (!result.empty())
            result.push_back('\0');
        result.append(field);
    }
    return result;
}

inline Entry decode(std::string_view encoded) {
    std::vector<std::string_view> fields;
    fields.reserve(6);
    std::size_t begin = 0;
    while (fields.size() < 5) {
        const auto end = encoded.find('\0', begin);
        if (end == std::string_view::npos)
            throw std::runtime_error("invalid projected access-session entry");
        fields.push_back(encoded.substr(begin, end - begin));
        begin = end + 1;
    }
    fields.push_back(encoded.substr(begin));
    Entry result;
    result.id.assign(fields[0]);
    result.name.assign(fields[1]);
    result.status.assign(fields[2]);
    try {
        result.expiresAtMs = std::stoll(std::string(fields[3]));
    } catch (const std::exception&) {
        throw std::runtime_error("invalid projected access-session expiration");
    }
    for (auto& scope : parseStringArray(fields[4]))
        result.scopes.emplace(std::move(scope));
    for (auto& device : parseStringArray(fields[5]))
        result.deviceIds.emplace(std::move(device));
    return result;
}

template <typename Redis>
ruvia::Task<std::optional<Entry>> load(const Redis& redis, std::string_view rawKey) {
    static constexpr std::string_view script = R"lua(
local version = redis.call('GET', KEYS[1])
if not version then return nil end
return redis.call('HGET', ARGV[2] .. version, ARGV[1])
)lua";
    const auto keyHash = sha256(rawKey);
    const std::string activeKey(kActiveVersionKey);
    const std::string_view keys[]{activeKey};
    const std::string prefix(kVersionPrefix);
    const std::string_view args[]{keyHash, prefix};
    const auto reply = co_await redis.eval(script, keys, args);
    if (reply.null())
        co_return std::nullopt;
    if (reply.kind() != ruvia::RedisValue::Kind::kString)
        service::message::redis::throwValue("load projected access session", reply);
    co_return decode(reply.string());
}

template <typename Context> ruvia::Task<void> refresh(Context& context) {
    // Serialize the database snapshot and Redis pointer swap across service instances.
    // The lock is held only by cold-path configuration projection, never by API reads.
    auto transaction = co_await context.db().beginTransaction();
    (void)co_await transaction.query(
        "SELECT pg_advisory_xact_lock(734622::bigint)");
    const auto rows = co_await transaction.query(R"sql(
SELECT key.access_key_hash, key.id::text, key.name, key.status::text,
       COALESCE((EXTRACT(EPOCH FROM key.expires_at) * 1000)::bigint, 0)::text,
       key.scopes::text,
       COALESCE(jsonb_agg(binding.device_id::text ORDER BY binding.device_id)
         FILTER (WHERE binding.device_id IS NOT NULL), '[]'::jsonb)::text
FROM open_access_key key
LEFT JOIN open_access_key_device binding ON binding.access_key_id = key.id
WHERE key.deleted_at IS NULL
GROUP BY key.id
ORDER BY key.id)sql");

    const auto version = service::common::nextUuidV7();
    const auto versionKey = std::string(kVersionPrefix) + version;
    constexpr std::size_t chunkSize = 128;
    for (std::size_t offset = 0; offset < rows.rows().size(); offset += chunkSize) {
        const auto end = std::min(rows.rows().size(), offset + chunkSize);
        std::vector<std::string> command{"HSET", versionKey};
        command.reserve(2 + (end - offset) * 2);
        for (std::size_t index = offset; index < end; ++index) {
            const auto& row = rows.rows()[index];
            command.emplace_back(row[0].text());
            command.push_back(encode(row[1].text(), row[2].text(), row[3].text(),
                                     row[4].text(), row[5].text(), row[6].text()));
        }
        (void)co_await service::message::redis::command(context.redis(), command);
    }
    if (rows.rows().empty())
        (void)co_await service::message::redis::command(
            context.redis(), {"HSET", versionKey, "__ready", "1"});
    (void)co_await service::message::redis::command(
        context.redis(), {"EXPIRE", versionKey, "600"});

    static constexpr std::string_view swapScript = R"lua(
local previous = redis.call('GET', KEYS[1])
redis.call('SET', KEYS[1], ARGV[1])
redis.call('PERSIST', KEYS[2])
if previous and previous ~= ARGV[1] then
  redis.call('EXPIRE', ARGV[2] .. previous, ARGV[3])
end
return previous or ''
)lua";
    const std::string activeKey(kActiveVersionKey);
    const std::string_view keys[]{activeKey, versionKey};
    const std::string prefix(kVersionPrefix);
    const std::string grace = "120";
    const std::string_view args[]{version, prefix, grace};
    const auto reply = co_await context.redis().eval(swapScript, keys, args);
    if (reply.kind() != ruvia::RedisValue::Kind::kString)
        service::message::redis::throwValue("activate access-session projection", reply);
    co_await transaction.commit();
}

inline bool expired(const Entry& entry, std::int64_t nowMs) noexcept {
    return entry.expiresAtMs > 0 && entry.expiresAtMs <= nowMs;
}

} // namespace service::access::session
