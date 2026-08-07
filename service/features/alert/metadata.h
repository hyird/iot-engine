#pragma once

#include <cstddef>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/core/Task.h>

#include "service/common/message/contract.h"
#include "service/features/collector/stream.h"

namespace service::alert::metadata {

inline constexpr std::string_view kRuleDevicesKey{"iot:alert:rule-devices"};
inline constexpr std::string_view kOfflineRulesKey{"iot:alert:offline-rules"};
inline constexpr std::string_view kReadyKey{"iot:alert:metadata-ready"};

template <typename Context>
inline ruvia::Task<void> refresh(Context& context) {
    const auto rows = co_await context.db().query(R"sql(
SELECT DISTINCT rule.device_id::text,
       EXISTS (
         SELECT 1
         FROM alert_rule offline
         CROSS JOIN LATERAL jsonb_array_elements(offline.conditions) condition(value)
         WHERE offline.deleted_at IS NULL AND offline.status = 'enabled'
           AND condition.value->>'type' = 'offline'
       )::text
FROM alert_rule rule
WHERE rule.deleted_at IS NULL AND rule.status = 'enabled'
ORDER BY rule.device_id)sql");
    bool offline = false;
    std::vector<std::string> arguments;
    arguments.reserve(rows.rows().size() + 1);
    arguments.emplace_back("0");
    for (const auto& row : rows.rows()) {
        arguments.emplace_back(row[0].text());
        offline = offline || row[1].text() == "t" || row[1].text() == "true";
    }
    arguments.front() = offline ? "1" : "0";
    static constexpr std::string_view script = R"lua(
redis.call('DEL', KEYS[1])
for index = 2, #ARGV do redis.call('SADD', KEYS[1], ARGV[index]) end
redis.call('SET', KEYS[2], ARGV[1])
redis.call('SET', KEYS[3], '1')
return #ARGV - 1
)lua";
    const std::string devicesKey(kRuleDevicesKey);
    const std::string offlineKey(kOfflineRulesKey);
    const std::string readyKey(kReadyKey);
    const std::string_view keys[]{devicesKey, offlineKey, readyKey};
    std::vector<std::string_view> views;
    views.reserve(arguments.size());
    for (const auto& argument : arguments)
        views.push_back(argument);
    const auto reply = co_await context.redis().eval(script, keys, views);
    if (reply.kind() == ruvia::RedisValue::Kind::kError)
        service::message::redis::throwValue("refresh alert metadata", reply);
}

template <typename Context>
inline ruvia::Task<std::vector<bool>> activeDevices(
    Context& context, const std::vector<service::message::ParsedDeviceMessage>& messages) {
    if (messages.empty())
        co_return std::vector<bool>{};
    std::map<std::string, std::vector<std::size_t>, std::less<>> positions;
    for (std::size_t index = 0; index < messages.size(); ++index)
        positions[messages[index].deviceId].push_back(index);
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto pipeline = context.redis().pipeline();
        pipeline.exists(kReadyKey);
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
        std::vector<bool> result(messages.size(), false);
        std::size_t replyIndex = 1;
        for (const auto& [deviceId, indexes] : positions) {
            (void)deviceId;
            const bool active = replyIndex < replies.size() &&
                                replies[replyIndex].kind() == ruvia::RedisValue::Kind::kInteger &&
                                replies[replyIndex].integer() != 0;
            for (const auto index : indexes)
                result[index] = active;
            ++replyIndex;
        }
        co_return result;
    }
    throw std::runtime_error("alert metadata did not become ready");
}

template <typename Context>
inline ruvia::Task<bool> hasOfflineRules(Context& context) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto reply = co_await service::message::redis::command(
            context.redis(), {"GET", std::string(kOfflineRulesKey)});
        if (reply.kind() == ruvia::RedisValue::Kind::kString)
            co_return reply.string() == "1";
        co_await refresh(context);
    }
    throw std::runtime_error("offline alert metadata did not become ready");
}

} // namespace service::alert::metadata
