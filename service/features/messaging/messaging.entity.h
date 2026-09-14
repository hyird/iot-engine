#pragma once
#include <ruvia/web/db/DbEntity.h>
#include "service/common/message.h"
namespace service::message {
RUVIA_DB_ENTITY(WorkerSnapshotEntity, service::message::worker_metrics::kSnapshotTable,
    RUVIA_DB_COLUMN(id, ruvia::String, ruvia::DbColumnOptions{.primaryKey = true}),
    RUVIA_DB_COLUMN(metrics, ruvia::String),
    RUVIA_DB_COLUMN(ready, bool),
    RUVIA_DB_COLUMN(health, ruvia::String));
}

namespace service::messaging::persistence {

RUVIA_DB_ENTITY(OutboxConsumerReceiptEntity, "outbox_consumer_receipt",
    RUVIA_DB_COLUMN(consumer_name, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .primaryKey = true, .length = 150}),
    RUVIA_DB_COLUMN(event_id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid, .primaryKey = true}),
    RUVIA_DB_COLUMN(processed_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz}))

RUVIA_DB_ENTITY(OutboxEventEntity, "outbox_event",
    RUVIA_DB_COLUMN(id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid, .primaryKey = true}),
    RUVIA_DB_COLUMN(event_type, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 100}),
    RUVIA_DB_COLUMN(aggregate_type, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 100}),
    RUVIA_DB_COLUMN(aggregate_id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kText}),
    RUVIA_DB_COLUMN(action, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .length = 50}),
    RUVIA_DB_COLUMN(schema_version, std::int32_t,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kInteger}),
    RUVIA_DB_COLUMN(payload, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kJsonb}),
    RUVIA_DB_COLUMN(occurred_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz}),
    RUVIA_DB_COLUMN(available_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz}),
    RUVIA_DB_COLUMN(attempts, std::int32_t,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kInteger}),
    RUVIA_DB_COLUMN(last_error, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kText, .nullable = true}),
    RUVIA_DB_COLUMN(published_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .nullable = true}),
    RUVIA_DB_COLUMN(dead_lettered_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .nullable = true}))

} // namespace service::messaging::persistence
