#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/web/WebWorker.h>
#include <ruvia/web/redis/Redis.h>

#include "service/common/http.h"
#include "service/common/message.h"
#include "service/features/messaging/messaging.transport.h"

namespace service::telemetry::latest {

inline constexpr std::string_view kOnlineDeadlinesBase =
    "iot:v2:schedule:device:online-deadlines";
inline constexpr std::string_view kRealtimeChangesStream =
    "iot:live:changes";

template <typename Redis>
ruvia::Task<void> publishRealtimeChange(const Redis& redis) {
    const auto reply = co_await service::message::redis::command(
        redis,
        { "XADD", std::string(kRealtimeChangesStream), "MAXLEN", "~", "100000", "*", "topic", "device" }
    );
    if (reply.kind() != ruvia::RedisValue::Kind::kString) {
        service::message::redis::throwValue("publish device change", reply);
    }
}

inline std::string onlineDeadlinesKey() {
    return std::string(kOnlineDeadlinesBase);
}

inline std::string jsonEscape(std::string_view value) {
    std::string output;
    output.reserve(value.size() + 8);
    for (const char ch : value) {
        switch (ch) {
            case '"':
                output += "\\\"";
                break;
            case '\\':
                output += "\\\\";
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
                if (static_cast<unsigned char>(ch) < 0x20) {
                    output += ' ';
                } else {
                    output.push_back(ch);
                }
        }
    }
    return output;
}

inline std::string jsonQuoted(std::string_view value) {
    return "\"" + jsonEscape(value) + "\"";
}

inline std::string jsonKeySet(const std::vector<std::string>& keys) {
    std::string result = "{";
    bool first = true;
    for (const auto& key : keys) {
        if (!first) {
            result.push_back(',');
        }
        first = false;
        result += jsonQuoted(key);
        result += ":true";
    }
    result.push_back('}');
    return result;
}

inline std::string stateJson(std::string_view state, std::string_view reason, std::string_view lastReport, std::string_view onlineUntil, std::string_view updatedAt) {
    std::string result = "{\"state\":" + jsonQuoted(state) +
        ",\"reason\":" + jsonQuoted(reason) + ",\"lastReportAt\":";
    result += lastReport.empty() ? "null" : std::string(lastReport);
    result += ",\"onlineUntil\":";
    result += onlineUntil.empty() ? "null" : std::string(onlineUntil);
    result += ",\"updatedAt\":";
    result += updatedAt.empty() ? "null" : std::string(updatedAt);
    result.push_back('}');
    return result;
}

template <typename Pipeline>
ruvia::Task<bool> executeProjectionPipeline(Pipeline pipeline, std::string_view operation) {
    const auto replies = co_await std::move(pipeline).exec();
    service::message::redis::requirePipelineSuccess(operation, replies);
    co_return true;
}

template <typename Redis>
ruvia::Task<void> initializeDevice(const Redis& redis, std::string_view deviceId, std::string_view deviceCode) {
    const auto now = std::to_string(service::message::utcNowMilliseconds());
    co_await service::message::redis::setHash(
        redis,
        runtimeKey(deviceId),
        { { "device_id", std::string(deviceId) },
          { "device_code", std::string(deviceCode) },
          { "state", "offline" },
          { "state_reason", "no_connection" },
          { "updated_at_ms", now } }
    );
    co_await service::message::redis::setHash(
        redis,
        latestKey(deviceId),
        { { "_device_id", std::string(deviceId) },
          { "_device_code", std::string(deviceCode) },
          { "_state", stateJson("offline", "no_connection", {}, {}, now) },
          { "_updated_at_ms", now } }
    );
    co_await publishRealtimeChange(redis);
}

template <typename Redis>
ruvia::Task<void> eraseDevice(const Redis& redis, std::string_view deviceId) {
    co_await service::message::redis::eraseHash(redis, runtimeKey(deviceId));
    co_await service::message::redis::eraseHash(redis, latestKey(deviceId));
    (void)co_await service::message::redis::command(
        redis,
        { "ZREM", onlineDeadlinesKey(), std::string(deviceId) }
    );
    co_await publishRealtimeChange(redis);
}

template <typename Redis>
ruvia::Task<void> signalFreshness(const Redis& redis, std::string_view deadline = {}) {
    (void)deadline;
    co_await service::message::redis::wakeWorker(
        redis,
        std::nullopt,
        service::message::WorkerStreamTask::Freshness
    );
}

template <typename Redis>
ruvia::Task<void> update(const Redis& redis, const std::vector<service::message::ParsedDeviceMessage>& messages) {
    if (messages.empty()) {
        co_return;
    }
    static constexpr std::string_view script = R"lua(
local function number_or(value, fallback)
  local number = tonumber(value)
  if number == nil then return fallback end
  return number
end
local function point_value(value, data_type)
  if data_type == 'BOOL' then
    if value == true or value == 1 or value == '1' or value == 'true' then return '1' end
    if value == false or value == 0 or value == '0' or value == 'false' then return '0' end
  end
  return tostring(value)
end
local payload = cjson.decode(ARGV[8])
local runtime_key = 'iot:v2:runtime:device:' .. ARGV[1]
local latest_key = 'iot:v2:device:' .. ARGV[1] .. ':latest'
if redis.call('HGET', runtime_key, 'device_id') ~= ARGV[1] then
  return -1
end
local configured_ids = {}
local has_configured_ids = false
local configured_json = redis.call('HGET', latest_key, '_element_ids')
if configured_json ~= false and configured_json ~= nil and configured_json ~= '' then
  local ok, decoded = pcall(cjson.decode, configured_json)
  if ok and type(decoded) == 'table' then
    configured_ids = decoded
    has_configured_ids = true
  end
end
local observed_at = number_or(ARGV[4], 0)
local online_until = observed_at + number_or(ARGV[7], 300000)
local now = number_or(ARGV[5], observed_at)
local current_report = number_or(redis.call('HGET', runtime_key, 'last_report_at_ms'), -1)
if observed_at >= current_report then
  local deadlines_key = KEYS[1]
  local earliest = redis.call('ZRANGE', deadlines_key, 0, 0, 'WITHSCORES')
  local wake = #earliest == 0 or online_until < number_or(earliest[2], online_until + 1)
  local state = 'online'
  local reason = ''
  if online_until < now then
    state = 'offline'
    reason = 'data_stale'
  end
  local state_json = cjson.encode({
    state = state,
    reason = reason,
    lastReportAt = observed_at,
    onlineUntil = online_until,
    updatedAt = now
  })
  redis.call('HSET', runtime_key,
    'device_id', ARGV[1], 'device_code', ARGV[2],
    'last_report_at_ms', ARGV[4], 'online_until_ms', tostring(online_until),
    'state', state, 'state_reason', reason, 'updated_at_ms', ARGV[5])
  redis.call('HSET', latest_key,
    '_device_id', ARGV[1], '_device_code', ARGV[2],
    '_state', state_json, '_updated_at_ms', ARGV[5])
  redis.call('ZADD', deadlines_key, online_until, ARGV[1])
  if wake then
    redis.call('XADD', KEYS[2],
               'MAXLEN', '~', '100000', '*', 'task', 'freshness')
  end
end
local count = 0
local touched = false
for element_id, point in pairs(payload.values or {}) do
  if not (has_configured_ids and configured_ids[element_id] == nil) then
    local value = '-'
    local existing = redis.call('HGET', latest_key, element_id)
    local previous = {}
    if existing ~= false and existing ~= nil and existing ~= '' then
      local ok, decoded = pcall(cjson.decode, existing)
      if ok and type(decoded) == 'table' then previous = decoded end
    end
    local data_type = tostring(point.dataType or previous.dataType or '')
    if point.value ~= nil and point.value ~= cjson.null then
      value = point_value(point.value, data_type)
    end
    local current = number_or(previous.observedAt, -1)
    if observed_at >= current then
      local elementName = tostring(point.name or previous.name or element_id)
      local unit = tostring(point.unit or previous.unit or '')
      local scale = previous.scale
      local decimals = previous.decimals
      local group = tostring(previous.group or '')
      local encode = tostring(previous.encode or '')
      local sort = number_or(previous.sort, 0)
      local document = cjson.encode({
        id = element_id,
        name = elementName,
        value = value,
        dataType = data_type,
        unit = unit,
        scale = scale,
        decimals = decimals,
        group = group,
        encode = encode,
        sort = sort,
        protocol = ARGV[3],
        observedAt = observed_at,
        updatedAt = number_or(ARGV[5], observed_at),
        source = ARGV[6]
      })
      redis.call('HSET', latest_key, element_id, document)
      count = count + 1
      touched = true
    end
  end
end
if observed_at >= current_report then touched = true end
if touched then redis.call('XADD', KEYS[3], 'MAXLEN', '~', '100000', '*', 'topic', 'device') end
return count
    )lua";
    const auto scriptSha = co_await redis.scriptLoad(script);
    auto pipeline = redis.pipeline();
    for (const auto& parsed : messages) {
        const auto observedAt = std::to_string(parsed.observedAtMs);
        const auto updatedAt = std::to_string(service::message::utcNowMilliseconds());
        const auto onlineWindow = std::to_string(parsed.onlineWindowMs);
        const auto deadlinesKey = onlineDeadlinesKey();
        const auto wakeStream = service::message::workerWakeStream(std::nullopt);
        const std::array<std::string_view, 14> command{
            "EVALSHA",
            scriptSha,
            "3",
            deadlinesKey,
            wakeStream,
            kRealtimeChangesStream,
            parsed.deviceId,
            parsed.deviceCode,
            parsed.protocol,
            observedAt,
            updatedAt,
            parsed.source,
            onlineWindow,
            parsed.valuesJson
        };
        // RedisPipeline copies every argument synchronously.
        pipeline.command(command);
    }
    const auto replies = co_await std::move(pipeline).exec();
    for (const auto& reply : replies) {
        if (reply.kind() == ruvia::RedisValue::Kind::kError) {
            service::message::redis::throwValue("device latest update", reply);
        }
    }
}

template <typename Redis>
ruvia::Task<void> expireStaleForKey(const Redis& redis, std::string deadlineKey) {
    const auto now = std::to_string(service::message::utcNowMilliseconds());
    const auto due = co_await service::message::redis::command(
        redis,
        { "ZRANGEBYSCORE", deadlineKey, "-inf", now, "LIMIT", "0", "1000" }
    );
    if (due.kind() != ruvia::RedisValue::Kind::kArray) {
        service::message::redis::throwValue("read device online deadlines", due);
    }
    static constexpr std::string_view script = R"lua(
local function number_or(value, fallback)
  local number = tonumber(value)
  if number == nil then return fallback end
  return number
end
local runtime_key = 'iot:v2:runtime:device:' .. ARGV[1]
local latest_key = 'iot:v2:device:' .. ARGV[1] .. ':latest'
local now = tonumber(ARGV[2])
if redis.call('ZREM', KEYS[1], ARGV[1]) ~= 1 then return 0 end
local expected = tonumber(redis.call('HGET', runtime_key, 'online_until_ms') or '-1')
if expected <= now then
  local state_json = cjson.encode({
    state = 'offline',
    reason = 'data_stale',
    lastReportAt = number_or(redis.call('HGET', runtime_key, 'last_report_at_ms'), 0),
    onlineUntil = expected,
    updatedAt = now
  })
  redis.call('HSET', runtime_key, 'state', 'offline', 'state_reason', 'data_stale',
             'updated_at_ms', ARGV[2])
  redis.call('HSET', latest_key, '_state', state_json, '_updated_at_ms', ARGV[2])
  redis.call('XADD', KEYS[2], 'MAXLEN', '~', '100000', '*', 'topic', 'device')
  return 1
end
redis.call('ZADD', KEYS[1], expected, ARGV[1])
return 0
)lua";
    if (due.array().empty()) {
        co_return;
    }
    const auto scriptSha = co_await redis.scriptLoad(script);
    auto pipeline = redis.pipeline();
    for (const auto& code : due.array()) {
        if (code.kind() != ruvia::RedisValue::Kind::kString) {
            continue;
        }
        const std::array<std::string_view, 7> command{
            "EVALSHA",
            scriptSha,
            "2",
            deadlineKey,
            kRealtimeChangesStream,
            code.string(),
            now
        };
        pipeline.command(command);
    }
    const auto replies = co_await std::move(pipeline).exec();
    for (const auto& reply : replies) {
        if (reply.kind() == ruvia::RedisValue::Kind::kError) {
            service::message::redis::throwValue("expire device online deadline", reply);
        }
    }
}

template <typename Redis>
ruvia::Task<void> expireStale(const Redis& redis) {
    co_await expireStaleForKey(redis, onlineDeadlinesKey());
}

template <typename Redis>
ruvia::Task<std::optional<std::int64_t>> nextDeadlineForKey(
    const Redis& redis,
    std::string deadlineKey
) {
    const auto reply = co_await service::message::redis::command(
        redis,
        { "ZRANGE", deadlineKey, "0", "0", "WITHSCORES" }
    );
    if (reply.kind() != ruvia::RedisValue::Kind::kArray) {
        service::message::redis::throwValue("read next device online deadline", reply);
    }
    if (reply.array().empty()) {
        co_return std::nullopt;
    }
    if (reply.array().size() != 2 ||
        reply.array()[1].kind() != ruvia::RedisValue::Kind::kString) {
        service::message::redis::throwValue("parse next device online deadline", reply);
    }
    const auto score = service::common::parseInt64(
        std::optional<std::string_view>{ reply.array()[1].string() }
    );
    if (!score) {
        throw std::runtime_error("invalid device online deadline score");
    }
    co_return *score;
}

template <typename Redis>
ruvia::Task<std::optional<std::int64_t>> nextDeadline(const Redis& redis) {
    co_return co_await nextDeadlineForKey(redis, onlineDeadlinesKey());
}

inline std::optional<std::chrono::milliseconds>
deadlineWait(std::int64_t now, std::optional<std::int64_t> deadline) {
    if (!deadline.has_value()) {
        return std::nullopt;
    }
    if (*deadline <= now) {
        return std::chrono::milliseconds::zero();
    }
    return std::chrono::milliseconds(*deadline - now);
}

template <typename Context>
ruvia::Task<void> project(Context& context, std::string filter, std::vector<ruvia::DbValue> params, bool resetRuntime, bool preserveExisting = false) {
    const auto redis = context.redis();
    const auto nowMs = service::message::utcNowMilliseconds();
    const auto now = std::to_string(nowMs);
    const auto devices = co_await context.db().query(
        R"sql(
SELECT d.id::text, d.protocol_params->>'device_code',
       COALESCE(
         CASE WHEN COALESCE(d.protocol_params->>'online_timeout', '') ~ '^-?[0-9]{1,18}$'
              THEN (d.protocol_params->>'online_timeout')::bigint END,
         300) * 1000
FROM device d
WHERE d.deleted_at IS NULL)sql" +
            filter + " ORDER BY d.id",
        params
    );
    if (devices.empty()) {
        co_return;
    }

    std::set<std::string, std::less<>> recoveryDeviceIds;
    std::map<std::string, std::string, std::less<>> preservedDeadlines;
    if (preserveExisting) {
        auto existencePipeline = redis.pipeline();
        std::vector<std::vector<std::string>> existenceCommands;
        existenceCommands.reserve(devices.size() * 2);
        for (const auto& row : devices) {
            existenceCommands.push_back(
                { "HMGET", latestKey(row[0].value().value_or(std::string_view{})), "_device_id", "_element_ids" }
            );
            std::vector<std::string_view> views(existenceCommands.back().begin(), existenceCommands.back().end());
            existencePipeline.command(views);
            existenceCommands.push_back(
                { "HMGET", runtimeKey(row[0].value().value_or(std::string_view{})), "device_id", "online_until_ms" }
            );
            std::vector<std::string_view> runtimeViews(existenceCommands.back().begin(), existenceCommands.back().end());
            existencePipeline.command(runtimeViews);
        }
        const auto replies = co_await std::move(existencePipeline).exec();
        for (std::size_t index = 0; index < devices.size(); ++index) {
            const auto& row = devices[index];
            const auto latestIndex = index * 2;
            const auto runtimeIndex = latestIndex + 1;
            const bool matches =
                runtimeIndex < replies.size() &&
                replies[latestIndex].kind() == ruvia::RedisValue::Kind::kArray &&
                replies[latestIndex].array().size() == 2 &&
                replies[latestIndex].array()[0].kind() == ruvia::RedisValue::Kind::kString &&
                replies[latestIndex].array()[0].string() == row[0].value().value_or(std::string_view{}) &&
                replies[latestIndex].array()[1].kind() == ruvia::RedisValue::Kind::kString &&
                !replies[latestIndex].array()[1].string().empty() &&
                replies[runtimeIndex].kind() == ruvia::RedisValue::Kind::kArray &&
                replies[runtimeIndex].array().size() == 2 &&
                replies[runtimeIndex].array()[0].kind() == ruvia::RedisValue::Kind::kString &&
                replies[runtimeIndex].array()[0].string() == row[0].value().value_or(std::string_view{});
            if (!matches) {
                recoveryDeviceIds.emplace(row[0].value().value_or(std::string_view{}));
            } else if (replies[runtimeIndex].array()[1].kind() ==
                       ruvia::RedisValue::Kind::kString) {
                const auto deadline = service::common::parseInt64(
                    std::optional<std::string_view>{ replies[runtimeIndex].array()[1].string() }
                );
                if (deadline) {
                    preservedDeadlines.insert_or_assign(std::string(row[0].value().value_or(std::string_view{})), std::to_string(*deadline));
                } else {
                    recoveryDeviceIds.emplace(row[0].value().value_or(std::string_view{}));
                }
            }
        }
    } else {
        for (const auto& row : devices) {
            recoveryDeviceIds.emplace(row[0].value().value_or(std::string_view{}));
        }
    }

    auto metaPipeline = redis.pipeline();
    std::vector<std::vector<std::string>> metaCommands;
    metaCommands.reserve(devices.size() * 6);
    std::map<std::string, std::int64_t, std::less<>> onlineWindows;
    std::map<std::string, std::vector<std::string>, std::less<>> elementIds;
    for (const auto& row : devices) {
        const std::string deviceId(row[0].value().value_or(std::string_view{}));
        const std::string deviceCode(row[1].value().value_or(std::string_view{}));
        onlineWindows.insert_or_assign(
            deviceId,
            service::common::parseInt64(std::optional<std::string_view>{ row[2].value().value_or(std::string_view{}) })
                .value_or(300000)
        );
        elementIds.insert_or_assign(deviceId, std::vector<std::string>{});
        if (recoveryDeviceIds.contains(deviceId)) {
            metaCommands.push_back({ "DEL", latestKey(deviceId) });
            std::vector<std::string_view> views(metaCommands.back().begin(), metaCommands.back().end());
            metaPipeline.command(views);
            metaCommands.push_back({ "DEL", runtimeKey(deviceId) });
            std::vector<std::string_view> runtimeViews(metaCommands.back().begin(), metaCommands.back().end());
            metaPipeline.command(runtimeViews);
        }
        metaCommands.push_back({ "HSET", latestKey(deviceId), "_device_id", deviceId, "_device_code", deviceCode, "_updated_at_ms", now });
        {
            std::vector<std::string_view> views(metaCommands.back().begin(), metaCommands.back().end());
            metaPipeline.command(views);
        }
        metaCommands.push_back({ "HSETNX", latestKey(deviceId), "_state", stateJson("offline", "no_data", {}, {}, now) });
        {
            std::vector<std::string_view> views(metaCommands.back().begin(), metaCommands.back().end());
            metaPipeline.command(views);
        }
        metaCommands.push_back({ "HSET", runtimeKey(deviceId), "device_id", deviceId, "device_code", deviceCode, "updated_at_ms", now });
        if (resetRuntime || recoveryDeviceIds.contains(deviceId)) {
            metaCommands.back().push_back("state");
            metaCommands.back().push_back("offline");
            metaCommands.back().push_back("state_reason");
            metaCommands.back().push_back(recoveryDeviceIds.contains(deviceId) ? "no_data" : "startup");
        }
        {
            std::vector<std::string_view> views(metaCommands.back().begin(), metaCommands.back().end());
            metaPipeline.command(views);
        }
        if (recoveryDeviceIds.contains(deviceId)) {
            metaCommands.push_back(
                { "ZREM", onlineDeadlinesKey(), deviceId }
            );
            std::vector<std::string_view> views(metaCommands.back().begin(), metaCommands.back().end());
            metaPipeline.command(views);
        } else if (preservedDeadlines.contains(deviceId)) {
            metaCommands.push_back({ "ZADD", onlineDeadlinesKey(), preservedDeadlines.at(deviceId), deviceId });
            std::vector<std::string_view> views(metaCommands.back().begin(), metaCommands.back().end());
            metaPipeline.command(views);
        }
    }
    co_await executeProjectionPipeline(std::move(metaPipeline), "project latest metadata");

    const auto elements = co_await context.db().query(R"sql(
WITH configured AS (
  SELECT d.id AS device_id, d.protocol_params->>'device_code' AS device_code,
         p.protocol, element,
         1 AS protocol_order, position AS function_order, 0::bigint AS element_order
  FROM device d
  JOIN device_model p ON p.device_id = d.id AND p.protocol = 'Modbus'
  CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'registers', '[]'::jsonb))
    WITH ORDINALITY AS entry(element, position)
  WHERE d.deleted_at IS NULL)sql" + filter + R"sql(
  UNION ALL
  SELECT d.id, d.protocol_params->>'device_code', p.protocol,
         element, 2, position, 0::bigint
  FROM device d
  JOIN device_model p ON p.device_id = d.id AND p.protocol = 'S7'
  CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'areas', '[]'::jsonb))
    WITH ORDINALITY AS entry(element, position)
  WHERE d.deleted_at IS NULL)sql" + filter + R"sql(
  UNION ALL
  SELECT d.id, d.protocol_params->>'device_code', p.protocol, element, 3,
         function_position, element_position
  FROM device d
  JOIN device_model p ON p.device_id = d.id AND p.protocol = 'SL651'
  CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'funcs', '[]'::jsonb))
    WITH ORDINALITY AS functions(function, function_position)
  CROSS JOIN LATERAL jsonb_array_elements(
    COALESCE(function->'elements', '[]'::jsonb) ||
    COALESCE(function->'responseElements', '[]'::jsonb))
    WITH ORDINALITY AS elements(element, element_position)
  WHERE d.deleted_at IS NULL)sql" + filter + R"sql(
), numbered AS (
  SELECT configured.*,
         row_number() OVER (
           PARTITION BY configured.device_id
           ORDER BY configured.protocol_order, configured.function_order,
                    configured.element_order) - 1 AS sort_order
  FROM configured
)
SELECT numbered.device_id::text, numbered.device_code, numbered.protocol,
       numbered.element->>'id', numbered.element->>'name',
       COALESCE(numbered.element->>'unit', ''),
       CASE WHEN jsonb_typeof(point.value->'value') = 'boolean'
            THEN CASE WHEN (point.value->>'value')::boolean THEN '1' ELSE '0' END
            ELSE COALESCE(point.value->>'value', '-') END,
       COALESCE((EXTRACT(EPOCH FROM point.observed_at) * 1000)::bigint::text, ''),
       COALESCE(NULLIF(numbered.element->>'scale', ''), '1'),
       COALESCE(NULLIF(COALESCE(numbered.element->>'decimals', numbered.element->>'digits'), ''), '-1'),
       COALESCE(numbered.element->>'group', ''),
       COALESCE(numbered.element->>'encode', ''),
       numbered.sort_order::text,
       jsonb_build_object(
         'id', numbered.element->>'id',
         'name', numbered.element->>'name',
         'value', CASE WHEN jsonb_typeof(point.value->'value') = 'boolean'
                       THEN CASE WHEN (point.value->>'value')::boolean THEN '1' ELSE '0' END
                       ELSE COALESCE(point.value->>'value', '-') END,
         'dataType', COALESCE(numbered.element->>'dataType', ''),
         'unit', COALESCE(numbered.element->>'unit', ''),
         'scale',
           COALESCE(
             CASE WHEN COALESCE(numbered.element->>'scale', '') ~
                       '^-?([0-9]+(\.[0-9]*)?|\.[0-9]+)([eE][+-]?[0-9]+)?$'
                  THEN (numbered.element->>'scale')::numeric END,
             1),
         'decimals',
           COALESCE(
             CASE WHEN COALESCE(numbered.element->>'decimals',
                                numbered.element->>'digits', '') ~ '^-?[0-9]{1,18}$'
                  THEN COALESCE(numbered.element->>'decimals',
                                numbered.element->>'digits')::bigint END,
             -1),
         'group', COALESCE(numbered.element->>'group', ''),
         'encode', COALESCE(numbered.element->>'encode', ''),
         'sort', numbered.sort_order,
         'protocol', numbered.protocol,
         'observedAt',
           CASE WHEN point.observed_at IS NULL THEN NULL
                ELSE (EXTRACT(EPOCH FROM point.observed_at) * 1000)::bigint END,
         'updatedAt', (EXTRACT(EPOCH FROM clock_timestamp()) * 1000)::bigint,
         'source', CASE WHEN point.observed_at IS NULL THEN 'empty' ELSE 'database' END
        )::text
FROM numbered
LEFT JOIN device_latest_value point
  ON point.device_id = numbered.device_id
 AND point.element_id = numbered.element->>'id'
ORDER BY numbered.device_id, numbered.protocol_order,
         numbered.function_order, numbered.element_order)sql",
                                                      params);
    static constexpr std::string_view kRefreshElementMetadataScript = R"lua(
local element_id = ARGV[1]
local ok, incoming = pcall(cjson.decode, ARGV[2])
if not ok or type(incoming) ~= 'table' then
  return redis.error_reply('invalid latest element metadata')
end
local existing = redis.call('HGET', KEYS[1], element_id)
if existing ~= false and existing ~= nil and existing ~= '' then
  local decoded_ok, previous = pcall(cjson.decode, existing)
  if decoded_ok and type(previous) == 'table' then
    if previous.value ~= nil then
      incoming.value = previous.value
      if incoming.dataType == 'BOOL' then
        if previous.value == true or previous.value == 1 or previous.value == '1' or previous.value == 'true' then
          incoming.value = '1'
        elseif previous.value == false or previous.value == 0 or previous.value == '0' or previous.value == 'false' then
          incoming.value = '0'
        end
      end
    end
    if previous.observedAt ~= nil then incoming.observedAt = previous.observedAt end
    if previous.updatedAt ~= nil then incoming.updatedAt = previous.updatedAt end
    if previous.source ~= nil then incoming.source = previous.source end
  end
end
redis.call('HSET', KEYS[1], element_id, cjson.encode(incoming))
return 1
)lua";
    auto pipeline = redis.pipeline();
    std::vector<std::vector<std::string>> commands;
    commands.reserve(elements.size() + elementIds.size());
    std::map<std::string, std::int64_t, std::less<>> lastReports;
    for (const auto& row : elements) {
        const std::string deviceId(row[0].value().value_or(std::string_view{}));
        const std::string elementId(row[3].value().value_or(std::string_view{}));
        elementIds[deviceId].push_back(elementId);
        commands.push_back({ "EVAL", std::string(kRefreshElementMetadataScript), "1", latestKey(deviceId), elementId, std::string(row[13].value().value_or(std::string_view{})) });
        std::vector<std::string_view> views;
        views.reserve(commands.back().size());
        for (const auto& argument : commands.back()) {
            views.push_back(argument);
        }
        pipeline.command(views);
        if (!row[7].value().value_or(std::string_view{}).empty()) {
            const auto parsed = service::common::parseInt64(
                std::optional<std::string_view>{ row[7].value().value_or(std::string_view{}) }
            );
            if (parsed) {
                auto& lastReport = lastReports[deviceId];
                lastReport = std::max(lastReport, *parsed);
            }
        }
    }
    for (const auto& [deviceId, ids] : elementIds) {
        commands.push_back({ "HSET", latestKey(deviceId), "_element_ids", jsonKeySet(ids) });
        std::vector<std::string_view> views;
        views.reserve(commands.back().size());
        for (const auto& argument : commands.back()) {
            views.push_back(argument);
        }
        pipeline.command(views);
    }
    if (!commands.empty()) {
        co_await executeProjectionPipeline(std::move(pipeline), "project latest elements");
    }

    if (!lastReports.empty()) {
        auto reportPipeline = redis.pipeline();
        std::vector<std::vector<std::string>> reportCommands;
        reportCommands.reserve(lastReports.size() * 2);
        for (const auto& [deviceId, lastReport] : lastReports) {
            const auto window =
                onlineWindows.contains(deviceId) ? onlineWindows.at(deviceId) : 300000;
            const auto onlineUntil = lastReport + window;
            const auto online = onlineUntil >= nowMs;
            const auto state = online ? std::string_view("online") : std::string_view("offline");
            const auto reason = online ? std::string_view{} : std::string_view("data_stale");
            const auto lastReportText = std::to_string(lastReport);
            const auto onlineUntilText = std::to_string(onlineUntil);
            reportCommands.push_back(
                { "HSET", runtimeKey(deviceId), "last_report_at_ms", lastReportText, "online_until_ms", onlineUntilText, "state", std::string(state), "state_reason", std::string(reason), "updated_at_ms", now }
            );
            std::vector<std::string_view> views(reportCommands.back().begin(), reportCommands.back().end());
            reportPipeline.command(views);
            reportCommands.push_back({ "HSET", latestKey(deviceId), "_state", stateJson(state, reason, lastReportText, onlineUntilText, now), "_updated_at_ms", now });
            std::vector<std::string_view> latestViews(reportCommands.back().begin(), reportCommands.back().end());
            reportPipeline.command(latestViews);
            reportCommands.push_back({ "ZADD", onlineDeadlinesKey(), std::to_string(onlineUntil), deviceId });
            std::vector<std::string_view> deadlineViews(reportCommands.back().begin(), reportCommands.back().end());
            reportPipeline.command(deadlineViews);
        }
        co_await executeProjectionPipeline(std::move(reportPipeline), "project latest state");
    }
    co_await signalFreshness(redis);
    co_await publishRealtimeChange(redis);
}

template <typename Context>
ruvia::Task<void> projectDevice(Context& context, std::string_view id) {
    co_await project(context, " AND d.id = $1::uuid", service::common::dbParams(id), false, true);
}

template <typename Context>
ruvia::Task<void> projectProtocol(Context& context, std::string_view id) {
    co_await project(context, " AND d.protocol_config_id = $1::uuid", service::common::dbParams(id), false, true);
}

inline ruvia::Task<void> hydrate(ruvia::WebWorkerContext& context, std::size_t workerIndex = 0, std::size_t workerCount = 1) {
    if (workerCount == 0 || workerIndex >= workerCount) {
        throw std::invalid_argument("invalid hydration worker ownership");
    }
    // Redis is the realtime read model. Keep its persisted hashes across a
    // service restart; PostgreSQL repairs metadata and is consulted only for
    // devices whose Redis projection is missing or incomplete.
    // A web worker owns exactly one PostgreSQL connection. Bound recovery result
    // sets so a large protocol definition cannot monopolize that connection for
    // the whole startup or retain every point JSON row in memory at once.
    static constexpr std::size_t kHydrationBatchSize = 32;
    std::string cursor = "00000000-0000-0000-0000-000000000000";
    for (;;) {
        const auto devices = co_await context.db().query(
            R"sql(
SELECT id::text
FROM device
WHERE deleted_at IS NULL AND id > $1::uuid
ORDER BY id
LIMIT 32)sql",
            service::common::dbParams(std::string_view(cursor))
        );
        if (devices.empty()) {
            break;
        }

        std::string ids = "{";
        for (const auto& row : devices) {
            if (ids.size() != 1) {
                ids.push_back(',');
            }
            ids.append(row[0].value().value_or(std::string_view{}));
        }
        ids.push_back('}');
        cursor.assign(devices[devices.size() - 1][0].value().value_or(std::string_view{}));
        if (ids != "{}") {
            co_await project(context, " AND d.id = ANY($1::uuid[])", service::common::dbParams(std::string_view(ids)), false, true);
        }
        if (devices.size() < kHydrationBatchSize) {
            break;
        }
    }
}

} // namespace service::telemetry::latest
