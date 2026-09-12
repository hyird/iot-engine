#pragma once
#include <ruvia/web/db/DbQuery.h>
#include <utility>
#include "service/features/messaging/messaging.transport.h"
#include "service/features/messaging/messaging.types.h"
#include "service/features/telemetry/latest/latest.service.h"
#include "service/features/telemetry/telemetry.protocol.h"

namespace service::telemetry {
enum class Consumer { Dispatch,
                      History,
                      Latest,
                      Alerts,
                      Delivery };
inline constexpr std::array<std::string_view, 5> consumerNames{ "dispatch", "history", "latest", "alerts", "delivery" };

inline std::string consumerStream(Consumer consumer) {
    auto stream = service::message::parsedStream();
    if (consumer != Consumer::Dispatch) {
        stream += ':' + std::string(consumerNames[static_cast<std::size_t>(consumer)]);
    }
    return stream;
}

// The ingress receipt and all four durable copies are committed atomically.
// Consumer streams never trim pending entries. Each consumer owns XACK/XDEL.
inline constexpr std::string_view kFanoutScript = R"lua(
if redis.call('EXISTS',KEYS[1]) ~= 0 then return 0 end
for i=2,5 do
 local kind = redis.call('TYPE',KEYS[i]).ok
 if kind ~= 'none' and kind ~= 'stream' then
  return redis.error_reply('telemetry consumer key must be a stream')
 end
end
local args = {'*'}
for i=1,#ARGV do args[#args+1]=ARGV[i] end
for i=2,5 do redis.call('XADD',KEYS[i],unpack(args)) end
redis.call('SET',KEYS[1],'1','EX',604800)
return 1
)lua";

template <class Redis>
ruvia::Task<void> fanout(const Redis& redis, const std::vector<service::message::StreamMessage>& messages) {
    for (const auto& input : messages) {
        auto parsed = service::message::parsedFrom(input);
        contract::normalize(parsed);
        const auto fields = service::message::parsedFields(parsed);
        const std::vector<std::string> store{
            "iot:telemetry:fanout:" + parsed.messageId,
            consumerStream(Consumer::History),
            consumerStream(Consumer::Latest),
            consumerStream(Consumer::Alerts),
            consumerStream(Consumer::Delivery)
        };
        const std::vector<std::string_view> keys(store.begin(), store.end());
        std::vector<std::string_view> args;
        for (const auto& field : fields) {
            args.push_back(field.name);
            args.push_back(field.value);
        }
        const auto reply = co_await redis.eval(kFanoutScript, keys, args);
        if (reply.kind() != ruvia::RedisValue::Kind::kInteger) {
            service::message::redis::throwValue("telemetry fanout", reply);
        }
    }
}
} // namespace service::telemetry

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/core/Timer.h>
#include <ruvia/web/WebWorker.h>
#include <ruvia/web/db/Db.h>

#include "service/common/message.h"
#include "service/features/access/access.transport.h"
#include "service/features/alert/alert.service.h"

namespace service::telemetry {

namespace detail {

inline std::string sanitizeJsonUtf8(std::string_view value) {
    static constexpr std::array<char, 16> digits{ '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F' };
    std::string result;
    result.reserve(value.size());
    const auto continuation = [value](std::size_t index) {
        return index < value.size() && (static_cast<unsigned char>(value[index]) & 0xC0U) == 0x80U;
    };
    for (std::size_t index = 0; index < value.size();) {
        const auto byte = static_cast<unsigned char>(value[index]);
        if (byte < 0x80) {
            result.push_back(static_cast<char>(byte));
            ++index;
            continue;
        }
        std::size_t length = 0;
        if (byte >= 0xC2 && byte <= 0xDF && continuation(index + 1)) {
            length = 2;
        } else if (byte == 0xE0 && index + 2 < value.size() &&
                   static_cast<unsigned char>(value[index + 1]) >= 0xA0 &&
                   static_cast<unsigned char>(value[index + 1]) <= 0xBF && continuation(index + 2)) {
            length = 3;
        } else if (((byte >= 0xE1 && byte <= 0xEC) || (byte >= 0xEE && byte <= 0xEF)) &&
                   continuation(index + 1) && continuation(index + 2)) {
            length = 3;
        } else if (byte == 0xED && index + 2 < value.size() &&
                   static_cast<unsigned char>(value[index + 1]) >= 0x80 &&
                   static_cast<unsigned char>(value[index + 1]) <= 0x9F && continuation(index + 2)) {
            length = 3;
        } else if (byte == 0xF0 && index + 3 < value.size() &&
                   static_cast<unsigned char>(value[index + 1]) >= 0x90 &&
                   static_cast<unsigned char>(value[index + 1]) <= 0xBF && continuation(index + 2) &&
                   continuation(index + 3)) {
            length = 4;
        } else if (byte >= 0xF1 && byte <= 0xF3 && continuation(index + 1) &&
                   continuation(index + 2) && continuation(index + 3)) {
            length = 4;
        } else if (byte == 0xF4 && index + 3 < value.size() &&
                   static_cast<unsigned char>(value[index + 1]) >= 0x80 &&
                   static_cast<unsigned char>(value[index + 1]) <= 0x8F && continuation(index + 2) &&
                   continuation(index + 3)) {
            length = 4;
        }
        if (length != 0) {
            result.append(value.substr(index, length));
            index += length;
            continue;
        }
        result += "\\u00";
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 0x0FU]);
        ++index;
    }
    return result;
}

} // namespace detail

class TelemetryService {
  public:
    static ruvia::Task<void> ingest(ruvia::WebWorkerContext& context, const std::vector<message::StreamMessage>& messages) {
        co_await fanout(context.redis(), messages);
    }

    static ruvia::Task<void> apply(ruvia::WebWorkerContext& context, Consumer consumer, const std::vector<message::StreamMessage>& messages) {
        if (consumer == Consumer::Dispatch) {
            co_await ingest(context, messages);
            co_return;
        }
        if (messages.empty()) {
            co_return;
        }
        std::vector<message::ParsedDeviceMessage> parsedMessages;
        parsedMessages.reserve(messages.size());
        for (const auto& message : messages) {
            auto parsed = message::parsedFrom(message);
            parsed.valuesJson = detail::sanitizeJsonUtf8(parsed.valuesJson);
            parsedMessages.push_back(std::move(parsed));
        }
        const auto redis = context.redis();
        switch (consumer) {
            case Consumer::History:
                (void)co_await persist(context, parsedMessages, std::vector<bool>(parsedMessages.size(), false));
                co_await latest::publishRealtimeChange(redis);
                break;
            case Consumer::Latest:
                co_await latest::update(redis, parsedMessages);
                break;
            case Consumer::Delivery:
                co_await service::access::event::publishMany(redis, parsedMessages);
                break;
            case Consumer::Alerts: {
                const auto metadata = co_await service::alert::metadata::activity(context, parsedMessages);
                std::vector<std::string> previous;
                for (const auto& parsed : parsedMessages) {
                    previous.push_back(co_await prepareAlert(context, parsed));
                }
                co_await service::alert::AlertEvaluationService::evaluateTelemetry(context, parsedMessages, previous, metadata.devices);
                if (metadata.offlineRules) {
                    co_await service::alert::metadata::schedule(redis, parsedMessages);
                }
                break;
            }
            case Consumer::Dispatch:
                break;
        }
    }

  protected:
    static ruvia::Task<std::string> prepareAlert(ruvia::WebWorkerContext& context, const message::ParsedDeviceMessage& value) {
        ruvia::DbQuery state(context.resource());
        const auto newer = state.binary(state.tuple({ state.excluded("observed_at_ms"), state.excluded("message_id") }), ruvia::DbBinaryOperator::kGreater,
            state.tuple({ state.column("observed_at_ms", "alert_input_state"), state.column("message_id", "alert_input_state") }));
        state.insertInto("alert_input_state", { "device_id", "observed_at_ms", "message_id", "data", "previous_data" })
            .values({ state.cast(state.value(value.deviceId), ruvia::DbDataType::kUuid), state.cast(state.value(value.observedAtMs), ruvia::DbDataType::kBigInt),
                state.cast(state.value(value.messageId), ruvia::DbDataType::kUuid), state.cast(state.value(value.valuesJson), ruvia::DbDataType::kJsonb),
                state.cast(state.value("{}"), ruvia::DbDataType::kJsonb) })
            .onConflict({ .columns = { "device_id" }, .update = {
                { "previous_data", state.caseWhen({ { newer, state.column("data", "alert_input_state") } }, state.column("previous_data", "alert_input_state")) },
                { "data", state.caseWhen({ { newer, state.excluded("data") } }, state.column("data", "alert_input_state")) },
                { "observed_at_ms", state.greatest({ state.excluded("observed_at_ms"), state.column("observed_at_ms", "alert_input_state") }) },
                { "message_id", state.caseWhen({ { newer, state.excluded("message_id") } }, state.column("message_id", "alert_input_state")) }
            } })
            .returning({ state.cast(state.caseWhen({ { state.binary(state.column("message_id"), ruvia::DbBinaryOperator::kEqual,
                state.cast(state.value(value.messageId), ruvia::DbDataType::kUuid)), state.column("previous_data") } },
                state.cast(state.value("{}"), ruvia::DbDataType::kJsonb)), ruvia::DbDataType::kText) });
        const auto rows = co_await context.db().query(state);
        co_return std::string(rows.front()[0].value().value_or("{}"));
    }

    static ruvia::DbQuery persistenceQuery(std::pmr::memory_resource* resource,
        const std::vector<message::ParsedDeviceMessage>& messages, const std::vector<bool>& alertActive) {
        std::vector<std::string> rawPayloadArrays;
        rawPayloadArrays.reserve(messages.size());
        for (const auto& message : messages) {
            rawPayloadArrays.push_back(message::rawPayloadsJson(message.rawPayloads));
        }

        using Query = ruvia::DbQuery;
        using Op = ruvia::DbBinaryOperator;
        using Type = ruvia::DbDataType;
        const auto selectColumns = [](Query& query, std::initializer_list<std::string_view> columns, std::string_view table = {}) {
            for (const auto column : columns) query.addSelect(query.column(column, table));
        };
        const auto emptyJson = [](Query& query) { return query.cast(query.value("{}"), Type::kJsonb); };
        const auto timestamp = [](Query& query, std::int64_t milliseconds) {
            return query.call("to_timestamp", { query.binary(query.cast(query.value(milliseconds), Type::kDouble), Op::kDivide, query.value(1000.0)) });
        };
        Query incoming(resource);
        for (std::size_t index = 0; index < messages.size(); ++index) {
            const auto& parsed = messages[index];
            incoming.values({ incoming.cast(incoming.value(static_cast<std::int64_t>(index)), Type::kBigInt), timestamp(incoming, parsed.observedAtMs),
                incoming.cast(incoming.value(parsed.messageId), Type::kUuid), incoming.cast(incoming.value(parsed.deviceId), Type::kUuid),
                incoming.cast(incoming.value(parsed.linkId), Type::kUuid), incoming.cast(incoming.value(parsed.connectionId), Type::kUuid),
                incoming.cast(incoming.value(parsed.protocol), Type::kText), incoming.cast(incoming.value(parsed.source), Type::kText), timestamp(incoming, parsed.occurredAtMs),
                incoming.cast(incoming.value(parsed.valuesJson), Type::kJsonb), incoming.cast(incoming.value(rawPayloadArrays[index]), Type::kJsonb),
                incoming.cast(incoming.value(parsed.storagePolicy), Type::kText), incoming.cast(incoming.value(static_cast<bool>(alertActive[index])), Type::kBoolean) });
        }
        Query valid(resource);
        valid.select(valid.star("incoming")).from("incoming")
            .join(ruvia::DbJoinType::kInner, "device", valid.binary(
                valid.binary(valid.column("id", "current_device"), Op::kEqual, valid.column("device_id", "incoming")), Op::kAnd,
                valid.binary(valid.column("link_id", "current_device"), Op::kEqual, valid.column("link_id", "incoming"))), "current_device");
        Query requested(resource);
        requested.select(requested.column("device_id")).distinct().from("valid_incoming");
        Query states(resource);
        selectColumns(states, { "device_id", "last_stored_at", "last_observed_at", "last_observed_id", "last_data" }, "state");
        states.from("device_data_ingest_state", "state").join(ruvia::DbJoinType::kInner, "requested",
            states.binary(states.column("device_id", "state"), Op::kEqual, states.column("device_id", "requested")));
        Query ordered(resource);
        const ruvia::DbWindowOptions reportOrder{ .partitionBy = { ordered.column("device_id", "incoming") },
            .orderBy = { { ordered.column("report_time", "incoming") }, { ordered.column("id", "incoming") } } };
        ordered.select({ ordered.star("incoming"), ordered.alias(ordered.over(ordered.call("row_number"), reportOrder), "sequence"),
                ordered.alias(ordered.column("last_stored_at", "states"), "last_stored"), ordered.alias(ordered.column("last_observed_at", "states"), "baseline_observed_at"),
                ordered.alias(ordered.column("last_observed_id", "states"), "baseline_observed_id"), ordered.alias(ordered.column("last_data", "states"), "baseline_data") })
            .from("valid_incoming", "incoming").join(ruvia::DbJoinType::kInner, "states",
                ordered.binary(ordered.column("device_id", "incoming"), Op::kEqual, ordered.column("device_id", "states")));
        Query lagged(resource);
        const ruvia::DbWindowOptions lagOrder{ .partitionBy = { lagged.column("device_id") }, .orderBy = { { lagged.column("report_time") }, { lagged.column("id") } } };
        lagged.select(lagged.star("ordered")).from("ordered");
        for (const auto& [column, alias] : { std::pair{ "report_time", "prior_report_time" }, std::pair{ "id", "prior_id" }, std::pair{ "data", "prior_data" } })
            lagged.addSelect(lagged.alias(lagged.over(lagged.call("lag", { lagged.column(column) }), lagOrder), alias));
        Query predecessors(resource);
        const auto baselinePresent = predecessors.unary(ruvia::DbUnaryOperator::kIsNotNull, predecessors.column("baseline_observed_at"));
        const auto baselineOrder = predecessors.tuple({ predecessors.column("baseline_observed_at"), predecessors.column("baseline_observed_id") });
        const auto stale = predecessors.binary(baselinePresent, Op::kAnd, predecessors.binary(
            predecessors.tuple({ predecessors.column("report_time"), predecessors.column("id") }), Op::kLessEqual, baselineOrder));
        const auto prior = predecessors.binary(predecessors.unary(ruvia::DbUnaryOperator::kIsNotNull, predecessors.column("prior_report_time")), Op::kAnd,
            predecessors.binary(predecessors.unary(ruvia::DbUnaryOperator::kIsNull, predecessors.column("baseline_observed_at")), Op::kOr,
                predecessors.binary(predecessors.tuple({ predecessors.column("prior_report_time"), predecessors.column("prior_id") }), Op::kGreater, baselineOrder)));
        predecessors.select({ predecessors.column("input_sequence"), predecessors.alias(predecessors.caseWhen({ { stale, emptyJson(predecessors) },
            { prior, predecessors.column("prior_data") } }, predecessors.coalesce({ predecessors.column("baseline_data"), emptyJson(predecessors) })), "previous_data") }).from("lagged");
        const auto unpackValue = [](Query& query, ruvia::DbExpression value) {
            return query.caseWhen({ { query.binary(query.binary(query.call("jsonb_typeof", { value }), Op::kEqual, query.value("object")), Op::kAnd,
                query.binary(value, Op::kJsonHasKey, query.value("value"))), query.binary(value, Op::kJsonGet, query.value("value")) } }, value);
        };
        Query incomingPoints(resource);
        incomingPoints.select({ incomingPoints.column("input_sequence", "ordered"), incomingPoints.column("device_id", "ordered"), incomingPoints.column("report_time", "ordered"),
                incomingPoints.alias(incomingPoints.column("id", "ordered"), "record_id"), incomingPoints.alias(incomingPoints.column("key", "point"), "element_id"),
                incomingPoints.alias(unpackValue(incomingPoints, incomingPoints.column("value", "point")), "point_value") })
            .from("ordered").joinFunction(ruvia::DbJoinType::kCross, incomingPoints.call("jsonb_each", { incomingPoints.coalesce({
                incomingPoints.binary(incomingPoints.column("data", "ordered"), Op::kJsonGet, incomingPoints.value("values")), emptyJson(incomingPoints) }) }), {}, "point", { .lateral = true });
        Query timeline(resource);
        timeline.select({ timeline.column("input_sequence"), timeline.column("device_id"), timeline.column("element_id"), timeline.alias(timeline.column("report_time"), "observed_at"),
                timeline.column("record_id"), timeline.column("point_value"), timeline.alias(timeline.cast(timeline.value(false), Type::kBoolean), "baseline") }).from("incoming_points");
        Query baseline(resource);
        baseline.select({ baseline.cast(baseline.nullValue(), Type::kBigInt), baseline.column("device_id", "latest"), baseline.column("element_id", "latest"),
                baseline.column("observed_at", "latest"), baseline.column("record_id", "latest"), unpackValue(baseline, baseline.column("value", "latest")), baseline.cast(baseline.value(true), Type::kBoolean) })
            .from("device_latest_value", "latest").join(ruvia::DbJoinType::kInner, "requested", baseline.binary(baseline.column("device_id", "latest"), Op::kEqual, baseline.column("device_id", "requested")));
        timeline.combine(ruvia::DbSetOperation::kUnionAll, baseline);
        Query pointLagged(resource);
        const ruvia::DbWindowOptions pointOrder{ .partitionBy = { pointLagged.column("device_id"), pointLagged.column("element_id") },
            .orderBy = { { pointLagged.column("observed_at") }, { pointLagged.column("record_id") }, { pointLagged.column("baseline"), ruvia::DbOrderDirection::kDesc },
                { pointLagged.column("input_sequence"), ruvia::DbOrderDirection::kAsc, ruvia::DbNullsOrder::kFirst } } };
        pointLagged.select({ pointLagged.star("timeline"), pointLagged.alias(pointLagged.over(pointLagged.call("lag", { pointLagged.column("point_value") }), pointOrder), "prior_value"),
            pointLagged.alias(pointLagged.over(pointLagged.call("lag", { pointLagged.cast(pointLagged.value(true), Type::kBoolean) }), pointOrder), "prior_present") }).from("point_timeline", "timeline");
        Query changes(resource);
        changes.select({ changes.column("input_sequence"), changes.alias(changes.aggregate("bool_or", { changes.binary(
                changes.unary(ruvia::DbUnaryOperator::kIsNull, changes.column("prior_present")), Op::kOr,
                changes.binary(changes.column("point_value"), Op::kIsDistinctFrom, changes.column("prior_value"))) }), "changed") })
            .from("point_lagged").andWhere(changes.unary(ruvia::DbUnaryOperator::kNot, changes.column("baseline"))).groupBy({ changes.column("input_sequence") });
        Query filtered(resource);
        const auto accepted = filtered.binary(filtered.binary(filtered.column("storage_policy"), Op::kEqual, filtered.value("report")), Op::kOr,
            filtered.binary(filtered.binary(filtered.column("storage_policy"), Op::kEqual, filtered.value("change")), Op::kAnd,
                filtered.coalesce({ filtered.column("changed", "point_changes"), filtered.value(false) })));
        filtered.select({ filtered.star("ordered"), filtered.alias(accepted, "accepted") }).from("ordered")
            .join(ruvia::DbJoinType::kLeft, "point_changes", filtered.binary(filtered.column("input_sequence", "ordered"), Op::kEqual, filtered.column("input_sequence", "point_changes")));
        Query records(resource);
        selectColumns(records, { "report_time", "id", "device_id", "link_id", "connection_id", "protocol", "source", "occurred_at", "data", "raw_payload_hex" });
        const auto model = records.binary(records.column("data"), Op::kJsonGet, records.value("model"));
        records.addSelect(records.cast(records.binary(model, Op::kJsonGetText, records.value("id")), Type::kUuid))
            .addSelect(records.cast(records.binary(model, Op::kJsonGetText, records.value("revision")), Type::kBigInt)).from("filtered").andWhere(records.column("accepted"));
        Query inserted(resource);
        inserted.insertInto("device_data", { "report_time", "id", "device_id", "link_id", "connection_id", "protocol", "source", "occurred_at", "data", "raw_payload_hex", "model_id", "model_revision" })
            .insertFrom(records).onConflict({ .columns = { "id", "report_time" }, .doNothing = true }).returning({ inserted.column("device_id") });
        Query storage(resource);
        const auto lastStored = storage.aggregate("max", { storage.column("last_stored") });
        const auto lastAccepted = storage.filter(storage.aggregate("max", { storage.column("report_time") }), storage.column("accepted"));
        storage.select({ storage.column("device_id"), storage.alias(storage.coalesce({ storage.greatest({ lastStored, lastAccepted }), lastStored, lastAccepted }), "last_stored_at") })
            .from("filtered").groupBy({ storage.column("device_id") });
        Query newObserved(resource);
        const ruvia::DbWindowOptions newestOrder{ .partitionBy = { newObserved.column("device_id", "incoming") },
            .orderBy = { { newObserved.column("report_time", "incoming"), ruvia::DbOrderDirection::kDesc }, { newObserved.column("id", "incoming"), ruvia::DbOrderDirection::kDesc } } };
        newObserved.select({ newObserved.star("incoming"), newObserved.alias(newObserved.over(newObserved.call("row_number"), newestOrder), "newest") })
            .from("valid_incoming", "incoming").join(ruvia::DbJoinType::kInner, "states", newObserved.binary(newObserved.column("device_id", "incoming"), Op::kEqual, newObserved.column("device_id", "states")))
            .andWhere(newObserved.binary(newObserved.unary(ruvia::DbUnaryOperator::kIsNull, newObserved.column("last_observed_at", "states")), Op::kOr,
                newObserved.binary(newObserved.tuple({ newObserved.column("report_time", "incoming"), newObserved.column("id", "incoming") }), Op::kGreater,
                    newObserved.tuple({ newObserved.column("last_observed_at", "states"), newObserved.column("last_observed_id", "states" ) }))));
        Query observed(resource);
        observed.select({ observed.column("device_id", "newest"), observed.alias(observed.column("report_time", "newest"), "last_observed_at"),
                observed.alias(observed.column("id", "newest"), "last_observed_id"), observed.alias(observed.column("data", "newest"), "last_data"),
                observed.alias(observed.coalesce({ observed.column("data", "previous"), observed.column("last_data", "states") }), "previous_data") })
            .from("new_observed", "newest").join(ruvia::DbJoinType::kInner, "states", observed.binary(observed.column("device_id", "newest"), Op::kEqual, observed.column("device_id", "states")))
            .join(ruvia::DbJoinType::kLeft, "new_observed", observed.binary(observed.binary(observed.column("device_id", "previous"), Op::kEqual, observed.column("device_id", "newest")), Op::kAnd,
                observed.binary(observed.column("newest", "previous"), Op::kEqual, observed.value(2))), "previous")
            .andWhere(observed.binary(observed.column("newest", "newest"), Op::kEqual, observed.value(1)));
        Query latestElements(resource);
        latestElements.select({ latestElements.column("device_id", "incoming"), latestElements.alias(latestElements.column("key", "point"), "element_id"), latestElements.column("value", "point"),
                latestElements.alias(latestElements.column("report_time", "incoming"), "observed_at"), latestElements.alias(latestElements.column("id", "incoming"), "record_id") })
            .distinctOn({ latestElements.column("device_id", "incoming"), latestElements.column("key", "point") }).from("valid_incoming", "incoming")
            .join(ruvia::DbJoinType::kInner, "states", latestElements.binary(latestElements.column("device_id", "incoming"), Op::kEqual, latestElements.column("device_id", "states")))
            .joinFunction(ruvia::DbJoinType::kCross, latestElements.call("jsonb_each", { latestElements.coalesce({ latestElements.binary(latestElements.column("data", "incoming"), Op::kJsonGet, latestElements.value("values")), emptyJson(latestElements) }) }), {}, "point", { .lateral = true })
            .addOrderBy(latestElements.column("device_id", "incoming")).addOrderBy(latestElements.column("key", "point"))
            .addOrderBy(latestElements.column("report_time", "incoming"), ruvia::DbOrderDirection::kDesc).addOrderBy(latestElements.column("id", "incoming"), ruvia::DbOrderDirection::kDesc);
        Query latestRows(resource);
        selectColumns(latestRows, { "device_id", "element_id", "value", "observed_at", "record_id" });
        latestRows.addSelect(latestRows.call("now")).from("latest_elements");
        Query latestValues(resource);
        latestValues.insertInto("device_latest_value", { "device_id", "element_id", "value", "observed_at", "record_id", "updated_at" }).insertFrom(latestRows)
            .onConflict({ .columns = { "device_id", "element_id" }, .update = { { "value", latestValues.excluded("value") }, { "observed_at", latestValues.excluded("observed_at") },
                { "record_id", latestValues.excluded("record_id") }, { "updated_at", latestValues.call("now") } },
                .updateWhere = latestValues.binary(latestValues.tuple({ latestValues.excluded("observed_at"), latestValues.excluded("record_id") }), Op::kGreater,
                    latestValues.tuple({ latestValues.column("observed_at", "device_latest_value"), latestValues.column("record_id", "device_latest_value") })) })
            .returning({ latestValues.column("device_id") });
        const auto barrier = [&](std::string_view table, std::string_view countName) {
            Query query(resource);
            query.select(query.alias(query.aggregate("count", { query.star() }), countName)).from(table);
            return query;
        };
        auto insertedBarrier = barrier("inserted", "inserted_count");
        Query stateUpdated(resource);
        stateUpdated.update("device_data_ingest_state", "state").set("last_stored_at", stateUpdated.column("last_stored_at", "storage"))
            .set("last_observed_at", stateUpdated.coalesce({ stateUpdated.column("last_observed_at", "observed"), stateUpdated.column("last_observed_at", "state") }))
            .set("last_observed_id", stateUpdated.coalesce({ stateUpdated.column("last_observed_id", "observed"), stateUpdated.column("last_observed_id", "state") }))
            .set("last_data", stateUpdated.coalesce({ stateUpdated.column("last_data", "observed"), stateUpdated.column("last_data", "state") }))
            .set("previous_data", stateUpdated.caseWhen({ { stateUpdated.unary(ruvia::DbUnaryOperator::kIsNull, stateUpdated.column("device_id", "observed")), stateUpdated.column("previous_data", "state") } }, stateUpdated.column("previous_data", "observed")))
            .set("updated_at", stateUpdated.call("now")).updateFrom("storage_summary", "storage")
            .join(ruvia::DbJoinType::kLeft, "observed_summary", stateUpdated.binary(stateUpdated.column("device_id", "storage"), Op::kEqual, stateUpdated.column("device_id", "observed")), "observed")
            .join(ruvia::DbJoinType::kCross, insertedBarrier, {}, "inserted_barrier")
            .andWhere(stateUpdated.binary(stateUpdated.column("device_id", "state"), Op::kEqual, stateUpdated.column("device_id", "storage")))
            .andWhere(stateUpdated.binary(stateUpdated.column("inserted_count", "inserted_barrier"), Op::kGreaterEqual, stateUpdated.value(0))).returning({ stateUpdated.column("device_id", "state") });
        auto updateBarrier = barrier("state_updated", "updated_count");
        auto latestBarrier = barrier("latest_values", "latest_count");
        Query persisted(resource);
        const ruvia::DbCteOptions materialized{ .materialization = ruvia::DbMaterialization::kMaterialized };
        persisted.with("incoming", incoming, { .columns = { "input_sequence", "report_time", "id", "device_id", "link_id", "connection_id", "protocol", "source", "occurred_at", "data", "raw_payload_hex", "storage_policy", "needs_previous" } })
            .with("valid_incoming", valid, materialized).with("requested", requested, materialized).with("states", states, materialized).with("ordered", ordered)
            .with("lagged", lagged, materialized).with("predecessors", predecessors, materialized).with("incoming_points", incomingPoints, materialized)
            .with("point_timeline", timeline, materialized).with("point_lagged", pointLagged, materialized).with("point_changes", changes, materialized)
            .with("filtered", filtered, materialized).with("inserted", inserted).with("storage_summary", storage, materialized).with("new_observed", newObserved, materialized)
            .with("observed_summary", observed, materialized).with("latest_elements", latestElements, materialized).with("latest_values", latestValues).with("state_updated", stateUpdated)
            .select({ persisted.cast(persisted.column("input_sequence", "incoming"), Type::kText),
                persisted.cast(persisted.caseWhen({ { persisted.column("needs_previous", "incoming"), persisted.coalesce({ persisted.column("previous_data", "predecessors"), emptyJson(persisted) }) } }, emptyJson(persisted)), Type::kText) })
            .from("incoming").join(ruvia::DbJoinType::kLeft, "predecessors", persisted.binary(persisted.column("input_sequence", "incoming"), Op::kEqual, persisted.column("input_sequence", "predecessors")))
            .join(ruvia::DbJoinType::kCross, updateBarrier, {}, "update_barrier").join(ruvia::DbJoinType::kCross, latestBarrier, {}, "latest_barrier")
            .andWhere(persisted.binary(persisted.column("updated_count", "update_barrier"), Op::kGreaterEqual, persisted.value(0)))
            .andWhere(persisted.binary(persisted.column("latest_count", "latest_barrier"), Op::kGreaterEqual, persisted.value(0))).addOrderBy(persisted.column("input_sequence", "incoming"));
        return persisted;
    }

    static ruvia::Task<std::vector<std::string>>
    persist(ruvia::WebWorkerContext& context, const std::vector<message::ParsedDeviceMessage>& messages, const std::vector<bool>& alertActive) {
        if (messages.empty()) {
            co_return std::vector<std::string>{};
        }
        if (messages.size() != alertActive.size()) {
            throw std::invalid_argument("telemetry alert-active batch size mismatch");
        }
        using Query = ruvia::DbQuery;
        using Op = ruvia::DbBinaryOperator;
        using Type = ruvia::DbDataType;
        auto persisted = persistenceQuery(context.resource(), messages, alertActive);
        // Acquire per-device locks before taking the statement snapshot. Seeding
        // in a data-modifying CTE is invisible to sibling SELECTs and drops a
        // newly created device's first sample.
        auto transaction = co_await context.db("telemetry-history").beginTransaction();
        std::set<std::string_view> deviceIds;
        for (const auto& value : messages) {
            deviceIds.insert(value.deviceId);
        }
        Query requestedDevices(context.resource());
        for (const auto deviceId : deviceIds) requestedDevices.values({ requestedDevices.cast(requestedDevices.value(deviceId), Type::kUuid) });
        Query locks(context.resource());
        locks.with("requested", requestedDevices, { .columns = { "device_id" } })
            .select(locks.call("pg_advisory_xact_lock", { locks.call("hashtextextended", {
                locks.cast(locks.column("device_id"), Type::kText), locks.cast(locks.value(734621), Type::kBigInt) }) }))
            .from("requested").addOrderBy(locks.column("device_id"));
        (void)co_await transaction.query(locks);
        Query deviceRows(context.resource());
        deviceRows.select(deviceRows.column("device_id")).from("requested");
        Query seed(context.resource());
        seed.with("requested", requestedDevices, { .columns = { "device_id" } })
            .insertInto("device_data_ingest_state", { "device_id" }).insertFrom(deviceRows)
            .onConflict({ .columns = { "device_id" }, .doNothing = true });
        (void)co_await transaction.execute(seed);
        const auto rows = co_await transaction.query(persisted);
        co_await transaction.commit();
        std::vector<std::string> previous(messages.size(), "{}");
        for (const auto& row : rows) {
            const auto parsedSequence =
                service::common::parseInt64(std::optional<std::string_view>{ row[0].value().value_or(std::string_view{}) });
            if (!parsedSequence || *parsedSequence < 0) {
                continue;
            }
            const auto sequence = static_cast<std::size_t>(*parsedSequence);
            if (sequence < previous.size() && row[1].value().has_value()) {
                previous[sequence].assign(row[1].value().value_or(std::string_view{}));
            }
        }
        co_return previous;
    }
};

} // namespace service::telemetry
