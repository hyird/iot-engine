#pragma once
#include <ruvia/web/db/DbQuery.h>

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

inline ruvia::DbQuery refreshQuery() {
    using Op = ruvia::DbBinaryOperator;
    using Type = ruvia::DbDataType;
    ruvia::DbQuery duration;
    const auto text = duration.binary(duration.column("value", "condition"), Op::kJsonGetText, duration.value("duration"));
    duration.select(duration.alias(duration.least({ duration.greatest({ duration.cast(text, Type::kBigInt), duration.value(1) }), duration.value(86400) }), "duration_seconds"))
        .andWhere(duration.binary(text, Op::kRegex, duration.value("^[0-9]{1,10}$")));
    ruvia::DbQuery query;
    const auto deviceId = query.cast(query.column("device_id", "rule"), Type::kText);
    const auto ruleId = query.cast(query.column("id", "rule"), Type::kText);
    const auto durationText = query.caseWhen({ { query.unary(ruvia::DbUnaryOperator::kIsNull, query.column("value", "condition")), query.nullValue() } },
        query.cast(query.coalesce({ query.column("duration_seconds", "offline_duration"), query.value(300) }), Type::kText));
    query.select({ deviceId, ruleId, durationText, query.cast(query.coalesce({ query.column("observed_at_ms", "state"), query.value(0) }), Type::kText),
            query.cast(query.column("id", "device"), Type::kText) }).distinct().from("alert_rule", "rule")
        .join(ruvia::DbJoinType::kInner, "device", query.binary(query.column("id", "device"), Op::kEqual, query.column("device_id", "rule")))
        .join(ruvia::DbJoinType::kLeft, "alert_input_state", query.binary(query.column("device_id", "state"), Op::kEqual, query.column("device_id", "rule")), "state")
        .joinFunction(ruvia::DbJoinType::kLeft, query.call("jsonb_array_elements", { query.column("conditions", "rule") }),
            query.binary(query.binary(query.column("value", "condition"), Op::kJsonGetText, query.value("type")), Op::kEqual, query.value("offline")),
            "condition", { .lateral = true, .columns = { { .name = "value" } } })
        .join(ruvia::DbJoinType::kLeft, duration, query.value(true), "offline_duration", { .lateral = true })
        .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull, query.column("deleted_at", "rule")))
        .andWhere(query.binary(query.column("status", "rule"), Op::kEqual, query.value("enabled")))
        .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull, query.column("deleted_at", "device")))
        .andWhere(query.binary(query.column("status", "device"), Op::kEqual, query.value("enabled")))
        .addOrderBy(deviceId).addOrderBy(ruleId).addOrderBy(durationText);
    return query;
}

} // namespace detail

template <typename Context>
ruvia::Task<void> refresh(Context& context) {
    // Serialize the authoritative snapshot and Redis replacement across instances.
    // Telemetry reads never take this cold-path configuration lock.
    auto transaction = co_await context.db().beginTransaction();
    ruvia::DbQuery lock;
    lock.select(lock.call("pg_advisory_xact_lock", { lock.cast(lock.value(734623), ruvia::DbDataType::kBigInt) }));
    (void)co_await transaction.query(lock);
    const auto rows = co_await transaction.query(detail::refreshQuery());

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
    using Query = ruvia::DbQuery;
    using Op = ruvia::DbBinaryOperator;
    using Type = ruvia::DbDataType;
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

        const auto rules = co_await context.db().query(telemetryEvaluationQuery(relevant, relevantPrevious));
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
            const auto rules = co_await context.db().query(offlineEvaluationQuery(uniqueRules));
            co_await apply(context, rules, "{}", now);
        }
        co_await metadata::removeOfflineDeadlines(context.redis(), due);
    }

#ifdef IOT_ENGINE_TESTING
    static std::string evaluationTailForTest() {
        Query query;
        appendEvaluation(query);
        const auto statement = query.compile(ruvia::DbDriver::kPostgreSql, nullptr, ruvia::DbParameterMode::kLiteral);
        return std::string(statement.sql());
    }
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

        Query incoming(context.resource());
        for (auto index = begin; index < end; ++index) {
            const auto& evaluation = evaluations[index];
            incoming.values({
                incoming.cast(incoming.value(std::string_view(evaluation.ruleId)), Type::kUuid),
                incoming.cast(incoming.value(evaluation.matched), Type::kBoolean),
                incoming.cast(incoming.value(std::string_view(evaluation.recordId)), Type::kUuid),
                incoming.cast(incoming.value(std::string_view(evaluation.deviceId)), Type::kUuid),
                incoming.cast(incoming.value(std::string_view(evaluation.severity)), Type::kText),
                incoming.cast(incoming.value(std::string_view(evaluation.message)), Type::kText),
                incoming.cast(incoming.value(std::string_view(evaluation.data)), Type::kJsonb),
                incoming.cast(incoming.value(std::string_view(evaluation.silence)), Type::kInteger),
                incoming.cast(incoming.value(std::string_view(evaluation.recovery)), Type::kText),
                incoming.cast(incoming.value(std::string_view(evaluation.recoveryWait)), Type::kInteger),
                incoming.cast(incoming.value(std::string_view(evaluation.ruleName)), Type::kText),
                incoming.cast(incoming.value(std::string_view(evaluation.deviceCode)), Type::kText),
                incoming.cast(incoming.value(occurredAtMs), Type::kBigInt),
                incoming.cast(incoming.value(receiptId), Type::kText)
            });
        }

        Query stateRows(context.resource());
        const auto initialRecovery = stateRows.caseWhen(
            {{stateRows.column("matched", "incoming"), stateRows.nullValue()}},
            stateRows.call("now"));
        stateRows
            .select({stateRows.column("rule_id", "incoming"),
                     stateRows.column("matched", "incoming"), initialRecovery,
                     stateRows.call("now"), stateRows.call("now")})
            .from("incoming", "incoming");

        Query states(context.resource());
        const auto recoveryStarted = states.caseWhen(
            {{states.excluded("matched"), states.nullValue()},
             {states.column("matched", "alert_rule_state"), states.call("now")}},
            states.coalesce({states.column("recovery_started_at", "alert_rule_state"),
                             states.call("now")}));
        states
            .insertInto("alert_rule_state", {"rule_id", "matched", "recovery_started_at",
                                               "last_evaluated_at", "updated_at"})
            .insertFrom(stateRows)
            .onConflict({.columns = {"rule_id"},
                         .update = {{"recovery_started_at", recoveryStarted},
                                    {"matched", states.excluded("matched")},
                                    {"last_evaluated_at", states.call("now")},
                                    {"updated_at", states.call("now")}}})
            .returning({states.column("rule_id"), states.column("recovery_started_at")});

        Query activeRecord(context.resource());
        activeRecord
            .select(activeRecord.cast(activeRecord.value(std::int64_t{1}), Type::kInteger))
            .from("open_alert_record", "record")
            .andWhere(activeRecord.binary(activeRecord.column("rule_id", "record"), Op::kEqual,
                                          activeRecord.column("rule_id", "incoming")))
            .andWhere(activeRecord.binary(
                activeRecord.column("status", "record"), Op::kIn,
                activeRecord.list({activeRecord.value("active"),
                                   activeRecord.value("acknowledged")})));
        Query recentRecord(context.resource());
        const auto silenceWindow = recentRecord.binary(
            recentRecord.column("silence_duration", "incoming"), Op::kMultiply,
            recentRecord.cast(recentRecord.value("1 second"), Type::kInterval));
        recentRecord
            .select(recentRecord.cast(recentRecord.value(std::int64_t{1}), Type::kInteger))
            .from("open_alert_record", "record")
            .andWhere(recentRecord.binary(recentRecord.column("rule_id", "record"), Op::kEqual,
                                          recentRecord.column("rule_id", "incoming")))
            .andWhere(recentRecord.binary(
                recentRecord.column("triggered_at", "record"), Op::kGreater,
                recentRecord.binary(recentRecord.call("now"), Op::kSubtract, silenceWindow)));

        Query createdRows(context.resource());
        createdRows
            .select({createdRows.column("record_id", "incoming"),
                     createdRows.column("rule_id", "incoming"),
                     createdRows.column("device_id", "incoming"),
                     createdRows.column("severity", "incoming"),
                     createdRows.cast(createdRows.value("active"), Type::kText),
                     createdRows.column("message", "incoming"),
                     createdRows.column("detail", "incoming"), createdRows.call("now")})
            .from("incoming", "incoming")
            .andWhere(createdRows.column("matched", "incoming"))
            .andWhere(createdRows.unary(ruvia::DbUnaryOperator::kNot,
                                        createdRows.exists(activeRecord)))
            .andWhere(createdRows.unary(ruvia::DbUnaryOperator::kNot,
                                        createdRows.exists(recentRecord)));
        Query created(context.resource());
        created
            .insertInto("open_alert_record", {"id", "rule_id", "device_id", "severity",
                                                "status", "message", "detail", "triggered_at"})
            .insertFrom(createdRows)
            .onConflict({.doNothing = true})
            .returning({created.column("id"), created.column("rule_id")});

        Query resolved(context.resource());
        const auto reverseReady = resolved.binary(
            resolved.binary(resolved.column("recovery_condition", "incoming"), Op::kEqual,
                            resolved.value("reverse")),
            Op::kAnd,
            resolved.binary(
                resolved.unary(ruvia::DbUnaryOperator::kIsNotNull,
                               resolved.column("recovery_started_at", "states")),
                Op::kAnd,
                resolved.binary(
                    resolved.column("recovery_started_at", "states"), Op::kLessEqual,
                    resolved.binary(
                        resolved.call("now"), Op::kSubtract,
                        resolved.binary(
                            resolved.column("recovery_wait_seconds", "incoming"), Op::kMultiply,
                            resolved.cast(resolved.value("1 second"), Type::kInterval))))));
        const auto autoSuffix = resolved.call(
            "substring", {resolved.column("recovery_condition", "incoming"),
                          resolved.cast(resolved.value(6), Type::kInteger)});
        const auto autoSeconds = resolved.cast(
            resolved.nullIf(autoSuffix, resolved.cast(resolved.value(""), Type::kText)),
            Type::kInteger);
        const auto autoWait = resolved.coalesce({
            autoSeconds, resolved.cast(resolved.value(0), Type::kInteger)});
        const auto autoWindow = resolved.binary(
            resolved.greatest({resolved.cast(resolved.value(0), Type::kInteger), autoWait}),
            Op::kMultiply, resolved.cast(resolved.value("1 second"), Type::kInterval));
        const auto autoReady = resolved.binary(
            resolved.binary(resolved.column("recovery_condition", "incoming"), Op::kLike,
                            resolved.value("auto_%")),
            Op::kAnd,
            resolved.binary(
                resolved.column("triggered_at", "record"), Op::kLessEqual,
                resolved.binary(resolved.call("now"), Op::kSubtract, autoWindow)));
        resolved
            .update("open_alert_record", "record")
            .set("status", resolved.cast(resolved.value("resolved"), Type::kText))
            .set("resolved_at", resolved.call("now"))
            .set("updated_at", resolved.call("now"))
            .updateFrom("incoming", "incoming")
            .join(ruvia::DbJoinType::kInner, "states",
                  resolved.binary(resolved.column("rule_id", "states"), Op::kEqual,
                                  resolved.column("rule_id", "incoming")),
                  "states")
            .andWhere(resolved.unary(ruvia::DbUnaryOperator::kNot,
                                     resolved.column("matched", "incoming")))
            .andWhere(resolved.binary(resolved.column("rule_id", "record"), Op::kEqual,
                                      resolved.column("rule_id", "incoming")))
            .andWhere(resolved.binary(
                resolved.column("status", "record"), Op::kIn,
                resolved.list({resolved.value("active"), resolved.value("acknowledged")})))
            .andWhere(resolved.binary(reverseReady, Op::kOr, autoReady))
            .returning({resolved.column("id", "record"),
                        resolved.column("rule_id", "incoming")});

        Query changes(context.resource());
        changes
            .select({changes.alias(
                         changes.cast(changes.value("device.alert.triggered"), Type::kText),
                         "event_type"),
                     changes.alias(changes.cast(changes.value("active"), Type::kText), "status"),
                     changes.alias(changes.column("id", "created"), "id"),
                     changes.alias(changes.column("rule_id", "created"), "rule_id")})
            .from("created", "created");
        Query resolvedChanges(context.resource());
        resolvedChanges
            .select({resolvedChanges.cast(resolvedChanges.value("device.alert.resolved"),
                                          Type::kText),
                     resolvedChanges.cast(resolvedChanges.value("resolved"), Type::kText),
                     resolvedChanges.column("id", "resolved"),
                     resolvedChanges.column("rule_id", "resolved")})
            .from("resolved", "resolved");
        changes.combine(ruvia::DbSetOperation::kUnionAll, resolvedChanges);

        Query queuedRows(context.resource());
        const auto jsonKey = [&queuedRows](std::string_view key) {
            return queuedRows.cast(queuedRows.value(key), Type::kText);
        };
        const auto alertObject = queuedRows.call(
            "jsonb_build_object",
            {jsonKey("id"), queuedRows.column("id", "changes"),
             jsonKey("ruleId"), queuedRows.column("rule_id", "incoming"),
             jsonKey("ruleName"), queuedRows.column("rule_name", "incoming"),
             jsonKey("severity"), queuedRows.column("severity", "incoming"),
             jsonKey("status"), queuedRows.column("status", "changes")});
        const auto eventData = queuedRows.call(
            "jsonb_build_object", {jsonKey("alert"), alertObject,
                                   jsonKey("values"), queuedRows.column("detail", "incoming")});
        queuedRows
            .select({queuedRows.column("id", "changes"),
                     queuedRows.column("event_type", "changes"),
                     queuedRows.column("rule_id", "changes"),
                     queuedRows.column("device_id", "incoming"),
                     queuedRows.column("device_code", "incoming"),
                     queuedRows.column("occurred_at_ms", "incoming"), eventData})
            .from("changes", "changes")
            .join(ruvia::DbJoinType::kInner, "incoming",
                  queuedRows.binary(queuedRows.column("rule_id", "incoming"), Op::kEqual,
                                   queuedRows.column("rule_id", "changes")),
                  "incoming");
        Query queued(context.resource());
        queued
            .insertInto("alert_event_outbox", {"event_id", "event_type", "rule_id", "device_id",
                                                 "device_code", "occurred_at_ms", "data"})
            .insertFrom(queuedRows)
            .onConflict({.columns = {"event_id", "event_type"}, .doNothing = true})
            .returning({queued.column("event_id"), queued.column("event_type"),
                        queued.column("rule_id"), queued.column("device_id"),
                        queued.column("device_code"), queued.column("occurred_at_ms"),
                        queued.column("data"), queued.column("created_at")});

        Query queuedBarrier(context.resource());
        queuedBarrier
            .select(queuedBarrier.alias(queuedBarrier.aggregate("count", {queuedBarrier.star()}),
                                        "queued_count"))
            .from("queued");
        Query receiptedRows(context.resource());
        receiptedRows
            .select({receiptedRows.cast(
                         receiptedRows.nullIf(
                             receiptedRows.cast(receiptedRows.column("receipt_id", "incoming"),
                                                Type::kText),
                             receiptedRows.cast(receiptedRows.value(""), Type::kText)),
                         Type::kUuid),
                     receiptedRows.column("device_id", "incoming")})
            .distinct()
            .from("incoming", "incoming")
            .join(ruvia::DbJoinType::kCross, queuedBarrier, {}, "queued_barrier")
            .andWhere(receiptedRows.binary(
                receiptedRows.column("receipt_id", "incoming"), Op::kNotEqual,
                receiptedRows.cast(receiptedRows.value(""), Type::kText)))
            .andWhere(receiptedRows.binary(
                receiptedRows.column("queued_count", "queued_barrier"), Op::kGreaterEqual,
                receiptedRows.cast(receiptedRows.value(std::int64_t{0}), Type::kBigInt)));
        Query receipted(context.resource());
        receipted
            .insertInto("alert_evaluation_receipt", {"message_id", "device_id"})
            .insertFrom(receiptedRows)
            .onConflict({.columns = {"message_id"}, .doNothing = true})
            .returning({receipted.column("message_id")});

        Query receiptedCount(context.resource());
        receiptedCount
            .select(receiptedCount.aggregate("count", {receiptedCount.star()}))
            .from("receipted");
        Query pruned(context.resource());
        pruned
            .deleteFrom("alert_evaluation_receipt")
            .andWhere(pruned.binary(
                pruned.column("created_at"), Op::kLess,
                pruned.binary(pruned.call("now"), Op::kSubtract,
                              pruned.cast(pruned.value("7 days"), Type::kInterval))))
            .andWhere(pruned.binary(pruned.subquery(receiptedCount), Op::kGreaterEqual,
                                    pruned.cast(pruned.value(std::int64_t{0}), Type::kBigInt)))
            .returning({pruned.column("message_id")});

        Query relevant(context.resource());
        relevant
            .select(relevant.column("rule_id", "incoming"))
            .distinct()
            .from("incoming", "incoming");
        Query deliverable(context.resource());
        deliverable
            .select({deliverable.column("event_id", "queued"),
                     deliverable.column("event_type", "queued"),
                     deliverable.column("rule_id", "queued"),
                     deliverable.column("device_id", "queued"),
                     deliverable.column("device_code", "queued"),
                     deliverable.column("occurred_at_ms", "queued"),
                     deliverable.column("data", "queued"),
                     deliverable.column("created_at", "queued")})
            .from("queued", "queued");
        Query existingDeliverable(context.resource());
        existingDeliverable
            .select({existingDeliverable.column("event_id", "outbox"),
                     existingDeliverable.column("event_type", "outbox"),
                     existingDeliverable.column("rule_id", "outbox"),
                     existingDeliverable.column("device_id", "outbox"),
                     existingDeliverable.column("device_code", "outbox"),
                     existingDeliverable.column("occurred_at_ms", "outbox"),
                     existingDeliverable.column("data", "outbox"),
                     existingDeliverable.column("created_at", "outbox")})
            .from("alert_event_outbox", "outbox")
            .join(ruvia::DbJoinType::kInner, "relevant",
                  existingDeliverable.binary(existingDeliverable.column("rule_id", "outbox"),
                                             Op::kEqual,
                                             existingDeliverable.column("rule_id", "relevant")),
                  "relevant");
        deliverable.combine(ruvia::DbSetOperation::kUnionAll, existingDeliverable);

        Query pruneBarrier(context.resource());
        pruneBarrier
            .select(pruneBarrier.alias(pruneBarrier.aggregate("count", {pruneBarrier.star()}),
                                       "pruned_count"))
            .from("pruned");

        Query result(context.resource());
        result
            .with("incoming", incoming,
                  {.columns = {"rule_id", "matched", "record_id", "device_id", "severity",
                               "message", "detail", "silence_duration", "recovery_condition",
                               "recovery_wait_seconds", "rule_name", "device_code",
                               "occurred_at_ms", "receipt_id"}})
            .with("states", states)
            .with("created", created)
            .with("resolved", resolved)
            .with("changes", changes,
                  {.materialization = ruvia::DbMaterialization::kMaterialized})
            .with("queued", queued)
            .with("receipted", receipted)
            .with("pruned", pruned)
            .with("relevant", relevant)
            .with("deliverable", deliverable)
            .with("prune_barrier", pruneBarrier)
            .select({result.cast(result.column("event_id", "deliverable"), Type::kText),
                     result.column("event_type", "deliverable"),
                     result.cast(result.column("device_id", "deliverable"), Type::kText),
                     result.column("device_code", "deliverable"),
                     result.cast(result.column("occurred_at_ms", "deliverable"), Type::kText),
                     result.cast(result.column("data", "deliverable"), Type::kText)})
            .from("deliverable", "deliverable")
            .join(ruvia::DbJoinType::kCross, "prune_barrier", {}, "prune_barrier")
            .andWhere(result.binary(result.column("pruned_count", "prune_barrier"),
                                    Op::kGreaterEqual,
                                    result.cast(result.value(std::int64_t{0}), Type::kBigInt)))
            .orderBy(result.column("created_at", "deliverable"))
            .addOrderBy(result.column("event_id", "deliverable"));
        const auto events = co_await context.db().query(result);
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

        Query remove(context.resource());
        std::vector<Query::Expr> keys;
        keys.reserve(events.size());
        for (const auto& event : events) {
            keys.push_back(remove.tuple({
                remove.cast(remove.value(event[0].value().value_or(std::string_view{})),
                            Type::kUuid),
                remove.cast(remove.value(event[1].value().value_or(std::string_view{})),
                            Type::kText)
            }));
        }
        remove.deleteFrom("alert_event_outbox")
            .andWhere(remove.binary(
                remove.tuple({remove.column("event_id"), remove.column("event_type")}),
                Op::kIn, remove.list(keys)));
        (void)co_await context.db().execute(remove);
    }

    static ruvia::Task<void> drainOutbox(ruvia::WebWorkerContext& context) {
        Query prune(context.resource());
        prune.deleteFrom("alert_evaluation_receipt")
            .andWhere(prune.binary(
                prune.column("created_at"), Op::kLess,
                prune.binary(prune.call("now"), Op::kSubtract,
                             prune.cast(prune.value("7 days"), Type::kInterval))));
        (void)co_await context.db().execute(prune);
        while (true) {
            Query eventsQuery(context.resource());
            eventsQuery
                .select({eventsQuery.cast(eventsQuery.column("event_id"), Type::kText),
                         eventsQuery.column("event_type"),
                         eventsQuery.cast(eventsQuery.column("device_id"), Type::kText),
                         eventsQuery.column("device_code"),
                         eventsQuery.cast(eventsQuery.column("occurred_at_ms"), Type::kText),
                         eventsQuery.cast(eventsQuery.column("data"), Type::kText)})
                .from("alert_event_outbox")
                .orderBy(eventsQuery.column("created_at"))
                .addOrderBy(eventsQuery.column("event_id"))
                .limit(256);
            const auto events = co_await context.db().query(eventsQuery);
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

    static Query offlineEvaluationQuery(const std::set<std::string, std::less<>>& ruleIds) {
        Query requested;
        for (const auto& id : ruleIds) requested.values({ requested.cast(requested.value(id), Type::kUuid) });
        Query rules;
        rules.select({ rules.alias(rules.cast(rules.value(0), Type::kBigInt), "input_sequence"), rules.star("rule"),
            rules.alias(rules.column("name", "device"), "device_name"),
            rules.alias(rules.binary(rules.column("protocol_params", "device"), Op::kJsonGetText, rules.value("device_code")), "device_code") })
            .from("requested").join(ruvia::DbJoinType::kInner, "alert_rule", rules.binary(rules.column("id", "rule"), Op::kEqual, rules.column("rule_id", "requested")), "rule")
            .join(ruvia::DbJoinType::kInner, "device", rules.binary(rules.column("id", "device"), Op::kEqual, rules.column("device_id", "rule")));
        activeRules(rules);
        Query samples;
        samples.select({ samples.star("rules"), samples.column("data", "state"),
                samples.alias(samples.call("to_timestamp", { samples.binary(samples.cast(samples.column("observed_at_ms", "state"), Type::kDouble), Op::kDivide, samples.value(1000.0)) }), "observed_at"), samples.column("previous_data", "state") })
            .from("rules").join(ruvia::DbJoinType::kLeft, "alert_input_state", samples.binary(samples.column("device_id", "state"), Op::kEqual, samples.column("device_id", "rules")), "state");
        Query query;
        query.with("requested", requested, { .columns = { "rule_id" } }).with("rules", rules).with("samples", samples);
        appendEvaluation(query);
        return query;
    }

    static Query telemetryEvaluationQuery(
        const std::vector<const service::message::ParsedDeviceMessage*>& messages,
        const std::vector<std::string_view>& previousData
    ) {
        Query input;
        for (std::size_t i = 0; i < messages.size(); ++i) {
            input.values({ input.cast(input.value(static_cast<std::int64_t>(i)), Type::kBigInt),
                input.cast(input.value(messages[i]->messageId), Type::kUuid), input.cast(input.value(messages[i]->deviceId), Type::kUuid),
                input.cast(input.value(messages[i]->valuesJson), Type::kJsonb), input.cast(input.value(previousData[i]), Type::kJsonb),
                input.cast(input.value(messages[i]->observedAtMs), Type::kBigInt) });
        }
        Query rules;
        rules.select({ rules.column("input_sequence", "input"), rules.star("rule"), rules.alias(rules.column("name", "device"), "device_name"),
                rules.alias(rules.binary(rules.column("protocol_params", "device"), Op::kJsonGetText, rules.value("device_code")), "device_code"),
                rules.alias(rules.column("data", "input"), "input_data"), rules.column("observed_at_ms", "input"), rules.column("previous_data", "input") })
            .from("input").join(ruvia::DbJoinType::kInner, "alert_rule", rules.binary(rules.column("device_id", "rule"), Op::kEqual, rules.column("device_id", "input")), "rule")
            .join(ruvia::DbJoinType::kInner, "device", rules.binary(rules.column("id", "device"), Op::kEqual, rules.column("device_id", "rule")))
            .join(ruvia::DbJoinType::kLeft, "alert_evaluation_receipt", rules.binary(rules.column("message_id", "receipt"), Op::kEqual, rules.column("message_id", "input")), "receipt")
            .andWhere(rules.unary(ruvia::DbUnaryOperator::kIsNull, rules.column("message_id", "receipt")));
        activeRules(rules);
        Query samples;
        samples.select({ samples.star("rules"), samples.alias(samples.column("input_data", "rules"), "data"),
                samples.alias(samples.call("to_timestamp", { samples.binary(samples.cast(samples.column("observed_at_ms", "rules"), Type::kDouble), Op::kDivide, samples.value(1000.0)) }), "observed_at") }).from("rules");
        Query query;
        query.with("input", input, { .columns = { "input_sequence", "message_id", "device_id", "data", "previous_data", "observed_at_ms" } }).with("rules", rules).with("samples", samples);
        appendEvaluation(query);
        return query;
    }

    static void activeRules(Query& query) {
        for (const auto table : { "rule", "device" }) {
            query.andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull, query.column("deleted_at", table)))
                .andWhere(query.binary(query.column("status", table), Op::kEqual, query.value("enabled")));
        }
    }

    static Query::Expr conditionText(Query& query, std::string_view key) {
        return query.binary(query.column("value", "condition"), Op::kJsonGetText, query.value(key));
    }

    static Query::Expr sampleValue(Query& query, std::string_view column) {
        return query.binary(query.binary(query.binary(query.column(column, "samples"), Op::kJsonGet, query.value("values")),
            Op::kJsonGet, conditionText(query, "elementKey")), Op::kJsonGetText, query.value("value"));
    }

    static void appendEvaluation(Query& query) {
        Query numbers;
        const auto duration = conditionText(numbers, "duration");
        const auto threshold = conditionText(numbers, "value");
        const auto rate = conditionText(numbers, "changeRate");
        const auto bit = conditionText(numbers, "bitIndex");
        numbers.select({
            numbers.alias(numbers.caseWhen({ { numbers.binary(duration, Op::kRegex, numbers.value("^[0-9]{1,10}$")),
                numbers.cast(numbers.least({ numbers.greatest({ numbers.cast(duration, Type::kBigInt), numbers.value(1) }), numbers.value(86400) }), Type::kInteger) } }), "duration_seconds"),
            numbers.alias(numbers.caseWhen({ { numbers.binary(numbers.binary(numbers.call("length", { numbers.coalesce({ threshold, numbers.value("") }) }), Op::kLessEqual, numbers.value(64)), Op::kAnd,
                numbers.binary(threshold, Op::kRegex, numbers.value("^-?[0-9]+([.][0-9]+)?$"))), numbers.cast(threshold, Type::kNumeric) } }), "threshold_value"),
            numbers.alias(numbers.caseWhen({
                { numbers.binary(numbers.unary(ruvia::DbUnaryOperator::kIsNull, rate), Op::kOr, numbers.binary(rate, Op::kEqual, numbers.value(""))), numbers.cast(numbers.value(0), Type::kNumeric) },
                { numbers.binary(numbers.binary(numbers.call("length", { rate }), Op::kLessEqual, numbers.value(64)), Op::kAnd,
                    numbers.binary(rate, Op::kRegex, numbers.value("^[0-9]+([.][0-9]+)?$"))), numbers.cast(rate, Type::kNumeric) } }), "change_rate"),
            numbers.alias(numbers.caseWhen({ { numbers.binary(bit, Op::kRegex, numbers.value("^([0-9]|[1-5][0-9]|6[0-2])$")), numbers.cast(bit, Type::kInteger) } }), "bit_index") });
        Query current;
        const auto raw = sampleValue(current, "data");
        const auto hasBit = current.binary(current.column("value", "condition"), Op::kJsonHasKey, current.value("bitIndex"));
        const auto validBit = current.binary(current.binary(hasBit, Op::kAnd, current.unary(ruvia::DbUnaryOperator::kIsNotNull, current.column("bit_index", "condition_number"))),
            Op::kAnd, current.binary(raw, Op::kRegex, current.value("^-?[0-9]+$")));
        current.select({ current.alias(current.caseWhen({
                { validBit, current.cast(current.binary(current.call("int8shr", { current.cast(raw, Type::kBigInt), current.column("bit_index", "condition_number") }), Op::kBitAnd, current.cast(current.value(1), Type::kBigInt)), Type::kText) },
                { hasBit, current.nullValue() } }, raw), "text_value"),
            current.alias(current.caseWhen({ { current.binary(raw, Op::kRegex, current.value("^-?[0-9]+([.][0-9]+)?$")), current.cast(raw, Type::kNumeric) } }), "numeric_value") });
        Query previous;
        const auto priorRaw = sampleValue(previous, "previous_data");
        previous.select(previous.alias(previous.caseWhen({ { previous.binary(priorRaw, Op::kRegex, previous.value("^-?[0-9]+([.][0-9]+)?$")), previous.cast(priorRaw, Type::kNumeric) } }), "numeric_value"));
        Query conditions;
        const auto numeric = conditions.column("numeric_value", "current_value");
        const auto priorNumeric = conditions.column("numeric_value", "previous_value");
        const auto expected = conditions.column("threshold_value", "condition_number");
        const auto changeRate = conditions.column("change_rate", "condition_number");
        std::vector<ruvia::DbCaseBranch> thresholdCases;
        for (const auto& [name, op] : std::array<std::pair<std::string_view, Op>, 4>{ { { ">", Op::kGreater }, { ">=", Op::kGreaterEqual }, { "<", Op::kLess }, { "<=", Op::kLessEqual } } }) {
            thresholdCases.push_back({ conditions.binary(conditionText(conditions, "operator"), Op::kEqual, conditions.value(name)), conditions.coalesce({ conditions.binary(numeric, op, expected), conditions.value(false) }) });
        }
        for (const auto& [name, op] : std::array<std::pair<std::string_view, Op>, 2>{ { { "==", Op::kEqual }, { "!=", Op::kNotEqual } } }) {
            thresholdCases.push_back({ conditions.binary(conditionText(conditions, "operator"), Op::kEqual, conditions.value(name)), conditions.binary(conditions.column("text_value", "current_value"), op, conditionText(conditions, "value")) });
        }
        const auto delta = conditions.binary(conditions.call("abs", { conditions.binary(conditions.binary(conditions.binary(numeric, Op::kSubtract, priorNumeric), Op::kDivide, priorNumeric), Op::kMultiply, conditions.value(100)) }), Op::kGreaterEqual, changeRate);
        const auto missingRate = conditions.binary(conditions.binary(conditions.unary(ruvia::DbUnaryOperator::kIsNull, priorNumeric), Op::kOr, conditions.binary(priorNumeric, Op::kEqual, conditions.value(0))), Op::kOr,
            conditions.binary(conditions.unary(ruvia::DbUnaryOperator::kIsNull, numeric), Op::kOr, conditions.unary(ruvia::DbUnaryOperator::kIsNull, changeRate)));
        const auto direction = conditions.coalesce({ conditionText(conditions, "changeDirection"), conditions.value("any") });
        const auto rateMatched = conditions.caseWhen({
            { missingRate, conditions.value(false) },
            { conditions.binary(direction, Op::kEqual, conditions.value("rise")), conditions.binary(conditions.binary(numeric, Op::kGreater, priorNumeric), Op::kAnd, delta) },
            { conditions.binary(direction, Op::kEqual, conditions.value("fall")), conditions.binary(conditions.binary(numeric, Op::kLess, priorNumeric), Op::kAnd, delta) } }, delta);
        const auto offline = conditions.binary(conditions.unary(ruvia::DbUnaryOperator::kIsNull, conditions.column("observed_at", "samples")), Op::kOr,
            conditions.binary(conditions.column("observed_at", "samples"), Op::kLess, conditions.binary(conditions.call("now"), Op::kSubtract,
                conditions.binary(conditions.coalesce({ conditions.column("duration_seconds", "condition_number"), conditions.value(300) }), Op::kMultiply, conditions.cast(conditions.value("1 second"), Type::kInterval)))));
        const auto kind = conditionText(conditions, "type");
        conditions.select({ conditions.star("samples"), conditions.alias(conditions.column("value", "condition"), "condition"),
                conditions.alias(conditions.caseWhen({ { conditions.binary(kind, Op::kEqual, conditions.value("offline")), offline },
                    { conditions.binary(kind, Op::kEqual, conditions.value("threshold")), conditions.caseWhen(thresholdCases, conditions.value(false)) },
                    { conditions.binary(kind, Op::kEqual, conditions.value("rate_of_change")), rateMatched } }, conditions.value(false)), "condition_matched") })
            .from("samples").joinFunction(ruvia::DbJoinType::kCross, conditions.call("jsonb_array_elements", { conditions.column("conditions", "samples") }), {}, "condition", { .lateral = true, .columns = { { .name = "value" } } })
            .join(ruvia::DbJoinType::kLeft, numbers, conditions.value(true), "condition_number", { .lateral = true })
            .join(ruvia::DbJoinType::kLeft, current, conditions.value(true), "current_value", { .lateral = true })
            .join(ruvia::DbJoinType::kLeft, previous, conditions.value(true), "previous_value", { .lateral = true });
        Query evaluated;
        for (const auto name : { "input_sequence", "id", "name", "severity", "device_id", "device_code", "silence_duration", "recovery_condition", "recovery_wait_seconds", "data" }) {
            evaluated.addSelect(evaluated.column(name)).addGroupBy(evaluated.column(name));
        }
        evaluated.addSelect(evaluated.alias(evaluated.caseWhen({ { evaluated.binary(evaluated.column("logic"), Op::kEqual, evaluated.value("and")), evaluated.aggregate("bool_and", { evaluated.column("condition_matched") }) } }, evaluated.aggregate("bool_or", { evaluated.column("condition_matched") })), "matched"))
            .from("conditions").addGroupBy(evaluated.column("logic"));
        query.with("conditions", conditions).with("evaluated", evaluated).select({ query.column("input_sequence"), query.cast(query.column("id"), Type::kText), query.column("name"), query.column("severity"),
            query.cast(query.column("device_id"), Type::kText), query.coalesce({ query.column("device_code"), query.value("") }), query.column("matched"), query.column("silence_duration"), query.column("recovery_condition"), query.column("recovery_wait_seconds"),
            query.cast(query.coalesce({ query.column("data"), query.cast(query.value("{}"), Type::kJsonb) }), Type::kText) }).from("evaluated").addOrderBy(query.column("input_sequence")).addOrderBy(query.column("id"));
    }
};

} // namespace service::alert
