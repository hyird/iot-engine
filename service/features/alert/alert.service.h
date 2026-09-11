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

#include "service/common/http.h"
#include "service/common/message.h"
#include "service/features/messaging/messaging.transport.h"
#include "service/features/telemetry/latest/latest.service.h"

namespace service::alert::metadata {

template <typename Context>
ruvia::Task<void> refresh(Context& context);

inline constexpr std::string_view kRuleDevicesKey{ "iot:alert:rule-devices" };
inline constexpr std::string_view kOfflineRulesKey{ "iot:alert:offline-rules" };
inline constexpr std::string_view kReadyKey{ "iot:alert:metadata-ready" };
inline constexpr std::string_view kOfflineDurationKeys{ "iot:alert:offline-duration-keys" };
inline constexpr std::string_view kOfflineDurationPrefix{ "iot:alert:offline-duration:" };
inline constexpr std::string_view kOfflineDeadlinesBase{
    "iot:schedule:alert:offline-deadlines"
};

inline std::string offlineDeadlinesKey() {
    return std::string(kOfflineDeadlinesBase);
}

inline std::string offlineDurationKey(std::string_view deviceId) {
    return std::string(kOfflineDurationPrefix) + std::string(deviceId);
}

template <typename Redis>
ruvia::Task<void> schedule(
    const Redis& redis,
    const std::vector<service::message::ParsedDeviceMessage>& messages
) {
    if (messages.empty()) {
        co_return;
    }
    static constexpr std::string_view script = R"lua(
local values = redis.call('HGETALL', KEYS[1])
if #values == 0 then return 0 end
local earliest = redis.call('ZRANGE', KEYS[2], 0, 0, 'WITHSCORES')
local old_earliest = #earliest == 0 and nil or tonumber(earliest[2])
local wake = false
local changed = 0
local observed = tonumber(ARGV[1])
if not observed then return 0 end
for index = 1, #values, 2 do
  local duration = tonumber(values[index + 1])
  if duration then
    local deadline = observed + duration
    local current = redis.call('ZSCORE', KEYS[2], values[index])
    if not current or deadline > tonumber(current) then
      redis.call('ZADD', KEYS[2], deadline, values[index])
      changed = changed + 1
      if not old_earliest or deadline < old_earliest then wake = true end
    end
  end
end
if wake then
  redis.call('XADD', KEYS[3], 'MAXLEN', '~', '100000', '*', 'task', 'freshness-alerts')
end
return changed
)lua";
    const auto scriptSha = co_await redis.scriptLoad(script);
    auto pipeline = redis.pipeline();
    for (const auto& message : messages) {
        const auto durationKey = offlineDurationKey(message.deviceId);
        const auto deadlinesKey = offlineDeadlinesKey();
        const auto wakeStream = service::message::workerWakeStream(std::nullopt);
        const auto observedAt = std::to_string(message.observedAtMs);
        const std::array<std::string_view, 3> keys{ durationKey, deadlinesKey, wakeStream };
        const std::array<std::string_view, 1> arguments{ observedAt };
        service::message::redis::queueEvalSha(pipeline, scriptSha, keys, arguments);
    }
    const auto replies = co_await std::move(pipeline).exec();
    service::message::redis::requirePipelineSuccess("schedule offline alerts", replies);
}

template <typename Redis>
ruvia::Task<std::optional<std::int64_t>> nextOfflineDeadline(
    const Redis& redis
) {
    const auto reply = co_await service::message::redis::command(
        redis,
        { "ZRANGE", offlineDeadlinesKey(), "0", "0", "WITHSCORES" }
    );
    if (reply.kind() != ruvia::RedisValue::Kind::kArray) {
        service::message::redis::throwValue("read next offline-alert deadline", reply);
    }
    if (reply.array().empty()) {
        co_return std::nullopt;
    }
    if (reply.array().size() != 2 ||
        reply.array()[1].kind() != ruvia::RedisValue::Kind::kString) {
        service::message::redis::throwValue("parse next offline-alert deadline", reply);
    }
    const auto score = service::common::parseInt64(
        std::optional<std::string_view>{ reply.array()[1].string() }
    );
    if (!score) {
        throw std::runtime_error("invalid offline-alert deadline score");
    }
    co_return *score;
}

template <typename Redis>
ruvia::Task<std::vector<std::string>> dueOffline(
    const Redis& redis,
    std::int64_t nowMs,
    std::size_t maximum = 1000
) {
    // Read and remove due members in one Redis script. This is the claim point
    // shared by all Service Workers, so only one worker evaluates a deadline.
    static constexpr std::string_view claimScript = R"lua(
local members = redis.call('ZRANGEBYSCORE', KEYS[1], '-inf', ARGV[1],
                           'LIMIT', '0', ARGV[2])
for _, member in ipairs(members) do
  redis.call('ZREM', KEYS[1], member)
end
return members
)lua";
    const auto key = offlineDeadlinesKey();
    const auto now = std::to_string(nowMs);
    const auto limit = std::to_string(maximum);
    const std::string_view keys[]{ key };
    const std::string_view arguments[]{ now, limit };
    const auto reply = co_await redis.eval(claimScript, keys, arguments);
    if (reply.kind() != ruvia::RedisValue::Kind::kArray) {
        service::message::redis::throwValue("read due offline-alert deadlines", reply);
    }
    std::vector<std::string> result;
    result.reserve(reply.array().size());
    for (const auto& value : reply.array()) {
        if (value.kind() != ruvia::RedisValue::Kind::kString) {
            service::message::redis::throwValue("parse due offline-alert deadline", reply);
        }
        result.emplace_back(value.string());
    }
    co_return result;
}

template <typename Redis>
ruvia::Task<void> removeOfflineDeadlines(
    const Redis& redis,
    const std::vector<std::string>& members
) {
    if (members.empty()) {
        co_return;
    }
    std::vector<std::string> command{ "ZREM", offlineDeadlinesKey() };
    command.insert(command.end(), members.begin(), members.end());
    (void)co_await service::message::redis::command(redis, command);
}

struct Activity final {
    std::vector<bool> devices;
    bool offlineRules{ false };
};

template <typename Context>
ruvia::Task<Activity> activity(
    Context& context,
    const std::vector<service::message::ParsedDeviceMessage>& messages
) {
    if (messages.empty()) {
        co_return Activity{};
    }
    std::map<std::string, std::vector<std::size_t>, std::less<>> positions;
    for (std::size_t index = 0; index < messages.size(); ++index) {
        positions[messages[index].deviceId].push_back(index);
    }
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
            for (const auto index : indexes) {
                result.devices[index] = active;
            }
            ++replyIndex;
        }
        co_return result;
    }
    throw std::runtime_error("alert metadata did not become ready");
}

} // namespace service::alert::metadata

namespace service::alert::metadata {

namespace detail {

inline constexpr std::string_view kRefreshQuery = R"sql(
SELECT DISTINCT rule.device_id::text, rule.id::text,
       CASE WHEN condition.value IS NULL THEN NULL ELSE
         COALESCE(offline_duration.duration_seconds, 300)::text
       END,
       COALESCE(state.observed_at_ms, 0)::text,
       device.id::text
FROM alert_rule rule
JOIN device ON device.id = rule.device_id
LEFT JOIN alert_input_state state ON state.device_id = rule.device_id
LEFT JOIN LATERAL jsonb_array_elements(rule.conditions) condition(value)
  ON condition.value->>'type' = 'offline'
LEFT JOIN LATERAL (
  SELECT LEAST(GREATEST((condition.value->>'duration')::bigint, 1), 86400) AS duration_seconds
  WHERE condition.value->>'duration' ~ '^[0-9]{1,10}$'
) offline_duration ON TRUE
WHERE rule.deleted_at IS NULL AND rule.status = 'enabled'
  AND device.deleted_at IS NULL AND device.status = 'enabled'
ORDER BY 1, 2, 3)sql";

} // namespace detail

template <typename Context>
ruvia::Task<void> refresh(Context& context) {
    // Serialize the authoritative snapshot and Redis replacement across instances.
    // Telemetry reads never take this cold-path configuration lock.
    auto transaction = co_await context.db().beginTransaction();
    (void)co_await transaction.query(
        "SELECT pg_advisory_xact_lock(734623::bigint)"
    );
    const auto rows = co_await transaction.query(detail::kRefreshQuery);

    struct OfflineEntry final {
        std::string durationKey;
        std::string member;
        std::int64_t durationMs{ 0 };
        std::int64_t deadlineMs{ 0 };
        std::string deviceCode;
    };

    std::set<std::string, std::less<>> devices;
    std::vector<OfflineEntry> offlineEntries;
    const auto now = service::message::utcNowMilliseconds();
    for (const auto& row : rows) {
        const std::string deviceId(row[0].value().value_or(std::string_view{}));
        devices.emplace(deviceId);
        if (!row[2].value().has_value()) {
            continue;
        }
        const auto durationSeconds =
            service::common::parseInt64(std::optional<std::string_view>{ row[2].value().value_or(std::string_view{}) });
        if (!durationSeconds || *durationSeconds <= 0) {
            continue;
        }
        const auto observedAtMs =
            service::common::parseInt64(std::optional<std::string_view>{ row[3].value().value_or(std::string_view{}) })
                .value_or(0);
        const auto durationMs = *durationSeconds * 1000;
        offlineEntries.push_back(
            { offlineDurationKey(deviceId),
              std::string(row[1].value().value_or(std::string_view{})) + ":" + std::to_string(durationMs),
              durationMs,
              observedAtMs > 0 ? observedAtMs + durationMs : now,
              std::string(row[4].value().value_or(std::string_view{})) }
        );
    }

    std::vector<std::string> arguments;
    arguments.reserve(2 + devices.size() + offlineEntries.size() * 5);
    arguments.push_back(std::to_string(devices.size()));
    for (const auto& deviceId : devices) {
        arguments.push_back(deviceId);
    }
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
local had_deadlines = redis.call('ZCARD', KEYS[5]) > 0
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
    'HGET', 'iot:v2:runtime:device:' .. ARGV[cursor + 4], 'last_report_at_ms')
  local runtime_observed_ms = runtime_observed and tonumber(runtime_observed) or nil
  if runtime_observed_ms then
    deadline = math.max(deadline, runtime_observed_ms + duration)
  end
  redis.call('HSET', duration_key, member, duration)
  redis.call('SADD', KEYS[4], duration_key)
  redis.call('ZADD', KEYS[5], deadline, member)
  cursor = cursor + 5
end
redis.call('SET', KEYS[2], offline_count > 0 and '1' or '0')
redis.call('SET', KEYS[3], '1')
if had_deadlines or offline_count > 0 then
  redis.call('XADD', KEYS[6], 'MAXLEN', '~', '100000', '*',
             'task', 'freshness-alerts')
end
return offline_count
)lua";
    std::vector<std::string> keys{
        std::string(kRuleDevicesKey),
        std::string(kOfflineRulesKey),
        std::string(kReadyKey),
        std::string(kOfflineDurationKeys),
        offlineDeadlinesKey(),
        service::message::workerWakeStream(std::nullopt)
    };
    std::vector<std::string_view> keyViews(keys.begin(), keys.end());
    std::vector<std::string_view> views;
    views.reserve(arguments.size());
    for (const auto& argument : arguments) {
        views.push_back(argument);
    }
    const auto reply = co_await context.redis().eval(script, keyViews, views);
    if (reply.kind() == ruvia::RedisValue::Kind::kError) {
        service::message::redis::throwValue("refresh alert metadata", reply);
    }
    co_await transaction.commit();
}

} // namespace service::alert::metadata

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <utility>

#include <ruvia/web/WebWorker.h>

#include "service/common/uuid.h"
#include "service/features/access/access.transport.h"

namespace service::alert {

class AlertEvaluationService final {
  public:
    static ruvia::Task<void> evaluateTelemetry(
        ruvia::WebWorkerContext& context,
        const std::vector<service::message::ParsedDeviceMessage>& messages,
        const std::vector<std::string>& previousData,
        const std::vector<bool>& active
    ) {
        if (messages.empty()) {
            co_return;
        }
        if (messages.size() != previousData.size() || messages.size() != active.size()) {
            throw std::invalid_argument("alert telemetry batch size mismatch");
        }
        std::vector<const service::message::ParsedDeviceMessage*> relevant;
        std::vector<std::string_view> relevantPrevious;
        relevant.reserve(messages.size());
        relevantPrevious.reserve(messages.size());
        for (std::size_t index = 0; index < messages.size(); ++index) {
            if (index < active.size() && active[index]) {
                relevant.push_back(&messages[index]);
                relevantPrevious.push_back(previousData[index]);
            }
        }
        if (relevant.empty()) {
            co_return;
        }

        std::vector<ruvia::DbValue> params;
        const auto rules = co_await context.db().query(
            telemetryEvaluationSql(relevant, relevantPrevious, params),
            params
        );
        if (rules.empty()) {
            co_await drainOutbox(context);
            co_return;
        }
        std::vector<std::vector<Evaluation>> evaluations(relevant.size());
        for (const auto& row : rules) {
            const auto parsedSequence =
                service::common::parseInt64(std::optional<std::string_view>{ row[0].value().value_or(std::string_view{}) });
            if (!parsedSequence || *parsedSequence < 0) {
                continue;
            }
            const auto sequence = static_cast<std::size_t>(*parsedSequence);
            if (sequence >= relevant.size()) {
                continue;
            }
            evaluations[sequence].push_back(
                evaluation(row, 1, relevant[sequence]->valuesJson)
            );
        }
        for (std::size_t sequence = 0; sequence < relevant.size(); ++sequence) {
            co_await applyEvaluations(context, evaluations[sequence], relevant[sequence]->observedAtMs, relevant[sequence]->messageId);
        }
        co_await drainOutbox(context);
    }

    static ruvia::Task<void>
    evaluateOfflineDue(ruvia::WebWorkerContext& context) {
        const auto now = nowMilliseconds();
        const auto due = co_await metadata::dueOffline(context.redis(), now);
        if (due.empty()) {
            co_return;
        }
        std::set<std::string, std::less<>> uniqueRules;
        for (const auto& member : due) {
            const auto separator = member.find(':');
            const auto ruleId = member.substr(0, separator);
            if (separator != std::string::npos && service::common::isUuid(ruleId)) {
                uniqueRules.emplace(ruleId);
            }
        }
        if (!uniqueRules.empty()) {
            std::vector<ruvia::DbValue> params;
            const auto rules = co_await context.db().query(
                offlineEvaluationSql(uniqueRules, params),
                params
            );
            co_await apply(context, rules, "{}", now);
        }
        co_await metadata::removeOfflineDeadlines(context.redis(), due);
    }

#ifdef IOT_ENGINE_TESTING
    static std::string evaluationTailForTest() { return evaluationTail(); }
#endif
    struct Evaluation {
        std::string ruleId;
        std::string ruleName;
        std::string severity;
        std::string deviceId;
        std::string deviceCode;
        bool matched{ false };
        std::string silence;
        std::string recovery;
        std::string recoveryWait;
        std::string data;
        std::string recordId;
        std::string message;
    };

    template <typename Rows>
    static ruvia::Task<void>
    apply(ruvia::WebWorkerContext& context, const Rows& rows, std::string_view fallbackData, std::int64_t occurredAtMs) {
        std::vector<Evaluation> evaluations;
        evaluations.reserve(rows.size());
        for (const auto& row : rows) {
            evaluations.push_back(evaluation(row, 1, fallbackData));
        }
        co_await applyEvaluations(context, evaluations, occurredAtMs, {});
    }

    static ruvia::Task<void>
    applyEvaluations(ruvia::WebWorkerContext& context, const std::vector<Evaluation>& evaluations, std::int64_t occurredAtMs, std::string_view receiptId) {
        constexpr std::size_t kBatchSize = 256;
        for (std::size_t begin = 0; begin < evaluations.size();
             begin += kBatchSize) {
            co_await applyBatch(context, evaluations, begin, std::min(begin + kBatchSize, evaluations.size()), occurredAtMs, begin + kBatchSize >= evaluations.size() ? receiptId : std::string_view{});
        }
    }

    template <typename Row>
    static Evaluation evaluation(const Row& row, std::size_t offset, std::string_view fallbackData) {
        const auto ruleName = std::string(row[offset + 1].value().value_or(std::string_view{}));
        return Evaluation{
            .ruleId = std::string(row[offset].value().value_or(std::string_view{})),
            .ruleName = ruleName,
            .severity = std::string(row[offset + 2].value().value_or(std::string_view{})),
            .deviceId = std::string(row[offset + 3].value().value_or(std::string_view{})),
            .deviceCode = std::string(row[offset + 4].value().value_or(std::string_view{})),
            .matched =
                row[offset + 5].value().value_or(std::string_view{}) == "t" || row[offset + 5].value().value_or(std::string_view{}) == "true",
            .silence = std::string(row[offset + 6].value().value_or(std::string_view{})),
            .recovery = std::string(row[offset + 7].value().value_or(std::string_view{})),
            .recoveryWait = std::string(row[offset + 8].value().value_or(std::string_view{})),
            .data = !row[offset + 9].value().has_value() ? std::string(fallbackData)
                                                         : std::string(row[offset + 9].value().value_or(std::string_view{})),
            .recordId = service::common::nextUuidV7(),
            .message = ruleName + " 触发告警",
        };
    }

    static ruvia::Task<void>
    applyBatch(ruvia::WebWorkerContext& context, const std::vector<Evaluation>& evaluations, std::size_t begin, std::size_t end, std::int64_t occurredAtMs, std::string_view receiptId) {
        if (begin >= end) {
            co_return;
        }

        std::string sql = R"sql(
WITH incoming(
  rule_id, matched, record_id, device_id, severity, message, detail,
  silence_duration, recovery_condition, recovery_wait_seconds,
  rule_name, device_code, occurred_at_ms, receipt_id) AS (VALUES )sql";
        std::vector<ruvia::DbValue> params;
        params.reserve((end - begin) * 14);
        for (auto index = begin; index < end; ++index) {
            const auto& evaluation = evaluations[index];
            if (index != begin) {
                sql.push_back(',');
            }
            const auto base = params.size() + 1;
            sql += "($" + std::to_string(base) + "::uuid,$" +
                std::to_string(base + 1) + "::boolean,$" +
                std::to_string(base + 2) + "::uuid,$" + std::to_string(base + 3) +
                "::uuid,$" + std::to_string(base + 4) + "::text,$" +
                std::to_string(base + 5) + "::text,$" + std::to_string(base + 6) +
                "::jsonb,$" + std::to_string(base + 7) + "::integer,$" +
                std::to_string(base + 8) + "::text,$" + std::to_string(base + 9) +
                "::integer,$" + std::to_string(base + 10) + "::text,$" +
                std::to_string(base + 11) + "::text,$" +
                std::to_string(base + 12) + "::bigint,$" +
                std::to_string(base + 13) + "::text)";
            params.emplace_back(std::string_view(evaluation.ruleId));
            params.emplace_back(evaluation.matched);
            params.emplace_back(std::string_view(evaluation.recordId));
            params.emplace_back(std::string_view(evaluation.deviceId));
            params.emplace_back(std::string_view(evaluation.severity));
            params.emplace_back(std::string_view(evaluation.message));
            params.emplace_back(std::string_view(evaluation.data));
            params.emplace_back(std::string_view(evaluation.silence));
            params.emplace_back(std::string_view(evaluation.recovery));
            params.emplace_back(std::string_view(evaluation.recoveryWait));
            params.emplace_back(std::string_view(evaluation.ruleName));
            params.emplace_back(std::string_view(evaluation.deviceCode));
            params.emplace_back(occurredAtMs);
            params.emplace_back(receiptId);
        }
        sql += R"sql(), states AS (
  INSERT INTO alert_rule_state(
    rule_id, matched, recovery_started_at, last_evaluated_at, updated_at)
  SELECT incoming.rule_id, incoming.matched,
         CASE WHEN incoming.matched THEN NULL ELSE NOW() END, NOW(), NOW()
  FROM incoming
  ON CONFLICT (rule_id) DO UPDATE SET
    recovery_started_at = CASE
      WHEN EXCLUDED.matched THEN NULL
      WHEN alert_rule_state.matched THEN NOW()
      ELSE COALESCE(alert_rule_state.recovery_started_at, NOW())
    END,
    matched = EXCLUDED.matched,
    last_evaluated_at = NOW(),
    updated_at = NOW()
  RETURNING rule_id, recovery_started_at
), created AS (
  INSERT INTO open_alert_record(
    id, rule_id, device_id, severity, status, message, detail, triggered_at)
  SELECT incoming.record_id, incoming.rule_id, incoming.device_id,
         incoming.severity, 'active', incoming.message, incoming.detail, NOW()
  FROM incoming
  WHERE incoming.matched
    AND NOT EXISTS (
      SELECT 1 FROM open_alert_record record
      WHERE record.rule_id = incoming.rule_id
        AND record.status IN ('active', 'acknowledged'))
    AND NOT EXISTS (
      SELECT 1 FROM open_alert_record record
      WHERE record.rule_id = incoming.rule_id
        AND record.triggered_at >
            NOW() - (incoming.silence_duration * interval '1 second'))
  ON CONFLICT DO NOTHING
  RETURNING id, rule_id
), resolved AS (
  UPDATE open_alert_record record
  SET status = 'resolved', resolved_at = NOW(), updated_at = NOW()
  FROM incoming
  JOIN states ON states.rule_id = incoming.rule_id
  WHERE NOT incoming.matched
    AND record.rule_id = incoming.rule_id
    AND record.status IN ('active', 'acknowledged')
    AND (
      (incoming.recovery_condition = 'reverse'
        AND states.recovery_started_at IS NOT NULL
        AND states.recovery_started_at <=
            NOW() - (incoming.recovery_wait_seconds * interval '1 second'))
      OR
      (incoming.recovery_condition LIKE 'auto_%'
        AND record.triggered_at <= NOW() -
            (GREATEST(
                0,
                COALESCE(
                  NULLIF(substring(incoming.recovery_condition FROM 6), '')::integer,
                  0))
             * interval '1 second'))
    )
  RETURNING record.id, incoming.rule_id
), changes AS MATERIALIZED (
  SELECT 'device.alert.triggered'::text AS event_type,
         'active'::text AS status, id, rule_id FROM created
  UNION ALL
  SELECT 'device.alert.resolved'::text,
         'resolved'::text, id, rule_id FROM resolved
), queued AS (
  INSERT INTO alert_event_outbox(
    event_id, event_type, rule_id, device_id, device_code,
    occurred_at_ms, data)
  SELECT changes.id, changes.event_type, changes.rule_id, incoming.device_id,
         incoming.device_code, incoming.occurred_at_ms,
         jsonb_build_object(
           'alert', jsonb_build_object(
             'id', changes.id, 'ruleId', incoming.rule_id,
             'ruleName', incoming.rule_name, 'severity', incoming.severity,
             'status', changes.status),
           'values', incoming.detail)
  FROM changes JOIN incoming USING (rule_id)
  ON CONFLICT (event_id, event_type) DO NOTHING
  RETURNING event_id, event_type, rule_id, device_id, device_code,
            occurred_at_ms, data, created_at
), receipted AS (
  INSERT INTO alert_evaluation_receipt(message_id, device_id)
  SELECT DISTINCT NULLIF(incoming.receipt_id, '')::uuid, incoming.device_id
  FROM incoming
  CROSS JOIN (SELECT count(*) AS queued_count FROM queued) queued_barrier
  WHERE incoming.receipt_id <> '' AND queued_barrier.queued_count >= 0
  ON CONFLICT (message_id) DO NOTHING
  RETURNING message_id
), pruned AS (
  DELETE FROM alert_evaluation_receipt
  WHERE created_at < NOW() - interval '7 days'
    AND (SELECT count(*) FROM receipted) >= 0
  RETURNING message_id
), relevant AS (
  SELECT DISTINCT rule_id FROM incoming
), deliverable AS (
  SELECT event_id, event_type, rule_id, device_id, device_code,
         occurred_at_ms, data, created_at
  FROM queued
  UNION ALL
  SELECT outbox.event_id, outbox.event_type, outbox.rule_id,
         outbox.device_id, outbox.device_code, outbox.occurred_at_ms,
         outbox.data, outbox.created_at
  FROM alert_event_outbox outbox
  JOIN relevant USING (rule_id)
)
SELECT event_id::text, event_type, device_id::text, device_code,
       occurred_at_ms::text, data::text
FROM deliverable
CROSS JOIN (SELECT count(*) AS pruned_count FROM pruned) prune_barrier
WHERE prune_barrier.pruned_count >= 0
ORDER BY created_at, event_id)sql";

        const auto events = co_await context.db().query(sql, params);
        co_await publishOutboxRows(context, events);
    }

    template <typename Rows>
    static ruvia::Task<void> publishOutboxRows(
        ruvia::WebWorkerContext& context,
        const Rows& events
    ) {
        if (events.empty()) {
            co_return;
        }
        const auto scriptSha = co_await context.redis().scriptLoad(
            service::access::event::kPublishScript
        );
        auto pipeline = context.redis().pipeline();
        for (const auto& event : events) {
            const auto occurredAt = service::common::parseInt64(
                std::optional<std::string_view>(event[4].value().value_or(std::string_view{}))
            );
            if (!occurredAt) {
                throw std::runtime_error("invalid alert outbox timestamp");
            }
            service::access::event::queue(
                pipeline,
                scriptSha,
                event[0].value().value_or(std::string_view{}),
                event[1].value().value_or(std::string_view{}),
                event[2].value().value_or(std::string_view{}),
                event[3].value().value_or(std::string_view{}),
                *occurredAt,
                event[5].value().value_or(std::string_view{})
            );
        }
        const auto replies = co_await std::move(pipeline).exec();
        service::message::redis::requirePipelineSuccess("publish alert outbox", replies);

        std::string remove = "DELETE FROM alert_event_outbox WHERE (event_id, event_type) IN (";
        std::vector<ruvia::DbValue> params;
        params.reserve(events.size() * 2);
        for (const auto& event : events) {
            if (!params.empty()) {
                remove.push_back(',');
            }
            const auto base = params.size() + 1;
            remove += "($" + std::to_string(base) + "::uuid,$" +
                std::to_string(base + 1) + "::text)";
            params.emplace_back(event[0].value().value_or(std::string_view{}));
            params.emplace_back(event[1].value().value_or(std::string_view{}));
        }
        remove.push_back(')');
        (void)co_await context.db().execute(remove, params);
    }

    static ruvia::Task<void> drainOutbox(ruvia::WebWorkerContext& context) {
        (void)co_await context.db().execute(
            "DELETE FROM alert_evaluation_receipt "
            "WHERE created_at < NOW() - interval '7 days'"
        );
        while (true) {
            const auto events = co_await context.db().query(R"sql(
SELECT event_id::text, event_type, device_id::text, device_code,
       occurred_at_ms::text, data::text
FROM alert_event_outbox
ORDER BY created_at, event_id
LIMIT 256)sql");
            if (events.empty()) {
                co_return;
            }
            co_await publishOutboxRows(context, events);
        }
    }

    static std::int64_t nowMilliseconds() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()
        )
            .count();
    }

    static std::string offlineEvaluationSql(
        const std::set<std::string, std::less<>>& ruleIds,
        std::vector<ruvia::DbValue>& params
    ) {
        std::string input = "WITH requested(rule_id) AS (VALUES ";
        params.clear();
        params.reserve(ruleIds.size());
        for (const auto& ruleId : ruleIds) {
            if (!params.empty()) {
                input.push_back(',');
            }
            params.emplace_back(std::string_view(ruleId));
            input += "($" + std::to_string(params.size()) + "::uuid)";
        }
        input += R"sql(), rules AS (
  SELECT 0::bigint AS input_sequence, rule.*, device.name AS device_name,
          device.protocol_params->>'device_code' AS device_code
  FROM requested
  JOIN alert_rule rule ON rule.id = requested.rule_id
  JOIN device ON device.id = rule.device_id
  WHERE rule.deleted_at IS NULL AND rule.status = 'enabled'
    AND device.deleted_at IS NULL AND device.status = 'enabled'
), samples AS (
  SELECT rules.*,
         state.data AS data,
         to_timestamp(state.observed_at_ms / 1000.0) AS observed_at,
         state.previous_data
  FROM rules
  LEFT JOIN alert_input_state state ON state.device_id = rules.device_id
)
)sql";
        return input + evaluationTail();
    }

    static std::string telemetryEvaluationSql(
        const std::vector<const service::message::ParsedDeviceMessage*>& messages,
        const std::vector<std::string_view>& previousData,
        std::vector<ruvia::DbValue>& params
    ) {
        std::string sql =
            "WITH input(input_sequence, message_id, device_id, data, previous_data, observed_at_ms) AS "
            "(VALUES ";
        params.clear();
        params.reserve(messages.size() * 6);
        for (std::size_t index = 0; index < messages.size(); ++index) {
            if (index != 0) {
                sql.push_back(',');
            }
            const auto base = params.size() + 1;
            sql += "($" + std::to_string(base) + "::bigint,$" +
                std::to_string(base + 1) + "::uuid,$" +
                std::to_string(base + 2) + "::uuid,$" + std::to_string(base + 3) +
                "::jsonb,$" + std::to_string(base + 4) + "::jsonb,$" +
                std::to_string(base + 5) + "::bigint)";
            params.emplace_back(static_cast<std::int64_t>(index));
            params.emplace_back(std::string_view(messages[index]->messageId));
            params.emplace_back(std::string_view(messages[index]->deviceId));
            params.emplace_back(std::string_view(messages[index]->valuesJson));
            params.emplace_back(previousData[index]);
            params.emplace_back(messages[index]->observedAtMs);
        }
        sql += R"sql(), rules AS (
  SELECT input.input_sequence, rule.*, device.name AS device_name,
         device.protocol_params->>'device_code' AS device_code,
         input.data AS input_data,
         input.observed_at_ms,
         input.previous_data
  FROM input
  JOIN alert_rule rule ON rule.device_id = input.device_id
  JOIN device ON device.id = rule.device_id
  LEFT JOIN alert_evaluation_receipt receipt ON receipt.message_id = input.message_id
  WHERE rule.deleted_at IS NULL AND rule.status = 'enabled'
    AND device.deleted_at IS NULL AND device.status = 'enabled'
    AND receipt.message_id IS NULL
), samples AS (
  SELECT rules.*, rules.input_data AS data,
         to_timestamp(rules.observed_at_ms::double precision / 1000.0)
           AS observed_at
  FROM rules
)
)sql";
        return sql + evaluationTail();
    }

    static std::string evaluationTail() {
        return R"sql(
, conditions AS (
  SELECT samples.*,
         condition.value AS condition,
	         CASE condition.value->>'type'
	           WHEN 'offline' THEN
	             samples.observed_at IS NULL OR samples.observed_at <
	               NOW() - (COALESCE(condition_number.duration_seconds, 300)
	                        * interval '1 second')
	           WHEN 'threshold' THEN
	             CASE condition.value->>'operator'
	               WHEN '>' THEN COALESCE(current_value.numeric_value > condition_number.threshold_value, FALSE)
	               WHEN '>=' THEN COALESCE(current_value.numeric_value >= condition_number.threshold_value, FALSE)
	               WHEN '<' THEN COALESCE(current_value.numeric_value < condition_number.threshold_value, FALSE)
	               WHEN '<=' THEN COALESCE(current_value.numeric_value <= condition_number.threshold_value, FALSE)
	               WHEN '==' THEN current_value.text_value = condition.value->>'value'
	               WHEN '!=' THEN current_value.text_value <> condition.value->>'value'
	               ELSE FALSE
	             END
	           WHEN 'rate_of_change' THEN
	             CASE
	               WHEN previous_value.numeric_value IS NULL OR previous_value.numeric_value = 0
	                    OR current_value.numeric_value IS NULL
	                    OR condition_number.change_rate IS NULL THEN FALSE
	               WHEN COALESCE(condition.value->>'changeDirection', 'any') = 'rise' THEN
	                 current_value.numeric_value > previous_value.numeric_value
	                 AND ABS((current_value.numeric_value - previous_value.numeric_value)
	                         / previous_value.numeric_value * 100)
	                     >= condition_number.change_rate
	               WHEN COALESCE(condition.value->>'changeDirection', 'any') = 'fall' THEN
	                 current_value.numeric_value < previous_value.numeric_value
	                 AND ABS((current_value.numeric_value - previous_value.numeric_value)
	                         / previous_value.numeric_value * 100)
	                     >= condition_number.change_rate
	               ELSE
	                 ABS((current_value.numeric_value - previous_value.numeric_value)
	                     / previous_value.numeric_value * 100)
	                   >= condition_number.change_rate
	             END
	           ELSE FALSE
	         END AS condition_matched
	  FROM samples
	  CROSS JOIN LATERAL jsonb_array_elements(samples.conditions) condition(value)
	  LEFT JOIN LATERAL (
	    SELECT
	      CASE
	        WHEN condition.value->>'duration' ~ '^[0-9]{1,10}$'
	        THEN LEAST(GREATEST((condition.value->>'duration')::bigint, 1), 86400)::integer
	      END AS duration_seconds,
	      CASE
	        WHEN length(COALESCE(condition.value->>'value', '')) <= 64
	             AND condition.value->>'value' ~ '^-?[0-9]+([.][0-9]+)?$'
	        THEN (condition.value->>'value')::numeric
	      END AS threshold_value,
	      CASE
	        WHEN condition.value->>'changeRate' IS NULL
	             OR condition.value->>'changeRate' = '' THEN 0::numeric
	        WHEN length(condition.value->>'changeRate') <= 64
	             AND condition.value->>'changeRate' ~ '^[0-9]+([.][0-9]+)?$'
	        THEN (condition.value->>'changeRate')::numeric
	      END AS change_rate,
	      CASE
	        WHEN condition.value->>'bitIndex' ~ '^([0-9]|[1-5][0-9]|6[0-2])$'
	        THEN (condition.value->>'bitIndex')::integer
	      END AS bit_index
	  ) condition_number ON TRUE
	  LEFT JOIN LATERAL (
	    SELECT
	      CASE
	        WHEN condition.value ? 'bitIndex'
	          AND condition_number.bit_index IS NOT NULL
	          AND (samples.data->'values'->(condition.value->>'elementKey')->>'value')
	              ~ '^-?[0-9]+$'
	        THEN (((samples.data->'values'->(condition.value->>'elementKey')->>'value')::bigint
	               >> condition_number.bit_index) & 1)::text
	        WHEN condition.value ? 'bitIndex' THEN NULL
	        ELSE samples.data->'values'->(condition.value->>'elementKey')->>'value'
	      END AS text_value,
      CASE
        WHEN (samples.data->'values'->(condition.value->>'elementKey')->>'value')
             ~ '^-?[0-9]+([.][0-9]+)?$'
        THEN (samples.data->'values'->(condition.value->>'elementKey')->>'value')::numeric
      END AS numeric_value
  ) current_value ON TRUE
  LEFT JOIN LATERAL (
    SELECT CASE
      WHEN (samples.previous_data->'values'->(condition.value->>'elementKey')->>'value')
           ~ '^-?[0-9]+([.][0-9]+)?$'
      THEN (samples.previous_data->'values'->(condition.value->>'elementKey')->>'value')::numeric
    END AS numeric_value
  ) previous_value ON TRUE
), evaluated AS (
  SELECT input_sequence, id, name, severity, device_id, device_code,
         silence_duration,
         recovery_condition, recovery_wait_seconds, data,
         CASE WHEN logic = 'and' THEN bool_and(condition_matched)
              ELSE bool_or(condition_matched) END AS matched
  FROM conditions
  GROUP BY input_sequence, id, name, severity, device_id, device_code,
           silence_duration,
           recovery_condition, recovery_wait_seconds, data, logic
)
SELECT input_sequence, id::text, name, severity, device_id::text,
       COALESCE(device_code, ''),
       matched, silence_duration, recovery_condition, recovery_wait_seconds,
       COALESCE(data, '{}'::jsonb)::text
FROM evaluated
ORDER BY input_sequence, id)sql";
    }
};

} // namespace service::alert
