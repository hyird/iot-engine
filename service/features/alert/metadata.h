#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/core/Task.h>

#include "service/common/message/contract.h"
#include "service/features/collector/stream.h"
#include "service/features/telemetry/latest.h"

namespace service::alert::metadata {

inline constexpr std::string_view kRuleDevicesKey{"iot:alert:rule-devices"};
inline constexpr std::string_view kOfflineRulesKey{"iot:alert:offline-rules"};
inline constexpr std::string_view kReadyKey{"iot:alert:metadata-ready"};
inline constexpr std::string_view kOfflineDurationKeys{"iot:alert:offline-duration-keys"};
inline constexpr std::string_view kOfflineDurationPrefix{"iot:alert:offline-duration:"};
inline constexpr std::string_view kOfflineDeadlinesKey{"iot:schedule:alert:offline-deadlines"};

inline std::string offlineDurationKey(std::string_view deviceId) {
    return std::string(kOfflineDurationPrefix) + std::string(deviceId);
}

template <typename Context>
inline ruvia::Task<void> refresh(Context& context) {
    // Serialize the authoritative snapshot and Redis replacement across instances.
    // Telemetry reads never take this cold-path configuration lock.
    auto transaction = co_await context.db().beginTransaction();
    (void)co_await transaction.query(
        "SELECT pg_advisory_xact_lock(734623::bigint)");
    const auto rows = co_await transaction.query(R"sql(
SELECT DISTINCT rule.device_id::text, rule.id::text,
       CASE WHEN condition.value IS NULL THEN NULL ELSE
         GREATEST(1, COALESCE(NULLIF(condition.value->>'duration', '')::bigint, 300))::text
       END,
       COALESCE((EXTRACT(EPOCH FROM state.last_observed_at) * 1000)::bigint, 0)::text,
       COALESCE(device.protocol_params->>'device_code', '')
FROM alert_rule rule
JOIN device ON device.id = rule.device_id
LEFT JOIN device_data_ingest_state state ON state.device_id = rule.device_id
LEFT JOIN LATERAL jsonb_array_elements(rule.conditions) condition(value)
  ON condition.value->>'type' = 'offline'
WHERE rule.deleted_at IS NULL AND rule.status = 'enabled'
  AND device.deleted_at IS NULL AND device.status = 'enabled'
ORDER BY 1, 2, 3)sql");

    struct OfflineEntry final {
        std::string durationKey;
        std::string member;
        std::int64_t durationMs{0};
        std::int64_t deadlineMs{0};
        std::string deviceCode;
    };
    std::set<std::string, std::less<>> devices;
    std::vector<OfflineEntry> offlineEntries;
    const auto now = service::message::utcNowMilliseconds();
    for (const auto& row : rows.rows()) {
        const std::string deviceId(row[0].text());
        devices.emplace(deviceId);
        if (row[2].isNull())
            continue;
        const auto durationMs = std::stoll(std::string(row[2].text())) * 1000;
        const auto observedAtMs = std::stoll(std::string(row[3].text()));
        offlineEntries.push_back(
            {offlineDurationKey(deviceId),
             std::string(row[1].text()) + ":" + std::to_string(durationMs),
             durationMs, observedAtMs > 0 ? observedAtMs + durationMs : now,
             std::string(row[4].text())});
    }

    std::vector<std::string> arguments;
    arguments.reserve(2 + devices.size() + offlineEntries.size() * 5);
    arguments.push_back(std::to_string(devices.size()));
    for (const auto& deviceId : devices)
        arguments.push_back(deviceId);
    arguments.push_back(std::to_string(offlineEntries.size()));
    for (const auto& entry : offlineEntries) {
        arguments.push_back(entry.durationKey);
        arguments.push_back(entry.member);
        arguments.push_back(std::to_string(entry.durationMs));
        arguments.push_back(std::to_string(entry.deadlineMs));
        arguments.push_back(entry.deviceCode);
    }
    static constexpr std::string_view script = R"lua(
local previous = redis.call('SMEMBERS', KEYS[4])
for _, key in ipairs(previous) do redis.call('DEL', key) end
redis.call('DEL', KEYS[1], KEYS[4], KEYS[5])
local device_count = tonumber(ARGV[1]) or 0
local cursor = 2
for index = 1, device_count do
  redis.call('SADD', KEYS[1], ARGV[cursor])
  cursor = cursor + 1
end
local offline_count = tonumber(ARGV[cursor]) or 0
cursor = cursor + 1
for index = 1, offline_count do
  local duration_key = ARGV[cursor]
  local member = ARGV[cursor + 1]
  local duration = tonumber(ARGV[cursor + 2])
  local deadline = tonumber(ARGV[cursor + 3])
  local runtime_observed = redis.call(
    'HGET', 'iot:runtime:device:' .. ARGV[cursor + 4], 'last_report_at_ms')
  if runtime_observed then
    deadline = math.max(deadline, tonumber(runtime_observed) + duration)
  end
  redis.call('HSET', duration_key, member, duration)
  redis.call('SADD', KEYS[4], duration_key)
  redis.call('ZADD', KEYS[5], deadline, member)
  cursor = cursor + 5
end
redis.call('SET', KEYS[2], offline_count > 0 and '1' or '0')
redis.call('SET', KEYS[3], '1')
redis.call('XADD', KEYS[6], 'MAXLEN', '~', '128', '*', 'source', 'alert-metadata')
return offline_count
)lua";
    const std::string devicesKey(kRuleDevicesKey);
    const std::string offlineKey(kOfflineRulesKey);
    const std::string readyKey(kReadyKey);
    const std::string durationKeys(kOfflineDurationKeys);
    const std::string deadlinesKey(kOfflineDeadlinesKey);
    const std::string wakeStream(service::telemetry::latest::kFreshnessWakeStream);
    const std::string_view keys[]{devicesKey, offlineKey, readyKey, durationKeys,
                                  deadlinesKey, wakeStream};
    std::vector<std::string_view> views;
    views.reserve(arguments.size());
    for (const auto& argument : arguments)
        views.push_back(argument);
    const auto reply = co_await context.redis().eval(script, keys, views);
    if (reply.kind() == ruvia::RedisValue::Kind::kError)
        service::message::redis::throwValue("refresh alert metadata", reply);
    co_await transaction.commit();
}

template <typename Redis>
inline ruvia::Task<void> schedule(
    const Redis& redis,
    const std::vector<service::message::ParsedDeviceMessage>& messages) {
    if (messages.empty())
        co_return;
    static constexpr std::string_view script = R"lua(
local values = redis.call('HGETALL', KEYS[1])
if #values == 0 then return 0 end
local earliest = redis.call('ZRANGE', KEYS[2], 0, 0, 'WITHSCORES')
local old_earliest = #earliest == 0 and nil or tonumber(earliest[2])
local wake = false
local changed = 0
for index = 1, #values, 2 do
  local deadline = tonumber(ARGV[1]) + tonumber(values[index + 1])
  local current = redis.call('ZSCORE', KEYS[2], values[index])
  if not current or deadline > tonumber(current) then
    redis.call('ZADD', KEYS[2], deadline, values[index])
    changed = changed + 1
    if not old_earliest or deadline < old_earliest then wake = true end
  end
end
if wake then
  redis.call('XADD', KEYS[3], 'MAXLEN', '~', '128', '*', 'source', 'alert-telemetry')
end
return changed
)lua";
    const auto scriptSha = co_await redis.scriptLoad(script);
    auto pipeline = redis.pipeline();
    const std::string deadlinesKey(kOfflineDeadlinesKey);
    const std::string wakeStream(service::telemetry::latest::kFreshnessWakeStream);
    for (const auto& message : messages) {
        const auto durationKey = offlineDurationKey(message.deviceId);
        const auto observedAt = std::to_string(message.observedAtMs);
        const std::array<std::string_view, 3> keys{durationKey, deadlinesKey, wakeStream};
        const std::array<std::string_view, 1> arguments{observedAt};
        service::message::redis::queueEvalSha(pipeline, scriptSha, keys, arguments);
    }
    const auto replies = co_await std::move(pipeline).exec();
    service::message::redis::requirePipelineSuccess("schedule offline alerts", replies);
}

template <typename Redis>
inline ruvia::Task<std::optional<std::int64_t>> nextOfflineDeadline(const Redis& redis) {
    const auto reply = co_await service::message::redis::command(
        redis, {"ZRANGE", std::string(kOfflineDeadlinesKey), "0", "0", "WITHSCORES"});
    if (reply.kind() != ruvia::RedisValue::Kind::kArray)
        service::message::redis::throwValue("read next offline-alert deadline", reply);
    if (reply.array().empty())
        co_return std::nullopt;
    if (reply.array().size() != 2 ||
        reply.array()[1].kind() != ruvia::RedisValue::Kind::kString)
        service::message::redis::throwValue("parse next offline-alert deadline", reply);
    try {
        co_return std::stoll(std::string(reply.array()[1].string()));
    } catch (const std::exception&) {
        throw std::runtime_error("invalid offline-alert deadline score");
    }
}

template <typename Redis>
inline ruvia::Task<std::vector<std::string>> dueOffline(
    const Redis& redis, std::int64_t nowMs, std::size_t maximum = 1000) {
    const auto reply = co_await service::message::redis::command(
        redis, {"ZRANGEBYSCORE", std::string(kOfflineDeadlinesKey), "-inf",
                std::to_string(nowMs), "LIMIT", "0", std::to_string(maximum)});
    if (reply.kind() != ruvia::RedisValue::Kind::kArray)
        service::message::redis::throwValue("read due offline-alert deadlines", reply);
    std::vector<std::string> result;
    result.reserve(reply.array().size());
    for (const auto& value : reply.array()) {
        if (value.kind() != ruvia::RedisValue::Kind::kString)
            service::message::redis::throwValue("parse due offline-alert deadline", reply);
        result.emplace_back(value.string());
    }
    co_return result;
}

template <typename Redis>
inline ruvia::Task<void> removeOfflineDeadlines(
    const Redis& redis, const std::vector<std::string>& members) {
    if (members.empty())
        co_return;
    std::vector<std::string> command{"ZREM", std::string(kOfflineDeadlinesKey)};
    command.insert(command.end(), members.begin(), members.end());
    (void)co_await service::message::redis::command(redis, command);
}

struct Activity final {
    std::vector<bool> devices;
    bool offlineRules{false};
};

template <typename Context>
inline ruvia::Task<Activity> activity(
    Context& context, const std::vector<service::message::ParsedDeviceMessage>& messages) {
    if (messages.empty())
        co_return Activity{};
    std::map<std::string, std::vector<std::size_t>, std::less<>> positions;
    for (std::size_t index = 0; index < messages.size(); ++index)
        positions[messages[index].deviceId].push_back(index);
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto pipeline = context.redis().pipeline();
        pipeline.exists(kReadyKey);
        pipeline.command("GET", kOfflineRulesKey);
        for (const auto& [deviceId, indexes] : positions) {
            (void)indexes;
            pipeline.command("SISMEMBER", kRuleDevicesKey, deviceId);
        }
        const auto replies = co_await std::move(pipeline).exec();
        service::message::redis::requirePipelineSuccess("read alert metadata", replies);
        const bool ready = !replies.empty() &&
                           replies.front().kind() == ruvia::RedisValue::Kind::kInteger &&
                           replies.front().integer() != 0;
        if (!ready) {
            co_await refresh(context);
            continue;
        }
        Activity result;
        result.devices.resize(messages.size(), false);
        result.offlineRules = replies.size() > 1 &&
                              replies[1].kind() == ruvia::RedisValue::Kind::kString &&
                              replies[1].string() == "1";
        std::size_t replyIndex = 2;
        for (const auto& [deviceId, indexes] : positions) {
            (void)deviceId;
            const bool active = replyIndex < replies.size() &&
                                replies[replyIndex].kind() == ruvia::RedisValue::Kind::kInteger &&
                                replies[replyIndex].integer() != 0;
            for (const auto index : indexes)
                result.devices[index] = active;
            ++replyIndex;
        }
        co_return result;
    }
    throw std::runtime_error("alert metadata did not become ready");
}

} // namespace service::alert::metadata
