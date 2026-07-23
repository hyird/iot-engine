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
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/core/Timer.h>
#include <ruvia/web/WebWorker.h>
#include <ruvia/web/db/Db.h>

#include "service/common/bridge/message.contract.h"
#include "service/modules/northbridge/open/open_access.event.h"
#include "service/modules/southbridge/queue/redis_stream_async.h"
#include "service/modules/northbridge/telemetry/device_latest.redis.h"

namespace service::northbridge::telemetry {

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

class TelemetryPersistenceRuntime final {
  public:
    TelemetryPersistenceRuntime() = default;
    TelemetryPersistenceRuntime(const TelemetryPersistenceRuntime&) = delete;
    TelemetryPersistenceRuntime& operator=(const TelemetryPersistenceRuntime&) = delete;
    ~TelemetryPersistenceRuntime() { stop(); }

    void start(std::vector<ruvia::WebWorkerHandle> workers, std::size_t southWorkerCount) {
        if (running_.exchange(true))
            return;
        workers_ = std::move(workers);
        southWorkerCount_ = southWorkerCount;
        if (workers_.empty() || southWorkerCount_ == 0) {
            running_.store(false);
            throw std::runtime_error("telemetry persistence requires north and south workers");
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
                throw std::runtime_error("north worker rejected telemetry consumer");
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

  private:
    static constexpr std::string_view kGroup = "iot-engine:telemetry-persistence";

    ruvia::Task<void> run(ruvia::WebWorkerContext& context, std::size_t index,
                          std::shared_ptr<std::promise<void>> ready,
                          std::shared_ptr<std::promise<void>> stopped) {
        try {
            const auto redis = context.redis();
            std::vector<std::size_t> partitions;
            for (auto partition = index; partition < southWorkerCount_;
                 partition += workers_.size()) {
                partitions.push_back(partition);
                co_await bridge::redis_async::ensureGroup(
                    redis, bridge::parsedStream(partition), kGroup);
            }
            ready->set_value();
            std::map<std::size_t, bool> recovering;
            for (const auto partition : partitions)
                recovering.emplace(partition, true);
            const auto consumer = "north-" + std::to_string(index);
            auto lastFreshnessCheck = std::chrono::steady_clock::time_point{};
            while (running_.load() && !context.stopToken().stopRequested()) {
                const auto now = std::chrono::steady_clock::now();
                if (index == 0 && now - lastFreshnessCheck >= std::chrono::milliseconds(250)) {
                    lastFreshnessCheck = now;
                    co_await latest::expireStale(redis);
                }
                bool handled = false;
                bool failed = false;
                for (const auto partition : partitions) {
                    const auto stream = bridge::parsedStream(partition);
                    auto& partitionRecovering = recovering.at(partition);
                    const auto messages = co_await bridge::redis_async::readGroup(
                        redis, stream, kGroup, consumer,
                        partitionRecovering ? "0" : ">", std::chrono::milliseconds(0), 100);
                    if (partitionRecovering && messages.empty())
                        partitionRecovering = false;
                    if (messages.empty())
                        continue;
                    handled = true;
                    try {
                        co_await persist(context, messages);
                        co_await latest::update(redis, messages);
                        for (const auto& message : messages) {
                            const auto parsed = bridge::parsedFrom(message);
                            const auto eventType =
                                parsed.valuesJson.find("\"type\":\"JPEG\"") != std::string::npos
                                    ? "device.image.reported"
                                    : "device.data.reported";
                            co_await service::open_access::event::publish(
                                redis, parsed.messageId, eventType, parsed.deviceId,
                                parsed.deviceCode, parsed.observedAtMs, parsed.valuesJson);
                            co_await bridge::redis_async::acknowledgeAndDelete(
                                redis, stream, kGroup, message.id);
                        }
                    } catch (const std::exception& error) {
                        std::cerr << "telemetry persistence failed for south worker " << partition
                                  << ": " << error.what() << '\n';
                        partitionRecovering = true;
                        failed = true;
                    }
                }
                if (!handled) {
                    (void)co_await ruvia::sleepFor(context.worker(), std::chrono::milliseconds(10));
                    continue;
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
                                     const std::vector<bridge::StreamMessage>& messages) {
        if (messages.empty())
            co_return;
        std::vector<bridge::ParsedDeviceMessage> parsedMessages;
        parsedMessages.reserve(messages.size());
        std::vector<std::string> rawPayloadArrays;
        rawPayloadArrays.reserve(messages.size());
        std::set<std::string, std::less<>> deviceIds;
        for (const auto& message : messages) {
            parsedMessages.push_back(bridge::parsedFrom(message));
            parsedMessages.back().valuesJson =
                detail::sanitizeJsonUtf8(parsedMessages.back().valuesJson);
            rawPayloadArrays.push_back(bridge::rawPayloadsJson(parsedMessages.back().rawPayloads));
            deviceIds.insert(parsedMessages.back().deviceId);
        }

        auto transaction = co_await context.db().beginTransaction();
        std::string lockSql =
            "SELECT pg_advisory_xact_lock(hashtextextended(id::text, 734621)) FROM device "
            "WHERE id IN (";
        std::vector<ruvia::DbValue> lockParams;
        lockParams.reserve(deviceIds.size());
        for (const auto& deviceId : deviceIds) {
            if (!lockParams.empty())
                lockSql.push_back(',');
            lockSql += "$" + std::to_string(lockParams.size() + 1) + "::uuid";
            lockParams.emplace_back(std::string_view(deviceId));
        }
        lockSql += ") ORDER BY id";
        (void)co_await transaction.execute(lockSql, lockParams);

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
                   std::to_string(base + 2) + "::uuid,$" + std::to_string(base + 3) + "::uuid,NULLIF($" +
                   std::to_string(base + 4) + ",'')::uuid,$" + std::to_string(base + 5) + "::uuid,$" +
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
        sql += R"sql(), valid_incoming AS (
  SELECT incoming.*
  FROM incoming
  JOIN device current_device ON current_device.id = incoming.device_id
), ordered AS (
  SELECT incoming.*,
         row_number() OVER (PARTITION BY device_id ORDER BY report_time, id) AS sequence,
         (SELECT max(stored.report_time) FROM device_data stored
          WHERE stored.device_id = incoming.device_id) AS last_stored
  FROM valid_incoming incoming
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
        (void)co_await transaction.execute(sql, params);
        co_await transaction.commit();
    }

    std::vector<ruvia::WebWorkerHandle> workers_;
    std::vector<std::shared_future<void>> stopped_;
    std::size_t southWorkerCount_ = 0;
    std::atomic_bool running_{false};
};

} // namespace service::northbridge::telemetry
