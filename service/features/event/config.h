#pragma once

#include <string>
#include <string_view>
#include <stdexcept>
#include <vector>

#include <ruvia/web/Context.h>

#include "service/common/http.h"
#include "service/common/message/contract.h"
#include "service/common/message/shard.h"
#include "service/features/access/stream.h"
#include "service/features/collector/stream.h"

namespace service::message {

inline constexpr std::string_view kRuntimeConfigChangesBase{
    "iot:channel:runtime:config-change"};

inline std::string webhookCatalogChangesStream(std::size_t workerIndex) {
    return service::access::stream::catalogChanges(workerIndex);
}

inline std::string runtimeConfigChangesStream(std::size_t shardIndex) {
    return std::string(kRuntimeConfigChangesBase) + ":" +
           std::to_string(shardIndex);
}

inline std::string runtimeConfigChangesStream(std::string_view aggregateId) {
    return runtimeConfigChangesStream(service::message::shard::index(aggregateId));
}

inline constexpr std::string_view kConfigEventType{"config.changed"};
inline constexpr std::string_view kConfigEventSchemaVersion{"1"};

template <typename Database>
inline ruvia::Task<void> enqueueConfigEvent(Database& database, std::string_view aggregate,
                                            std::string_view action,
                                            std::string_view aggregateId) {
    const bool supported = aggregate == "link" || aggregate == "device" ||
                           aggregate == "protocol" || aggregate == "access_key" ||
                           aggregate == "webhook";
    if (!supported)
        throw std::invalid_argument("unsupported outbox aggregate: " + std::string(aggregate));
    const auto eventId = service::message::nextMessageId();
    (void)co_await database.execute(
        R"sql(INSERT INTO outbox_event(
  id, event_type, aggregate_type, aggregate_id, action, schema_version, payload)
VALUES ($1::uuid, $2, $3, $4, $5, $6::integer, '{}'::jsonb))sql",
        service::common::dbParams(eventId, kConfigEventType,
                                  aggregate, aggregateId, action,
                                  kConfigEventSchemaVersion));
}

template <typename Redis>
inline ruvia::Task<void> publishConfigEnvelope(
    const Redis& redis, std::string_view eventId, std::string_view eventType,
    std::string_view aggregate, std::string_view aggregateId, std::string_view action,
    std::string_view schemaVersion, std::string_view occurredAtMs,
    std::size_t serviceWorkerCount = 1) {
    const bool runtimeChange =
        aggregate == "link" || aggregate == "device" || aggregate == "protocol";
    const bool webhookChange = aggregate == "access_key" || aggregate == "webhook" ||
                               aggregate == "device" || aggregate == "protocol";
    const bool accessSessionChange = aggregate == "access_key" || aggregate == "device";
    if (!runtimeChange && !webhookChange && !accessSessionChange)
        co_return;
    if (serviceWorkerCount == 0 ||
        serviceWorkerCount > service::access::stream::kPartitionCount)
        throw std::invalid_argument("invalid Service Worker count for config event");
    const std::vector<service::message::StreamField> fields{
        {"message_id", std::string(eventId)},
        {"event_id", std::string(eventId)},
        {"event_type", std::string(eventType)},
        {"schema_version", std::string(schemaVersion)},
        {"aggregate", std::string(aggregate)},
        {"aggregate_type", std::string(aggregate)},
        {"action", std::string(action)},
        {"aggregate_id", std::string(aggregateId)},
        {"created_at_ms", std::string(occurredAtMs)},
        {"occurred_at_ms", std::string(occurredAtMs)}};
    auto pipeline = redis.pipeline();
    const auto partition = service::message::shard::index(aggregateId);
    const auto targetWorker = partition % serviceWorkerCount;
    if (runtimeChange)
        service::message::redis::queueAddAndWake(
            pipeline, runtimeConfigChangesStream(partition), fields, targetWorker,
            WorkerStreamTask::Reconciler, 10000);
    if (webhookChange) {
        // Every Worker owns an independent in-memory catalog, so each receives one
        // refresh event. Data events themselves remain partitioned, never broadcast.
        for (std::size_t workerIndex = 0; workerIndex < serviceWorkerCount;
             ++workerIndex)
            service::message::redis::queueAddAndWake(
                pipeline, webhookCatalogChangesStream(workerIndex), fields,
                workerIndex, WorkerStreamTask::Webhook, 10000);
    }
    if (accessSessionChange)
        service::message::redis::queueAddAndWake(
            pipeline, service::access::stream::sessionChanges(partition), fields,
            targetWorker, WorkerStreamTask::Webhook, 10000);
    const auto replies = co_await std::move(pipeline).exec();
    service::message::redis::requirePipelineSuccess("publish config change", replies);
    co_return;
}

} // namespace service::message
