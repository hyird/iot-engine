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
#include "service/features/telemetry/latest.h"

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
        for (std::size_t index = 0; index < workers_.size(); ++index) {
            auto ready = std::make_shared<std::promise<void>>();
            auto stopped = std::make_shared<std::promise<void>>();
            readiness.push_back(ready->get_future());
            stopped_.push_back(stopped->get_future().share());
            const auto posted = workers_[index].post(
                [this, index, ready, stopped](ruvia::WebWorkerContext& context) {
                    return run(context, index, ready, stopped);
                });
            if (!posted.accepted()) {
                running_.store(false);
                throw std::runtime_error("service worker rejected telemetry consumer");
            }
        }
        {
            auto ready = std::make_shared<std::promise<void>>();
            auto stopped = std::make_shared<std::promise<void>>();
            readiness.push_back(ready->get_future());
            stopped_.push_back(stopped->get_future().share());
            const auto posted = workers_.front().post(
                [this, ready, stopped](ruvia::WebWorkerContext& context) {
                    return maintainFreshness(context, ready, stopped);
                });
            if (!posted.accepted()) {
                running_.store(false);
                throw std::runtime_error("service worker rejected telemetry freshness task");
            }
        }
        for (auto& ready : readiness)
            ready.get();
    }

    void stop() noexcept {
        if (!running_.exchange(false))
            return;
        for (const auto& stopped : stopped_)
            if (stopped.valid())
                (void)stopped.wait_for(std::chrono::seconds(3));
        stopped_.clear();
        workers_.clear();
    }

    static ruvia::Task<void> ingest(
        ruvia::WebWorkerContext& context,
        const std::vector<message::StreamMessage>& messages) {
        if (messages.empty())
            co_return;
        const auto redis = context.redis();
        co_await persist(context, messages);
        co_await latest::update(redis, messages);
        std::vector<message::ParsedDeviceMessage> alertMessages;
        alertMessages.reserve(messages.size());
        for (const auto& message : messages)
            alertMessages.push_back(message::parsedFrom(message));
        try {
            co_await service::alert::Runtime::evaluateTelemetry(context, alertMessages);
        } catch (const std::exception& error) {
            // A malformed rule must not block durable telemetry or queue acknowledgement.
            // The next matching report retries evaluation against durable telemetry.
            std::cerr << "alert telemetry evaluation failed: " << error.what() << '\n';
        }
        co_await service::access::event::publishMany(redis, messages);
    }

  private:
    static constexpr std::string_view kGroup = "iot-engine:telemetry-persistence";
    static constexpr std::size_t kBatchSize = 256;

    ruvia::Task<void> maintainFreshness(
        ruvia::WebWorkerContext& context, std::shared_ptr<std::promise<void>> ready,
        std::shared_ptr<std::promise<void>> stopped) {
        try {
            const auto redis = context.redis();
            ready->set_value();
            while (running_.load() && !context.stopToken().stopRequested()) {
                try {
                    co_await latest::expireStale(redis);
                } catch (const std::exception& error) {
                    if (context.stopToken().stopRequested())
                        break;
                    std::cerr << "telemetry freshness maintenance failed: " << error.what()
                              << '\n';
                }
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

    ruvia::Task<void> run(ruvia::WebWorkerContext& context, std::size_t index,
                          std::shared_ptr<std::promise<void>> ready,
                          std::shared_ptr<std::promise<void>> stopped) {
        try {
            const auto redis = context.redis();
            std::vector<std::string> streams;
            std::map<std::string, std::size_t, std::less<>> streamPartitions;
            for (auto partition = index; partition < collectorWorkerCount_;
                  partition += workers_.size()) {
                streams.push_back(message::parsedStream(partition));
                streamPartitions.emplace(streams.back(), partition);
                co_await message::redis::ensureGroup(
                    redis, streams.back(), kGroup);
            }
            ready->set_value();
            bool recovering = true;
            const auto consumer = "service-" + std::to_string(index);
            while (running_.load() && !context.stopToken().stopRequested()) {
                if (streams.empty()) {
                    (void)co_await ruvia::sleepFor(context.worker(), std::chrono::seconds(1));
                    continue;
                }
                std::vector<message::redis::StreamBatch> batches;
                bool readFailed = false;
                try {
                    batches = recovering
                        ? co_await message::redis::readGroupMany(
                              redis, streams, kGroup, consumer, "0", kBatchSize)
                        : co_await message::redis::readGroupManyBlocking(
                              redis, streams, kGroup, consumer,
                              context.stopToken(), kBatchSize);
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
                bool failed = false;
                for (const auto& batch : batches) {
                    const auto partitionEntry = streamPartitions.find(batch.stream);
                    if (partitionEntry == streamPartitions.end())
                        continue;
                    const auto partition = partitionEntry->second;
                    try {
                        co_await ingest(context, batch.messages);
                        co_await message::redis::acknowledgeAndDeleteMany(
                            redis, batch.stream, kGroup, batch.messages);
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

    static ruvia::Task<void> persist(ruvia::WebWorkerContext& context,
                                     const std::vector<message::StreamMessage>& messages) {
        if (messages.empty())
            co_return;
        std::vector<message::ParsedDeviceMessage> parsedMessages;
        parsedMessages.reserve(messages.size());
        std::vector<std::string> rawPayloadArrays;
        rawPayloadArrays.reserve(messages.size());
        for (const auto& message : messages) {
            parsedMessages.push_back(message::parsedFrom(message));
            parsedMessages.back().valuesJson =
                detail::sanitizeJsonUtf8(parsedMessages.back().valuesJson);
            rawPayloadArrays.push_back(message::rawPayloadsJson(parsedMessages.back().rawPayloads));
        }

        std::string sql = R"sql(WITH RECURSIVE incoming(
report_time, id, device_id, link_id, connection_id, protocol, source,
occurred_at, data, raw_payload_hex, storage_interval) AS (VALUES )sql";
        std::vector<ruvia::DbValue> params;
        params.reserve(messages.size() * 11);
        for (std::size_t index = 0; index < messages.size(); ++index) {
            const auto& parsed = parsedMessages[index];
            if (index != 0)
                sql.push_back(',');
            const auto base = index * 11;
            sql += "(to_timestamp($" + std::to_string(base + 1) + "::double precision / 1000.0),$" +
                   std::to_string(base + 2) + "::uuid,$" + std::to_string(base + 3) + "::uuid,$" +
                   std::to_string(base + 4) + "::uuid,$" + std::to_string(base + 5) + "::uuid,$" +
                   std::to_string(base + 6) + ",$" + std::to_string(base + 7) + ",to_timestamp($" +
                   std::to_string(base + 8) + "::double precision / 1000.0),$" +
                   std::to_string(base + 9) + "::jsonb,$" + std::to_string(base + 10) +
                   "::jsonb,$" + std::to_string(base + 11) + "::integer)";
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
            params.emplace_back(std::clamp<std::int64_t>(parsed.storageInterval, 1, 86400));
        }
        sql += R"sql(), locks AS MATERIALIZED (
  SELECT pg_advisory_xact_lock(hashtextextended(ordered.id::text, 734621))
  FROM (
    SELECT DISTINCT current_device.id
    FROM incoming
    JOIN device current_device ON current_device.id = incoming.device_id
                               AND current_device.link_id = incoming.link_id
    ORDER BY current_device.id
  ) ordered
), valid_incoming AS (
  SELECT incoming.*
  FROM incoming
  JOIN device current_device ON current_device.id = incoming.device_id
                             AND current_device.link_id = incoming.link_id
  CROSS JOIN (SELECT count(*) AS lock_count FROM locks) acquired
  WHERE acquired.lock_count >= 0
), device_last_stored AS (
  SELECT requested.device_id, stored.report_time AS last_stored
  FROM (SELECT DISTINCT device_id FROM valid_incoming) requested
  LEFT JOIN LATERAL (
    SELECT history.report_time
    FROM device_data history
    WHERE history.device_id = requested.device_id
    ORDER BY history.report_time DESC, history.id DESC
    LIMIT 1
  ) stored ON TRUE
), ordered AS (
  SELECT incoming.*,
         row_number() OVER (PARTITION BY device_id ORDER BY report_time, id) AS sequence,
         device_last_stored.last_stored
  FROM valid_incoming incoming
  JOIN device_last_stored USING (device_id)
), filtered AS (
  SELECT ordered.*,
         (storage_interval <= 1 OR last_stored IS NULL OR
          (source = 'edge' AND report_time = last_stored) OR
          report_time >= last_stored + storage_interval * interval '1 second') AS accepted,
         CASE WHEN storage_interval <= 1 OR last_stored IS NULL OR
                   (source = 'edge' AND report_time = last_stored) OR
                   report_time >= last_stored + storage_interval * interval '1 second'
              THEN report_time ELSE last_stored END AS last_accepted
  FROM ordered WHERE sequence = 1
  UNION ALL
  SELECT next.*,
         (next.storage_interval <= 1 OR previous.last_accepted IS NULL OR
          (next.source = 'edge' AND next.report_time = previous.last_accepted) OR
          next.report_time >= previous.last_accepted +
                              next.storage_interval * interval '1 second') AS accepted,
         CASE WHEN next.storage_interval <= 1 OR previous.last_accepted IS NULL OR
                   (next.source = 'edge' AND next.report_time = previous.last_accepted) OR
                   next.report_time >= previous.last_accepted +
                                       next.storage_interval * interval '1 second'
              THEN next.report_time ELSE previous.last_accepted END AS last_accepted
  FROM filtered previous
  JOIN ordered next ON next.device_id = previous.device_id
                   AND next.sequence = previous.sequence + 1
)
INSERT INTO device_data(
report_time, id, device_id, link_id, connection_id, protocol, source,
occurred_at, data, raw_payload_hex)
SELECT report_time, id, device_id, link_id, connection_id, protocol, source,
       occurred_at, data, raw_payload_hex
FROM filtered WHERE accepted
ON CONFLICT (id, report_time) DO NOTHING)sql";
        (void)co_await context.db().execute(sql, params);
    }

    std::vector<ruvia::WebWorkerHandle> workers_;
    std::vector<std::shared_future<void>> stopped_;
    std::size_t collectorWorkerCount_ = 0;
    std::atomic_bool running_{false};
};

} // namespace service::telemetry
