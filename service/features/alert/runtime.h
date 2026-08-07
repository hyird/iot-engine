#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/web/WebWorker.h>

#include "service/common/message/contract.h"
#include "service/common/uuid.h"
#include "service/features/access/contract.h"
#include "service/features/access/event.h"
#include "service/features/alert/metadata.h"

namespace service::alert {

class Runtime final {
public:
  Runtime() = default;
  Runtime(const Runtime &) = delete;
  Runtime &operator=(const Runtime &) = delete;
  ~Runtime() { stop(); }

  void start(ruvia::WebWorkerHandle worker) {
    if (running_.exchange(true))
      return;
    worker_ = std::move(worker);
    auto ready = std::make_shared<std::promise<void>>();
    auto stopped = std::make_shared<std::promise<void>>();
    stopped_ = stopped->get_future().share();
    auto readiness = ready->get_future();
    const auto posted =
        worker_.post([this, ready, stopped](ruvia::WebWorkerContext &context) {
          return run(context, ready, stopped);
        });
    if (!posted.accepted()) {
      running_.store(false);
      throw std::runtime_error("service worker rejected alert runtime");
    }
    readiness.get();
  }

  void stop() noexcept {
    if (!running_.exchange(false))
      return;
    if (stopped_.valid())
      (void)stopped_.wait_for(std::chrono::seconds(3));
    stopped_ = {};
    worker_ = {};
  }

  static ruvia::Task<void> evaluateTelemetry(
      ruvia::WebWorkerContext &context,
      const std::vector<service::message::ParsedDeviceMessage> &messages,
      const std::vector<std::string> &previousData,
      const std::vector<bool> &active) {
    if (messages.empty())
      co_return;
    if (messages.size() != previousData.size() || messages.size() != active.size())
      throw std::invalid_argument("alert telemetry batch size mismatch");
    std::vector<const service::message::ParsedDeviceMessage *> relevant;
    std::vector<std::string_view> relevantPrevious;
    relevant.reserve(messages.size());
    relevantPrevious.reserve(messages.size());
    for (std::size_t index = 0; index < messages.size(); ++index) {
      if (index < active.size() && active[index]) {
        relevant.push_back(&messages[index]);
        relevantPrevious.push_back(previousData[index]);
      }
    }
    if (relevant.empty())
      co_return;

    std::vector<ruvia::DbValue> params;
    const auto rules = co_await context.db().query(
        telemetryEvaluationSql(relevant, relevantPrevious, params), params);
    if (rules.rows().empty()) {
      co_await drainOutbox(context);
      co_return;
    }
    std::vector<std::vector<Evaluation>> evaluations(relevant.size());
    for (const auto &row : rules.rows()) {
      const auto sequence =
          static_cast<std::size_t>(std::stoull(std::string(row[0].text())));
      if (sequence >= relevant.size())
        continue;
      evaluations[sequence].push_back(
          evaluation(row, 1, relevant[sequence]->valuesJson));
    }
    for (std::size_t sequence = 0; sequence < relevant.size(); ++sequence) {
      co_await applyEvaluations(context, evaluations[sequence],
                                relevant[sequence]->observedAtMs,
                                relevant[sequence]->messageId);
    }
    co_await drainOutbox(context);
  }

  static ruvia::Task<void>
  evaluateOfflineDue(ruvia::WebWorkerContext &context) {
    const auto now = nowMilliseconds();
    const auto due = co_await metadata::dueOffline(context.redis(), now);
    if (due.empty())
      co_return;
    std::set<std::string, std::less<>> uniqueRules;
    for (const auto &member : due) {
      const auto separator = member.find(':');
      const auto ruleId = member.substr(0, separator);
      if (separator != std::string::npos && service::common::isUuid(ruleId))
        uniqueRules.emplace(ruleId);
    }
    if (!uniqueRules.empty()) {
      std::vector<ruvia::DbValue> params;
      const auto rules = co_await context.db().query(
          offlineEvaluationSql(uniqueRules, params), params);
      co_await apply(context, rules, "{}", now);
    }
    co_await metadata::removeOfflineDeadlines(context.redis(), due);
  }

private:
  struct Evaluation {
    std::string ruleId;
    std::string ruleName;
    std::string severity;
    std::string deviceId;
    std::string deviceCode;
    bool matched{false};
    std::string silence;
    std::string recovery;
    std::string recoveryWait;
    std::string data;
    std::string recordId;
    std::string message;
  };

  ruvia::Task<void> run(ruvia::WebWorkerContext &context,
                        std::shared_ptr<std::promise<void>> ready,
                        std::shared_ptr<std::promise<void>> stopped) {
    try {
      co_await metadata::refresh(context);
      co_await drainOutbox(context);
      ready->set_value();
    } catch (...) {
      try {
        ready->set_exception(std::current_exception());
      } catch (...) {
      }
    }
    try {
      stopped->set_value();
    } catch (...) {
    }
  }

  template <typename Rows>
  static ruvia::Task<void>
  apply(ruvia::WebWorkerContext &context, const Rows &rows,
        std::string_view fallbackData, std::int64_t occurredAtMs) {
    std::vector<Evaluation> evaluations;
    evaluations.reserve(rows.rows().size());
    for (const auto &row : rows.rows())
      evaluations.push_back(evaluation(row, 1, fallbackData));
    co_await applyEvaluations(context, evaluations, occurredAtMs, {});
  }

  static ruvia::Task<void>
  applyEvaluations(ruvia::WebWorkerContext &context,
                   const std::vector<Evaluation> &evaluations,
                   std::int64_t occurredAtMs,
                   std::string_view receiptId) {
    constexpr std::size_t kBatchSize = 256;
    for (std::size_t begin = 0; begin < evaluations.size();
         begin += kBatchSize) {
      co_await applyBatch(context, evaluations, begin,
                          std::min(begin + kBatchSize, evaluations.size()),
                          occurredAtMs,
                          begin + kBatchSize >= evaluations.size() ? receiptId
                                                                   : std::string_view{});
    }
  }

  template <typename Row>
  static Evaluation evaluation(const Row &row, std::size_t offset,
                               std::string_view fallbackData) {
    const auto ruleName = std::string(row[offset + 1].text());
    return Evaluation{
        .ruleId = std::string(row[offset].text()),
        .ruleName = ruleName,
        .severity = std::string(row[offset + 2].text()),
        .deviceId = std::string(row[offset + 3].text()),
        .deviceCode = std::string(row[offset + 4].text()),
        .matched =
            row[offset + 5].text() == "t" || row[offset + 5].text() == "true",
        .silence = std::string(row[offset + 6].text()),
        .recovery = std::string(row[offset + 7].text()),
        .recoveryWait = std::string(row[offset + 8].text()),
        .data = row[offset + 9].isNull() ? std::string(fallbackData)
                                         : std::string(row[offset + 9].text()),
        .recordId = service::common::nextUuidV7(),
        .message = ruleName + " 触发告警",
    };
  }

  static ruvia::Task<void>
  applyBatch(ruvia::WebWorkerContext &context,
             const std::vector<Evaluation> &evaluations, std::size_t begin,
             std::size_t end, std::int64_t occurredAtMs,
             std::string_view receiptId) {
    if (begin >= end)
      co_return;

    std::string sql = R"sql(
WITH incoming(
  rule_id, matched, record_id, device_id, severity, message, detail,
  silence_duration, recovery_condition, recovery_wait_seconds,
  rule_name, device_code, occurred_at_ms, receipt_id) AS (VALUES )sql";
    std::vector<ruvia::DbValue> params;
    params.reserve((end - begin) * 14);
    for (auto index = begin; index < end; ++index) {
      const auto &evaluation = evaluations[index];
      if (index != begin)
        sql.push_back(',');
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
      ruvia::WebWorkerContext &context, const Rows &events) {
    if (events.rows().empty())
      co_return;
    const auto scriptSha = co_await context.redis().scriptLoad(
        service::access::event::kPublishScript);
    auto pipeline = context.redis().pipeline();
    for (const auto &event : events.rows()) {
      const auto occurredAt = service::common::parseInt64(
          std::optional<std::string_view>(event[4].text()));
      if (!occurredAt)
        throw std::runtime_error("invalid alert outbox timestamp");
      service::access::event::queue(
          pipeline, scriptSha, event[0].text(), event[1].text(),
          event[2].text(), event[3].text(), *occurredAt, event[5].text());
    }
    const auto replies = co_await std::move(pipeline).exec();
    service::message::redis::requirePipelineSuccess("publish alert outbox", replies);

    std::string remove = "DELETE FROM alert_event_outbox WHERE (event_id, event_type) IN (";
    std::vector<ruvia::DbValue> params;
    params.reserve(events.rows().size() * 2);
    for (const auto &event : events.rows()) {
      if (!params.empty())
        remove.push_back(',');
      const auto base = params.size() + 1;
      remove += "($" + std::to_string(base) + "::uuid,$" +
                std::to_string(base + 1) + "::text)";
      params.emplace_back(event[0].text());
      params.emplace_back(event[1].text());
    }
    remove.push_back(')');
    (void)co_await context.db().execute(remove, params);
  }

  static ruvia::Task<void> drainOutbox(ruvia::WebWorkerContext &context) {
    (void)co_await context.db().execute(
        "DELETE FROM alert_evaluation_receipt "
        "WHERE created_at < NOW() - interval '7 days'");
    while (true) {
      const auto events = co_await context.db().query(R"sql(
SELECT event_id::text, event_type, device_id::text, device_code,
       occurred_at_ms::text, data::text
FROM alert_event_outbox
ORDER BY created_at, event_id
LIMIT 256)sql");
      if (events.rows().empty())
        co_return;
      co_await publishOutboxRows(context, events);
    }
  }

  static std::int64_t nowMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  static std::string offlineEvaluationSql(
      const std::set<std::string, std::less<>> &ruleIds,
      std::vector<ruvia::DbValue> &params) {
    std::string input = "WITH requested(rule_id) AS (VALUES ";
    params.clear();
    params.reserve(ruleIds.size());
    for (const auto &ruleId : ruleIds) {
      if (!params.empty())
        input.push_back(',');
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
         state.last_data AS data,
         state.last_observed_at AS observed_at,
         state.previous_data
  FROM rules
  LEFT JOIN device_data_ingest_state state ON state.device_id = rules.device_id
)
)sql";
    return input + evaluationTail();
  }

  static std::string telemetryEvaluationSql(
      const std::vector<const service::message::ParsedDeviceMessage *> &messages,
      const std::vector<std::string_view> &previousData,
      std::vector<ruvia::DbValue> &params) {
    std::string sql =
        "WITH input(input_sequence, message_id, device_id, data, previous_data, observed_at_ms) AS "
        "(VALUES ";
    params.clear();
    params.reserve(messages.size() * 6);
    for (std::size_t index = 0; index < messages.size(); ++index) {
      if (index != 0)
        sql.push_back(',');
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
               NOW() - (COALESCE(NULLIF(condition.value->>'duration', '')::integer, 300)
                        * interval '1 second')
           WHEN 'threshold' THEN
             CASE condition.value->>'operator'
               WHEN '>' THEN current_value.numeric_value > NULLIF(condition.value->>'value', '')::numeric
               WHEN '>=' THEN current_value.numeric_value >= NULLIF(condition.value->>'value', '')::numeric
               WHEN '<' THEN current_value.numeric_value < NULLIF(condition.value->>'value', '')::numeric
               WHEN '<=' THEN current_value.numeric_value <= NULLIF(condition.value->>'value', '')::numeric
               WHEN '==' THEN current_value.text_value = condition.value->>'value'
               WHEN '!=' THEN current_value.text_value <> condition.value->>'value'
               ELSE FALSE
             END
           WHEN 'rate_of_change' THEN
             CASE
               WHEN previous_value.numeric_value IS NULL OR previous_value.numeric_value = 0
                    OR current_value.numeric_value IS NULL THEN FALSE
               WHEN COALESCE(condition.value->>'changeDirection', 'any') = 'rise' THEN
                 current_value.numeric_value > previous_value.numeric_value
                 AND ABS((current_value.numeric_value - previous_value.numeric_value)
                         / previous_value.numeric_value * 100)
                     >= COALESCE(NULLIF(condition.value->>'changeRate', '')::numeric, 0)
               WHEN COALESCE(condition.value->>'changeDirection', 'any') = 'fall' THEN
                 current_value.numeric_value < previous_value.numeric_value
                 AND ABS((current_value.numeric_value - previous_value.numeric_value)
                         / previous_value.numeric_value * 100)
                     >= COALESCE(NULLIF(condition.value->>'changeRate', '')::numeric, 0)
               ELSE
                 ABS((current_value.numeric_value - previous_value.numeric_value)
                     / previous_value.numeric_value * 100)
                   >= COALESCE(NULLIF(condition.value->>'changeRate', '')::numeric, 0)
             END
           ELSE FALSE
         END AS condition_matched
  FROM samples
  CROSS JOIN LATERAL jsonb_array_elements(samples.conditions) condition(value)
  LEFT JOIN LATERAL (
    SELECT
      CASE
        WHEN condition.value ? 'bitIndex'
          AND (samples.data->'values'->(condition.value->>'elementKey')->>'value')
              ~ '^-?[0-9]+$'
        THEN (((samples.data->'values'->(condition.value->>'elementKey')->>'value')::bigint
               >> (condition.value->>'bitIndex')::integer) & 1)::text
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

  ruvia::WebWorkerHandle worker_;
  std::shared_future<void> stopped_;
  std::atomic_bool running_{false};
};

} // namespace service::alert
