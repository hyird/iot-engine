#pragma once

#include <cstdint>
#include <memory_resource>
#include <string>

#include <ruvia/web/db/DbEntity.h>

namespace service::system {

RUVIA_DB_ENTITY(OutboxReplayCounterEntity, "outbox_replay_counter",
    RUVIA_DB_COLUMN(id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kText, .primaryKey = true}),
    RUVIA_DB_COLUMN(replays, std::int64_t,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kBigInt, .defaultExpression = ruvia::FixedString{"0"}}),
    RUVIA_DB_COLUMN(updated_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}))

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
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kJsonb, .defaultExpression = ruvia::FixedString{"'{}'::jsonb"}}),
    RUVIA_DB_COLUMN(occurred_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}),
    RUVIA_DB_COLUMN(available_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}),
    RUVIA_DB_COLUMN(attempts, std::int32_t,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kInteger, .defaultExpression = ruvia::FixedString{"0"}}),
    RUVIA_DB_COLUMN(last_error, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kText, .nullable = true}),
    RUVIA_DB_COLUMN(published_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .nullable = true}),
    RUVIA_DB_COLUMN(dead_lettered_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .nullable = true}))

} // namespace service::system
