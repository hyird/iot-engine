#pragma once

#include <optional>
#include <charconv>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/web/Context.h>
#include <ruvia/web/db/DbQuery.h>
#include <ruvia/web/redis/RedisRepository.h>
#include "service/features/messaging/messaging.entity.h"

#include "service/common/http.h"
#include "service/common/message.h"
#include "service/features/access/access.service.h"
#include "service/features/messaging/messaging.transport.h"

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

    ruvia::DbQuery receipts(context.resource());
    std::vector<ruvia::DbExpression> ids;
    ids.reserve(eventIds.size());
    for (const auto& id : eventIds) {
        ids.push_back(receipts.cast(receipts.value(id), ruvia::DbDataType::kUuid));
    }
    receipts.select(receipts.cast(receipts.column(service::messaging::persistence::OutboxConsumerReceiptEntity::columnName<"event_id">()), ruvia::DbDataType::kText))
        .from(service::messaging::persistence::OutboxConsumerReceiptEntity::tableName())
        .andWhere(receipts.binary(receipts.column(service::messaging::persistence::OutboxConsumerReceiptEntity::columnName<"consumer_name">()), ruvia::DbBinaryOperator::kEqual, receipts.value(consumer)))
        .andWhere(receipts.binary(receipts.column(service::messaging::persistence::OutboxConsumerReceiptEntity::columnName<"event_id">()), ruvia::DbBinaryOperator::kIn, receipts.list(ids)));
    const auto rows = co_await context.db().query(receipts);
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
    ruvia::DbQuery receipts(context.resource());
    receipts.insertInto(service::messaging::persistence::OutboxConsumerReceiptEntity::tableName(), { "consumer_name", "event_id" });
    for (const auto& id : eventIds) {
        receipts.values({ receipts.value(consumer), receipts.cast(receipts.value(id), ruvia::DbDataType::kUuid) });
    }
    receipts.onConflict({ .columns = { "consumer_name", "event_id" }, .doNothing = true });
    (void)co_await context.db().execute(receipts);
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

#include "service/features/observability/observability.service.h"
#include "service/features/messaging/postgres_notifier/postgres_notifier.transport.h"
#include "service/features/live/live.service.h"
#include "service/features/messaging/messaging.config.h"

namespace service::message::outbox {

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

    OutboxService(observability::RuntimeDiagnostics& diagnostics, std::size_t collectors, std::size_t workers, Policy policy)
        : observability_(diagnostics), collectorWorkerCount_(collectors), serviceWorkerCount_(workers), policy_(policy) {}

    ruvia::Task<std::optional<Clock::time_point>> nextAvailable(ruvia::WebWorkerContext& context) {
        // No periodic empty-queue scan. A timer is armed only for a durable
        // retry, or for eligible rows currently locked by another dispatcher.
        ruvia::DbQuery available(context.resource());
        const auto delayMs = available.binary(
            available.extract(ruvia::DbDatePart::kEpoch,
                available.binary(available.aggregate("min", { available.column(service::messaging::persistence::OutboxEventEntity::columnName<"available_at">()) }),
                    ruvia::DbBinaryOperator::kSubtract, available.call("clock_timestamp"))),
            ruvia::DbBinaryOperator::kMultiply, available.value(1000));
        available.select(available.cast(available.cast(available.call("ceil", { delayMs }),
                ruvia::DbDataType::kBigInt), ruvia::DbDataType::kText))
            .from(service::messaging::persistence::OutboxEventEntity::tableName())
            .andWhere(available.unary(ruvia::DbUnaryOperator::kIsNull, available.column(service::messaging::persistence::OutboxEventEntity::columnName<"published_at">())))
            .andWhere(available.unary(ruvia::DbUnaryOperator::kIsNull, available.column(service::messaging::persistence::OutboxEventEntity::columnName<"dead_lettered_at">())));
        const auto rows = co_await context.db().query(available);
        if (rows.empty() || !rows.front()[0].value()) {
            co_return std::nullopt;
        }
        const auto delay = std::chrono::milliseconds(std::max<std::int64_t>(25, integer(*rows.front()[0].value())));
        co_return Clock::now() + delay;
    }

    ruvia::Task<bool> dispatch(ruvia::WebWorkerContext& context) {
        auto transaction = co_await context.db().beginTransaction();
        ruvia::DbQuery pending(context.resource());
        const auto occurredAtMs = pending.call("floor", { pending.binary(
            pending.extract(ruvia::DbDatePart::kEpoch, pending.column(service::messaging::persistence::OutboxEventEntity::columnName<"occurred_at">())),
            ruvia::DbBinaryOperator::kMultiply, pending.value(1000)) });
        pending.select(pending.cast(pending.column(service::messaging::persistence::OutboxEventEntity::columnName<"id">()), ruvia::DbDataType::kText))
            .addSelect(pending.column(service::messaging::persistence::OutboxEventEntity::columnName<"event_type">())).addSelect(pending.column(service::messaging::persistence::OutboxEventEntity::columnName<"aggregate_type">()))
            .addSelect(pending.column(service::messaging::persistence::OutboxEventEntity::columnName<"aggregate_id">())).addSelect(pending.column(service::messaging::persistence::OutboxEventEntity::columnName<"action">()))
            .addSelect(pending.cast(pending.column(service::messaging::persistence::OutboxEventEntity::columnName<"schema_version">()), ruvia::DbDataType::kText))
            .addSelect(pending.cast(pending.cast(occurredAtMs, ruvia::DbDataType::kBigInt), ruvia::DbDataType::kText))
            .addSelect(pending.coalesce({ pending.binary(pending.column(service::messaging::persistence::OutboxEventEntity::columnName<"payload">()),
                ruvia::DbBinaryOperator::kJsonGetText, pending.value("device_code")), pending.value("") }))
            .addSelect(pending.cast(pending.coalesce({ pending.binary(pending.column(service::messaging::persistence::OutboxEventEntity::columnName<"payload">()),
                ruvia::DbBinaryOperator::kJsonGet, pending.value("data")),
                pending.cast(pending.value("{}"), ruvia::DbDataType::kJsonb) }), ruvia::DbDataType::kText))
            .from(service::messaging::persistence::OutboxEventEntity::tableName())
            .andWhere(pending.unary(ruvia::DbUnaryOperator::kIsNull, pending.column(service::messaging::persistence::OutboxEventEntity::columnName<"published_at">())))
            .andWhere(pending.unary(ruvia::DbUnaryOperator::kIsNull, pending.column(service::messaging::persistence::OutboxEventEntity::columnName<"dead_lettered_at">())))
            .andWhere(pending.binary(pending.column(service::messaging::persistence::OutboxEventEntity::columnName<"available_at">()), ruvia::DbBinaryOperator::kLessEqual, pending.call("now")))
            .addOrderBy(pending.column(service::messaging::persistence::OutboxEventEntity::columnName<"occurred_at">())).addOrderBy(pending.column(service::messaging::persistence::OutboxEventEntity::columnName<"id">()))
            .lock({ .mode = ruvia::DbRowLock::kUpdate, .skipLocked = true }).limit(100);
        const auto rows = co_await transaction.query(pending);
        if (rows.empty()) {
            co_await transaction.commit();
            observability_.setGauge("iot_engine_outbox_last_batch_size", 0);
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
                ruvia::DbQuery retry(context.resource());
                const auto attempts = retry.binary(retry.column(service::messaging::persistence::OutboxEventEntity::columnName<"attempts">()), ruvia::DbBinaryOperator::kAdd, retry.value(1));
                const auto seconds = retry.least({ retry.value(300), retry.cast(
                    retry.call("int8shl", { retry.cast(retry.value(1), ruvia::DbDataType::kBigInt),
                        retry.least({ retry.column(service::messaging::persistence::OutboxEventEntity::columnName<"attempts">()), retry.value(8) }) }), ruvia::DbDataType::kInteger) });
                const std::vector<ruvia::DbNamedArgument> intervalArgs{ { "secs", seconds } };
                retry.update(service::messaging::persistence::OutboxEventEntity::tableName())
                    .set(service::messaging::persistence::OutboxEventEntity::columnName<"attempts">(), attempts).set(service::messaging::persistence::OutboxEventEntity::columnName<"last_error">(), retry.value(publishError))
                    .set(service::messaging::persistence::OutboxEventEntity::columnName<"available_at">(), retry.binary(retry.call("now"), ruvia::DbBinaryOperator::kAdd,
                        retry.call("make_interval", {}, intervalArgs)))
                    .set(service::messaging::persistence::OutboxEventEntity::columnName<"dead_lettered_at">(), retry.caseWhen({ { retry.binary(attempts,
                        ruvia::DbBinaryOperator::kGreaterEqual, retry.value(20)), retry.call("now") } },
                        retry.column(service::messaging::persistence::OutboxEventEntity::columnName<"dead_lettered_at">())))
                    .andWhere(retry.binary(retry.column(service::messaging::persistence::OutboxEventEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual,
                        retry.cast(retry.value(event.id), ruvia::DbDataType::kUuid)));
                (void)co_await transaction.execute(retry);
                co_await transaction.commit();
                observability_.incrementCounter("iot_engine_outbox_publish_retries_total");
                co_return true;
            }
            ruvia::DbQuery published(context.resource());
            published.update(service::messaging::persistence::OutboxEventEntity::tableName()).set(service::messaging::persistence::OutboxEventEntity::columnName<"published_at">(), published.call("now"))
                .set(service::messaging::persistence::OutboxEventEntity::columnName<"attempts">(), published.binary(published.column(service::messaging::persistence::OutboxEventEntity::columnName<"attempts">()), ruvia::DbBinaryOperator::kAdd, published.value(1)))
                .set(service::messaging::persistence::OutboxEventEntity::columnName<"last_error">(), published.nullValue())
                .andWhere(published.binary(published.column(service::messaging::persistence::OutboxEventEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual,
                    published.cast(published.value(event.id), ruvia::DbDataType::kUuid)));
            (void)co_await transaction.execute(published);
        }
        co_await transaction.commit();
        observability_.incrementCounter("iot_engine_outbox_published_total", events.size());
        observability_.setGauge("iot_engine_outbox_last_batch_size", static_cast<std::int64_t>(events.size()));
        co_return true;
    }

    ruvia::Task<void> collectMetrics(ruvia::WebWorkerContext& context) {
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
        using service::messaging::persistence::OutboxReplayCounterEntity;
        const auto counterId = service::runtime::instanceId() + ":" + std::to_string(workerIndex);
        ruvia::DbQuery heartbeat(context.resource());
        heartbeat.update(OutboxReplayCounterEntity::tableName())
            .set(OutboxReplayCounterEntity::columnName<"updated_at">(), heartbeat.call("now"))
            .andWhere(heartbeat.binary(heartbeat.column(OutboxReplayCounterEntity::columnName<"id">()),
                ruvia::DbBinaryOperator::kEqual, heartbeat.value(counterId)))
            .returning({heartbeat.cast(heartbeat.column(OutboxReplayCounterEntity::columnName<"replays">()), ruvia::DbDataType::kText)});
        const auto replayCounters = co_await context.db().query(heartbeat);
        if (!replayCounters.empty())
            observability_.setCounter("iot_engine_outbox_dead_letter_replays_total",
                static_cast<std::uint64_t>(integer(replayCounters.front()[0].value().value_or("0"))));
        ruvia::DbQuery metrics(context.resource());
        const auto pending = metrics.unary(ruvia::DbUnaryOperator::kIsNull, metrics.column(service::messaging::persistence::OutboxEventEntity::columnName<"dead_lettered_at">()));
        const auto count = metrics.aggregate("count", { metrics.star() });
        const auto oldest = metrics.filter(metrics.aggregate("min", { metrics.column(service::messaging::persistence::OutboxEventEntity::columnName<"occurred_at">()) }), pending);
        const auto ageMs = metrics.call("floor", { metrics.binary(metrics.extract(ruvia::DbDatePart::kEpoch,
            metrics.binary(metrics.call("now"), ruvia::DbBinaryOperator::kSubtract, oldest)),
            ruvia::DbBinaryOperator::kMultiply, metrics.value(1000)) });
        metrics.select(metrics.cast(metrics.filter(count, pending), ruvia::DbDataType::kText))
            .addSelect(metrics.cast(metrics.cast(metrics.coalesce({ ageMs, metrics.value(0) }),
                ruvia::DbDataType::kBigInt), ruvia::DbDataType::kText))
            .addSelect(metrics.cast(metrics.filter(count, metrics.unary(ruvia::DbUnaryOperator::kIsNotNull,
                metrics.column(service::messaging::persistence::OutboxEventEntity::columnName<"dead_lettered_at">()))), ruvia::DbDataType::kText))
            .from(service::messaging::persistence::OutboxEventEntity::tableName())
            .andWhere(metrics.unary(ruvia::DbUnaryOperator::kIsNull, metrics.column(service::messaging::persistence::OutboxEventEntity::columnName<"published_at">())));
        const auto rows = co_await context.db().query(metrics);
        if (!rows.empty()) {
            const auto pending =
                integer(rows.front()[0].value().value_or(std::string_view{}));
            const auto oldestAge =
                integer(rows.front()[1].value().value_or(std::string_view{}));
            const auto deadLettered =
                integer(rows.front()[2].value().value_or(std::string_view{}));
            observability_.setGauge("iot_engine_outbox_pending", pending);
            observability_.setGauge("iot_engine_outbox_oldest_age_ms", oldestAge);
            observability_.setGauge("iot_engine_outbox_dead_lettered", deadLettered);
            updateAlert("outbox_pending", pending, policy_.pendingAlertThreshold);
            updateAlert("outbox_oldest_age", oldestAge, policy_.oldestAgeAlertMs);
            updateAlert("outbox_dead_lettered", deadLettered, policy_.deadLetterAlertThreshold);
        }
        observability_.setGauge("iot_engine_outbox_receipt_retention_days", policy_.receiptRetentionDays);

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
        // worker without reading another worker's in-memory runtime diagnostics.
        WorkerSnapshotEntity snapshot(context.resource());
        snapshot.set<"id">(service::message::worker_metrics::snapshotId(workerIndex));
        snapshot.set<"metrics">(observability_.prometheus());
        snapshot.set<"ready">(observability_.areComponentsReady());
        snapshot.set<"health">(observability_.healthJson());
        auto snapshots = context.redis().getRepository<WorkerSnapshotEntity>();
        const ruvia::RedisWriteOptions expiration{
            .ttl = service::message::worker_metrics::kSnapshotTtl};
        (void)co_await snapshots.upsert(snapshot, expiration);
    }

    ruvia::Task<void> collectStream(ruvia::WebWorkerContext& context, std::string metricSuffix, std::string stream, std::string_view group) {
        const auto length = co_await service::message::redis::command(
            context.redis(),
            { "XLEN", stream }
        );
        if (length.kind() == ruvia::RedisValue::Kind::kInteger) {
            observability_.setGauge("iot_engine_stream_" + metricSuffix + "_entries", length.integer());
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
            observability_.setGauge("iot_engine_stream_" + metricSuffix + "_pending", pending.array().front().integer());
        }
    }

    static std::int64_t integer(std::string_view value) {
        std::int64_t result{};
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
        return error == std::errc{} && end == value.data() + value.size() ? result : 0;
    }

    ruvia::Task<void> cleanupReplayCounters(ruvia::WebWorkerContext& context) {
        using service::messaging::persistence::OutboxReplayCounterEntity;
        ruvia::DbQuery expired(context.resource());
        const std::vector<ruvia::DbNamedArgument> intervalArgs{
            {"days", expired.cast(expired.value(policy_.replayCounterRetentionDays), ruvia::DbDataType::kInteger)}};
        expired.deleteFrom(OutboxReplayCounterEntity::tableName())
            .andWhere(expired.binary(expired.column(OutboxReplayCounterEntity::columnName<"updated_at">()),
                ruvia::DbBinaryOperator::kLess, expired.binary(expired.call("now"),
                    ruvia::DbBinaryOperator::kSubtract, expired.call("make_interval", {}, intervalArgs))))
            .andWhere(expired.binary(expired.call("split_part", {
                expired.column(OutboxReplayCounterEntity::columnName<"id">()), expired.value(":"), expired.value(1)}),
                ruvia::DbBinaryOperator::kNotEqual, expired.value(service::runtime::instanceId())));
        (void)co_await context.db().execute(expired);
    }

    ruvia::Task<void> cleanupReceipts(ruvia::WebWorkerContext& context) {
        ruvia::DbQuery expired(context.resource());
        const std::vector<ruvia::DbNamedArgument> intervalArgs{
            { "days", expired.cast(expired.value(policy_.receiptRetentionDays), ruvia::DbDataType::kInteger) }
        };
        expired.deleteFrom(service::messaging::persistence::OutboxConsumerReceiptEntity::tableName())
            .andWhere(expired.binary(expired.column(service::messaging::persistence::OutboxConsumerReceiptEntity::columnName<"processed_at">()), ruvia::DbBinaryOperator::kLess,
                expired.binary(expired.call("now"), ruvia::DbBinaryOperator::kSubtract,
                    expired.call("make_interval", {}, intervalArgs))));
        (void)co_await context.db().execute(expired);
    }

    void updateAlert(std::string name, std::int64_t value, std::int64_t threshold) {
        const bool active = threshold > 0 && value >= threshold;
        const auto detail = "value=" + std::to_string(value) +
            ", threshold=" + std::to_string(threshold);
        if (observability_.setAlertState(name, active, detail)) {
            std::cerr << "operational alert " << (active ? "active" : "cleared")
                      << ": " << name << " (" << detail << ")\n";
        }
    }

    observability::RuntimeDiagnostics& observability_;
    std::size_t collectorWorkerCount_{};
    std::size_t serviceWorkerCount_{};
    Policy policy_;
};

} // namespace service::message::outbox

namespace service::rpc {

class RpcReceiptService final {
  public:
    static ruvia::Task<std::optional<RpcReplyRecord>> beginRequest(
        const ruvia::RedisHandle& redis, const std::string& id) {
        const auto previous = co_await service::message::redis::command(redis, {"GET", Contract::reply(id)});
        if (previous.kind() == ruvia::RedisValue::Kind::kString)
            co_return RpcReplyRecord{std::string(previous.string())};
        if (previous.kind() == ruvia::RedisValue::Kind::kError)
            service::message::redis::throwValue("RPC receipt", previous);
        const auto claimed = co_await service::message::redis::command(redis,
            {"SET", Contract::claim(id), "1", "NX", "EX", "86400"});
        if (claimed.kind() == ruvia::RedisValue::Kind::kError)
            service::message::redis::throwValue("RPC claim", claimed);
        if (claimed.kind() != ruvia::RedisValue::Kind::kString)
            service::common::fail(10004, "Background operation was interrupted; inspect its state", 503);
        const auto cancelled = co_await service::message::redis::command(redis,
            {"EXISTS", Contract::cancelled(id)});
        if (cancelled.kind() == ruvia::RedisValue::Kind::kError)
            service::message::redis::throwValue("RPC cancellation", cancelled);
        if (cancelled.kind() == ruvia::RedisValue::Kind::kInteger && cancelled.integer() != 0)
            service::common::fail(10004, "Background operation cancelled", 503);
        co_return std::nullopt;
    }

    static ruvia::Task<void> saveAndAcknowledge(const ruvia::RedisHandle& redis,
        const std::string& id, const std::string& stream, std::string_view group,
        const std::string& messageId, const RpcReplyRecord& record) {
    static constexpr std::string_view kReplyScript = R"lua(
redis.call('SET', KEYS[1], ARGV[1], 'EX', ARGV[2])
redis.call('XADD', KEYS[2], 'MAXLEN', '~', 100000, '*', 'topic', KEYS[1], 'schema_version', '1')
redis.call('XACK', KEYS[3], ARGV[3], ARGV[4])
redis.call('XDEL', KEYS[3], ARGV[4])
return 1
)lua";

                const auto replyKey = Contract::reply(id);
                const std::string_view keys[]{replyKey, service::message::live::kChanges, stream};
                const std::string_view args[]{record.payload, Contract::replyLifetime, group, messageId};
                const auto reply = co_await redis.eval(kReplyScript, keys, args);
                if (reply.kind() == ruvia::RedisValue::Kind::kError)
                    service::message::redis::throwValue("RPC persist reply", reply);
    }
};

} // namespace service::rpc
