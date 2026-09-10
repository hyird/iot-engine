#pragma once

#include <charconv>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/web/Context.h>

#include "service/common/http.h"
#include "service/common/message.h"
#include "service/features/access/access.transport.h"
#include "service/features/event/event.transport.h"

namespace service::message {

inline std::string webhookCatalogChangesStream(std::size_t workerIndex) {
    return service::access::stream::catalogChanges(workerIndex);
}

inline std::string runtimeConfigChangesStream() {
    return "iot:channel:runtime:config-work";
}

inline constexpr std::string_view kConfigEventType{ "config.changed" };
inline constexpr std::string_view kConfigEventSchemaVersion{ "1" };

template <typename Redis>
ruvia::Task<void> publishConfigEnvelope(
    const Redis& redis,
    std::string_view eventId,
    std::string_view eventType,
    std::string_view aggregate,
    std::string_view aggregateId,
    std::string_view action,
    std::string_view schemaVersion,
    std::string_view occurredAtMs,
    std::size_t serviceWorkerCount = 1
) {
    const bool runtimeChange =
        aggregate == "link" || aggregate == "device" || aggregate == "protocol";
    const bool webhookChange = aggregate == "access_key" || aggregate == "webhook" ||
        aggregate == "device" || aggregate == "protocol";
    const bool accessSessionChange = aggregate == "access_key" || aggregate == "device";
    if (!runtimeChange && !webhookChange && !accessSessionChange) {
        co_return;
    }
    if (serviceWorkerCount == 0) {
        throw std::invalid_argument("invalid Service Worker count for config event");
    }
    const std::vector<service::message::StreamField> fields{
        { "message_id", std::string(eventId) },
        { "event_id", std::string(eventId) },
        { "event_type", std::string(eventType) },
        { "schema_version", std::string(schemaVersion) },
        { "aggregate", std::string(aggregate) },
        { "aggregate_type", std::string(aggregate) },
        { "action", std::string(action) },
        { "aggregate_id", std::string(aggregateId) },
        { "created_at_ms", std::string(occurredAtMs) },
        { "occurred_at_ms", std::string(occurredAtMs) }
    };
    auto pipeline = redis.pipeline();
    if (runtimeChange) {
        service::message::redis::queueAddAndWake(
            pipeline,
            runtimeConfigChangesStream(),
            fields,
            std::nullopt,
            WorkerStreamTask::Reconciler,
            10000
        );
    }
    if (webhookChange) {
        // Every Worker owns an independent in-memory catalog, so each receives one
        // refresh event. Data events are claimed independently from their shared work queue.
        for (std::size_t workerIndex = 0; workerIndex < serviceWorkerCount;
             ++workerIndex) {
            service::message::redis::queueAddAndWake(
                pipeline,
                webhookCatalogChangesStream(workerIndex),
                fields,
                workerIndex,
                WorkerStreamTask::Webhook,
                10000
            );
        }
    }
    if (accessSessionChange) {
        service::message::redis::queueAddAndWake(
            pipeline,
            service::access::stream::sessionChanges(),
            fields,
            std::nullopt,
            WorkerStreamTask::Webhook,
            10000
        );
    }
    const auto replies = co_await std::move(pipeline).exec();
    service::message::redis::requirePipelineSuccess("publish config change", replies);
    co_return;
}

} // namespace service::message

#include <set>

#include <ruvia/web/WebWorker.h>

#include "service/common/uuid.h"

namespace service::message::idempotency {

inline std::vector<std::string>
eventIds(const std::vector<service::message::StreamMessage>& messages) {
    std::set<std::string, std::less<>> unique;
    for (const auto& message : messages) {
        const auto id = message.get("event_id");
        if (service::common::isUuid(id)) {
            unique.emplace(id);
        }
    }
    return { unique.begin(), unique.end() };
}

inline ruvia::Task<std::set<std::string, std::less<>>>
pending(ruvia::WebWorkerContext& context, std::string_view consumer, const std::vector<std::string>& eventIds) {
    std::set<std::string, std::less<>> result(eventIds.begin(), eventIds.end());
    if (eventIds.empty()) {
        co_return result;
    }

    std::string sql = "SELECT event_id::text FROM outbox_consumer_receipt WHERE "
                      "consumer_name = $1 AND "
                      "event_id IN (";
    std::vector<ruvia::DbValue> params;
    params.reserve(eventIds.size() + 1);
    params.emplace_back(consumer);
    for (std::size_t index = 0; index < eventIds.size(); ++index) {
        if (index != 0) {
            sql.push_back(',');
        }
        sql += "$" + std::to_string(index + 2) + "::uuid";
        params.emplace_back(eventIds[index]);
    }
    sql.push_back(')');
    const auto rows = co_await context.db().query(sql, params);
    for (const auto& row : rows) {
        result.erase(std::string(row[0].value().value_or(std::string_view{})));
    }
    co_return result;
}

inline bool
shouldProcess(const service::message::StreamMessage& message, const std::set<std::string, std::less<>>& pendingIds) {
    const auto id = message.get("event_id");
    return !service::common::isUuid(id) || pendingIds.contains(id);
}

inline ruvia::Task<void>
markProcessed(ruvia::WebWorkerContext& context, std::string_view consumer, const std::vector<std::string>& eventIds) {
    if (eventIds.empty()) {
        co_return;
    }
    std::string sql =
        "INSERT INTO outbox_consumer_receipt(consumer_name, event_id) VALUES ";
    std::vector<ruvia::DbValue> params;
    params.reserve(eventIds.size() * 2);
    for (std::size_t index = 0; index < eventIds.size(); ++index) {
        if (index != 0) {
            sql.push_back(',');
        }
        const auto base = index * 2 + 1;
        sql += "($" + std::to_string(base) + ",$" + std::to_string(base + 1) +
            "::uuid)";
        params.emplace_back(consumer);
        params.emplace_back(eventIds[index]);
    }
    sql += " ON CONFLICT (consumer_name, event_id) DO NOTHING";
    (void)co_await context.db().execute(sql, params);
}

} // namespace service::message::idempotency

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>

#include <ruvia/core/Channel.h>
#include <ruvia/core/StopToken.h>
#include <ruvia/core/Timer.h>

#include "service/common/observability.h"
#include "service/features/event/postgres_notifier/postgres_notifier.transport.h"
#include "service/features/live/live.service.h"

namespace service::message::outbox {

struct Policy final {
    std::int64_t pendingAlertThreshold{ 1000 };
    std::int64_t oldestAgeAlertMs{ 300000 };
    std::int64_t deadLetterAlertThreshold{ 1 };
    std::int64_t receiptRetentionDays{ 30 };
};

class OutboxService {
  protected:
    struct Event {
        std::string id;
        std::string type;
        std::string aggregate;
        std::string aggregateId;
        std::string action;
        std::string schemaVersion;
        std::string occurredAtMs;
        std::string deviceCode;
        std::string data;
    };

    using Clock = std::chrono::steady_clock;

    OutboxService(observability::Registry& registry, std::size_t collectors, std::size_t workers, Policy policy)
        : observability_(registry), collectorWorkerCount_(collectors), serviceWorkerCount_(workers), policy_(policy) {}

    ruvia::Task<std::optional<Clock::time_point>> nextAvailable(ruvia::WebWorkerContext& context) {
        // No periodic empty-queue scan. A timer is armed only for a durable
        // retry, or for eligible rows currently locked by another dispatcher.
        const auto rows = co_await context.db().query(R"sql(
SELECT ceil(extract(epoch FROM (min(available_at) - clock_timestamp())) * 1000)::bigint::text
FROM outbox_event WHERE published_at IS NULL AND dead_lettered_at IS NULL)sql");
        if (rows.empty() || !rows.front()[0].value()) {
            co_return std::nullopt;
        }
        const auto delay = std::chrono::milliseconds(std::max<std::int64_t>(25, integer(*rows.front()[0].value())));
        co_return Clock::now() + delay;
    }

    ruvia::Task<bool> dispatch(ruvia::WebWorkerContext& context) {
        auto transaction = co_await context.db().beginTransaction();
        const auto rows = co_await transaction.query(R"sql(
SELECT id::text, event_type, aggregate_type, aggregate_id, action,
       schema_version::text,
       floor(extract(epoch FROM occurred_at) * 1000)::bigint::text,
       COALESCE(payload->>'device_code',''), COALESCE(payload->'data','{}'::jsonb)::text
FROM outbox_event
WHERE published_at IS NULL AND dead_lettered_at IS NULL AND available_at <= NOW()
ORDER BY occurred_at, id
FOR UPDATE SKIP LOCKED
LIMIT 100)sql");
        if (rows.empty()) {
            co_await transaction.commit();
            observability_.gauge("iot_engine_outbox_last_batch_size", 0);
            co_return false;
        }

        std::vector<Event> events;
        events.reserve(rows.size());
        for (const auto& row : rows) {
            Event event;
            event.id = std::string(row[0].value().value_or(std::string_view{}));
            event.type = std::string(row[1].value().value_or(std::string_view{}));
            event.aggregate = std::string(row[2].value().value_or(std::string_view{}));
            event.aggregateId = std::string(row[3].value().value_or(std::string_view{}));
            event.action = std::string(row[4].value().value_or(std::string_view{}));
            event.schemaVersion = std::string(row[5].value().value_or(std::string_view{ "1" }));
            event.occurredAtMs = std::string(row[6].value().value_or(std::string_view{ "0" }));
            event.deviceCode = row[7].value().value_or(std::string_view{});
            event.data = row[8].value().value_or(std::string_view{});
            events.push_back(std::move(event));
        }

        for (const auto& event : events) {
            std::string publishError;
            try {
                if (event.type == "query.changed") {
                    co_await service::live::publish(context.redis(), event.aggregate);
                } else if (event.aggregate == "command") {
                    co_await service::access::event::publish(context.redis(), event.id, event.type, event.aggregateId, event.deviceCode, integer(event.occurredAtMs), event.data);
                } else {
                    co_await publishConfigEnvelope(context.redis(), event.id, event.type, event.aggregate, event.aggregateId, event.action, event.schemaVersion, event.occurredAtMs, serviceWorkerCount_);
                }
            } catch (const std::exception& error) {
                publishError = error.what();
            } catch (...) {
                publishError = "unknown publish failure";
            }
            if (!publishError.empty()) {
                if (publishError.size() > 2000) {
                    publishError.resize(2000);
                }
                (void)co_await transaction.execute(R"sql(
UPDATE outbox_event
SET attempts = attempts + 1,
    last_error = $2,
    available_at = NOW() + make_interval(
      secs => LEAST(300, (1::bigint << LEAST(attempts, 8))::integer)),
    dead_lettered_at = CASE WHEN attempts + 1 >= 20 THEN NOW()
                            ELSE dead_lettered_at END
WHERE id = $1::uuid)sql",
                                                   service::common::dbParams(event.id, publishError));
                co_await transaction.commit();
                observability_.increment("iot_engine_outbox_publish_retries_total");
                co_return true;
            }
            (void)co_await transaction.execute(
                "UPDATE outbox_event SET published_at = NOW(), attempts = attempts + 1, "
                "last_error = NULL WHERE id = $1::uuid",
                service::common::dbParams(event.id)
            );
        }
        co_await transaction.commit();
        observability_.increment("iot_engine_outbox_published_total", events.size());
        observability_.gauge("iot_engine_outbox_last_batch_size", static_cast<std::int64_t>(events.size()));
        co_return true;
    }

    ruvia::Task<void> collectMetrics(ruvia::WebWorkerContext& context) {
        const auto rows = co_await context.db().query(R"sql(
SELECT count(*) FILTER (WHERE dead_lettered_at IS NULL)::text,
       COALESCE(floor(extract(epoch FROM (
         NOW() - min(occurred_at) FILTER (WHERE dead_lettered_at IS NULL))) * 1000), 0)::bigint::text,
       count(*) FILTER (WHERE dead_lettered_at IS NOT NULL)::text
FROM outbox_event WHERE published_at IS NULL)sql");
        if (!rows.empty()) {
            const auto pending =
                integer(rows.front()[0].value().value_or(std::string_view{}));
            const auto oldestAge =
                integer(rows.front()[1].value().value_or(std::string_view{}));
            const auto deadLettered =
                integer(rows.front()[2].value().value_or(std::string_view{}));
            observability_.gauge("iot_engine_outbox_pending", pending);
            observability_.gauge("iot_engine_outbox_oldest_age_ms", oldestAge);
            observability_.gauge("iot_engine_outbox_dead_lettered", deadLettered);
            updateAlert("outbox_pending", pending, policy_.pendingAlertThreshold);
            updateAlert("outbox_oldest_age", oldestAge, policy_.oldestAgeAlertMs);
            updateAlert("outbox_dead_lettered", deadLettered, policy_.deadLetterAlertThreshold);
        }
        observability_.gauge("iot_engine_outbox_receipt_retention_days", policy_.receiptRetentionDays);

        co_await collectStream(context, "runtime_config", runtimeConfigChangesStream(), "iot-engine:runtime-reconciler");
        co_await collectStream(context, "access_session", service::access::stream::sessionChanges(), "iot-engine:open-webhook");
        co_await collectStream(context, "telemetry", message::parsedStream(), "iot-engine:telemetry-persistence");
        co_await collectStream(context, "command_result", message::commandResultStream(), "iot-engine:command-result");

        for (std::size_t index = 0; index < serviceWorkerCount_; ++index) {
            co_await collectStream(context, "webhook_config_" + std::to_string(index), webhookCatalogChangesStream(index), "iot-engine:open-webhook");
        }
        for (std::size_t index = 0; index < collectorWorkerCount_; ++index) {
            const auto suffix = std::to_string(index);
            co_await collectStream(context, "ingress_" + suffix, message::ingressStream(index), "iot-engine:collector");
            co_await collectStream(context, "dead_letter_" + suffix, message::deadLetterStream(index), {});
        }

        // The operations endpoints run on any Service Worker. Publish this
        // worker's complete snapshot so those endpoints can aggregate every
        // worker without reading another worker's in-memory Registry.
        const auto workerText = observability_.workerIndex();
        if (workerText.empty())
            co_return;
        std::size_t workerIndex{};
        const auto parsed = std::from_chars(
            workerText.data(), workerText.data() + workerText.size(), workerIndex);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != workerText.data() + workerText.size()) {
            co_return;
        }
        const auto metricsSnapshot = observability_.prometheus();
        const auto readinessSnapshot = service::message::worker_metrics::encodeReadinessSnapshot(
            observability_.ready(), observability_.healthJson());
        const auto ttl = std::to_string(
            service::message::worker_metrics::kSnapshotTtl.count());
        auto pipeline = context.redis().pipeline();
        pipeline.command(
            "SET", service::message::worker_metrics::metricsSnapshotKey(workerIndex),
            metricsSnapshot, "PX", ttl);
        pipeline.command(
            "SET", service::message::worker_metrics::readinessSnapshotKey(workerIndex),
            readinessSnapshot, "PX", ttl);
        const auto replies = co_await std::move(pipeline).exec();
        service::message::redis::requirePipelineSuccess("publish observability snapshot", replies);
    }

    ruvia::Task<void> collectStream(ruvia::WebWorkerContext& context, std::string metricSuffix, std::string stream, std::string_view group) {
        const auto length = co_await service::message::redis::command(
            context.redis(),
            { "XLEN", stream }
        );
        if (length.kind() == ruvia::RedisValue::Kind::kInteger) {
            observability_.gauge("iot_engine_stream_" + metricSuffix + "_entries", length.integer());
        }
        if (group.empty()) {
            co_return;
        }
        const auto pending = co_await service::message::redis::command(
            context.redis(),
            { "XPENDING", stream, std::string(group) }
        );
        if (pending.kind() == ruvia::RedisValue::Kind::kArray &&
            !pending.array().empty() &&
            pending.array().front().kind() == ruvia::RedisValue::Kind::kInteger) {
            observability_.gauge("iot_engine_stream_" + metricSuffix + "_pending", pending.array().front().integer());
        }
    }

    static std::int64_t integer(std::string_view value) {
        std::int64_t result{};
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
        return error == std::errc{} && end == value.data() + value.size() ? result : 0;
    }

    ruvia::Task<void> cleanupReceipts(ruvia::WebWorkerContext& context) {
        (void)co_await context.db().execute(R"sql(
DELETE FROM outbox_consumer_receipt
WHERE processed_at < NOW() - make_interval(days => $1::integer))sql",
                                            service::common::dbParams(policy_.receiptRetentionDays));
    }

    void updateAlert(std::string name, std::int64_t value, std::int64_t threshold) {
        const bool active = threshold > 0 && value >= threshold;
        const auto detail = "value=" + std::to_string(value) +
            ", threshold=" + std::to_string(threshold);
        if (observability_.alert(name, active, detail)) {
            std::cerr << "operational alert " << (active ? "active" : "cleared")
                      << ": " << name << " (" << detail << ")\n";
        }
    }

    observability::Registry& observability_;
    std::size_t collectorWorkerCount_{};
    std::size_t serviceWorkerCount_{};
    Policy policy_;
};

} // namespace service::message::outbox
