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

#include "service/common/bridge/message.contract.h"
#include "service/modules/southbridge/queue/redis_stream_async.h"

namespace service::northbridge::telemetry::latest {

inline constexpr std::string_view kOnlineDeadlinesKey = "iot:schedule:device:online-deadlines";

inline std::string elementKey(std::string_view deviceCode, std::string_view elementId) {
    return "iot:latest:device:" + std::string(deviceCode) + ":element:" + std::string(elementId);
}

inline std::string stateKey(std::string_view deviceCode) {
    return "iot:state:device:" + std::string(deviceCode);
}

template <typename Redis>
ruvia::Task<void> initializeDevice(const Redis& redis, std::string_view deviceId,
                                   std::string_view deviceCode) {
    co_await service::bridge::redis_async::setHash(
        redis, stateKey(deviceCode),
        {{"device_id", std::string(deviceId)},
         {"device_code", std::string(deviceCode)},
         {"state", "offline"},
         {"state_reason", "no_connection"},
         {"updated_at_ms", std::to_string(service::bridge::utcNowMilliseconds())}});
}

template <typename Redis>
ruvia::Task<void> eraseDevice(const Redis& redis, std::string_view deviceCode) {
    co_await service::bridge::redis_async::eraseHash(redis, stateKey(deviceCode));
    co_await service::bridge::redis_async::eraseMatching(
        redis, "iot:latest:device:" + std::string(deviceCode) + ":element:*");
}

template <typename Redis>
ruvia::Task<void> update(const Redis& redis,
                         const std::vector<service::bridge::StreamMessage>& messages) {
    if (messages.empty())
        co_return;
    static constexpr std::string_view script = R"lua(
local function number_or(value, fallback)
  local number = tonumber(value)
  if number == nil then return fallback end
  return number
end
local payload = cjson.decode(ARGV[8])
local state_key = 'iot:state:device:' .. ARGV[2]
if redis.call('HGET', state_key, 'device_id') ~= ARGV[1] then
  return -1
end
local observed_at = number_or(ARGV[4], 0)
local online_until = observed_at + number_or(ARGV[7], 300000)
local now = number_or(ARGV[5], observed_at)
local current_report = number_or(redis.call('HGET', state_key, 'last_report_at_ms'), -1)
if observed_at >= current_report then
  local state = 'online'
  local reason = ''
  if online_until < now then
    state = 'offline'
    reason = 'data_stale'
  end
  redis.call('HSET', state_key,
    'device_id', ARGV[1], 'device_code', ARGV[2],
    'last_report_at_ms', ARGV[4], 'online_until_ms', tostring(online_until),
    'state', state, 'state_reason', reason, 'updated_at_ms', ARGV[5])
  redis.call('ZADD', 'iot:schedule:device:online-deadlines', online_until, ARGV[2])
end
local count = 0
for element_id, point in pairs(payload.values or {}) do
  local key = 'iot:latest:device:' .. ARGV[2] .. ':element:' .. element_id
  local value = '-'
  local document_value = '-'
  if point.value ~= nil and point.value ~= cjson.null then
    value = tostring(point.value)
    document_value = point.value
  end
  local current = number_or(redis.call('HGET', key, 'observed_at_ms'), -1)
  if observed_at >= current then
    local document = cjson.encode({
      element_id = element_id,
      element_name = tostring(point.name or element_id),
      value = document_value,
      unit = tostring(point.unit or ''),
      protocol = ARGV[3],
      observed_at_ms = observed_at,
      updated_at_ms = number_or(ARGV[5], observed_at),
      source = ARGV[6]
    })
    redis.call('HSET', key,
      'element_id', element_id,
      'element_name', tostring(point.name or element_id),
      'value', value,
      'unit', tostring(point.unit or ''),
      'protocol', ARGV[3],
      'observed_at_ms', ARGV[4],
      'updated_at_ms', ARGV[5],
      'source', ARGV[6],
      'data', document)
    count = count + 1
  end
end
return count
)lua";
    auto pipeline = redis.pipeline();
    std::vector<std::vector<std::string>> commands;
    commands.reserve(messages.size());
    for (const auto& message : messages) {
        const auto parsed = service::bridge::parsedFrom(message);
        commands.push_back({"EVAL", std::string(script), "0", parsed.deviceId, parsed.deviceCode,
                            parsed.protocol, std::to_string(parsed.observedAtMs),
                            std::to_string(service::bridge::utcNowMilliseconds()), parsed.source,
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
            service::bridge::redis_async::throwValue("device latest update", reply);
}

template <typename Redis> ruvia::Task<void> expireStale(const Redis& redis) {
    const auto now = std::to_string(service::bridge::utcNowMilliseconds());
    const auto due = co_await service::bridge::redis_async::command(
        redis,
        {"ZRANGEBYSCORE", std::string(kOnlineDeadlinesKey), "-inf", now, "LIMIT", "0", "1000"});
    if (due.kind() != ruvia::RedisValue::Kind::kArray)
        service::bridge::redis_async::throwValue("read device online deadlines", due);
    static constexpr std::string_view script = R"lua(
local state_key = 'iot:state:device:' .. ARGV[1]
local expected = tonumber(redis.call('HGET', state_key, 'online_until_ms') or '-1')
local now = tonumber(ARGV[2])
if expected <= now then
  redis.call('HSET', state_key, 'state', 'offline', 'state_reason', 'data_stale',
             'updated_at_ms', ARGV[2])
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

inline ruvia::Task<void> hydrate(ruvia::WebWorkerContext& context) {
    const auto redis = context.redis();
    co_await service::bridge::redis_async::eraseMatching(redis, "iot:latest:device:*");
    co_await service::bridge::redis_async::eraseMatching(redis, "iot:state:device:*");
    co_await service::bridge::redis_async::erase(redis, kOnlineDeadlinesKey);

    const auto devices = co_await context.db().query(R"sql(
SELECT d.id::text, d.protocol_params->>'device_code',
       CASE WHEN p.protocol = 'SL651'
            THEN COALESCE((d.protocol_params->>'online_timeout')::bigint, 300) * 1000
            ELSE COALESCE((p.config->>'readInterval')::numeric,
                          (p.config->>'pollInterval')::numeric, 5) * 3000 END::bigint
FROM device d
JOIN protocol_config p ON p.id = d.protocol_config_id
WHERE d.deleted_at IS NULL ORDER BY d.id)sql");
    auto statePipeline = redis.pipeline();
    std::vector<std::array<std::string, 12>> stateCommands;
    stateCommands.reserve(devices.rows().size());
    const auto now = std::to_string(service::bridge::utcNowMilliseconds());
    std::map<std::string, std::int64_t, std::less<>> onlineWindows;
    for (const auto& row : devices.rows()) {
        stateCommands.push_back({"HSET", stateKey(row[1].text()), "device_id",
                                 std::string(row[0].text()), "device_code",
                                 std::string(row[1].text()), "state", "offline", "state_reason",
                                 "startup", "updated_at_ms", now});
        std::array<std::string_view, 12> views;
        for (std::size_t index = 0; index < views.size(); ++index)
            views[index] = stateCommands.back()[index];
        statePipeline.command(views);
        const std::array<std::string_view, 5> clearViews{
            "HDEL", stateCommands.back()[1], "worker_id", "connection_id", "session_epoch"};
        statePipeline.command(clearViews);
        onlineWindows.insert_or_assign(std::string(row[1].text()),
                                       std::stoll(std::string(row[2].text())));
    }
    if (!stateCommands.empty())
        (void)co_await std::move(statePipeline).exec();

    const auto elements = co_await context.db().query(R"sql(
WITH configured AS (
  SELECT d.id AS device_id, d.protocol_params->>'device_code' AS device_code,
         p.protocol, element,
         1 AS protocol_order, position AS function_order, 0::bigint AS element_order
  FROM device d
  JOIN protocol_config p ON p.id = d.protocol_config_id AND p.protocol = 'Modbus'
  CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'registers', '[]'::jsonb))
    WITH ORDINALITY AS entry(element, position)
  WHERE d.deleted_at IS NULL
  UNION ALL
  SELECT d.id, d.protocol_params->>'device_code', p.protocol,
         element, 2, position, 0::bigint
  FROM device d
  JOIN protocol_config p ON p.id = d.protocol_config_id AND p.protocol = 'S7'
  CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'areas', '[]'::jsonb))
    WITH ORDINALITY AS entry(element, position)
  WHERE d.deleted_at IS NULL
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
  WHERE d.deleted_at IS NULL
)
SELECT configured.device_id::text, configured.device_code, configured.protocol,
       configured.element->>'id', configured.element->>'name',
       COALESCE(configured.element->>'unit', ''), COALESCE(point.value_text, '-'),
       COALESCE((EXTRACT(EPOCH FROM point.observed_at) * 1000)::bigint::text, ''),
       jsonb_build_object(
         'element_id', configured.element->>'id',
         'element_name', configured.element->>'name',
         'value', COALESCE(point.value_json, '"-"'::jsonb),
         'unit', COALESCE(configured.element->>'unit', ''),
         'protocol', configured.protocol,
         'observed_at_ms',
           CASE WHEN point.observed_at IS NULL THEN NULL
                ELSE (EXTRACT(EPOCH FROM point.observed_at) * 1000)::bigint END,
         'updated_at_ms', (EXTRACT(EPOCH FROM clock_timestamp()) * 1000)::bigint,
         'source', CASE WHEN point.observed_at IS NULL THEN 'empty' ELSE 'database' END
       )::text
FROM configured
LEFT JOIN LATERAL (
  SELECT telemetry.report_time AS observed_at,
         telemetry.data->'values'->(configured.element->>'id')->>'value' AS value_text,
         telemetry.data->'values'->(configured.element->>'id')->'value' AS value_json
  FROM device_data telemetry
  WHERE telemetry.device_id = configured.device_id
    AND telemetry.data->'values'->(configured.element->>'id') IS NOT NULL
  ORDER BY telemetry.report_time DESC, telemetry.id DESC
  LIMIT 1
) point ON TRUE
ORDER BY configured.device_id, configured.protocol_order,
         configured.function_order, configured.element_order)sql");
    auto pipeline = redis.pipeline();
    std::vector<std::vector<std::string>> commands;
    commands.reserve(elements.rows().size());
    std::map<std::string, std::int64_t, std::less<>> lastReports;
    for (const auto& row : elements.rows()) {
        commands.push_back({"HSET",           elementKey(row[1].text(), row[3].text()),
                            "element_id",     std::string(row[3].text()),
                            "element_name",   std::string(row[4].text()),
                            "value",          std::string(row[6].text()),
                            "unit",           std::string(row[5].text()),
                            "protocol",       std::string(row[2].text()),
                            "observed_at_ms", std::string(row[7].text()),
                            "updated_at_ms",  now,
                            "source",         row[7].text().empty() ? "empty" : "database",
                            "data",           std::string(row[8].text())});
        std::vector<std::string_view> views;
        views.reserve(commands.back().size());
        for (const auto& argument : commands.back())
            views.push_back(argument);
        pipeline.command(views);
        if (!row[7].text().empty()) {
            auto& lastReport = lastReports[std::string(row[1].text())];
            lastReport = std::max(lastReport, std::stoll(std::string(row[7].text())));
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
            reportCommands.push_back(
                {"HSET", stateKey(deviceCode), "last_report_at_ms", std::to_string(lastReport),
                 "online_until_ms", std::to_string(onlineUntil), "state",
                 online ? "online" : "offline", "state_reason", online ? "" : "data_stale"});
            std::vector<std::string_view> views(reportCommands.back().begin(),
                                                reportCommands.back().end());
            reportPipeline.command(views);
            reportCommands.push_back({"ZADD", std::string(kOnlineDeadlinesKey),
                                      std::to_string(onlineUntil), deviceCode});
            std::vector<std::string_view> deadlineViews(reportCommands.back().begin(),
                                                        reportCommands.back().end());
            reportPipeline.command(deadlineViews);
        }
        (void)co_await std::move(reportPipeline).exec();
    }
}

} // namespace service::northbridge::telemetry::latest
