#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/core/Timer.h>
#include <ruvia/web/WebWorker.h>

#include "service/common/message/contract.h"
#include "service/common/uuid.h"
#include "service/features/access/event.h"
#include "service/features/access/contract.h"

namespace service::alert {

class Runtime final {
  public:
    Runtime() = default;
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    ~Runtime() { stop(); }

    void start(ruvia::WebWorkerHandle worker) {
        if (running_.exchange(true))
            return;
        worker_ = std::move(worker);
        auto ready = std::make_shared<std::promise<void>>();
        auto stopped = std::make_shared<std::promise<void>>();
        stopped_ = stopped->get_future().share();
        auto readiness = ready->get_future();
        const auto posted = worker_.post([this, ready, stopped](ruvia::WebWorkerContext& context) {
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
        ruvia::WebWorkerContext& context,
        const std::vector<service::message::ParsedDeviceMessage>& messages) {
        for (const auto& message : messages) {
            const auto rules = co_await context.db().query(
                evaluationSql(false),
                service::common::dbParams(message.deviceId, message.valuesJson,
                                          message.observedAtMs));
            co_await apply(context, rules, message.valuesJson, message.observedAtMs);
        }
    }

  private:
    ruvia::Task<void> run(ruvia::WebWorkerContext& context,
                          std::shared_ptr<std::promise<void>> ready,
                          std::shared_ptr<std::promise<void>> stopped) {
        try {
            ready->set_value();
            while (running_.load() && !context.stopToken().stopRequested()) {
                try {
                    const auto rules = co_await context.db().query(evaluationSql(true));
                    co_await apply(context, rules, "{}", nowMilliseconds());
                } catch (const std::exception& error) {
                    std::cerr << "alert offline evaluation failed: " << error.what() << '\n';
                }
                (void)co_await ruvia::sleepFor(context.worker(), std::chrono::seconds(10));
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
    static ruvia::Task<void> apply(ruvia::WebWorkerContext& context, const Rows& rows,
                                   std::string_view fallbackData, std::int64_t occurredAtMs) {
        for (const auto& row : rows.rows()) {
            const auto ruleId = std::string(row[0].text());
            const auto ruleName = std::string(row[1].text());
            const auto severity = std::string(row[2].text());
            const auto deviceId = std::string(row[3].text());
            const auto deviceCode = std::string(row[4].text());
            const auto matched = row[5].text() == "t" || row[5].text() == "true";
            const auto silence = std::string(row[6].text());
            const auto recovery = std::string(row[7].text());
            const auto recoveryWait = std::string(row[8].text());
            const auto data = row[9].isNull() ? std::string(fallbackData)
                                              : std::string(row[9].text());

            const auto state = co_await context.db().query(R"sql(
INSERT INTO alert_rule_state(
  rule_id, matched, recovery_started_at, last_evaluated_at, updated_at)
VALUES ($1::uuid, $2::boolean, CASE WHEN $2::boolean THEN NULL ELSE NOW() END, NOW(), NOW())
ON CONFLICT (rule_id) DO UPDATE SET
  recovery_started_at = CASE
    WHEN EXCLUDED.matched THEN NULL
    WHEN alert_rule_state.matched THEN NOW()
    ELSE COALESCE(alert_rule_state.recovery_started_at, NOW())
  END,
  matched = EXCLUDED.matched,
  last_evaluated_at = NOW(),
  updated_at = NOW()
RETURNING recovery_started_at::text)sql",
                                                           service::common::dbParams(
                                                               ruleId, matched ? "true" : "false"));
            const auto recoveryStarted =
                state.rows().empty() || state.rows().front()[0].isNull()
                    ? std::string{}
                    : std::string(state.rows().front()[0].text());

            if (matched) {
                const auto recordId = service::common::nextUuidV7();
                const auto alertMessage = ruleName + " 触发告警";
                const auto created = co_await context.db().query(R"sql(
INSERT INTO open_alert_record(
  id, rule_id, device_id, severity, status, message, detail, triggered_at)
SELECT $1::uuid, $2::uuid, $3::uuid, $4, 'active', $5, $6::jsonb, NOW()
WHERE NOT EXISTS (
  SELECT 1 FROM open_alert_record
  WHERE rule_id = $2::uuid AND status IN ('active', 'acknowledged'))
AND NOT EXISTS (
  SELECT 1 FROM open_alert_record
  WHERE rule_id = $2::uuid
    AND triggered_at > NOW() - ($7::integer * interval '1 second'))
RETURNING id::text)sql",
                                                               service::common::dbParams(
                                                                   recordId, ruleId, deviceId,
                                                                   severity, alertMessage, data,
                                                                   silence));
                if (!created.rows().empty()) {
                    const auto payload = alertEventJson(recordId, ruleId, ruleName, severity,
                                                        "active", data);
                    co_await service::access::event::publish(
                        context.redis(), recordId, "device.alert.triggered", deviceId, deviceCode,
                        occurredAtMs, payload);
                }
                continue;
            }

            const auto resolved = co_await context.db().query(R"sql(
UPDATE open_alert_record record
SET status = 'resolved', resolved_at = NOW(), updated_at = NOW()
WHERE record.rule_id = $1::uuid
  AND record.status IN ('active', 'acknowledged')
  AND (
    ($2 = 'reverse' AND NULLIF($3, '')::timestamptz IS NOT NULL
      AND NULLIF($3, '')::timestamptz <=
          NOW() - ($4::integer * interval '1 second'))
    OR
    ($2 LIKE 'auto_%'
      AND record.triggered_at <= NOW() -
          (GREATEST(0, COALESCE(NULLIF(substring($2 FROM 6), '')::integer, 0))
           * interval '1 second'))
  )
RETURNING record.id::text)sql",
                                                              service::common::dbParams(
                                                                  ruleId, recovery, recoveryStarted,
                                                                  recoveryWait));
            for (const auto& resolvedRow : resolved.rows()) {
                const auto recordId = std::string(resolvedRow[0].text());
                const auto payload = alertEventJson(recordId, ruleId, ruleName, severity,
                                                    "resolved", data);
                co_await service::access::event::publish(
                    context.redis(), recordId, "device.alert.resolved", deviceId, deviceCode,
                    occurredAtMs, payload);
            }
        }
    }

    static std::string alertEventJson(std::string_view recordId, std::string_view ruleId,
                                      std::string_view ruleName, std::string_view severity,
                                      std::string_view status, std::string_view data) {
        return "{\"alert\":{\"id\":" + service::access::jsonQuoted(recordId) +
               ",\"ruleId\":" + service::access::jsonQuoted(ruleId) +
               ",\"ruleName\":" + service::access::jsonQuoted(ruleName) +
               ",\"severity\":" + service::access::jsonQuoted(severity) +
               ",\"status\":" + service::access::jsonQuoted(status) + "},\"values\":" +
               std::string(data) + "}";
    }

    static std::int64_t nowMilliseconds() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    static std::string evaluationSql(bool allDevices) {
        const std::string input = allDevices ? R"sql(
WITH rules AS (
  SELECT rule.*, device.name AS device_name,
         device.protocol_params->>'device_code' AS device_code
  FROM alert_rule rule JOIN device ON device.id = rule.device_id
  WHERE rule.deleted_at IS NULL AND rule.status = 'enabled'
    AND device.deleted_at IS NULL AND device.status = 'enabled'
), samples AS (
  SELECT rules.*,
         latest.data,
         latest.report_time AS observed_at,
         previous.data AS previous_data
  FROM rules
  LEFT JOIN LATERAL (
    SELECT data, report_time FROM device_data
    WHERE device_id = rules.device_id
    ORDER BY report_time DESC LIMIT 1
  ) latest ON TRUE
  LEFT JOIN LATERAL (
    SELECT data FROM device_data
    WHERE device_id = rules.device_id
      AND (latest.report_time IS NULL OR report_time < latest.report_time)
    ORDER BY report_time DESC LIMIT 1
  ) previous ON TRUE
)sql"
                                                  : R"sql(
WITH rules AS (
  SELECT rule.*, device.name AS device_name,
         device.protocol_params->>'device_code' AS device_code
  FROM alert_rule rule JOIN device ON device.id = rule.device_id
  WHERE rule.deleted_at IS NULL AND rule.status = 'enabled'
    AND device.deleted_at IS NULL AND device.status = 'enabled'
    AND rule.device_id = $1::uuid
), samples AS (
  SELECT rules.*, $2::jsonb AS data,
         to_timestamp($3::double precision / 1000.0) AS observed_at,
         previous.data AS previous_data
  FROM rules
  LEFT JOIN LATERAL (
    SELECT data FROM device_data
    WHERE device_id = rules.device_id
      AND report_time < to_timestamp($3::double precision / 1000.0)
    ORDER BY report_time DESC LIMIT 1
  ) previous ON TRUE
)sql";
        return input + R"sql(
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
  SELECT id, name, severity, device_id, device_code, silence_duration,
         recovery_condition, recovery_wait_seconds, data,
         CASE WHEN logic = 'and' THEN bool_and(condition_matched)
              ELSE bool_or(condition_matched) END AS matched
  FROM conditions
  GROUP BY id, name, severity, device_id, device_code, silence_duration,
           recovery_condition, recovery_wait_seconds, data, logic
)
SELECT id::text, name, severity, device_id::text, COALESCE(device_code, ''),
       matched, silence_duration, recovery_condition, recovery_wait_seconds,
       COALESCE(data, '{}'::jsonb)::text
FROM evaluated
ORDER BY id)sql";
    }

    ruvia::WebWorkerHandle worker_;
    std::shared_future<void> stopped_;
    std::atomic_bool running_{false};
};

} // namespace service::alert
