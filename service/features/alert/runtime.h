#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/core/Timer.h>
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
      const std::vector<service::message::ParsedDeviceMessage> &messages) {
    if (messages.empty())
      co_return;

    const auto active = co_await metadata::activeDevices(context, messages);
    std::vector<service::message::ParsedDeviceMessage> relevant;
    relevant.reserve(messages.size());
    for (std::size_t index = 0; index < messages.size(); ++index)
      if (index < active.size() && active[index])
        relevant.push_back(messages[index]);
    if (relevant.empty())
      co_return;

    std::vector<ruvia::DbValue> params;
    const auto rules = co_await context.db().query(
        telemetryEvaluationSql(relevant, params), params);
    std::vector<std::vector<Evaluation>> evaluations(relevant.size());
    for (const auto &row : rules.rows()) {
      const auto sequence =
          static_cast<std::size_t>(std::stoull(std::string(row[0].text())));
      if (sequence >= relevant.size())
        continue;
      evaluations[sequence].push_back(
          evaluation(row, 1, relevant[sequence].valuesJson));
    }
    for (std::size_t sequence = 0; sequence < relevant.size(); ++sequence) {
      co_await applyEvaluations(context, evaluations[sequence],
                                relevant[sequence].observedAtMs);
    }
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
      ready->set_value();
      while (running_.load() && !context.stopToken().stopRequested()) {
        try {
          if (co_await metadata::hasOfflineRules(context)) {
            const auto rules =
                co_await context.db().query(offlineEvaluationSql());
            co_await apply(context, rules, "{}", nowMilliseconds());
          }
        } catch (const std::exception &error) {
          std::cerr << "alert offline evaluation failed: " << error.what()
                    << '\n';
        }
        (void)co_await ruvia::sleepFor(context.worker(),
                                       std::chrono::seconds(10));
      }
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
    co_await applyEvaluations(context, evaluations, occurredAtMs);
  }

  static ruvia::Task<void>
  applyEvaluations(ruvia::WebWorkerContext &context,
                   const std::vector<Evaluation> &evaluations,
                   std::int64_t occurredAtMs) {
    constexpr std::size_t kBatchSize = 256;
    for (std::size_t begin = 0; begin < evaluations.size();
         begin += kBatchSize) {
      co_await applyBatch(context, evaluations, begin,
                          std::min(begin + kBatchSize, evaluations.size()),
                          occurredAtMs);
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
             std::size_t end, std::int64_t occurredAtMs) {
    if (begin >= end)
      co_return;

    std::string sql = R"sql(
WITH incoming(
  rule_id, matched, record_id, device_id, severity, message, detail,
  silence_duration, recovery_condition, recovery_wait_seconds) AS (VALUES )sql";
    std::vector<ruvia::DbValue> params;
    params.reserve((end - begin) * 10);
    std::unordered_map<std::string_view, const Evaluation *> byRuleId;
    byRuleId.reserve(end - begin);
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
             "::integer)";
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
      byRuleId.emplace(evaluation.ruleId, &evaluation);
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
)
SELECT 'created', id::text, rule_id::text FROM created
UNION ALL
SELECT 'resolved', id::text, rule_id::text FROM resolved)sql";

    const auto events = co_await context.db().query(sql, params);
    for (const auto &event : events.rows()) {
      const auto action = event[0].text();
      const auto recordId = std::string(event[1].text());
      const auto ruleId = event[2].text();
      const auto found = byRuleId.find(ruleId);
      if (found == byRuleId.end())
        continue;
      const auto &evaluation = *found->second;
      const auto created = action == "created";
      const auto status =
          created ? std::string_view("active") : std::string_view("resolved");
      const auto payload =
          alertEventJson(recordId, evaluation.ruleId, evaluation.ruleName,
                         evaluation.severity, status, evaluation.data);
      co_await service::access::event::publish(
          context.redis(), recordId,
          created ? "device.alert.triggered" : "device.alert.resolved",
          evaluation.deviceId, evaluation.deviceCode, occurredAtMs, payload);
    }
  }

  static std::string
  alertEventJson(std::string_view recordId, std::string_view ruleId,
                 std::string_view ruleName, std::string_view severity,
                 std::string_view status, std::string_view data) {
    return "{\"alert\":{\"id\":" + service::access::jsonQuoted(recordId) +
           ",\"ruleId\":" + service::access::jsonQuoted(ruleId) +
           ",\"ruleName\":" + service::access::jsonQuoted(ruleName) +
           ",\"severity\":" + service::access::jsonQuoted(severity) +
           ",\"status\":" + service::access::jsonQuoted(status) +
           "},\"values\":" + std::string(data) + "}";
  }

  static std::int64_t nowMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  static std::string offlineEvaluationSql() {
    const std::string input = R"sql(
WITH rules AS (
  SELECT 0::bigint AS input_sequence, rule.*, device.name AS device_name,
         device.protocol_params->>'device_code' AS device_code
  FROM alert_rule rule JOIN device ON device.id = rule.device_id
  WHERE rule.deleted_at IS NULL AND rule.status = 'enabled'
    AND device.deleted_at IS NULL AND device.status = 'enabled'
), rule_devices AS (
  SELECT DISTINCT device_id FROM rules
), device_samples AS (
  SELECT rule_devices.device_id,
         history.data,
         history.observed_at,
         history.previous_data
  FROM rule_devices
  LEFT JOIN LATERAL (
    SELECT
      (array_agg(recent.data ORDER BY recent.report_time DESC))[1] AS data,
      (array_agg(recent.report_time ORDER BY recent.report_time DESC))[1]
        AS observed_at,
      (array_agg(recent.data ORDER BY recent.report_time DESC))[2]
        AS previous_data
    FROM (
      SELECT data, report_time
      FROM device_data
      WHERE device_id = rule_devices.device_id
      ORDER BY report_time DESC
      LIMIT 2
    ) recent
  ) history ON TRUE
), samples AS (
  SELECT rules.*,
         device_samples.data,
         device_samples.observed_at,
         device_samples.previous_data
  FROM rules
  JOIN device_samples ON device_samples.device_id = rules.device_id
)
)sql";
    return input + evaluationTail();
  }

  static std::string telemetryEvaluationSql(
      const std::vector<service::message::ParsedDeviceMessage> &messages,
      std::vector<ruvia::DbValue> &params) {
    std::string sql =
        "WITH input(input_sequence, device_id, data, observed_at_ms) AS "
        "(VALUES ";
    params.clear();
    params.reserve(messages.size() * 4);
    for (std::size_t index = 0; index < messages.size(); ++index) {
      if (index != 0)
        sql.push_back(',');
      const auto base = params.size() + 1;
      sql += "($" + std::to_string(base) + "::bigint,$" +
             std::to_string(base + 1) + "::uuid,$" + std::to_string(base + 2) +
             "::jsonb,$" + std::to_string(base + 3) + "::bigint)";
      params.emplace_back(static_cast<std::int64_t>(index));
      params.emplace_back(std::string_view(messages[index].deviceId));
      params.emplace_back(std::string_view(messages[index].valuesJson));
      params.emplace_back(messages[index].observedAtMs);
    }
    sql += R"sql(), input_samples AS (
  SELECT input.*, previous.data AS previous_data
  FROM input
  LEFT JOIN LATERAL (
    SELECT data FROM device_data
    WHERE device_id = input.device_id
      AND report_time <
          to_timestamp(input.observed_at_ms::double precision / 1000.0)
    ORDER BY report_time DESC LIMIT 1
  ) previous ON TRUE
), rules AS (
  SELECT input_samples.input_sequence, rule.*, device.name AS device_name,
         device.protocol_params->>'device_code' AS device_code,
         input_samples.data AS input_data,
         input_samples.observed_at_ms,
         input_samples.previous_data
  FROM input_samples
  JOIN alert_rule rule ON rule.device_id = input_samples.device_id
  JOIN device ON device.id = rule.device_id
  WHERE rule.deleted_at IS NULL AND rule.status = 'enabled'
    AND device.deleted_at IS NULL AND device.status = 'enabled'
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
