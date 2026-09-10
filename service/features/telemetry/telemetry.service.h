#pragma once
#include "service/features/event/event.transport.h"
#include "service/features/event/event.types.h"
#include "service/features/telemetry/latest/latest.service.h"
#include "service/features/telemetry/telemetry.protocol.h"

namespace service::telemetry {
enum class Consumer { Dispatch,
                      History,
                      Latest,
                      Alerts,
                      Delivery };
inline constexpr std::array<std::string_view, 5> consumerNames{ "dispatch", "history", "latest", "alerts", "delivery" };

inline std::string consumerStream(Consumer consumer) {
    auto stream = service::message::parsedStream();
    if (consumer != Consumer::Dispatch) {
        stream += ':' + std::string(consumerNames[static_cast<std::size_t>(consumer)]);
    }
    return stream;
}

// The ingress receipt and all four durable copies are committed atomically.
// Consumer streams never trim pending entries. Each consumer owns XACK/XDEL.
inline constexpr std::string_view kFanoutScript = R"lua(
if redis.call('EXISTS',KEYS[1]) ~= 0 then return 0 end
for i=2,5 do
 local kind = redis.call('TYPE',KEYS[i]).ok
 if kind ~= 'none' and kind ~= 'stream' then
  return redis.error_reply('telemetry consumer key must be a stream')
 end
end
local args = {'*'}
for i=1,#ARGV do args[#args+1]=ARGV[i] end
for i=2,5 do redis.call('XADD',KEYS[i],unpack(args)) end
redis.call('SET',KEYS[1],'1','EX',604800)
return 1
)lua";

template <class Redis>
ruvia::Task<void> fanout(const Redis& redis, const std::vector<service::message::StreamMessage>& messages) {
    for (const auto& input : messages) {
        auto parsed = service::message::parsedFrom(input);
        contract::normalize(parsed);
        const auto fields = service::message::parsedFields(parsed);
        const std::vector<std::string> store{
            "iot:telemetry:fanout:" + parsed.messageId,
            consumerStream(Consumer::History),
            consumerStream(Consumer::Latest),
            consumerStream(Consumer::Alerts),
            consumerStream(Consumer::Delivery)
        };
        const std::vector<std::string_view> keys(store.begin(), store.end());
        std::vector<std::string_view> args;
        for (const auto& field : fields) {
            args.push_back(field.name);
            args.push_back(field.value);
        }
        const auto reply = co_await redis.eval(kFanoutScript, keys, args);
        if (reply.kind() != ruvia::RedisValue::Kind::kInteger) {
            service::message::redis::throwValue("telemetry fanout", reply);
        }
    }
}
} // namespace service::telemetry

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/core/Timer.h>
#include <ruvia/web/WebWorker.h>
#include <ruvia/web/db/Db.h>

#include "service/common/message.h"
#include "service/features/access/access.transport.h"
#include "service/features/alert/alert.service.h"

namespace service::telemetry {

namespace detail {

inline std::string sanitizeJsonUtf8(std::string_view value) {
    static constexpr std::array<char, 16> digits{ '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F' };
    std::string result;
    result.reserve(value.size());
    const auto continuation = [value](std::size_t index) {
        return index < value.size() && (static_cast<unsigned char>(value[index]) & 0xC0U) == 0x80U;
    };
    for (std::size_t index = 0; index < value.size();) {
        const auto byte = static_cast<unsigned char>(value[index]);
        if (byte < 0x80) {
            result.push_back(static_cast<char>(byte));
            ++index;
            continue;
        }
        std::size_t length = 0;
        if (byte >= 0xC2 && byte <= 0xDF && continuation(index + 1)) {
            length = 2;
        } else if (byte == 0xE0 && index + 2 < value.size() &&
                   static_cast<unsigned char>(value[index + 1]) >= 0xA0 &&
                   static_cast<unsigned char>(value[index + 1]) <= 0xBF && continuation(index + 2)) {
            length = 3;
        } else if (((byte >= 0xE1 && byte <= 0xEC) || (byte >= 0xEE && byte <= 0xEF)) &&
                   continuation(index + 1) && continuation(index + 2)) {
            length = 3;
        } else if (byte == 0xED && index + 2 < value.size() &&
                   static_cast<unsigned char>(value[index + 1]) >= 0x80 &&
                   static_cast<unsigned char>(value[index + 1]) <= 0x9F && continuation(index + 2)) {
            length = 3;
        } else if (byte == 0xF0 && index + 3 < value.size() &&
                   static_cast<unsigned char>(value[index + 1]) >= 0x90 &&
                   static_cast<unsigned char>(value[index + 1]) <= 0xBF && continuation(index + 2) &&
                   continuation(index + 3)) {
            length = 4;
        } else if (byte >= 0xF1 && byte <= 0xF3 && continuation(index + 1) &&
                   continuation(index + 2) && continuation(index + 3)) {
            length = 4;
        } else if (byte == 0xF4 && index + 3 < value.size() &&
                   static_cast<unsigned char>(value[index + 1]) >= 0x80 &&
                   static_cast<unsigned char>(value[index + 1]) <= 0x8F && continuation(index + 2) &&
                   continuation(index + 3)) {
            length = 4;
        }
        if (length != 0) {
            result.append(value.substr(index, length));
            index += length;
            continue;
        }
        result += "\\u00";
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 0x0FU]);
        ++index;
    }
    return result;
}

} // namespace detail

class TelemetryService {
  public:
    static ruvia::Task<void> ingest(ruvia::WebWorkerContext& context, const std::vector<message::StreamMessage>& messages) {
        co_await fanout(context.redis(), messages);
    }

    static ruvia::Task<void> apply(ruvia::WebWorkerContext& context, Consumer consumer, const std::vector<message::StreamMessage>& messages) {
        if (consumer == Consumer::Dispatch) {
            co_await ingest(context, messages);
            co_return;
        }
        if (messages.empty()) {
            co_return;
        }
        std::vector<message::ParsedDeviceMessage> parsedMessages;
        parsedMessages.reserve(messages.size());
        for (const auto& message : messages) {
            auto parsed = message::parsedFrom(message);
            parsed.valuesJson = detail::sanitizeJsonUtf8(parsed.valuesJson);
            parsedMessages.push_back(std::move(parsed));
        }
        const auto redis = context.redis();
        switch (consumer) {
            case Consumer::History:
                (void)co_await persist(context, parsedMessages, std::vector<bool>(parsedMessages.size(), false));
                co_await latest::publishRealtimeChange(redis);
                break;
            case Consumer::Latest:
                co_await latest::update(redis, parsedMessages);
                break;
            case Consumer::Delivery:
                co_await service::access::event::publishMany(redis, parsedMessages);
                break;
            case Consumer::Alerts: {
                const auto metadata = co_await service::alert::metadata::activity(context, parsedMessages);
                std::vector<std::string> previous;
                for (const auto& parsed : parsedMessages) {
                    previous.push_back(co_await prepareAlert(context, parsed));
                }
                co_await service::alert::AlertEvaluationService::evaluateTelemetry(context, parsedMessages, previous, metadata.devices);
                if (metadata.offlineRules) {
                    co_await service::alert::metadata::schedule(redis, parsedMessages);
                }
                break;
            }
            case Consumer::Dispatch:
                break;
        }
    }

  protected:
    static ruvia::Task<std::string> prepareAlert(ruvia::WebWorkerContext& context, const message::ParsedDeviceMessage& value) {
        const auto rows = co_await context.db().query(R"sql(
INSERT INTO alert_input_state(device_id,observed_at_ms,message_id,data,previous_data)
VALUES($1::uuid,$2::bigint,$3::uuid,$4::jsonb,'{}')
ON CONFLICT(device_id) DO UPDATE SET
 previous_data=CASE WHEN (EXCLUDED.observed_at_ms,EXCLUDED.message_id) >
   (alert_input_state.observed_at_ms,alert_input_state.message_id) THEN alert_input_state.data ELSE alert_input_state.previous_data END,
 data=CASE WHEN (EXCLUDED.observed_at_ms,EXCLUDED.message_id) >
   (alert_input_state.observed_at_ms,alert_input_state.message_id) THEN EXCLUDED.data ELSE alert_input_state.data END,
 observed_at_ms=GREATEST(EXCLUDED.observed_at_ms,alert_input_state.observed_at_ms),
 message_id=CASE WHEN (EXCLUDED.observed_at_ms,EXCLUDED.message_id) >
   (alert_input_state.observed_at_ms,alert_input_state.message_id) THEN EXCLUDED.message_id ELSE alert_input_state.message_id END
RETURNING CASE WHEN message_id=$3::uuid THEN previous_data ELSE '{}'::jsonb END::text)sql",
                                                      service::common::dbParams(value.deviceId, value.observedAtMs, value.messageId, value.valuesJson));
        co_return std::string(rows.front()[0].value().value_or("{}"));
    }

    static ruvia::Task<std::vector<std::string>>
    persist(ruvia::WebWorkerContext& context, const std::vector<message::ParsedDeviceMessage>& messages, const std::vector<bool>& alertActive) {
        if (messages.empty()) {
            co_return std::vector<std::string>{};
        }
        if (messages.size() != alertActive.size()) {
            throw std::invalid_argument("telemetry alert-active batch size mismatch");
        }
        std::vector<std::string> rawPayloadArrays;
        rawPayloadArrays.reserve(messages.size());
        for (const auto& message : messages) {
            rawPayloadArrays.push_back(message::rawPayloadsJson(message.rawPayloads));
        }

        std::string sql = R"sql(WITH incoming(
input_sequence, report_time, id, device_id, link_id, connection_id, protocol, source,
occurred_at, data, raw_payload_hex, storage_policy, needs_previous) AS (VALUES )sql";
        std::vector<ruvia::DbValue> params;
        params.reserve(messages.size() * 13);
        for (std::size_t index = 0; index < messages.size(); ++index) {
            const auto& parsed = messages[index];
            if (index != 0) {
                sql.push_back(',');
            }
            const auto base = index * 13;
            sql += "($" + std::to_string(base + 1) + "::bigint,to_timestamp($" +
                std::to_string(base + 2) + "::double precision / 1000.0),$" +
                std::to_string(base + 3) + "::uuid,$" + std::to_string(base + 4) + "::uuid,$" +
                std::to_string(base + 5) + "::uuid,$" + std::to_string(base + 6) + "::uuid,$" +
                std::to_string(base + 7) + ",$" + std::to_string(base + 8) + ",to_timestamp($" +
                std::to_string(base + 9) + "::double precision / 1000.0),$" +
                std::to_string(base + 10) + "::jsonb,$" + std::to_string(base + 11) +
                "::jsonb,$" + std::to_string(base + 12) + "::text,$" +
                std::to_string(base + 13) + "::boolean)";
            params.emplace_back(static_cast<std::int64_t>(index));
            params.emplace_back(parsed.observedAtMs);
            params.emplace_back(std::string_view(parsed.messageId));
            params.emplace_back(std::string_view(parsed.deviceId));
            params.emplace_back(std::string_view(parsed.linkId));
            params.emplace_back(std::string_view(parsed.connectionId));
            params.emplace_back(std::string_view(parsed.protocol));
            params.emplace_back(std::string_view(parsed.source));
            params.emplace_back(parsed.occurredAtMs);
            params.emplace_back(std::string_view(parsed.valuesJson));
            params.emplace_back(std::string_view(rawPayloadArrays[index]));
            params.emplace_back(std::string_view(parsed.storagePolicy));
            params.emplace_back(alertActive[index]);
        }
        sql += R"sql(), valid_incoming AS MATERIALIZED (
  SELECT incoming.*
  FROM incoming
  JOIN device current_device ON current_device.id = incoming.device_id
                             AND current_device.link_id = incoming.link_id
), requested AS MATERIALIZED (
  SELECT DISTINCT device_id FROM valid_incoming
), states AS MATERIALIZED (
  SELECT state.device_id, state.last_stored_at, state.last_observed_at,
         state.last_observed_id, state.last_data
  FROM device_data_ingest_state state
  JOIN requested USING (device_id)
), ordered AS (
  SELECT incoming.*,
         row_number() OVER (PARTITION BY device_id ORDER BY report_time, id) AS sequence,
         states.last_stored_at AS last_stored,
         states.last_observed_at AS baseline_observed_at,
         states.last_observed_id AS baseline_observed_id,
         states.last_data AS baseline_data
  FROM valid_incoming incoming
  JOIN states USING (device_id)
), lagged AS MATERIALIZED (
  SELECT ordered.*,
         lag(report_time) OVER (
           PARTITION BY device_id ORDER BY report_time, id) AS prior_report_time,
         lag(id) OVER (
           PARTITION BY device_id ORDER BY report_time, id) AS prior_id,
         lag(data) OVER (
           PARTITION BY device_id ORDER BY report_time, id) AS prior_data
  FROM ordered
), predecessors AS MATERIALIZED (
  SELECT input_sequence,
         CASE
           WHEN baseline_observed_at IS NOT NULL
             AND (report_time, id) <=
                 (baseline_observed_at, baseline_observed_id)
             THEN '{}'::jsonb
           WHEN prior_report_time IS NOT NULL
             AND (baseline_observed_at IS NULL OR
                  (prior_report_time, prior_id) >
                  (baseline_observed_at, baseline_observed_id))
             THEN prior_data
           ELSE COALESCE(baseline_data, '{}'::jsonb)
         END AS previous_data
  FROM lagged
), incoming_points AS MATERIALIZED (
  SELECT ordered.input_sequence, ordered.device_id, ordered.report_time,
         ordered.id AS record_id, point.key AS element_id,
         CASE WHEN jsonb_typeof(point.value) = 'object' AND point.value ? 'value'
              THEN point.value->'value' ELSE point.value END AS point_value
  FROM ordered
  CROSS JOIN LATERAL jsonb_each(
    COALESCE(ordered.data->'values', '{}'::jsonb)) point
), point_timeline AS MATERIALIZED (
  SELECT input_sequence, device_id, element_id, report_time AS observed_at,
         record_id, point_value, FALSE AS baseline
  FROM incoming_points
  UNION ALL
  SELECT NULL::bigint, latest.device_id, latest.element_id, latest.observed_at,
         latest.record_id,
         CASE WHEN jsonb_typeof(latest.value) = 'object' AND latest.value ? 'value'
              THEN latest.value->'value' ELSE latest.value END,
         TRUE
  FROM device_latest_value latest
  JOIN requested USING (device_id)
), point_lagged AS MATERIALIZED (
  SELECT timeline.*,
         lag(point_value) OVER (
           PARTITION BY device_id, element_id
           ORDER BY observed_at, record_id, baseline DESC,
                    input_sequence NULLS FIRST) AS prior_value,
         lag(TRUE) OVER (
           PARTITION BY device_id, element_id
           ORDER BY observed_at, record_id, baseline DESC,
                    input_sequence NULLS FIRST) AS prior_present
  FROM point_timeline timeline
), point_changes AS MATERIALIZED (
  SELECT input_sequence,
         bool_or(prior_present IS NULL OR point_value IS DISTINCT FROM prior_value) AS changed
  FROM point_lagged
  WHERE NOT baseline
  GROUP BY input_sequence
), filtered AS MATERIALIZED (
  SELECT ordered.*,
         (storage_policy = 'report' OR
          (storage_policy = 'change' AND COALESCE(point_changes.changed, FALSE))) AS accepted
  FROM ordered
  LEFT JOIN point_changes USING (input_sequence)
), inserted AS (
  INSERT INTO device_data(
    report_time, id, device_id, link_id, connection_id, protocol, source,
    occurred_at, data, raw_payload_hex, model_id, model_revision)
  SELECT report_time, id, device_id, link_id, connection_id, protocol, source,
         occurred_at, data, raw_payload_hex,
         (data#>>'{model,id}')::uuid, (data#>>'{model,revision}')::bigint
  FROM filtered WHERE accepted
  ON CONFLICT (id, report_time) DO NOTHING
  RETURNING device_id
), storage_summary AS MATERIALIZED (
  SELECT device_id,
         COALESCE(
           GREATEST(max(last_stored), max(report_time) FILTER (WHERE accepted)),
           max(last_stored), max(report_time) FILTER (WHERE accepted)) AS last_stored_at
  FROM filtered
  GROUP BY device_id
), new_observed AS MATERIALIZED (
  SELECT incoming.*,
         row_number() OVER (
           PARTITION BY incoming.device_id
           ORDER BY incoming.report_time DESC, incoming.id DESC) AS newest
  FROM valid_incoming incoming
  JOIN states USING (device_id)
  WHERE states.last_observed_at IS NULL
     OR (incoming.report_time, incoming.id) >
        (states.last_observed_at, states.last_observed_id)
), observed_summary AS MATERIALIZED (
  SELECT newest.device_id, newest.report_time AS last_observed_at,
         newest.id AS last_observed_id, newest.data AS last_data,
         COALESCE(previous.data, states.last_data) AS previous_data
  FROM new_observed newest
  JOIN states USING (device_id)
  LEFT JOIN new_observed previous
    ON previous.device_id = newest.device_id AND previous.newest = 2
  WHERE newest.newest = 1
), latest_elements AS MATERIALIZED (
  SELECT DISTINCT ON (incoming.device_id, point.key)
         incoming.device_id, point.key AS element_id, point.value,
         incoming.report_time AS observed_at, incoming.id AS record_id
  FROM valid_incoming incoming
  JOIN states USING (device_id)
  CROSS JOIN LATERAL jsonb_each(
    COALESCE(incoming.data->'values', '{}'::jsonb)) point
  ORDER BY incoming.device_id, point.key, incoming.report_time DESC, incoming.id DESC
), latest_values AS (
  INSERT INTO device_latest_value(
    device_id, element_id, value, observed_at, record_id, updated_at)
  SELECT device_id, element_id, value, observed_at, record_id, NOW()
  FROM latest_elements
  ON CONFLICT (device_id, element_id) DO UPDATE SET
    value = EXCLUDED.value,
    observed_at = EXCLUDED.observed_at,
    record_id = EXCLUDED.record_id,
    updated_at = NOW()
  WHERE (EXCLUDED.observed_at, EXCLUDED.record_id) >
        (device_latest_value.observed_at, device_latest_value.record_id)
  RETURNING device_id
), state_updated AS (
  UPDATE device_data_ingest_state state
  SET last_stored_at = storage.last_stored_at,
      last_observed_at = COALESCE(observed.last_observed_at, state.last_observed_at),
      last_observed_id = COALESCE(observed.last_observed_id, state.last_observed_id),
      last_data = COALESCE(observed.last_data, state.last_data),
      previous_data = CASE WHEN observed.device_id IS NULL
                           THEN state.previous_data ELSE observed.previous_data END,
      updated_at = NOW()
  FROM storage_summary storage
  LEFT JOIN observed_summary observed USING (device_id)
  CROSS JOIN (SELECT count(*) AS inserted_count FROM inserted) inserted_barrier
  WHERE state.device_id = storage.device_id
    AND inserted_barrier.inserted_count >= 0
  RETURNING state.device_id
)
SELECT incoming.input_sequence::text,
       CASE WHEN incoming.needs_previous
            THEN COALESCE(predecessors.previous_data, '{}'::jsonb)
            ELSE '{}'::jsonb END::text
FROM incoming
LEFT JOIN predecessors USING (input_sequence)
CROSS JOIN (SELECT count(*) AS updated_count FROM state_updated) update_barrier
CROSS JOIN (SELECT count(*) AS latest_count FROM latest_values) latest_barrier
WHERE update_barrier.updated_count >= 0 AND latest_barrier.latest_count >= 0
ORDER BY incoming.input_sequence)sql";
        // Acquire per-device locks before taking the statement snapshot. Seeding
        // in a data-modifying CTE is invisible to sibling SELECTs and drops a
        // newly created device's first sample.
        auto transaction = co_await context.db("telemetry-history").beginTransaction();
        std::set<std::string_view> deviceIds;
        for (const auto& value : messages) {
            deviceIds.insert(value.deviceId);
        }
        std::vector<ruvia::DbValue> deviceParams;
        std::string requested = "WITH requested(device_id) AS (VALUES ";
        for (const auto deviceId : deviceIds) {
            if (!deviceParams.empty()) {
                requested += ',';
            }
            deviceParams.emplace_back(deviceId);
            requested += "($" + std::to_string(deviceParams.size()) + "::uuid)";
        }
        requested += ") ";
        (void)co_await transaction.query(requested + "SELECT pg_advisory_xact_lock(hashtextextended(device_id::text,734621)) FROM requested ORDER BY device_id", deviceParams);
        (void)co_await transaction.execute(requested + "INSERT INTO device_data_ingest_state(device_id) SELECT device_id FROM requested ON CONFLICT(device_id) DO NOTHING", deviceParams);
        const auto rows = co_await transaction.query(sql, params);
        co_await transaction.commit();
        std::vector<std::string> previous(messages.size(), "{}");
        for (const auto& row : rows) {
            const auto parsedSequence =
                service::common::parseInt64(std::optional<std::string_view>{ row[0].value().value_or(std::string_view{}) });
            if (!parsedSequence || *parsedSequence < 0) {
                continue;
            }
            const auto sequence = static_cast<std::size_t>(*parsedSequence);
            if (sequence < previous.size() && row[1].value().has_value()) {
                previous[sequence].assign(row[1].value().value_or(std::string_view{}));
            }
        }
        co_return previous;
    }
};

} // namespace service::telemetry
