#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/web/WebWorker.h>
#include <ruvia/web/redis/Redis.h>

#include "service/common/http.h"
#include "service/common/message/contract.h"
#include "service/features/collector/stream.h"

namespace service::telemetry::latest {

inline constexpr std::string_view kOnlineDeadlinesKey = "iot:schedule:device:online-deadlines";

inline std::string latestKey(std::string_view deviceCode) {
    return "iot:device:" + std::string(deviceCode) + ":latest";
}

inline std::string runtimeKey(std::string_view deviceCode) {
    return "iot:runtime:device:" + std::string(deviceCode);
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
            if (static_cast<unsigned char>(ch) < 0x20)
                output += ' ';
            else
                output.push_back(ch);
        }
    }
    return output;
}

inline std::string jsonQuoted(std::string_view value) {
    return "\"" + jsonEscape(value) + "\"";
}

inline std::string stateJson(std::string_view state, std::string_view reason,
                             std::string_view lastReport, std::string_view onlineUntil,
                             std::string_view updatedAt) {
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

template <typename Redis>
ruvia::Task<void> initializeDevice(const Redis& redis, std::string_view deviceId,
                                   std::string_view deviceCode) {
    const auto now = std::to_string(service::message::utcNowMilliseconds());
    co_await service::message::redis::setHash(
        redis, runtimeKey(deviceCode),
        {{"device_id", std::string(deviceId)},
         {"device_code", std::string(deviceCode)},
         {"state", "offline"},
         {"state_reason", "no_connection"},
         {"updated_at_ms", now}});
    co_await service::message::redis::setHash(
        redis, latestKey(deviceCode),
        {{"_device_id", std::string(deviceId)},
         {"_device_code", std::string(deviceCode)},
         {"_state", stateJson("offline", "no_connection", {}, {}, now)},
         {"_updated_at_ms", now}});
}

template <typename Redis>
ruvia::Task<void> eraseDevice(const Redis& redis, std::string_view deviceCode) {
    co_await service::message::redis::eraseHash(redis, runtimeKey(deviceCode));
    co_await service::message::redis::eraseHash(redis, latestKey(deviceCode));
}

template <typename Redis>
ruvia::Task<void> update(const Redis& redis,
                         const std::vector<service::message::StreamMessage>& messages) {
    if (messages.empty())
        co_return;
    static constexpr std::string_view script = R"lua(
local function number_or(value, fallback)
  local number = tonumber(value)
  if number == nil then return fallback end
  return number
end
local payload = cjson.decode(ARGV[8])
local runtime_key = 'iot:runtime:device:' .. ARGV[2]
local latest_key = 'iot:device:' .. ARGV[2] .. ':latest'
if redis.call('HGET', runtime_key, 'device_id') ~= ARGV[1] then
  return -1
end
local observed_at = number_or(ARGV[4], 0)
local online_until = observed_at + number_or(ARGV[7], 300000)
local now = number_or(ARGV[5], observed_at)
local current_report = number_or(redis.call('HGET', runtime_key, 'last_report_at_ms'), -1)
if observed_at >= current_report then
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
  redis.call('ZADD', 'iot:schedule:device:online-deadlines', online_until, ARGV[2])
end
local count = 0
for element_id, point in pairs(payload.values or {}) do
  local value = '-'
  if point.value ~= nil and point.value ~= cjson.null then
    value = tostring(point.value)
  end
  local existing = redis.call('HGET', latest_key, element_id)
  local previous = {}
  if existing ~= false and existing ~= nil and existing ~= '' then
    local ok, decoded = pcall(cjson.decode, existing)
    if ok and type(decoded) == 'table' then previous = decoded end
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
  end
end
return count
)lua";
    auto pipeline = redis.pipeline();
    std::vector<std::vector<std::string>> commands;
    commands.reserve(messages.size());
    for (const auto& message : messages) {
        const auto parsed = service::message::parsedFrom(message);
        commands.push_back({"EVAL", std::string(script), "0", parsed.deviceId, parsed.deviceCode,
                            parsed.protocol, std::to_string(parsed.observedAtMs),
                            std::to_string(service::message::utcNowMilliseconds()), parsed.source,
                            std::to_string(parsed.onlineWindowMs), parsed.valuesJson});
        std::vector<std::string_view> views;
        views.reserve(commands.back().size());
        for (const auto& argument : commands.back())
            views.push_back(argument);
        pipeline.command(views);
    }
    const auto replies = co_await std::move(pipeline).exec();
    for (const auto& reply : replies)
        if (reply.kind() == ruvia::RedisValue::Kind::kError)
            service::message::redis::throwValue("device latest update", reply);
}

template <typename Redis> ruvia::Task<void> expireStale(const Redis& redis) {
    const auto now = std::to_string(service::message::utcNowMilliseconds());
    const auto due = co_await service::message::redis::command(
        redis,
        {"ZRANGEBYSCORE", std::string(kOnlineDeadlinesKey), "-inf", now, "LIMIT", "0", "1000"});
    if (due.kind() != ruvia::RedisValue::Kind::kArray)
        service::message::redis::throwValue("read device online deadlines", due);
    static constexpr std::string_view script = R"lua(
local function number_or(value, fallback)
  local number = tonumber(value)
  if number == nil then return fallback end
  return number
end
local runtime_key = 'iot:runtime:device:' .. ARGV[1]
local latest_key = 'iot:device:' .. ARGV[1] .. ':latest'
local expected = tonumber(redis.call('HGET', runtime_key, 'online_until_ms') or '-1')
local now = tonumber(ARGV[2])
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
  redis.call('ZREM', KEYS[1], ARGV[1])
  return 1
end
redis.call('ZADD', KEYS[1], expected, ARGV[1])
return 0
)lua";
    for (const auto& code : due.array()) {
        if (code.kind() != ruvia::RedisValue::Kind::kString)
            continue;
        const std::string deadlineKey(kOnlineDeadlinesKey);
        const std::string deviceCode(code.string());
        const std::string_view keys[]{deadlineKey};
        const std::string_view args[]{deviceCode, now};
        (void)co_await redis.eval(script, keys, args);
    }
}

template <typename Context>
ruvia::Task<void> project(Context& context, std::string filter,
                          std::vector<ruvia::DbValue> params, bool resetRuntime) {
    const auto redis = context.redis();
    const auto now = std::to_string(service::message::utcNowMilliseconds());
    const auto devices = co_await context.db().query(
        R"sql(
SELECT d.id::text, d.protocol_params->>'device_code',
       CASE WHEN p.protocol = 'SL651'
            THEN COALESCE((d.protocol_params->>'online_timeout')::bigint, 300) * 1000
            ELSE COALESCE((p.config->>'readInterval')::numeric,
                          (p.config->>'pollInterval')::numeric, 5) * 3000 END::bigint
FROM device d
JOIN protocol_config p ON p.id = d.protocol_config_id
WHERE d.deleted_at IS NULL)sql" +
            filter + " ORDER BY d.id",
        params);
    if (devices.rows().empty())
        co_return;

    auto metaPipeline = redis.pipeline();
    std::vector<std::vector<std::string>> metaCommands;
    metaCommands.reserve(devices.rows().size() * 3);
    std::map<std::string, std::int64_t, std::less<>> onlineWindows;
    for (const auto& row : devices.rows()) {
        const std::string deviceId(row[0].text());
        const std::string deviceCode(row[1].text());
        onlineWindows.insert_or_assign(deviceCode, std::stoll(std::string(row[2].text())));
        metaCommands.push_back({"DEL", latestKey(deviceCode)});
        {
            std::vector<std::string_view> views(metaCommands.back().begin(),
                                                metaCommands.back().end());
            metaPipeline.command(views);
        }
        metaCommands.push_back({"HSET", latestKey(deviceCode), "_device_id", deviceId,
                                "_device_code", deviceCode, "_state",
                                stateJson("offline", "no_data", {}, {}, now), "_updated_at_ms",
                                now});
        {
            std::vector<std::string_view> views(metaCommands.back().begin(),
                                                metaCommands.back().end());
            metaPipeline.command(views);
        }
        metaCommands.push_back({"HSET", runtimeKey(deviceCode), "device_id", deviceId,
                                "device_code", deviceCode, "updated_at_ms", now});
        if (resetRuntime) {
            metaCommands.back().push_back("state");
            metaCommands.back().push_back("offline");
            metaCommands.back().push_back("state_reason");
            metaCommands.back().push_back("startup");
        }
        {
            std::vector<std::string_view> views(metaCommands.back().begin(),
                                                metaCommands.back().end());
            metaPipeline.command(views);
        }
    }
    (void)co_await std::move(metaPipeline).exec();

    const auto elements = co_await context.db().query(R"sql(
WITH configured AS (
  SELECT d.id AS device_id, d.protocol_params->>'device_code' AS device_code,
         p.protocol, element,
         1 AS protocol_order, position AS function_order, 0::bigint AS element_order
  FROM device d
  JOIN protocol_config p ON p.id = d.protocol_config_id AND p.protocol = 'Modbus'
  CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'registers', '[]'::jsonb))
    WITH ORDINALITY AS entry(element, position)
  WHERE d.deleted_at IS NULL)sql" +
                                                           filter + R"sql(
  UNION ALL
  SELECT d.id, d.protocol_params->>'device_code', p.protocol,
         element, 2, position, 0::bigint
  FROM device d
  JOIN protocol_config p ON p.id = d.protocol_config_id AND p.protocol = 'S7'
  CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'areas', '[]'::jsonb))
    WITH ORDINALITY AS entry(element, position)
  WHERE d.deleted_at IS NULL)sql" +
                                                           filter + R"sql(
  UNION ALL
  SELECT d.id, d.protocol_params->>'device_code', p.protocol, element, 3,
         function_position, element_position
  FROM device d
  JOIN protocol_config p ON p.id = d.protocol_config_id AND p.protocol = 'SL651'
  CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'funcs', '[]'::jsonb))
    WITH ORDINALITY AS functions(function, function_position)
  CROSS JOIN LATERAL jsonb_array_elements(
    COALESCE(function->'elements', '[]'::jsonb) ||
    COALESCE(function->'responseElements', '[]'::jsonb))
    WITH ORDINALITY AS elements(element, element_position)
  WHERE d.deleted_at IS NULL)sql" +
                                                           filter + R"sql(
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
       COALESCE(numbered.element->>'unit', ''), COALESCE(point.value_text, '-'),
       COALESCE((EXTRACT(EPOCH FROM point.observed_at) * 1000)::bigint::text, ''),
       COALESCE(NULLIF(numbered.element->>'scale', ''), '1'),
       COALESCE(NULLIF(COALESCE(numbered.element->>'decimals', numbered.element->>'digits'), ''), '-1'),
       COALESCE(numbered.element->>'group', ''),
       COALESCE(numbered.element->>'encode', ''),
       numbered.sort_order::text,
       jsonb_build_object(
         'id', numbered.element->>'id',
         'name', numbered.element->>'name',
         'value', COALESCE(point.value_text, '-'),
         'unit', COALESCE(numbered.element->>'unit', ''),
         'scale', COALESCE(NULLIF(numbered.element->>'scale', '')::numeric, 1),
         'decimals',
           COALESCE(NULLIF(COALESCE(numbered.element->>'decimals',
                                    numbered.element->>'digits'), '')::bigint, -1),
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
LEFT JOIN LATERAL (
  SELECT telemetry.report_time AS observed_at,
         telemetry.data->'values'->(numbered.element->>'id')->>'value' AS value_text,
         telemetry.data->'values'->(numbered.element->>'id')->'value' AS value_json
  FROM device_data telemetry
  WHERE telemetry.device_id = numbered.device_id
    AND telemetry.data->'values'->(numbered.element->>'id') IS NOT NULL
  ORDER BY telemetry.report_time DESC, telemetry.id DESC
  LIMIT 1
) point ON TRUE
ORDER BY numbered.device_id, numbered.protocol_order,
         numbered.function_order, numbered.element_order)sql",
                                                   params);
    auto pipeline = redis.pipeline();
    std::vector<std::vector<std::string>> commands;
    commands.reserve(elements.rows().size());
    std::map<std::string, std::int64_t, std::less<>> lastReports;
    for (const auto& row : elements.rows()) {
        commands.push_back({"HSET", latestKey(row[1].text()), std::string(row[3].text()),
                            std::string(row[13].text())});
        std::vector<std::string_view> views;
        views.reserve(commands.back().size());
        for (const auto& argument : commands.back())
            views.push_back(argument);
        pipeline.command(views);
        if (!row[7].text().empty()) {
            auto& lastReport = lastReports[std::string(row[1].text())];
            lastReport = std::max(
                lastReport, static_cast<std::int64_t>(std::stoll(std::string(row[7].text()))));
        }
    }
    if (!commands.empty())
        (void)co_await std::move(pipeline).exec();

    if (!lastReports.empty()) {
        auto reportPipeline = redis.pipeline();
        std::vector<std::vector<std::string>> reportCommands;
        reportCommands.reserve(lastReports.size() * 2);
        const auto nowMilliseconds = std::stoll(now);
        for (const auto& [deviceCode, lastReport] : lastReports) {
            const auto window =
                onlineWindows.contains(deviceCode) ? onlineWindows.at(deviceCode) : 300000;
            const auto onlineUntil = lastReport + window;
            const auto online = onlineUntil >= nowMilliseconds;
            const auto state = online ? std::string_view("online") : std::string_view("offline");
            const auto reason = online ? std::string_view{} : std::string_view("data_stale");
            const auto lastReportText = std::to_string(lastReport);
            const auto onlineUntilText = std::to_string(onlineUntil);
            reportCommands.push_back(
                {"HSET", runtimeKey(deviceCode), "last_report_at_ms", lastReportText,
                 "online_until_ms", onlineUntilText, "state", std::string(state), "state_reason",
                 std::string(reason), "updated_at_ms", now});
            std::vector<std::string_view> views(reportCommands.back().begin(),
                                                reportCommands.back().end());
            reportPipeline.command(views);
            reportCommands.push_back({"HSET", latestKey(deviceCode), "_state",
                                      stateJson(state, reason, lastReportText, onlineUntilText, now),
                                      "_updated_at_ms", now});
            std::vector<std::string_view> latestViews(reportCommands.back().begin(),
                                                      reportCommands.back().end());
            reportPipeline.command(latestViews);
            reportCommands.push_back({"ZADD", std::string(kOnlineDeadlinesKey),
                                      std::to_string(onlineUntil), deviceCode});
            std::vector<std::string_view> deadlineViews(reportCommands.back().begin(),
                                                        reportCommands.back().end());
            reportPipeline.command(deadlineViews);
        }
        (void)co_await std::move(reportPipeline).exec();
    }
}

template <typename Context> ruvia::Task<void> projectDevice(Context& context, std::string_view id) {
    co_await project(context, " AND d.id = $1::uuid", service::common::dbParams(id), false);
}

template <typename Context>
ruvia::Task<void> projectProtocol(Context& context, std::string_view id) {
    co_await project(context, " AND d.protocol_config_id = $1::uuid",
                     service::common::dbParams(id), false);
}

inline ruvia::Task<void> hydrate(ruvia::WebWorkerContext& context) {
    const auto redis = context.redis();
    co_await service::message::redis::eraseMatching(redis, "iot:device:*:latest");
    co_await service::message::redis::eraseMatching(redis, "iot:runtime:device:*");
    co_await service::message::redis::erase(redis, kOnlineDeadlinesKey);
    co_await project(context, "", {}, true);
}

} // namespace service::telemetry::latest
