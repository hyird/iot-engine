#pragma once

#include <charconv>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

#include <ruvia/web/db/Db.h>
#include <ruvia/web/db/DbQuery.h>

#include "service/common/database.h"
#include "service/common/http.h"
#include "service/common/worker.h"
#include "service/modules/system/outbox/outbox.entity.h"
#include "service/modules/system/outbox/outbox.types.h"

namespace service::system {

namespace db = service::common::database;

class OutboxService final {
  public:
    template <typename Database>
    static ruvia::Task<void> enqueueConfigEvent(Database& database, std::string_view aggregate, std::string_view action, std::string_view aggregateId, std::string_view eventId) {
        if (aggregate != "link" && aggregate != "device" && aggregate != "protocol" &&
            aggregate != "access_key" && aggregate != "webhook") {
            throw std::invalid_argument("unsupported outbox aggregate: " + std::string(aggregate));
        }
        ruvia::DbQuery query;
        query.insertInto(OutboxEventEntity::tableName(), { "id", "event_type", "aggregate_type", "aggregate_id", "action", "schema_version", "payload" })
            .values({ query.cast(query.value(eventId), ruvia::DbDataType::kUuid), query.value("config.changed"), query.value(aggregate), query.value(aggregateId), query.value(action), query.value(std::int32_t{ 1 }), query.cast(query.value("{}"), ruvia::DbDataType::kJsonb) });
        (void)co_await database.execute(query);
    }

    static OutboxService& instance() {
        thread_local OutboxService service;
        return service;
    }

    template <typename Context>
    ruvia::Task<ruvia::BoxedArray<OutboxDeadLetterDto>> deadLetters(
        Context& context
    ) {
        const auto rows = co_await context.db().query(deadLetterSelect(context.pool()));
        ruvia::BoxedArray<OutboxDeadLetterDto> items(
            ruvia::ModelOptions{ .resource = context.arena() }
        );
        for (const auto& row : rows) {
            auto& item = items.emplace(ruvia::ModelOptions{ .resource = context.arena() });
            item.set<"id">(row[0].value().value_or(std::string_view{}))
                .template set<"eventType">(row[1].value().value_or(std::string_view{}))
                .template set<"aggregateType">(row[2].value().value_or(std::string_view{}))
                .template set<"aggregateId">(row[3].value().value_or(std::string_view{}))
                .template set<"action">(row[4].value().value_or(std::string_view{}))
                .template set<"schemaVersion">(
                    integer(row[5].value().value_or(std::string_view{}))
                )
                .template set<"attempts">(integer(row[6].value().value_or(std::string_view{})))
                .template set<"lastError">(row[7].value().value_or(std::string_view{}))
                .template set<"occurredAt">(row[8].value().value_or(std::string_view{}))
                .template set<"deadLetteredAt">(row[9].value().value_or(std::string_view{}));
        }
        co_return items;
    }

    template <typename Context>
    ruvia::Task<void> replay(Context& context, std::string_view eventId, std::size_t workerIndex) {

        ruvia::DbQuery query(context.pool());
        const auto id = query.binary(
            query.column("id"),
            ruvia::DbBinaryOperator::kEqual,
            query.cast(query.value(eventId), ruvia::DbDataType::kUuid)
        );
        const auto pending = query.unary(
            ruvia::DbUnaryOperator::kIsNull,
            query.column("published_at")
        );
        const auto deadLettered = query.unary(
            ruvia::DbUnaryOperator::kIsNotNull,
            query.column("dead_lettered_at")
        );
        query.update(OutboxEventEntity::tableName())
            .set("attempts", query.value(std::int32_t{ 0 }))
            .set("last_error", query.nullValue())
            .set("available_at", query.call("now"))
            .set("dead_lettered_at", query.nullValue())
            .where(query.binary(query.binary(id, ruvia::DbBinaryOperator::kAnd, pending), ruvia::DbBinaryOperator::kAnd, deadLettered))
            .returning({ query.cast(query.column("id"), ruvia::DbDataType::kText) });

        auto transaction = co_await context.db().beginTransaction();
        const auto rows = co_await transaction.query(query);
        if (rows.empty()) {
            service::common::fail(service::common::kNotFoundErrorCode, "死信事件不存在或已重放", 404);
        }
        const auto counterId = service::runtime::instanceId() + ":" + std::to_string(workerIndex);
        ruvia::DbQuery counter(context.pool());
        counter.insertInto(OutboxReplayCounterEntity::tableName(), { "id", "replays" })
            .values({ counter.value(counterId), counter.value(std::int64_t{ 1 }) })
            .onConflict({ .columns = { "id" }, .update = { { "replays", counter.binary(counter.column(OutboxReplayCounterEntity::columnName<"replays">(), OutboxReplayCounterEntity::tableName()), ruvia::DbBinaryOperator::kAdd, counter.value(std::int64_t{ 1 })) }, { "updated_at", counter.call("now") } } });
        (void)co_await transaction.execute(counter);
        co_await transaction.commit();
    }

  private:
    static std::int64_t integer(std::string_view value) {
        std::int64_t result{};
        const auto [end, error] =
            std::from_chars(value.data(), value.data() + value.size(), result);
        return error == std::errc{} && end == value.data() + value.size() ? result : 0;
    }

    static ruvia::DbQuery deadLetterSelect(std::pmr::memory_resource* resource) {
        ruvia::DbQuery query(resource);
        const auto where = OutboxEventEntity::column<"published_at">().isNull() &&
            OutboxEventEntity::column<"dead_lettered_at">().isNotNull();
        query.select({ query.cast(query.column("id"), ruvia::DbDataType::kText), query.column("event_type"), query.column("aggregate_type"), query.column("aggregate_id"), query.column("action"), query.cast(query.column("schema_version"), ruvia::DbDataType::kText), query.cast(query.column("attempts"), ruvia::DbDataType::kText), db::emptyText(query, "last_error"), query.cast(query.column("occurred_at"), ruvia::DbDataType::kText), query.cast(query.column("dead_lettered_at"), ruvia::DbDataType::kText) })
            .from(OutboxEventEntity::tableName())
            .where(where.expression(query))
            .orderBy(query.column("dead_lettered_at"))
            .orderBy(query.column("occurred_at"))
            .orderBy(query.column("id"))
            .limit(100);
        return query;
    }
};

inline OutboxService& outboxService() {
    return OutboxService::instance();
}

} // namespace service::system
