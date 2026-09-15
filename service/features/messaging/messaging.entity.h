#pragma once
#include <ruvia/web/db/DbEntity.h>
#include <ruvia/web/redis/RedisEntity.h>
#include "service/common/message.h"
namespace service::message {
RUVIA_REDIS_ENTITY(WorkerSnapshotEntity, service::message::worker_metrics::kSnapshotTable,
    RUVIA_REDIS_COLUMN(id, ruvia::String, ruvia::RedisColumnOptions{.primaryKey = true}),
    RUVIA_REDIS_COLUMN(metrics, ruvia::String),
    RUVIA_REDIS_COLUMN(ready, bool),
    RUVIA_REDIS_COLUMN(health, ruvia::String));
}

namespace service::messaging::persistence {

RUVIA_DB_ENTITY(OutboxReplayCounterEntity, "outbox_replay_counter",
    RUVIA_DB_COLUMN(id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kText, .primaryKey = true}),
    RUVIA_DB_COLUMN(replays, std::int64_t,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kBigInt, .defaultExpression = ruvia::FixedString{"0"}}),
    RUVIA_DB_COLUMN(updated_at, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kTimestampTz, .defaultExpression = ruvia::FixedString{"now()"}}))

RUVIA_DB_ENTITY(OutboxConsumerReceiptEntity, "outbox_consumer_receipt",
    RUVIA_DB_COLUMN(consumer_name, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kVarchar, .primaryKey = true, .length = 150}),
    RUVIA_DB_COLUMN(event_id, std::pmr::string,
        ruvia::DbColumnOptions{.dataType = ruvia::DbDataType::kUuid, .primaryKey = true}),
    RUVIA_DB_COLUMN(processed_at, std::pmr::string,
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

} // namespace service::messaging::persistence

namespace service::rpc {
// RPC 结果以原始 JSON 存入固定 Redis String 键；与通知和消息确认原子写入。
// Redis ORM 的自动键前缀不能表达既有 Contract::reply 键，service 使用此映射。
struct RpcReplyRecord final {
    std::string payload;
};
} // namespace service::rpc
