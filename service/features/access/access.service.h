#pragma once

#include <memory>

#include "service/utils/number.h"
#include "service/features/access/access.entity.h"
#include "service/common/http.h"


#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/web/ModelObject.h>
#include <ruvia/web/db/DbQuery.h>

#include "service/common/uuid.h"
#include "service/common/timestamp.h"
#include "service/utils/text.h"
#include "service/utils/json.h"
#include "service/features/messaging/messaging.transport.h"

namespace service::access::session {

template <typename Context>
ruvia::Task<void> refresh(Context& context, bool onlyIfMissing = false) {
    // Serialize the database snapshot and Redis pointer swap across service instances.
    // The lock is held only by cold-path configuration projection, never by API reads.
    auto transaction = co_await context.db().beginTransaction();
    ruvia::DbQuery advisoryLock(context.pool());
    advisoryLock.select(advisoryLock.call(
        "pg_advisory_xact_lock",
        {advisoryLock.cast(advisoryLock.value(std::int64_t{734622}),
                           ruvia::DbDataType::kBigInt)}));
    (void)co_await transaction.query(advisoryLock);
    if (onlyIfMissing) {
        const auto active = co_await service::message::redis::command(
            context.redis(), {"GET", std::string(kActiveVersionKey)});
        if (!active.null()) {
            if (active.kind() != ruvia::RedisValue::Kind::kString)
                service::message::redis::throwValue(
                    "read active access-session projection", active);
            co_await transaction.commit();
            co_return;
        }
    }
    ruvia::DbQuery snapshot(context.pool());
    const auto expiresAtMs = snapshot.cast(
        snapshot.coalesce({
            snapshot.cast(
                snapshot.binary(
                    snapshot.extract(ruvia::DbDatePart::kEpoch,
                                     snapshot.column(service::access::persistence::OpenAccessKeyEntity::columnName<"expires_at">(), "key")),
                    ruvia::DbBinaryOperator::kMultiply,
                    snapshot.value(std::int64_t{1000})),
                ruvia::DbDataType::kBigInt),
            snapshot.value(std::int64_t{0})
        }),
        ruvia::DbDataType::kText);
    const std::array bindingOrder{
        ruvia::DbOrderTerm{snapshot.column(service::access::persistence::OpenAccessKeyDeviceEntity::columnName<"device_id">(), "binding")}
    };
    const auto bindingIds = snapshot.aggregate(
        "jsonb_agg", {snapshot.cast(snapshot.column(service::access::persistence::OpenAccessKeyDeviceEntity::columnName<"device_id">(), "binding"),
                                    ruvia::DbDataType::kText)},
        false, bindingOrder);
    const auto bindingIdsOrEmpty = snapshot.coalesce({
        snapshot.filter(bindingIds, snapshot.unary(
            ruvia::DbUnaryOperator::kIsNotNull,
            snapshot.column(service::access::persistence::OpenAccessKeyDeviceEntity::columnName<"device_id">(), "binding"))),
        snapshot.cast(snapshot.value("[]"), ruvia::DbDataType::kJsonb)
    });
    snapshot
        .select({snapshot.column(service::access::persistence::OpenAccessKeyEntity::columnName<"access_key_hash">(), "key"),
                 snapshot.cast(snapshot.column(service::access::persistence::OpenAccessKeyEntity::columnName<"id">(), "key"), ruvia::DbDataType::kText),
                 snapshot.column(service::access::persistence::OpenAccessKeyEntity::columnName<"name">(), "key"),
                 snapshot.cast(snapshot.column(service::access::persistence::OpenAccessKeyEntity::columnName<"status">(), "key"), ruvia::DbDataType::kText),
                 expiresAtMs,
                 snapshot.cast(snapshot.column(service::access::persistence::OpenAccessKeyEntity::columnName<"scopes">(), "key"), ruvia::DbDataType::kText),
                 snapshot.cast(bindingIdsOrEmpty, ruvia::DbDataType::kText)})
        .from(service::access::persistence::OpenAccessKeyEntity::tableName(), "key")
        .join(ruvia::DbJoinType::kLeft, service::access::persistence::OpenAccessKeyDeviceEntity::tableName(),
              snapshot.binary(snapshot.column(service::access::persistence::OpenAccessKeyDeviceEntity::columnName<"access_key_id">(), "binding"),
                              ruvia::DbBinaryOperator::kEqual,
                              snapshot.column(service::access::persistence::OpenAccessKeyEntity::columnName<"id">(), "key")),
              "binding")
        .where(snapshot.unary(ruvia::DbUnaryOperator::kIsNull,
                              snapshot.column(service::access::persistence::OpenAccessKeyEntity::columnName<"deleted_at">(), "key")))
        .groupBy({snapshot.column(service::access::persistence::OpenAccessKeyEntity::columnName<"id">(), "key")})
        .orderBy(snapshot.column(service::access::persistence::OpenAccessKeyEntity::columnName<"id">(), "key"));
    const auto rows = co_await transaction.query(snapshot);

    const auto version = context.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next();
    const auto versionKey = std::string(kVersionPrefix) + version;
    constexpr std::size_t chunkSize = 128;
    for (std::size_t offset = 0; offset < rows.size(); offset += chunkSize) {
        const auto end = std::min(rows.size(), offset + chunkSize);
        std::vector<std::string> command{"HSET", versionKey};
        command.reserve(2 + (end - offset) * 2);
        for (std::size_t index = offset; index < end; ++index) {
            const auto& row = rows[index];
            command.emplace_back(row[0].value().value_or(std::string_view{}));
            command.push_back(encode(row[1].value().value_or(std::string_view{}), row[2].value().value_or(std::string_view{}), row[3].value().value_or(std::string_view{}),
                                     row[4].value().value_or(std::string_view{}), row[5].value().value_or(std::string_view{}), row[6].value().value_or(std::string_view{})));
        }
        (void)co_await service::message::redis::command(context.redis(), command);
    }
    if (rows.empty())
        (void)co_await service::message::redis::command(
            context.redis(), {"HSET", versionKey, "__ready", "1"});
    (void)co_await service::message::redis::command(
        context.redis(), {"EXPIRE", versionKey, "600"});

    static constexpr std::string_view swapScript = R"lua(
local previous = redis.call('GET', KEYS[1])
redis.call('SET', KEYS[1], ARGV[1])
redis.call('PERSIST', KEYS[2])
if previous and previous ~= ARGV[1] then
  redis.call('EXPIRE', ARGV[2] .. previous, ARGV[3])
end
return previous or ''
)lua";
    const std::string activeKey(kActiveVersionKey);
    const std::string_view keys[]{activeKey, versionKey};
    const std::string prefix(kVersionPrefix);
    const std::string grace = "120";
    const std::string_view args[]{version, prefix, grace};
    const auto reply = co_await context.redis().eval(swapScript, keys, args);
    if (reply.kind() != ruvia::RedisValue::Kind::kString)
        service::message::redis::throwValue("activate access-session projection", reply);
    co_await transaction.commit();
}

template <typename Context> ruvia::Task<void> ensure(Context& context) {
    const auto active = co_await service::message::redis::command(
        context.redis(), {"GET", std::string(kActiveVersionKey)});
    if (!active.null()) {
        if (active.kind() != ruvia::RedisValue::Kind::kString)
            service::message::redis::throwValue(
                "read active access-session projection", active);
        co_return;
    }
    co_await refresh(context, true);
}

} // namespace service::access::session

#include <ruvia/web/WebWorker.h>
#include "service/common/message.h"
#include "service/features/access/access.types.h"

namespace service::access::webhook {

inline ruvia::Task<Catalog> loadCatalog(ruvia::WebWorkerContext& context) {
        ruvia::DbQuery catalog(context.pool());
        const auto expiresAtMs = catalog.cast(
            catalog.coalesce({
                catalog.cast(
                    catalog.binary(
                        catalog.extract(ruvia::DbDatePart::kEpoch,
                                        catalog.column(service::access::persistence::OpenAccessKeyEntity::columnName<"expires_at">(), "key")),
                        ruvia::DbBinaryOperator::kMultiply,
                        catalog.value(std::int64_t{1000})),
                    ruvia::DbDataType::kBigInt),
                catalog.value(std::int64_t{0})
            }),
            ruvia::DbDataType::kText);
        const auto eventTypes = catalog.call(
            "jsonb_array_elements_text",
            {catalog.caseWhen(
                {{catalog.binary(
                      catalog.call("jsonb_typeof",
                                   {catalog.column(service::access::persistence::OpenWebhookEntity::columnName<"event_types">(), "webhook")}),
                      ruvia::DbBinaryOperator::kEqual,
                      catalog.value("array")),
                  catalog.column(service::access::persistence::OpenWebhookEntity::columnName<"event_types">(), "webhook")}},
                catalog.cast(catalog.value("[]"), ruvia::DbDataType::kJsonb))});
        catalog
            .select({catalog.cast(catalog.column(service::access::persistence::OpenAccessKeyDeviceEntity::columnName<"device_id">(), "binding"),
                                  ruvia::DbDataType::kText),
                     catalog.column(service::access::persistence::DeviceEntity::columnName<"name">(), "device"),
                     catalog.coalesce({
                         catalog.binary(catalog.column(service::access::persistence::DeviceEntity::columnName<"protocol_params">(), "device"),
                                        ruvia::DbBinaryOperator::kJsonGetText,
                                        catalog.value("device_code")),
                         catalog.value("")}),
                     catalog.cast(catalog.column(service::access::persistence::OpenWebhookEntity::columnName<"id">(), "webhook"),
                                  ruvia::DbDataType::kText),
                     catalog.cast(catalog.column(service::access::persistence::OpenWebhookEntity::columnName<"access_key_id">(), "webhook"),
                                  ruvia::DbDataType::kText),
                     catalog.column(service::access::persistence::OpenWebhookEntity::columnName<"url">(), "webhook"),
                     catalog.coalesce({catalog.column(service::access::persistence::OpenWebhookEntity::columnName<"secret">(), "webhook"),
                                       catalog.value("")}),
                     catalog.cast(catalog.column(service::access::persistence::OpenWebhookEntity::columnName<"headers">(), "webhook"),
                                  ruvia::DbDataType::kText),
                     catalog.cast(catalog.column(service::access::persistence::OpenWebhookEntity::columnName<"timeout_seconds">(), "webhook"),
                                  ruvia::DbDataType::kText),
                     catalog.caseWhen(
                         {{catalog.column(service::access::persistence::OpenWebhookEntity::columnName<"skip_tls_verify">(), "webhook"),
                           catalog.value("1")}},
                         catalog.value("0")),
                     expiresAtMs,
                     catalog.column("value", "event_type")})
            .from(service::access::persistence::OpenWebhookEntity::tableName(), "webhook")
            .join(ruvia::DbJoinType::kInner, service::access::persistence::OpenAccessKeyEntity::tableName(),
                  catalog.binary(catalog.column(service::access::persistence::OpenAccessKeyEntity::columnName<"id">(), "key"),
                                 ruvia::DbBinaryOperator::kEqual,
                                 catalog.column(service::access::persistence::OpenWebhookEntity::columnName<"access_key_id">(), "webhook")),
                  "key")
            .join(ruvia::DbJoinType::kInner, service::access::persistence::OpenAccessKeyDeviceEntity::tableName(),
                  catalog.binary(catalog.column(service::access::persistence::OpenAccessKeyDeviceEntity::columnName<"access_key_id">(), "binding"),
                                 ruvia::DbBinaryOperator::kEqual,
                                 catalog.column(service::access::persistence::OpenAccessKeyEntity::columnName<"id">(), "key")),
                  "binding")
            .join(ruvia::DbJoinType::kInner, service::access::persistence::DeviceEntity::tableName(),
                  catalog.binary(catalog.column(service::access::persistence::DeviceEntity::columnName<"id">(), "device"),
                                 ruvia::DbBinaryOperator::kEqual,
                                 catalog.column(service::access::persistence::OpenAccessKeyDeviceEntity::columnName<"device_id">(), "binding")),
                  "device")
            .joinFunction(ruvia::DbJoinType::kCross, eventTypes, {}, "event_type",
                          {.lateral = true, .columns = {{.name = "value"}}})
            .where(catalog.unary(ruvia::DbUnaryOperator::kIsNull,
                                 catalog.column(service::access::persistence::OpenWebhookEntity::columnName<"deleted_at">(), "webhook")))
            .andWhere(catalog.binary(catalog.column(service::access::persistence::OpenWebhookEntity::columnName<"status">(), "webhook"),
                                    ruvia::DbBinaryOperator::kEqual,
                                    catalog.value("enabled")))
            .andWhere(catalog.unary(ruvia::DbUnaryOperator::kIsNull,
                                    catalog.column(service::access::persistence::OpenAccessKeyEntity::columnName<"deleted_at">(), "key")))
            .andWhere(catalog.binary(catalog.column(service::access::persistence::OpenAccessKeyEntity::columnName<"status">(), "key"),
                                    ruvia::DbBinaryOperator::kEqual,
                                    catalog.value("enabled")))
            .andWhere(catalog.binary(
                catalog.unary(ruvia::DbUnaryOperator::kIsNull,
                              catalog.column(service::access::persistence::OpenAccessKeyEntity::columnName<"expires_at">(), "key")),
                ruvia::DbBinaryOperator::kOr,
                catalog.binary(catalog.column(service::access::persistence::OpenAccessKeyEntity::columnName<"expires_at">(), "key"),
                               ruvia::DbBinaryOperator::kGreater,
                               catalog.call("now"))))
            .andWhere(catalog.unary(ruvia::DbUnaryOperator::kIsNull,
                                    catalog.column(service::access::persistence::DeviceEntity::columnName<"deleted_at">(), "device")))
            .orderBy(catalog.column(service::access::persistence::OpenAccessKeyDeviceEntity::columnName<"device_id">(), "binding"))
            .addOrderBy(catalog.column("value", "event_type"))
            .addOrderBy(catalog.column(service::access::persistence::OpenWebhookEntity::columnName<"id">(), "webhook"));
        const auto rows = co_await context.db().query(catalog);
        Catalog result;
        for (const auto& row : rows) {
            const std::string deviceId(row[0].value().value_or(std::string_view{}));
            auto& device = result[deviceId];
            device.name.assign(row[1].value().value_or(std::string_view{}));
            device.code.assign(row[2].value().value_or(std::string_view{}));
            const auto timeout =
                service::utils::parseInt64(std::optional<std::string_view>{row[8].value().value_or(std::string_view{})})
                    .value_or(5);
            const auto expiresAtMs =
                service::utils::parseInt64(std::optional<std::string_view>{row[10].value().value_or(std::string_view{})})
                    .value_or(0);
            device.targets[std::string(row[11].value().value_or(std::string_view{}))].push_back(
                {std::string(row[3].value().value_or(std::string_view{})), std::string(row[4].value().value_or(std::string_view{})),
                 std::string(row[5].value().value_or(std::string_view{})), std::string(row[6].value().value_or(std::string_view{})),
                 std::string(row[7].value().value_or(std::string_view{})), timeout > 0 ? timeout : std::int64_t{5},
                 row[9].value().value_or(std::string_view{}) == "1", expiresAtMs});
        }
        co_return result;
    }

inline ruvia::Task<void> persistResults(
    ruvia::WebWorkerContext& context,
    const std::vector<message::StreamMessage>& messages) {
        if (messages.empty())
            co_return;
        ruvia::DbQuery incoming(context.pool());
        for (std::size_t index = 0; index < messages.size(); ++index) {
            const auto httpStatus = service::utils::parseInt64(
                std::optional<std::string_view>(messages[index].get("http_status")));
            const auto completedAt = service::utils::parseInt64(
                std::optional<std::string_view>(messages[index].get("completed_at_ms")));
            incoming.values({
                incoming.cast(incoming.value(static_cast<std::int64_t>(index)),
                              ruvia::DbDataType::kBigInt),
                incoming.cast(incoming.value(messages[index].get("log_id")),
                              ruvia::DbDataType::kUuid),
                incoming.cast(incoming.value(messages[index].get("access_key_id")),
                              ruvia::DbDataType::kUuid),
                incoming.cast(incoming.value(messages[index].get("webhook_id")),
                              ruvia::DbDataType::kUuid),
                incoming.cast(incoming.value(messages[index].get("event_type")),
                              ruvia::DbDataType::kText),
                incoming.cast(incoming.value(messages[index].get("status")),
                              ruvia::DbDataType::kText),
                incoming.cast(incoming.value(messages[index].get("target")),
                              ruvia::DbDataType::kText),
                incoming.cast(incoming.value(httpStatus.value_or(0)),
                              ruvia::DbDataType::kBigInt),
                incoming.cast(incoming.value(messages[index].get("device_id")),
                              ruvia::DbDataType::kUuid),
                incoming.cast(incoming.value(messages[index].get("device_code")),
                              ruvia::DbDataType::kText),
                incoming.cast(incoming.value(messages[index].get("message")),
                              ruvia::DbDataType::kText),
                incoming.cast(incoming.value(messages[index].get("request_payload")),
                              ruvia::DbDataType::kJsonb),
                incoming.cast(incoming.value(messages[index].get("response_payload")),
                              ruvia::DbDataType::kJsonb),
                incoming.cast(
                    incoming.value(completedAt.value_or(
                        service::message::utcNowMilliseconds())),
                    ruvia::DbDataType::kBigInt)
            });
        }
        ruvia::DbQuery latest(context.pool());
        latest
            .select(latest.star())
            .distinctOn({latest.column("webhook_id", "incoming")})
            .from("incoming", "incoming")
            .orderBy(latest.column("webhook_id", "incoming"))
            .addOrderBy(latest.column("completed_at_ms", "incoming"),
                        ruvia::DbOrderDirection::kDesc)
            .addOrderBy(latest.column("sequence", "incoming"),
                        ruvia::DbOrderDirection::kDesc);

        ruvia::DbQuery summary(context.pool());
        const auto successfulAt = summary.filter(
            summary.aggregate("max", {summary.column("completed_at_ms", "incoming")}),
            summary.binary(summary.column("status", "incoming"),
                           ruvia::DbBinaryOperator::kEqual,
                           summary.value("success")));
        const auto failedAt = summary.filter(
            summary.aggregate("max", {summary.column("completed_at_ms", "incoming")}),
            summary.binary(summary.column("status", "incoming"),
                           ruvia::DbBinaryOperator::kEqual,
                           summary.value("failed")));
        summary
            .select({summary.column("webhook_id", "incoming"),
                     summary.alias(successfulAt, "last_success_ms"),
                     summary.alias(failedAt, "last_failure_ms")})
            .from("incoming", "incoming")
            .groupBy({summary.column("webhook_id", "incoming")});

        ruvia::DbQuery updated(context.pool());
        const auto asTimestamp = [&updated](ruvia::DbExpression milliseconds) {
            return updated.call(
                "to_timestamp",
                {updated.binary(
                    updated.cast(milliseconds, ruvia::DbDataType::kDouble),
                    ruvia::DbBinaryOperator::kDivide,
                    updated.value(1000.0))});
        };
        const auto epoch = updated.call("to_timestamp", {updated.value(0.0)});
        const auto latestTimestamp = asTimestamp(
            updated.column("completed_at_ms", "latest"));
        const auto successTimestamp = asTimestamp(
            updated.column("last_success_ms", "summary"));
        const auto failureTimestamp = asTimestamp(
            updated.column("last_failure_ms", "summary"));
        const auto latestIsNewer = updated.binary(
            updated.unary(ruvia::DbUnaryOperator::kIsNull,
                          updated.column(service::access::persistence::OpenWebhookEntity::columnName<"last_triggered_at">(), "webhook")),
            ruvia::DbBinaryOperator::kOr,
            updated.binary(updated.column(service::access::persistence::OpenWebhookEntity::columnName<"last_triggered_at">(), "webhook"),
                           ruvia::DbBinaryOperator::kLessEqual,
                           latestTimestamp));
        updated
            .update(service::access::persistence::OpenWebhookEntity::tableName(), "webhook")
            .set(service::access::persistence::OpenWebhookEntity::columnName<"last_triggered_at">(),
                 updated.greatest({
                     updated.coalesce({updated.column(service::access::persistence::OpenWebhookEntity::columnName<"last_triggered_at">(), "webhook"),
                                       epoch}),
                     latestTimestamp
                 }))
            .set(service::access::persistence::OpenWebhookEntity::columnName<"last_success_at">(),
                 updated.caseWhen(
                     {{updated.unary(
                           ruvia::DbUnaryOperator::kIsNull,
                           updated.column("last_success_ms", "summary")),
                       updated.column(service::access::persistence::OpenWebhookEntity::columnName<"last_success_at">(), "webhook")}},
                     updated.greatest({
                         updated.coalesce({updated.column(service::access::persistence::OpenWebhookEntity::columnName<"last_success_at">(), "webhook"),
                                           epoch}),
                         successTimestamp
                     })))
            .set(service::access::persistence::OpenWebhookEntity::columnName<"last_failure_at">(),
                 updated.caseWhen(
                     {{updated.unary(
                           ruvia::DbUnaryOperator::kIsNull,
                           updated.column("last_failure_ms", "summary")),
                       updated.column(service::access::persistence::OpenWebhookEntity::columnName<"last_failure_at">(), "webhook")}},
                     updated.greatest({
                         updated.coalesce({updated.column(service::access::persistence::OpenWebhookEntity::columnName<"last_failure_at">(), "webhook"),
                                           epoch}),
                         failureTimestamp
                     })))
            .set(service::access::persistence::OpenWebhookEntity::columnName<"last_http_status">(),
                 updated.caseWhen(
                     {{latestIsNewer,
                       updated.nullIf(updated.column("http_status", "latest"),
                                      updated.value(std::int64_t{0}))}},
                     updated.column(service::access::persistence::OpenWebhookEntity::columnName<"last_http_status">(), "webhook")))
            .set(service::access::persistence::OpenWebhookEntity::columnName<"last_error">(),
                 updated.caseWhen(
                     {{latestIsNewer,
                       updated.nullIf(updated.column("message", "latest"),
                                      updated.value(""))}},
                     updated.column(service::access::persistence::OpenWebhookEntity::columnName<"last_error">(), "webhook")))
            .set(service::access::persistence::OpenWebhookEntity::columnName<"updated_at">(), updated.call("now"))
            .updateFrom("summary", "summary")
            .join(ruvia::DbJoinType::kInner, "latest",
                  updated.binary(updated.column("webhook_id", "latest"),
                                 ruvia::DbBinaryOperator::kEqual,
                                 updated.column("webhook_id", "summary")),
                  "latest")
            .where(updated.binary(updated.column(service::access::persistence::OpenWebhookEntity::columnName<"id">(), "webhook"),
                                  ruvia::DbBinaryOperator::kEqual,
                                  updated.column("webhook_id", "summary")))
            .andWhere(updated.unary(ruvia::DbUnaryOperator::kIsNull,
                                    updated.column(service::access::persistence::OpenWebhookEntity::columnName<"deleted_at">(), "webhook")))
            .returning({updated.column(service::access::persistence::OpenWebhookEntity::columnName<"id">(), "webhook")});

        ruvia::DbQuery logRows(context.pool());
        logRows
            .select({logRows.column("log_id", "incoming"),
                     logRows.column("access_key_id", "incoming"),
                     logRows.column("webhook_id", "incoming"),
                     logRows.value("push"), logRows.value("webhook"),
                     logRows.column("event_type", "incoming"),
                     logRows.column("status", "incoming"), logRows.value("POST"),
                     logRows.column("target", "incoming"),
                     logRows.nullIf(logRows.column("http_status", "incoming"),
                                    logRows.value(std::int64_t{0})),
                     logRows.column("device_id", "incoming"),
                     logRows.nullIf(logRows.column("device_code", "incoming"),
                                    logRows.value("")),
                     logRows.nullIf(logRows.column("message", "incoming"),
                                    logRows.value("")),
                     logRows.column("request_payload", "incoming"),
                     logRows.column("response_payload", "incoming")})
            .from("incoming", "incoming");

        ruvia::DbQuery write(context.pool());
        write
            .with("incoming", incoming,
                  {.columns = {"sequence", "log_id", "access_key_id", "webhook_id",
                               "event_type", "status", "target", "http_status",
                               "device_id", "device_code", "message", "request_payload",
                               "response_payload", "completed_at_ms"}})
            .with("latest", latest)
            .with("summary", summary)
            .with("updated", updated)
            .insertInto(service::access::persistence::OpenAccessLogEntity::tableName(),
                        {"id", "access_key_id", "webhook_id", "direction", "action",
                         "event_type", "status", "http_method", "target", "http_status",
                         "device_id", "device_code", "message", "request_payload",
                         "response_payload"})
            .insertFrom(logRows)
            .onConflict({.columns = {"id"}, .doNothing = true});
        (void)co_await context.db().execute(write);
    }

inline ruvia::Task<void> persistAudits(
        ruvia::WebWorkerContext& context,
        const std::vector<message::StreamMessage>& messages) {
        if (messages.empty())
            co_return;
        ruvia::DbQuery incoming(context.pool());
        for (std::size_t index = 0; index < messages.size(); ++index) {
            const auto httpStatus = service::utils::parseInt64(
                std::optional<std::string_view>(messages[index].get("http_status")));
            const auto usedAt = service::utils::parseInt64(
                std::optional<std::string_view>(messages[index].get("used_at_ms")));
            incoming.values({
                incoming.cast(incoming.value(static_cast<std::int64_t>(index)),
                              ruvia::DbDataType::kBigInt),
                incoming.cast(incoming.value(messages[index].get("log_id")),
                              ruvia::DbDataType::kUuid),
                incoming.cast(incoming.value(messages[index].get("access_key_id")),
                              ruvia::DbDataType::kUuid),
                incoming.cast(incoming.value(messages[index].get("action")),
                              ruvia::DbDataType::kText),
                incoming.cast(incoming.value(messages[index].get("http_method")),
                              ruvia::DbDataType::kText),
                incoming.cast(incoming.value(messages[index].get("target")),
                              ruvia::DbDataType::kText),
                incoming.cast(incoming.value(messages[index].get("request_ip")),
                              ruvia::DbDataType::kText),
                incoming.cast(incoming.value(httpStatus.value_or(0)),
                              ruvia::DbDataType::kInteger),
                incoming.cast(
                    incoming.nullIf(incoming.value(messages[index].get("device_id")),
                                    incoming.value("")),
                    ruvia::DbDataType::kUuid),
                incoming.cast(incoming.value(messages[index].get("request_payload")),
                              ruvia::DbDataType::kJsonb),
                incoming.cast(incoming.value(messages[index].get("response_payload")),
                              ruvia::DbDataType::kJsonb),
                incoming.cast(
                    incoming.value(usedAt.value_or(service::message::utcNowMilliseconds())),
                    ruvia::DbDataType::kBigInt)
            });
        }
        ruvia::DbQuery latestUsage(context.pool());
        latestUsage
            .select({latestUsage.column("access_key_id", "incoming"),
                     latestUsage.column("request_ip", "incoming"),
                     latestUsage.column("used_at_ms", "incoming")})
            .distinctOn({latestUsage.column("access_key_id", "incoming")})
            .from("incoming", "incoming")
            .orderBy(latestUsage.column("access_key_id", "incoming"))
            .addOrderBy(latestUsage.column("used_at_ms", "incoming"),
                        ruvia::DbOrderDirection::kDesc)
            .addOrderBy(latestUsage.column("sequence", "incoming"),
                        ruvia::DbOrderDirection::kDesc);

        ruvia::DbQuery usageUpdated(context.pool());
        const auto latestTimestamp = usageUpdated.call(
            "to_timestamp",
            {usageUpdated.binary(
                usageUpdated.cast(usageUpdated.column("used_at_ms", "latest"),
                                 ruvia::DbDataType::kDouble),
                ruvia::DbBinaryOperator::kDivide,
                usageUpdated.value(1000.0))});
        usageUpdated
            .update(service::access::persistence::OpenAccessKeyEntity::tableName(), "key")
            .set(service::access::persistence::OpenAccessKeyEntity::columnName<"last_used_at">(), latestTimestamp)
            .set(service::access::persistence::OpenAccessKeyEntity::columnName<"last_used_ip">(),
                 usageUpdated.nullIf(usageUpdated.column("request_ip", "latest"),
                                     usageUpdated.value("")))
            .updateFrom("latest_usage", "latest")
            .where(usageUpdated.binary(usageUpdated.column(service::access::persistence::OpenAccessKeyEntity::columnName<"id">(), "key"),
                                       ruvia::DbBinaryOperator::kEqual,
                                       usageUpdated.column("access_key_id", "latest")))
            .andWhere(usageUpdated.binary(
                usageUpdated.unary(ruvia::DbUnaryOperator::kIsNull,
                                   usageUpdated.column(service::access::persistence::OpenAccessKeyEntity::columnName<"last_used_at">(), "key")),
                ruvia::DbBinaryOperator::kOr,
                usageUpdated.binary(usageUpdated.column(service::access::persistence::OpenAccessKeyEntity::columnName<"last_used_at">(), "key"),
                                   ruvia::DbBinaryOperator::kLessEqual,
                                   latestTimestamp)))
            .returning({usageUpdated.column(service::access::persistence::OpenAccessKeyEntity::columnName<"id">(), "key")});

        ruvia::DbQuery updateBarrier(context.pool());
        updateBarrier
            .select(updateBarrier.alias(
                updateBarrier.aggregate("count", {updateBarrier.star()}),
                "updated_count"))
            .from("usage_updated");

        ruvia::DbQuery logRows(context.pool());
        logRows
            .select({logRows.column("log_id", "incoming"),
                     logRows.column("access_key_id", "incoming"),
                     logRows.value("pull"), logRows.column("action", "incoming"),
                     logRows.value("success"),
                     logRows.nullIf(logRows.column("http_method", "incoming"),
                                    logRows.value("")),
                     logRows.nullIf(logRows.column("target", "incoming"),
                                    logRows.value("")),
                     logRows.nullIf(logRows.column("request_ip", "incoming"),
                                    logRows.value("")),
                     logRows.nullIf(logRows.column("http_status", "incoming"),
                                    logRows.value(std::int64_t{0})),
                     logRows.column("device_id", "incoming"),
                     logRows.column("request_payload", "incoming"),
                     logRows.column("response_payload", "incoming")})
            .from("incoming", "incoming")
            .join(ruvia::DbJoinType::kCross, updateBarrier, {}, "update_barrier")
            .where(logRows.binary(logRows.column("updated_count", "update_barrier"),
                                 ruvia::DbBinaryOperator::kGreaterEqual,
                                 logRows.value(std::int64_t{0})));

        ruvia::DbQuery write(context.pool());
        write
            .with("incoming", incoming,
                  {.columns = {"sequence", "log_id", "access_key_id", "action",
                               "http_method", "target", "request_ip", "http_status",
                               "device_id", "request_payload", "response_payload",
                               "used_at_ms"}})
            .with("latest_usage", latestUsage,
                  {.materialization = ruvia::DbMaterialization::kMaterialized})
            .with("usage_updated", usageUpdated)
            .insertInto(service::access::persistence::OpenAccessLogEntity::tableName(),
                        {"id", "access_key_id", "direction", "action", "status",
                         "http_method", "target", "request_ip", "http_status", "device_id",
                         "request_payload", "response_payload"})
            .insertFrom(logRows)
            .onConflict({.columns = {"id"}, .doNothing = true});
        (void)co_await context.db().execute(write);
    }

} // namespace service::access::webhook

namespace service::access::webhook {

class DeliveryService final {
  public:
    static std::string deviceReference(std::string_view id, std::string_view code, std::string_view name) {
        return "{\"id\":" + service::utils::jsonQuoted(id) + ",\"code\":" + service::utils::jsonQuoted(code) +
            ",\"name\":" + service::utils::jsonQuoted(name) + "}";
    }

    static std::string jsonFieldOr(const ruvia::JsonValue& object, std::string_view field, std::string_view fallback) {
        const auto value = object.get<ruvia::JsonValue>(field);
        return value ? std::string(value->view()) : std::string(fallback);
    }

    static std::string mergeEventData(std::string_view deviceJson, std::string_view rawData) {
        std::string result = "{\"device\":" + std::string(deviceJson);
        if (const auto parsed = ruvia::JsonValue::parse(rawData); parsed && parsed->isObject()) {
            (void)parsed->forEachField(
                [&](std::string_view name, const ruvia::JsonValue& value) {
                    if (name != "device") {
                        result += "," + service::utils::jsonQuoted(name) + ":" + std::string(value.view());
                    }
                    return true;
                }
            );
        }
        result.push_back('}');
        return result;
    }

    static std::string imageEventData(std::string_view deviceJson, std::string_view rawData, std::string_view occurredAt) {
        const auto parsed = ruvia::JsonValue::parse(rawData);
        if (!parsed || !parsed->isObject()) {
            return mergeEventData(deviceJson, rawData);
        }
        const auto values = parsed->get<ruvia::JsonValue>("values");
        if (!values || !values->isObject()) {
            return mergeEventData(deviceJson, rawData);
        }

        std::string image;
        (void)values->forEachField(
            [&](std::string_view id, const ruvia::JsonValue& item) {
                if (!image.empty()) {
                    return true;
                }
                if (!item.isObject()) {
                    return true;
                }
                const auto type = item.get<ruvia::String>("type");
                const auto value = item.get<ruvia::JsonValue>("value");
                const auto text = item.get<ruvia::String>("value");
                const bool jpeg = type && type->view() == "JPEG";
                const bool dataUrl = text && text->view().starts_with("data:image/");
                if (!value || (!jpeg && !dataUrl)) {
                    return true;
                }
                const auto name = item.get<ruvia::String>("name");
                image = "{\"id\":" + service::utils::jsonQuoted(id) + ",\"name\":" +
                    service::utils::jsonQuoted(name ? name->view() : std::string_view("image")) +
                    ",\"data\":" + std::string(value->view()) +
                    ",\"time\":" + service::utils::jsonQuoted(occurredAt) + "}";
                return true;
            }
        );
        if (image.empty()) {
            return mergeEventData(deviceJson, rawData);
        }
        return "{\"device\":" + std::string(deviceJson) + ",\"image\":" + image + "}";
    }

    static std::string commandEventData(std::string_view deviceJson, const ruvia::JsonValue& payload, bool dispatched) {
        (void)dispatched;
        return "{\"device\":" + std::string(deviceJson) + ",\"command\":{\"id\":" +
            jsonFieldOr(payload, "commandId", "null") + ",\"status\":" +
            jsonFieldOr(payload, "status", "null") + ",\"reason\":" +
            jsonFieldOr(payload, "reason", "null") + ",\"elements\":" +
            jsonFieldOr(payload, "elements", "[]") + ",\"actual_values\":" +
            jsonFieldOr(payload, "actualValues", "[]") + "}}";
    }

    static ruvia::Task<std::string> realtimeData(ruvia::WebWorkerContext& context, std::string_view deviceId, const DeviceCatalog& device) {
        const auto reply = co_await message::redis::command(
            context.redis(),
            std::vector<std::string>{ "HGETALL", telemetry::latest::latestKey(deviceId) }
        );
        auto record = LatestValuesRecord::decode(reply);
        std::vector<LatestValuesRecord::Point> points;
        points.reserve(record.latest.size());
        for (auto& [id, point] : record.latest) {
            if ((!record.hasConfigured || record.configured.contains(id)) && point.encode != "JPEG") {
                points.push_back(std::move(point));
            }
        }
        std::ranges::sort(points, [](const LatestValuesRecord::Point& left, const LatestValuesRecord::Point& right) {
            return left.sort == right.sort ? left.id < right.id : left.sort < right.sort;
        });
        std::string body = "{\"device\":" + deviceReference(deviceId, device.code, device.name) +
            ",\"points\":[";
        for (std::size_t index = 0; index < points.size(); ++index) {
            if (index != 0) {
                body.push_back(',');
            }
            const auto time = points[index].observedAt > 0
                ? service::utils::jsonQuoted(service::common::utcTimestampFromMilliseconds(points[index].observedAt))
                : std::string("null");
            body += "{\"id\":" + service::utils::jsonQuoted(points[index].id) +
                ",\"name\":" + service::utils::jsonQuoted(points[index].name) +
                ",\"value\":" + points[index].value +
                ",\"unit\":" + service::utils::jsonQuoted(points[index].unit) +
                ",\"time\":" + time + "}";
        }
        body += "]}";
        co_return body;
    }

    static ruvia::Task<Delivery> buildDelivery(ruvia::WebWorkerContext& context, const message::StreamMessage& message, const DeviceCatalog& catalog) {
        Delivery delivery;
        delivery.id = message.get("event_id").empty() ? context.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next()
                                                      : std::string(message.get("event_id"));
        delivery.eventType = std::string(message.get("event_type"));
        delivery.deviceId = std::string(message.get("device_id"));
        delivery.deviceCode = std::string(message.get("device_code"));
        const auto occurredAt = service::utils::parseInt64(
            std::optional<std::string_view>(message.get("occurred_at_ms"))
        );
        delivery.occurredAt = occurredAt ? service::common::utcTimestampFromMilliseconds(*occurredAt) : service::common::utcTimestampNow();

        const auto device =
            deviceReference(delivery.deviceId, delivery.deviceCode, catalog.name);
        const auto rawData = message.get("data_json");
        std::string data;
        if (delivery.eventType == "device.data.reported") {
            data = co_await realtimeData(context, delivery.deviceId, catalog);
        } else if (delivery.eventType == "device.image.reported") {
            data = imageEventData(device, rawData, delivery.occurredAt);
        } else if (delivery.eventType == "device.command.accepted" ||
                   delivery.eventType == "device.command.updated") {
            const auto payload = ruvia::JsonValue::parse(rawData);
            data = payload && payload->isObject()
                ? commandEventData(device, *payload, delivery.eventType == "device.command.accepted")
                : mergeEventData(device, rawData);
        } else {
            data = mergeEventData(device, rawData);
        }
        delivery.body =
            service::message::webhookEnvelope(delivery.eventType, delivery.occurredAt, delivery.id, data);
        co_return delivery;
    }

    static constexpr std::int64_t kDeliveryProgressTtlSeconds = 7 * 24 * 60 * 60;

    template <typename Redis>
    static ruvia::Task<std::set<std::string, std::less<>>>
    completedTargets(const Redis& redis, const Delivery& delivery) {
        const auto reply = co_await message::redis::command(
            redis,
            { "HKEYS", DeliveryProgressRecord::key(delivery.eventType, delivery.id) }
        );
        if (reply.kind() != ruvia::RedisValue::Kind::kArray) {
            message::redis::throwValue("read webhook delivery progress", reply);
        }
        co_return DeliveryProgressRecord::decode(reply).completedTargets;
    }

    static ruvia::Task<void> enqueueResult(ruvia::WebWorkerContext& context, const Target& target, const Delivery& delivery, const WebhookHttpResponse& response, bool success) {
        const auto error = service::utils::sanitize(response.error, 1000);
        const auto status = success ? "success" : "failed";
        const auto logId = context.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next();
        const auto responseJson = "{\"httpStatus\":" + std::to_string(response.status) +
            ",\"body\":" + service::utils::jsonQuoted(service::utils::sanitize(response.body, 2000)) +
            (error.empty() ? "" : ",\"error\":" + service::utils::jsonQuoted(error)) + "}";
        static constexpr std::string_view script = R"lua(
if redis.call('HEXISTS', KEYS[2], ARGV[1]) ~= 0 then return false end
local arguments = {'MAXLEN', '~', ARGV[2], '*'}
for index = 6, #ARGV do arguments[#arguments + 1] = ARGV[index] end
local id = redis.call('XADD', KEYS[1], unpack(arguments))
redis.call('HSET', KEYS[2], ARGV[1], '1')
redis.call('EXPIRE', KEYS[2], ARGV[3])
redis.call('XADD', KEYS[3], 'MAXLEN', '~', ARGV[4], '*', 'task', ARGV[5])
return id
)lua";
        const std::vector<std::string> keyStore{
            stream::deliveryResult(),
            DeliveryProgressRecord::key(delivery.eventType, delivery.id),
            service::message::workerWakeStream(std::nullopt, service::runtime::instanceId())
        };
        const std::vector<std::string> argumentStore{
            target.id,
            "100000",
            std::to_string(kDeliveryProgressTtlSeconds),
            std::to_string(service::message::kWorkerWakeCapacity),
            std::string(service::message::workerStreamTaskName(service::message::WorkerStreamTask::Webhook)),
            "log_id",
            logId,
            "access_key_id",
            target.accessKeyId,
            "webhook_id",
            target.id,
            "event_type",
            delivery.eventType,
            "status",
            status,
            "target",
            target.url,
            "http_status",
            std::to_string(response.status),
            "device_id",
            delivery.deviceId,
            "device_code",
            delivery.deviceCode,
            "message",
            error,
            "request_payload",
            delivery.body,
            "response_payload",
            responseJson,
            "completed_at_ms",
            std::to_string(service::message::utcNowMilliseconds()),
        };
        const std::vector<std::string_view> keys(keyStore.begin(), keyStore.end());
        const std::vector<std::string_view> arguments(argumentStore.begin(), argumentStore.end());
        const auto reply = co_await context.redis().eval(script, keys, arguments);
        if (!reply.null() && reply.kind() != ruvia::RedisValue::Kind::kString) {
            message::redis::throwValue("enqueue webhook result", reply);
        }
    }

};

} // namespace service::access::webhook

namespace service::access::event {

inline constexpr std::int64_t kPublicationTtlSeconds = 7 * 24 * 60 * 60;

inline std::string publicationKey(std::string_view eventId, std::string_view eventType) {
    return "iot:open-access:event:published:" + std::string(eventType) + ":" +
        std::string(eventId);
}

inline constexpr std::string_view kPublishScript = R"lua(
if redis.call('EXISTS', KEYS[2]) ~= 0 then return false end
local arguments = {'MAXLEN', '~', ARGV[1], '*'}
for index = 5, #ARGV do arguments[#arguments + 1] = ARGV[index] end
local id = redis.call('XADD', KEYS[1], unpack(arguments))
redis.call('SET', KEYS[2], '1', 'EX', ARGV[2])
redis.call('XADD', KEYS[3], 'MAXLEN', '~', ARGV[3], '*', 'task', ARGV[4])
return id
)lua";

template <typename Pipeline>
void queue(Pipeline& pipeline, std::string_view scriptSha, std::string_view eventId, std::string_view eventType, std::string_view deviceId, std::string_view deviceCode, std::int64_t occurredAtMs, std::string_view dataJson) {
    const auto publishedKey = publicationKey(eventId, eventType);
    const auto occurredAt = std::to_string(occurredAtMs);
    const auto outputStream = stream::event();
    const auto wakeStream = service::message::workerWakeStream(std::nullopt, service::runtime::instanceId());
    const std::array<std::string_view, 3> keys{ outputStream, publishedKey, wakeStream };
    const std::array<std::string_view, 16> arguments{
        "100000",
        "604800",
        "100000",
        "webhook",
        "event_id",
        eventId,
        "event_type",
        eventType,
        "device_id",
        deviceId,
        "device_code",
        deviceCode,
        "occurred_at_ms",
        occurredAt,
        "data_json",
        dataJson,
    };
    message::redis::queueEvalSha(pipeline, scriptSha, keys, arguments);
}

template <typename Redis>
ruvia::Task<void> publish(const Redis& redis, std::string_view eventId, std::string_view eventType, std::string_view deviceId, std::string_view deviceCode, std::int64_t occurredAtMs, std::string_view dataJson) {
    const std::vector<std::string> keyStore{
        stream::event(),
        publicationKey(eventId, eventType),
        service::message::workerWakeStream(std::nullopt, service::runtime::instanceId())
    };
    const std::vector<std::string> argumentStore{
        "100000",
        std::to_string(kPublicationTtlSeconds),
        std::to_string(service::message::kWorkerWakeCapacity),
        std::string(service::message::workerStreamTaskName(service::message::WorkerStreamTask::Webhook)),
        "event_id",
        std::string(eventId),
        "event_type",
        std::string(eventType),
        "device_id",
        std::string(deviceId),
        "device_code",
        std::string(deviceCode),
        "occurred_at_ms",
        std::to_string(occurredAtMs),
        "data_json",
        std::string(dataJson),
    };
    const std::vector<std::string_view> keys(keyStore.begin(), keyStore.end());
    const std::vector<std::string_view> arguments(argumentStore.begin(), argumentStore.end());
    const auto reply = co_await redis.eval(kPublishScript, keys, arguments);
    if (!reply.null() && reply.kind() != ruvia::RedisValue::Kind::kString) {
        message::redis::throwValue("publish open-access event", reply);
    }
}

template <typename Redis>
ruvia::Task<void> publishMany(
    const Redis& redis,
    const std::vector<message::ParsedDeviceMessage>& messages
) {
    if (messages.empty()) {
        co_return;
    }
    const auto scriptSha = co_await redis.scriptLoad(kPublishScript);
    auto pipeline = redis.pipeline();
    for (const auto& parsed : messages) {
        const auto eventType =
            parsed.eventKind == "image"
            ? "device.image.reported"
            : "device.data.reported";
        queue(pipeline, scriptSha, parsed.messageId, eventType, parsed.deviceId, parsed.deviceCode, parsed.observedAtMs, parsed.valuesJson);
    }
    const auto replies = co_await std::move(pipeline).exec();
    message::redis::requirePipelineSuccess("publish open-access events", replies);
}

} // namespace service::access::event

namespace service::access::audit {

inline constexpr std::size_t kCapacity = 100000;

template <typename Redis>
ruvia::Task<void> publish(const Redis& redis, std::string_view logId, std::string_view action, std::string_view accessKeyId, std::string_view method, std::string_view target, std::string_view requestIp, std::int64_t httpStatus, std::string_view deviceId = {}, std::string_view requestPayload = "{}", std::string_view responsePayload = "{}") {
    const std::vector<service::message::StreamField> fields{
        { "log_id", std::string(logId) },
        { "access_key_id", std::string(accessKeyId) },
        { "action", std::string(action) },
        { "http_method", std::string(method) },
        { "target", std::string(target) },
        { "request_ip", std::string(requestIp) },
        { "http_status", std::to_string(httpStatus) },
        { "device_id", std::string(deviceId) },
        { "request_payload", std::string(requestPayload) },
        { "response_payload", std::string(responsePayload) },
        { "used_at_ms", std::to_string(service::message::utcNowMilliseconds()) },
    };
    (void)co_await service::message::redis::addAndWake(
        redis,
        stream::audit(),
        fields,
        std::nullopt,
        service::message::WorkerStreamTask::Webhook,
        kCapacity
    );
}

} // namespace service::access::audit


namespace service::access {

class AccessOperationService final {
  public:
    static ruvia::Task<std::string> executeOperation(ruvia::WebWorkerContext& context, std::string_view operation, std::string_view payload, ruvia::StopToken stop) {
        if (stop.stopRequested()) {
            service::common::fail(10004, "Access projection cancelled", 503);
        }
        if (operation == "refresh") {
            co_await session::refresh(context);
            co_return "{}";
        }
        if (operation == "audit") {
            const auto request = ruvia::JsonValue::parse(payload);
            if (!request || !request->isObject()) {
                service::common::fail(10002, "Invalid access audit payload", 400);
            }
            const auto text = [&request](std::string_view field) {
                const auto value = request->get<ruvia::String>(field);
                return value ? std::string(value->view()) : std::string{};
            };
            const auto json = [&request](std::string_view field) {
                const auto value = request->get<ruvia::JsonValue>(field);
                return value ? std::string(value->view()) : std::string("{}");
            };
            std::int64_t status = 200;
            if (const auto value = request->get<ruvia::Int64>("httpStatus")) {
                status = static_cast<std::int64_t>(*value);
            }
            const auto action = text("action");
            const auto accessKeyId = text("accessKeyId");
            const auto method = text("method");
            const auto target = text("target");
            const auto requestIp = text("requestIp");
            const auto deviceId = text("deviceId");
            const auto requestPayload = json("requestPayload");
            const auto responsePayload = json("responsePayload");
            co_await audit::publish(context.redis(), context.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next(), action, accessKeyId, method, target, requestIp, status, deviceId, requestPayload, responsePayload);
            co_return "{}";
        }
        service::common::fail(10002, "Unknown access operation", 400);
    }
};

} // namespace service::access
