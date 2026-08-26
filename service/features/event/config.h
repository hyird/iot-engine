#pragma once

#include <string>
#include <string_view>
#include <stdexcept>
#include <vector>

#include <ruvia/web/Context.h>

#include "service/common/http.h"
#include "service/common/message/contract.h"
#include "service/features/collector/stream.h"

namespace service::message {

inline constexpr std::string_view kRuntimeConfigChangesStream{
    "iot:channel:runtime:config-change"};
inline constexpr std::string_view kWebhookCatalogChangesStream{
    "iot:channel:open-access:config-change"};

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
    std::string_view schemaVersion, std::string_view occurredAtMs) {
    const bool runtimeChange =
        aggregate == "link" || aggregate == "device" || aggregate == "protocol";
    const bool webhookChange = aggregate == "access_key" || aggregate == "webhook" ||
                               aggregate == "device" || aggregate == "protocol";
    if (!runtimeChange && !webhookChange)
        co_return;
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
    if (runtimeChange)
        service::message::redis::queueAdd(
            pipeline, kRuntimeConfigChangesStream, fields, 10000);
    if (webhookChange)
        service::message::redis::queueAdd(
            pipeline, kWebhookCatalogChangesStream, fields, 10000);
    const auto replies = co_await std::move(pipeline).exec();
    service::message::redis::requirePipelineSuccess("publish config change", replies);
    co_return;
}

} // namespace service::message
