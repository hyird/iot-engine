#pragma once
#include "service/common/http.h"


#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/web/ModelObject.h>
#include <ruvia/web/db/DbQuery.h>

#include "service/common/uuid.h"
#include "service/features/messaging/messaging.transport.h"

namespace service::access::session {

template <typename Context>
ruvia::Task<void> refresh(Context& context, bool onlyIfMissing = false) {
    // Serialize the database snapshot and Redis pointer swap across service instances.
    // The lock is held only by cold-path configuration projection, never by API reads.
    auto transaction = co_await context.db().beginTransaction();
    ruvia::DbQuery advisoryLock(context.resource());
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
    ruvia::DbQuery snapshot(context.resource());
    const auto expiresAtMs = snapshot.cast(
        snapshot.coalesce({
            snapshot.cast(
                snapshot.binary(
                    snapshot.extract(ruvia::DbDatePart::kEpoch,
                                     snapshot.column("expires_at", "key")),
                    ruvia::DbBinaryOperator::kMultiply,
                    snapshot.value(std::int64_t{1000})),
                ruvia::DbDataType::kBigInt),
            snapshot.value(std::int64_t{0})
        }),
        ruvia::DbDataType::kText);
    const std::array bindingOrder{
        ruvia::DbOrderTerm{snapshot.column("device_id", "binding")}
    };
    const auto bindingIds = snapshot.aggregate(
        "jsonb_agg", {snapshot.cast(snapshot.column("device_id", "binding"),
                                    ruvia::DbDataType::kText)},
        false, bindingOrder);
    const auto bindingIdsOrEmpty = snapshot.coalesce({
        snapshot.filter(bindingIds, snapshot.unary(
            ruvia::DbUnaryOperator::kIsNotNull,
            snapshot.column("device_id", "binding"))),
        snapshot.cast(snapshot.value("[]"), ruvia::DbDataType::kJsonb)
    });
    snapshot
        .select({snapshot.column("access_key_hash", "key"),
                 snapshot.cast(snapshot.column("id", "key"), ruvia::DbDataType::kText),
                 snapshot.column("name", "key"),
                 snapshot.cast(snapshot.column("status", "key"), ruvia::DbDataType::kText),
                 expiresAtMs,
                 snapshot.cast(snapshot.column("scopes", "key"), ruvia::DbDataType::kText),
                 snapshot.cast(bindingIdsOrEmpty, ruvia::DbDataType::kText)})
        .from("open_access_key", "key")
        .join(ruvia::DbJoinType::kLeft, "open_access_key_device",
              snapshot.binary(snapshot.column("access_key_id", "binding"),
                              ruvia::DbBinaryOperator::kEqual,
                              snapshot.column("id", "key")),
              "binding")
        .where(snapshot.unary(ruvia::DbUnaryOperator::kIsNull,
                              snapshot.column("deleted_at", "key")))
        .groupBy({snapshot.column("id", "key")})
        .orderBy(snapshot.column("id", "key"));
    const auto rows = co_await transaction.query(snapshot);

    const auto version = service::common::nextUuidV7();
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
        ruvia::DbQuery catalog(context.resource());
        const auto expiresAtMs = catalog.cast(
            catalog.coalesce({
                catalog.cast(
                    catalog.binary(
                        catalog.extract(ruvia::DbDatePart::kEpoch,
                                        catalog.column("expires_at", "key")),
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
                                   {catalog.column("event_types", "webhook")}),
                      ruvia::DbBinaryOperator::kEqual,
                      catalog.value("array")),
                  catalog.column("event_types", "webhook")}},
                catalog.cast(catalog.value("[]"), ruvia::DbDataType::kJsonb))});
        catalog
            .select({catalog.cast(catalog.column("device_id", "binding"),
                                  ruvia::DbDataType::kText),
                     catalog.column("name", "device"),
                     catalog.coalesce({
                         catalog.binary(catalog.column("protocol_params", "device"),
                                        ruvia::DbBinaryOperator::kJsonGetText,
                                        catalog.value("device_code")),
                         catalog.value("")}),
                     catalog.cast(catalog.column("id", "webhook"),
                                  ruvia::DbDataType::kText),
                     catalog.cast(catalog.column("access_key_id", "webhook"),
                                  ruvia::DbDataType::kText),
                     catalog.column("url", "webhook"),
                     catalog.coalesce({catalog.column("secret", "webhook"),
                                       catalog.value("")}),
                     catalog.cast(catalog.column("headers", "webhook"),
                                  ruvia::DbDataType::kText),
                     catalog.cast(catalog.column("timeout_seconds", "webhook"),
                                  ruvia::DbDataType::kText),
                     catalog.caseWhen(
                         {{catalog.column("skip_tls_verify", "webhook"),
                           catalog.value("1")}},
                         catalog.value("0")),
                     expiresAtMs,
                     catalog.column("value", "event_type")})
            .from("open_webhook", "webhook")
            .join(ruvia::DbJoinType::kInner, "open_access_key",
                  catalog.binary(catalog.column("id", "key"),
                                 ruvia::DbBinaryOperator::kEqual,
                                 catalog.column("access_key_id", "webhook")),
                  "key")
            .join(ruvia::DbJoinType::kInner, "open_access_key_device",
                  catalog.binary(catalog.column("access_key_id", "binding"),
                                 ruvia::DbBinaryOperator::kEqual,
                                 catalog.column("id", "key")),
                  "binding")
            .join(ruvia::DbJoinType::kInner, "device",
                  catalog.binary(catalog.column("id", "device"),
                                 ruvia::DbBinaryOperator::kEqual,
                                 catalog.column("device_id", "binding")),
                  "device")
            .joinFunction(ruvia::DbJoinType::kCross, eventTypes, {}, "event_type",
                          {.lateral = true, .columns = {{.name = "value"}}})
            .where(catalog.unary(ruvia::DbUnaryOperator::kIsNull,
                                 catalog.column("deleted_at", "webhook")))
            .andWhere(catalog.binary(catalog.column("status", "webhook"),
                                    ruvia::DbBinaryOperator::kEqual,
                                    catalog.value("enabled")))
            .andWhere(catalog.unary(ruvia::DbUnaryOperator::kIsNull,
                                    catalog.column("deleted_at", "key")))
            .andWhere(catalog.binary(catalog.column("status", "key"),
                                    ruvia::DbBinaryOperator::kEqual,
                                    catalog.value("enabled")))
            .andWhere(catalog.binary(
                catalog.unary(ruvia::DbUnaryOperator::kIsNull,
                              catalog.column("expires_at", "key")),
                ruvia::DbBinaryOperator::kOr,
                catalog.binary(catalog.column("expires_at", "key"),
                               ruvia::DbBinaryOperator::kGreater,
                               catalog.call("now"))))
            .andWhere(catalog.unary(ruvia::DbUnaryOperator::kIsNull,
                                    catalog.column("deleted_at", "device")))
            .orderBy(catalog.column("device_id", "binding"))
            .addOrderBy(catalog.column("value", "event_type"))
            .addOrderBy(catalog.column("id", "webhook"));
        const auto rows = co_await context.db().query(catalog);
        Catalog result;
        for (const auto& row : rows) {
            const std::string deviceId(row[0].value().value_or(std::string_view{}));
            auto& device = result[deviceId];
            device.name.assign(row[1].value().value_or(std::string_view{}));
            device.code.assign(row[2].value().value_or(std::string_view{}));
            const auto timeout =
                service::common::parseInt64(std::optional<std::string_view>{row[8].value().value_or(std::string_view{})})
                    .value_or(5);
            const auto expiresAtMs =
                service::common::parseInt64(std::optional<std::string_view>{row[10].value().value_or(std::string_view{})})
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
        ruvia::DbQuery incoming(context.resource());
        for (std::size_t index = 0; index < messages.size(); ++index) {
            const auto httpStatus = service::common::parseInt64(
                std::optional<std::string_view>(messages[index].get("http_status")));
            const auto completedAt = service::common::parseInt64(
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
        ruvia::DbQuery latest(context.resource());
        latest
            .select(latest.star())
            .distinctOn({latest.column("webhook_id", "incoming")})
            .from("incoming", "incoming")
            .orderBy(latest.column("webhook_id", "incoming"))
            .addOrderBy(latest.column("completed_at_ms", "incoming"),
                        ruvia::DbOrderDirection::kDesc)
            .addOrderBy(latest.column("sequence", "incoming"),
                        ruvia::DbOrderDirection::kDesc);

        ruvia::DbQuery summary(context.resource());
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

        ruvia::DbQuery updated(context.resource());
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
                          updated.column("last_triggered_at", "webhook")),
            ruvia::DbBinaryOperator::kOr,
            updated.binary(updated.column("last_triggered_at", "webhook"),
                           ruvia::DbBinaryOperator::kLessEqual,
                           latestTimestamp));
        updated
            .update("open_webhook", "webhook")
            .set("last_triggered_at",
                 updated.greatest({
                     updated.coalesce({updated.column("last_triggered_at", "webhook"),
                                       epoch}),
                     latestTimestamp
                 }))
            .set("last_success_at",
                 updated.caseWhen(
                     {{updated.unary(
                           ruvia::DbUnaryOperator::kIsNull,
                           updated.column("last_success_ms", "summary")),
                       updated.column("last_success_at", "webhook")}},
                     updated.greatest({
                         updated.coalesce({updated.column("last_success_at", "webhook"),
                                           epoch}),
                         successTimestamp
                     })))
            .set("last_failure_at",
                 updated.caseWhen(
                     {{updated.unary(
                           ruvia::DbUnaryOperator::kIsNull,
                           updated.column("last_failure_ms", "summary")),
                       updated.column("last_failure_at", "webhook")}},
                     updated.greatest({
                         updated.coalesce({updated.column("last_failure_at", "webhook"),
                                           epoch}),
                         failureTimestamp
                     })))
            .set("last_http_status",
                 updated.caseWhen(
                     {{latestIsNewer,
                       updated.nullIf(updated.column("http_status", "latest"),
                                      updated.value(std::int64_t{0}))}},
                     updated.column("last_http_status", "webhook")))
            .set("last_error",
                 updated.caseWhen(
                     {{latestIsNewer,
                       updated.nullIf(updated.column("message", "latest"),
                                      updated.value(""))}},
                     updated.column("last_error", "webhook")))
            .set("updated_at", updated.call("now"))
            .updateFrom("summary", "summary")
            .join(ruvia::DbJoinType::kInner, "latest",
                  updated.binary(updated.column("webhook_id", "latest"),
                                 ruvia::DbBinaryOperator::kEqual,
                                 updated.column("webhook_id", "summary")),
                  "latest")
            .where(updated.binary(updated.column("id", "webhook"),
                                  ruvia::DbBinaryOperator::kEqual,
                                  updated.column("webhook_id", "summary")))
            .andWhere(updated.unary(ruvia::DbUnaryOperator::kIsNull,
                                    updated.column("deleted_at", "webhook")))
            .returning({updated.column("id", "webhook")});

        ruvia::DbQuery logRows(context.resource());
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

        ruvia::DbQuery write(context.resource());
        write
            .with("incoming", incoming,
                  {.columns = {"sequence", "log_id", "access_key_id", "webhook_id",
                               "event_type", "status", "target", "http_status",
                               "device_id", "device_code", "message", "request_payload",
                               "response_payload", "completed_at_ms"}})
            .with("latest", latest)
            .with("summary", summary)
            .with("updated", updated)
            .insertInto("open_access_log",
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
        ruvia::DbQuery incoming(context.resource());
        for (std::size_t index = 0; index < messages.size(); ++index) {
            const auto httpStatus = service::common::parseInt64(
                std::optional<std::string_view>(messages[index].get("http_status")));
            const auto usedAt = service::common::parseInt64(
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
        ruvia::DbQuery latestUsage(context.resource());
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

        ruvia::DbQuery usageUpdated(context.resource());
        const auto latestTimestamp = usageUpdated.call(
            "to_timestamp",
            {usageUpdated.binary(
                usageUpdated.cast(usageUpdated.column("used_at_ms", "latest"),
                                 ruvia::DbDataType::kDouble),
                ruvia::DbBinaryOperator::kDivide,
                usageUpdated.value(1000.0))});
        usageUpdated
            .update("open_access_key", "key")
            .set("last_used_at", latestTimestamp)
            .set("last_used_ip",
                 usageUpdated.nullIf(usageUpdated.column("request_ip", "latest"),
                                     usageUpdated.value("")))
            .updateFrom("latest_usage", "latest")
            .where(usageUpdated.binary(usageUpdated.column("id", "key"),
                                       ruvia::DbBinaryOperator::kEqual,
                                       usageUpdated.column("access_key_id", "latest")))
            .andWhere(usageUpdated.binary(
                usageUpdated.unary(ruvia::DbUnaryOperator::kIsNull,
                                   usageUpdated.column("last_used_at", "key")),
                ruvia::DbBinaryOperator::kOr,
                usageUpdated.binary(usageUpdated.column("last_used_at", "key"),
                                   ruvia::DbBinaryOperator::kLessEqual,
                                   latestTimestamp)))
            .returning({usageUpdated.column("id", "key")});

        ruvia::DbQuery updateBarrier(context.resource());
        updateBarrier
            .select(updateBarrier.alias(
                updateBarrier.aggregate("count", {updateBarrier.star()}),
                "updated_count"))
            .from("usage_updated");

        ruvia::DbQuery logRows(context.resource());
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

        ruvia::DbQuery write(context.resource());
        write
            .with("incoming", incoming,
                  {.columns = {"sequence", "log_id", "access_key_id", "action",
                               "http_method", "target", "request_ip", "http_status",
                               "device_id", "request_payload", "response_payload",
                               "used_at_ms"}})
            .with("latest_usage", latestUsage,
                  {.materialization = ruvia::DbMaterialization::kMaterialized})
            .with("usage_updated", usageUpdated)
            .insertInto("open_access_log",
                        {"id", "access_key_id", "direction", "action", "status",
                         "http_method", "target", "request_ip", "http_status", "device_id",
                         "request_payload", "response_payload"})
            .insertFrom(logRows)
            .onConflict({.columns = {"id"}, .doNothing = true});
        (void)co_await context.db().execute(write);
    }

} // namespace service::access::webhook
