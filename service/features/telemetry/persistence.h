#pragma once

#include <atomic>
#include <array>
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

#include "service/common/message/contract.h"
#include "service/features/access/event.h"
#include "service/features/alert/runtime.h"
#include "service/features/collector/stream.h"
#include "service/features/event/stream-multiplexer.h"
#include "service/features/telemetry/latest.h"
#include "service/features/telemetry/fanout.h"

namespace service::telemetry {

namespace detail {

inline std::string sanitizeJsonUtf8(std::string_view value) {
    static constexpr std::array<char, 16> digits{'0', '1', '2', '3', '4', '5', '6', '7',
                                                 '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
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
        if (byte >= 0xC2 && byte <= 0xDF && continuation(index + 1))
            length = 2;
        else if (byte == 0xE0 && index + 2 < value.size() &&
                 static_cast<unsigned char>(value[index + 1]) >= 0xA0 &&
                 static_cast<unsigned char>(value[index + 1]) <= 0xBF && continuation(index + 2))
            length = 3;
        else if (((byte >= 0xE1 && byte <= 0xEC) || (byte >= 0xEE && byte <= 0xEF)) &&
                 continuation(index + 1) && continuation(index + 2))
            length = 3;
        else if (byte == 0xED && index + 2 < value.size() &&
                 static_cast<unsigned char>(value[index + 1]) >= 0x80 &&
                 static_cast<unsigned char>(value[index + 1]) <= 0x9F && continuation(index + 2))
            length = 3;
        else if (byte == 0xF0 && index + 3 < value.size() &&
                 static_cast<unsigned char>(value[index + 1]) >= 0x90 &&
                 static_cast<unsigned char>(value[index + 1]) <= 0xBF && continuation(index + 2) &&
                 continuation(index + 3))
            length = 4;
        else if (byte >= 0xF1 && byte <= 0xF3 && continuation(index + 1) &&
                 continuation(index + 2) && continuation(index + 3))
            length = 4;
        else if (byte == 0xF4 && index + 3 < value.size() &&
                 static_cast<unsigned char>(value[index + 1]) >= 0x80 &&
                 static_cast<unsigned char>(value[index + 1]) <= 0x8F && continuation(index + 2) &&
                 continuation(index + 3))
            length = 4;
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

class PersistenceRuntime final {
  public:
    PersistenceRuntime() = default;
    PersistenceRuntime(const PersistenceRuntime&) = delete;
    PersistenceRuntime& operator=(const PersistenceRuntime&) = delete;
    ~PersistenceRuntime() { stop(); }

    void start(std::vector<ruvia::WebWorkerHandle> workers, std::size_t collectorWorkerCount) {
        if (running_.exchange(true))
            return;
        workers_ = std::move(workers);
        collectorWorkerCount_ = collectorWorkerCount;
        if (workers_.empty() || collectorWorkerCount_ == 0) {
            running_.store(false);
            throw std::runtime_error("telemetry persistence requires north and collector workers");
        }
        std::vector<std::future<void>> readiness;
        for (const auto consumer : {Consumer::Dispatch,Consumer::History,Consumer::Latest,Consumer::Alerts,Consumer::Delivery})
        for (std::size_t index = 0; index < workers_.size(); ++index) {
            auto ready = std::make_shared<std::promise<void>>();
            auto stopped = std::make_shared<std::promise<void>>();
            readiness.push_back(ready->get_future());
            stopped_.push_back(stopped->get_future().share());
            const auto posted = workers_[index].post(
                [this, index, consumer, ready, stopped](ruvia::WebWorkerContext& context) {
                    return run(context, index, consumer, ready, stopped);
                });
            if (!posted.accepted()) {
                stopped->set_value();
                stop();
                throw std::runtime_error("service worker rejected telemetry consumer");
            }
        }
        for (const auto alerts : {false,true})
        for (std::size_t index = 0; index < workers_.size(); ++index) {
            auto ready = std::make_shared<std::promise<void>>();
            auto stopped = std::make_shared<std::promise<void>>();
            readiness.push_back(ready->get_future());
            stopped_.push_back(stopped->get_future().share());
            const auto posted = workers_[index].post(
                [this, index, alerts, ready, stopped](ruvia::WebWorkerContext& context) {
                    return maintainFreshness(context, index, alerts, ready, stopped);
                });
            if (!posted.accepted()) {
                stopped->set_value();
                stop();
                throw std::runtime_error("service worker rejected telemetry freshness task");
            }
        }
        for (auto& ready : readiness)
            ready.get();
    }

    void stop() noexcept {
        if (!running_.exchange(false))
            return;
        for (const auto task : {message::WorkerStreamTask::Telemetry,
             message::WorkerStreamTask::TelemetryHistory,message::WorkerStreamTask::TelemetryLatest,
             message::WorkerStreamTask::TelemetryAlerts,message::WorkerStreamTask::TelemetryDelivery})
            message::workerStreamMultiplexer().signal(task);
        service::message::workerStreamMultiplexer().signal(
            service::message::WorkerStreamTask::Freshness);
        service::message::workerStreamMultiplexer().signal(
            service::message::WorkerStreamTask::FreshnessAlerts);
        for (const auto& stopped : stopped_)
            if (stopped.valid())
                stopped.wait();
        stopped_.clear();
        workers_.clear();
    }

    static ruvia::Task<void> ingest(ruvia::WebWorkerContext& context,
        const std::vector<message::StreamMessage>& messages) {
        co_await fanout(context.redis(), messages);
        for (const auto task : {message::WorkerStreamTask::TelemetryHistory,message::WorkerStreamTask::TelemetryLatest,
             message::WorkerStreamTask::TelemetryAlerts,message::WorkerStreamTask::TelemetryDelivery})
            message::workerStreamMultiplexer().signal(task);
    }

    static ruvia::Task<void> apply(ruvia::WebWorkerContext& context,
        Consumer consumer, const std::vector<message::StreamMessage>& messages) {
        if (consumer == Consumer::Dispatch) { co_await ingest(context,messages); co_return; }
        if (messages.empty())
            co_return;
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
            (void)co_await persist(context,parsedMessages,std::vector<bool>(parsedMessages.size(),false));
            co_await latest::publishRealtimeChange(redis);
            break;
        case Consumer::Latest: co_await latest::update(redis,parsedMessages); break;
        case Consumer::Delivery: co_await service::access::event::publishMany(redis,parsedMessages); break;
        case Consumer::Alerts: {
            const auto metadata = co_await service::alert::metadata::activity(context,parsedMessages);
            std::vector<std::string> previous;
            for (const auto& parsed : parsedMessages)
                previous.push_back(co_await prepareAlert(context,parsed));
            co_await service::alert::Runtime::evaluateTelemetry(context,parsedMessages,previous,metadata.devices);
            if (metadata.offlineRules) co_await service::alert::metadata::schedule(redis,parsedMessages);
            break;
        }
        case Consumer::Dispatch: break;
        }

    }

  private:
    static ruvia::Task<std::string> prepareAlert(ruvia::WebWorkerContext& context,
        const message::ParsedDeviceMessage& value) {
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
            service::common::dbParams(value.deviceId,value.observedAtMs,value.messageId,value.valuesJson));
        co_return std::string(rows.front()[0].value().value_or("{}"));
    }

    static constexpr std::string_view kGroup = "iot-engine:telemetry-persistence";
    static constexpr std::size_t kBatchSize = 256;

    ruvia::Task<void> maintainFreshness(
        ruvia::WebWorkerContext& context, std::size_t index, bool alerts,
        std::shared_ptr<std::promise<void>> ready,
        std::shared_ptr<std::promise<void>> stopped) {
        try {
            const auto redis = context.redis();
            std::vector<std::size_t> shards;
            for (auto shardIndex = index;
                 shardIndex < service::message::shard::kCount;
                 shardIndex += workers_.size())
                shards.push_back(shardIndex);
            ready->set_value();
            while (running_.load() && !context.stopToken().stopRequested()) {
                bool failed = false;
                try {
                    std::optional<std::int64_t> deadline;
                    for (const auto shardIndex : shards) {
                        std::optional<std::int64_t> next;
                        if (alerts) {
                            co_await service::alert::Runtime::evaluateOfflineDue(context, shardIndex);
                            next = co_await service::alert::metadata::nextOfflineDeadline(redis, shardIndex);
                        } else {
                            co_await latest::expireStale(redis, shardIndex);
                            next = co_await latest::nextDeadline(redis, shardIndex);
                        }
                        if (next && (!deadline || *next < *deadline)) deadline = next;
                    }
                    const auto wait = latest::deadlineWait(
                        service::message::utcNowMilliseconds(),
                        deadline);
                    if (wait.has_value() && wait->count() == 0)
                        continue;
                    co_await service::message::workerStreamMultiplexer().wait(
                        index, alerts ? service::message::WorkerStreamTask::FreshnessAlerts : service::message::WorkerStreamTask::Freshness,
                        context.stopToken(), wait);
                } catch (const std::exception& error) {
                    if (context.stopToken().stopRequested())
                        break;
                    std::cerr << "telemetry freshness maintenance failed: " << error.what()
                              << '\n';
                    failed = true;
                }
                if (failed) {
                    (void)co_await ruvia::sleepFor(context.worker(),
                                                   std::chrono::milliseconds(250));
                }
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

    ruvia::Task<void> run(ruvia::WebWorkerContext& context, std::size_t index, Consumer consumerKind,
                          std::shared_ptr<std::promise<void>> ready,
                          std::shared_ptr<std::promise<void>> stopped) {
        const auto group = std::string(kGroup) + ":" + std::string(consumerNames[static_cast<std::size_t>(consumerKind)]);
        const auto wakeTask = static_cast<message::WorkerStreamTask>(static_cast<unsigned>(message::WorkerStreamTask::Telemetry) + static_cast<unsigned>(consumerKind));
        try {
            const auto redis = context.redis();
            std::vector<std::string> streams;
            std::map<std::string, std::size_t, std::less<>> streamPartitions;
            for (auto partition = index; partition < message::shard::kCount;
                  partition += workers_.size()) {
                streams.push_back(consumerStream(partition,consumerKind));
                streamPartitions.emplace(streams.back(), partition);
                co_await message::redis::ensureGroup(
                    redis, streams.back(), group);
            }
            ready->set_value();
            bool recovering = true;
            const auto consumer = service::runtime::instanceId() + ":service-" + std::to_string(index);
            while (running_.load() && !context.stopToken().stopRequested()) {
                if (streams.empty()) {
                    (void)co_await ruvia::sleepFor(context.worker(), std::chrono::seconds(1));
                    continue;
                }
                std::vector<message::redis::StreamBatch> batches;
                bool readFailed = false;
                try {
                    batches = recovering
                        ? co_await message::redis::claimGroupMany(
                              redis, streams, group, consumer, kBatchSize)
                        : co_await message::redis::readGroupMany(
                              redis, streams, group, consumer, ">", kBatchSize);
                } catch (const std::exception& error) {
                    if (context.stopToken().stopRequested())
                        break;
                    std::cerr << "telemetry stream read failed for service worker " << index
                              << ": " << error.what() << '\n';
                    recovering = true;
                    readFailed = true;
                }
                if (readFailed) {
                    (void)co_await ruvia::sleepFor(context.worker(),
                                                   std::chrono::milliseconds(250));
                    continue;
                }
                if (recovering && batches.empty()) {
                    recovering = false;
                    continue;
                }
                if (batches.empty()) {
                    co_await service::message::workerStreamMultiplexer().wait(
                        index, wakeTask,
                        context.stopToken());
                    continue;
                }
                bool failed = false;
                for (const auto& batch : batches) {
                    const auto partitionEntry = streamPartitions.find(batch.stream);
                    if (partitionEntry == streamPartitions.end())
                        continue;
                    const auto partition = partitionEntry->second;
                    try {
                        if (consumerKind != Consumer::Alerts) {
                            co_await apply(context,consumerKind,batch.messages);
                            co_await message::redis::acknowledgeAndDeleteMany(redis,batch.stream,group,batch.messages);
                        } else for (const auto& entry : batch.messages) {
                            const std::vector<message::StreamMessage> one{entry};
                            co_await apply(context,consumerKind,one);
                            co_await message::redis::acknowledgeAndDeleteMany(redis,batch.stream,group,one);
                        }
                    } catch (const std::exception& error) {
                        std::cerr << "telemetry persistence failed for collector worker " << partition
                                  << ": " << error.what() << '\n';
                        recovering = true;
                        failed = true;
                    }
                }
                if (failed)
                    (void)co_await ruvia::sleepFor(context.worker(),
                                                   std::chrono::milliseconds(250));
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

    static ruvia::Task<std::vector<std::string>>
    persist(ruvia::WebWorkerContext& context,
            const std::vector<message::ParsedDeviceMessage>& messages,
            const std::vector<bool>& alertActive) {
        if (messages.empty())
            co_return std::vector<std::string>{};
        if (messages.size() != alertActive.size())
            throw std::invalid_argument("telemetry alert-active batch size mismatch");
        std::vector<std::string> rawPayloadArrays;
        rawPayloadArrays.reserve(messages.size());
        for (const auto& message : messages)
            rawPayloadArrays.push_back(message::rawPayloadsJson(message.rawPayloads));

        std::string sql = R"sql(WITH incoming(
input_sequence, report_time, id, device_id, link_id, connection_id, protocol, source,
occurred_at, data, raw_payload_hex, storage_policy, needs_previous) AS (VALUES )sql";
        std::vector<ruvia::DbValue> params;
        params.reserve(messages.size() * 13);
        for (std::size_t index = 0; index < messages.size(); ++index) {
            const auto& parsed = messages[index];
            if (index != 0)
                sql.push_back(',');
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
        for (const auto& value : messages) deviceIds.insert(value.deviceId);
        std::vector<ruvia::DbValue> deviceParams;
        std::string requested = "WITH requested(device_id) AS (VALUES ";
        for (const auto deviceId : deviceIds) {
            if (!deviceParams.empty()) requested += ',';
            deviceParams.emplace_back(deviceId);
            requested += "($" + std::to_string(deviceParams.size()) + "::uuid)";
        }
        requested += ") ";
        (void)co_await transaction.query(requested +
            "SELECT pg_advisory_xact_lock(hashtextextended(device_id::text,734621)) FROM requested ORDER BY device_id",
            deviceParams);
        (void)co_await transaction.execute(requested +
            "INSERT INTO device_data_ingest_state(device_id) SELECT device_id FROM requested ON CONFLICT(device_id) DO NOTHING",
            deviceParams);
        const auto rows = co_await transaction.query(sql, params);
        co_await transaction.commit();
        std::vector<std::string> previous(messages.size(), "{}");
        for (const auto& row : rows) {
            const auto parsedSequence =
                service::common::parseInt64(std::optional<std::string_view>{row[0].value().value_or(std::string_view{})});
            if (!parsedSequence || *parsedSequence < 0)
                continue;
            const auto sequence = static_cast<std::size_t>(*parsedSequence);
            if (sequence < previous.size() && row[1].value().has_value())
                previous[sequence].assign(row[1].value().value_or(std::string_view{}));
        }
        co_return previous;
    }

    std::vector<ruvia::WebWorkerHandle> workers_;
    std::vector<std::shared_future<void>> stopped_;
    std::size_t collectorWorkerCount_ = 0;
    std::atomic_bool running_{false};
};

} // namespace service::telemetry
