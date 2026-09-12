#pragma once

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/web/db/DbQuery.h>

#include "service/common/message.h"
#include "service/features/messaging/messaging.transport.h"
#include "service/features/telemetry/latest/latest.service.h"

namespace service::command {

struct DeviceRoute {
    std::size_t workerIndex = 0;
    std::string instanceId;
    std::string connectionId;
    std::uint64_t sessionEpoch = 0;
};

class DeviceRouteError final : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

enum class PendingQueueKind { Stream, List };

struct PendingDispatch {
    message::ProtocolTask task;
    std::vector<message::StreamField> streamFields;
    std::string listPayload;
};

template <typename Redis>
ruvia::Task<bool> dispatchPendingBatch(const Redis& redis, std::string_view queueKey,
                                       PendingQueueKind kind,
                                       const std::vector<PendingDispatch>& dispatches,
                                       std::string_view submittedBy,
                                       std::size_t maxLength) {
    if (queueKey.empty() || dispatches.empty() || maxLength == 0)
        throw std::invalid_argument("pending command batch is incomplete");
    std::set<std::string, std::less<>> commandIds;
    std::vector<std::string> arguments{
        kind == PendingQueueKind::Stream ? "stream" : "list",
        std::to_string(maxLength), std::to_string(dispatches.size())};
    (void)submittedBy; // The durable operation owns actor and status, never the queue.
    for (const auto& dispatch : dispatches) {
        if (dispatch.task.messageId.empty() ||
            !commandIds.emplace(dispatch.task.messageId).second)
            throw std::invalid_argument("pending command identity is invalid");
        if (kind == PendingQueueKind::Stream) {
            if (dispatch.streamFields.empty()) throw std::invalid_argument("empty command payload");
            arguments.push_back(std::to_string(dispatch.streamFields.size()*2));
            for (const auto& field : dispatch.streamFields) {
                arguments.push_back(field.name); arguments.push_back(field.value);
            }
        } else {
            if (dispatch.listPayload.empty()) throw std::invalid_argument("empty command payload");
            arguments.emplace_back("1"); arguments.push_back(dispatch.listPayload);
        }
    }
    static constexpr std::string_view script = R"lua(
local mode=ARGV[1]
local count=tonumber(ARGV[3])
local depth=mode=='stream' and redis.call('XLEN',KEYS[1]) or redis.call('LLEN',KEYS[1])
if depth+count>tonumber(ARGV[2]) then return 0 end
local offset=4
for task=1,count do
  local size=tonumber(ARGV[offset])
  offset=offset+1
  local values={}
  if mode=='stream' then values[1]='*' end
  for index=1,size do values[#values+1]=ARGV[offset]; offset=offset+1 end
  if mode=='stream' then redis.call('XADD',KEYS[1],unpack(values))
  else redis.call('RPUSH',KEYS[1],unpack(values)) end
end
return count
)lua";
    const std::string_view keyViews[]{queueKey};
    std::vector<std::string_view> argumentViews(arguments.begin(),arguments.end());
    const auto reply = co_await redis.eval(script, keyViews, argumentViews);
    if (reply.kind() == ruvia::RedisValue::Kind::kInteger && reply.integer() == 0)
        co_return false;
    if (reply.kind() != ruvia::RedisValue::Kind::kInteger ||
        reply.integer() != static_cast<std::int64_t>(dispatches.size()))
        message::redis::throwValue("dispatch pending command batch", reply);
    co_return true;
}

inline std::string_view field(const std::vector<message::StreamField>& fields,
                              std::string_view name) noexcept {
    for (const auto& current : fields)
        if (current.name == name)
            return current.value;
    return {};
}

template <typename Redis>
ruvia::Task<DeviceRoute> deviceRoute(const Redis& redis, std::string_view deviceId) {
    const auto fields = co_await message::redis::hashEntries(
        redis, service::telemetry::latest::runtimeKey(deviceId));
    const auto worker = field(fields, "worker_id");
    const auto instance = field(fields, "instance_id");
    const auto connection = field(fields, "connection_id");
    const auto epoch = field(fields, "session_epoch");
    if (instance.empty() || worker.empty() || connection.empty() || epoch.empty())
        throw DeviceRouteError("device is offline or has no collector route");

    DeviceRoute route;
    const auto [workerEnd, workerError] =
        std::from_chars(worker.data(), worker.data() + worker.size(), route.workerIndex);
    const auto [epochEnd, epochError] =
        std::from_chars(epoch.data(), epoch.data() + epoch.size(), route.sessionEpoch);
    if (workerError != std::errc{} || workerEnd != worker.data() + worker.size() ||
        epochError != std::errc{} || epochEnd != epoch.data() + epoch.size() ||
        route.sessionEpoch == 0)
        throw DeviceRouteError("device collector route is invalid");
    route.connectionId = connection;
    route.instanceId = instance;
    const auto link = field(fields,"link_id");
    const auto owner = co_await message::redis::command(redis,
        {"GET","iot:v2:owner:link:" + std::string(link)});
    if (owner.kind() != ruvia::RedisValue::Kind::kString || owner.string() != instance)
        throw DeviceRouteError("device collector ownership expired");
    co_return route;
}


} // namespace service::command

#include "service/features/command/command.types.h"
#include "service/common/http.h"
#include "service/common/uuid.h"
#include "service/features/edge/edge.transport.h"
#include "service/utils/json.h"

#include <optional>

namespace service::command::repository {

template <typename Database>
ruvia::Task<void> event(Database& db, std::string_view commandId,
                         std::string_view type) {
    const auto eventId = service::common::nextUuidV7();
    ruvia::DbQuery operation;
    operation.select({ operation.cast(operation.value(eventId), ruvia::DbDataType::kUuid), operation.value(type), operation.value("command"),
            operation.cast(operation.column("device_id"), ruvia::DbDataType::kText), operation.value("updated"), operation.value(2),
            operation.call("jsonb_build_object", { operation.cast(operation.value("device_code"), ruvia::DbDataType::kText), operation.column("device_code"), operation.cast(operation.value("data"), ruvia::DbDataType::kText),
                operation.call("jsonb_build_object", { operation.cast(operation.value("commandId"), ruvia::DbDataType::kText), operation.cast(operation.column("id"), ruvia::DbDataType::kText),
                    operation.cast(operation.value("status"), ruvia::DbDataType::kText), operation.column("status"), operation.cast(operation.value("reason"), ruvia::DbDataType::kText), operation.column("reason"),
                    operation.cast(operation.value("elements"), ruvia::DbDataType::kText), operation.column("elements"), operation.cast(operation.value("actualValues"), ruvia::DbDataType::kText), operation.column("actual_values") }) }) })
        .from("command_operation")
        .andWhere(operation.binary(operation.column("id"), ruvia::DbBinaryOperator::kEqual, operation.cast(operation.value(commandId), ruvia::DbDataType::kUuid)));
    ruvia::DbQuery outbox;
    outbox.insertInto("outbox_event", { "id", "event_type", "aggregate_type", "aggregate_id", "action", "schema_version", "payload" }).insertFrom(operation);
    (void)co_await db.execute(outbox);
}


// Claim is committed BEFORE touching the physical delivery path. A crashed or ambiguous
// attempt is never replayed automatically: old EdgeNode cannot promise durable deduplication.
template <typename Context>
ruvia::Task<void> dispatch(Context& context) {
    auto tx = co_await context.db().beginTransaction();
    using Op = ruvia::DbBinaryOperator;
    using Type = ruvia::DbDataType;
    ruvia::DbQuery pending;
    pending.select({ pending.cast(pending.column("operation_id", "a"), Type::kText), pending.column("queue_key", "a"),
        pending.column("queue_kind", "a"), pending.column("submitted_by", "a"), pending.column("node_id", "a"),
        pending.cast(pending.column("max_length", "a"), Type::kText), pending.cast(pending.column("device_id", "o"), Type::kText),
        pending.column("device_code", "o"), pending.column("protocol", "o"),
        pending.cast(pending.cast(pending.binary(pending.extract(ruvia::DbDatePart::kEpoch, pending.column("created_at", "o")),
            Op::kMultiply, pending.value(1000)), Type::kBigInt), Type::kText) })
        .from("command_attempt", "a")
        .join(ruvia::DbJoinType::kInner, "command_operation", pending.binary(pending.column("id", "o"), Op::kEqual, pending.column("operation_id", "a")), "o")
        .andWhere(pending.unary(ruvia::DbUnaryOperator::kIsNull, pending.column("claimed_at", "a")))
        .andWhere(pending.binary(pending.column("deadline", "a"), Op::kGreater, pending.call("now")))
        .andWhere(pending.binary(pending.column("status", "o"), Op::kEqual, pending.value("ACCEPTED")))
        .addOrderBy(pending.column("created_at", "o")).lock({ .mode = ruvia::DbRowLock::kUpdate, .skipLocked = true, .tables = { "a", "o" } }).limit(16);
    const auto rows = co_await tx.query(pending);
    for (const auto& row : rows) {
        const auto id = row[0].value().value_or(std::string_view{});
        ruvia::DbQuery claim;
        claim.update("command_attempt").set("claimed_at", claim.call("now"))
            .andWhere(claim.binary(claim.column("operation_id"), Op::kEqual, claim.cast(claim.value(id), Type::kUuid)));
        (void)co_await tx.execute(claim);
        ruvia::DbQuery operation;
        operation.update("command_operation").set("status", operation.value("DISPATCHING"))
            .andWhere(operation.binary(operation.column("id"), Op::kEqual, operation.cast(operation.value(id), Type::kUuid)));
        (void)co_await tx.execute(operation);
    }
    co_await tx.commit();
    for (const auto& row : rows) {
        const auto cell = [&](std::size_t index) { return row[index].value().value_or(std::string_view{}); };
        std::string failure;
        bool published = false;
        try {
            ruvia::DbQuery payload;
            payload.select(payload.column("value")).from("command_attempt")
                .joinFunction(ruvia::DbJoinType::kCross, payload.call("jsonb_array_elements_text", { payload.column("payload") }),
                    {}, "p", { .withOrdinality = true, .columns = { { .name = "value" }, { .name = "idx" } } })
                .andWhere(payload.binary(payload.column("operation_id"), Op::kEqual, payload.cast(payload.value(cell(0)), Type::kUuid)))
                .addOrderBy(payload.column("idx"));
            const auto values = co_await context.db().query(payload);
            PendingDispatch item;
            item.task.messageId = cell(0); item.task.deviceId = cell(6);
            item.task.deviceCode = cell(7); item.task.protocol = cell(8);
            item.task.createdAtMs = common::parseInt64(cell(9)).value_or(0);
            const bool list = cell(2) == "list";
            if (list) {
                const auto bytes = message::fromHex(values.front()[0].value().value_or(std::string_view{}));
                item.listPayload.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            } else {
                for (std::size_t index = 0; index + 1 < values.size(); index += 2)
                    item.streamFields.push_back({std::string(values[index][0].value().value_or(std::string_view{})),
                                                  std::string(values[index+1][0].value().value_or(std::string_view{}))});
            }
            published = co_await dispatchPendingBatch(context.redis(), cell(1),
                list ? PendingQueueKind::List : PendingQueueKind::Stream,
                std::vector<PendingDispatch>{std::move(item)}, cell(3),
                static_cast<std::size_t>(common::parseInt64(cell(5)).value_or(1)));
            if (!published) failure = "queue_capacity_exceeded";
        } catch (const std::exception& error) { failure = error.what(); }
        // A known capacity rejection is safe to report as rejected. Other failures can
        // occur after Redis committed the enqueue, so the outcome must stay unknown.
        const std::string_view next = published ? "AWAITING_RESULT" :
            failure == "queue_capacity_exceeded" ? "REJECTED" : "UNKNOWN";
        auto update = co_await context.db().beginTransaction();
        ruvia::DbQuery outcome;
        outcome.update("command_operation").set("status", outcome.value(next)).set("reason", outcome.value(failure))
            .set("completed_at", outcome.caseWhen({ { outcome.binary(outcome.value(next), Op::kEqual, outcome.value("AWAITING_RESULT")), outcome.nullValue() } }, outcome.call("now")))
            .andWhere(outcome.binary(outcome.column("id"), Op::kEqual, outcome.cast(outcome.value(cell(0)), Type::kUuid)))
            .andWhere(outcome.binary(outcome.column("status"), Op::kEqual, outcome.value("DISPATCHING")));
        (void)co_await update.execute(outcome);
        if (published) {
            ruvia::DbQuery dispatched;
            dispatched.update("command_attempt").set("dispatched_at", dispatched.call("now"))
                .andWhere(dispatched.binary(dispatched.column("operation_id"), Op::kEqual, dispatched.cast(dispatched.value(cell(0)), Type::kUuid)));
            (void)co_await update.execute(dispatched);
        }
        else co_await event(update,cell(0),"device.command.updated");
        co_await update.commit();
        if (published && !cell(4).empty())
            co_await edge::dispatch::notifyNode(context.redis(),cell(4));
    }
    auto expiry = co_await context.db().beginTransaction();
    ruvia::DbQuery overdue;
    const auto unclaimed = overdue.unary(ruvia::DbUnaryOperator::kIsNull, overdue.column("claimed_at", "a"));
    overdue.update("command_operation", "o")
        .set("status", overdue.caseWhen({ { unclaimed, overdue.value("REJECTED") } }, overdue.value("UNKNOWN")))
        .set("reason", overdue.caseWhen({ { unclaimed, overdue.value("dispatch_deadline_expired") } }, overdue.value("result_not_confirmed")))
        .set("completed_at", overdue.call("now")).updateFrom("command_attempt", "a")
        .andWhere(overdue.binary(overdue.column("id", "o"), Op::kEqual, overdue.column("operation_id", "a")))
        .andWhere(overdue.binary(overdue.column("deadline", "a"), Op::kLessEqual, overdue.call("now")))
        .andWhere(overdue.binary(overdue.column("status", "o"), Op::kIn, overdue.list({ overdue.value("ACCEPTED"), overdue.value("DISPATCHING"), overdue.value("AWAITING_RESULT") })))
        .returning({ overdue.cast(overdue.column("id", "o"), Type::kText) });
    const auto expired = co_await expiry.query(overdue);
    for (const auto& row : expired)
        co_await event(expiry,row[0].value().value_or(std::string_view{}),"device.command.updated");
    co_await expiry.commit();
}

template <typename Context>
ruvia::Task<std::optional<std::chrono::milliseconds>>
nextDispatchDelay(Context& context) {
    using Op = ruvia::DbBinaryOperator;
    ruvia::DbQuery delayQuery;
    const auto claimable = delayQuery.binary(delayQuery.binary(
        delayQuery.binary(delayQuery.column("status", "o"), Op::kEqual, delayQuery.value("ACCEPTED")), Op::kAnd,
        delayQuery.unary(ruvia::DbUnaryOperator::kIsNull, delayQuery.column("claimed_at", "a"))), Op::kAnd,
        delayQuery.binary(delayQuery.column("deadline", "a"), Op::kGreater, delayQuery.call("now")));
    const auto deadlineMs = delayQuery.cast(delayQuery.call("ceil", { delayQuery.binary(delayQuery.extract(ruvia::DbDatePart::kEpoch,
        delayQuery.binary(delayQuery.aggregate("min", { delayQuery.column("deadline", "a") }), Op::kSubtract, delayQuery.call("now"))),
        Op::kMultiply, delayQuery.value(1000)) }), ruvia::DbDataType::kBigInt);
    delayQuery.select(delayQuery.cast(delayQuery.caseWhen({
            { delayQuery.binary(delayQuery.aggregate("count", { delayQuery.star() }), Op::kEqual, delayQuery.value(0)), delayQuery.nullValue() },
            { delayQuery.aggregate("bool_or", { claimable }), delayQuery.value(25) } },
            delayQuery.greatest({ delayQuery.value(25), deadlineMs })), ruvia::DbDataType::kText))
        .from("command_attempt", "a")
        .join(ruvia::DbJoinType::kInner, "command_operation", delayQuery.binary(delayQuery.column("id", "o"), Op::kEqual, delayQuery.column("operation_id", "a")), "o")
        .andWhere(delayQuery.binary(delayQuery.column("status", "o"), Op::kIn,
            delayQuery.list({ delayQuery.value("ACCEPTED"), delayQuery.value("DISPATCHING"), delayQuery.value("AWAITING_RESULT") })))
        .andWhere(delayQuery.unary(ruvia::DbUnaryOperator::kIsNotNull, delayQuery.column("deadline", "a")));
    const auto rows = co_await context.db().query(delayQuery);
    if (rows.empty())
        co_return std::nullopt;
    const auto value = rows.front()[0].value();
    if (!value)
        co_return std::nullopt;
    const auto delay = common::parseInt64(std::optional<std::string_view>{*value});
    if (!delay)
        throw std::runtime_error("invalid command dispatch deadline delay");
    co_return std::chrono::milliseconds(*delay);
}

} // namespace service::command::repository

namespace service::command {

class CommandResultService final {
  public:
    template <typename Context>
    static ruvia::Task<void> project(
        Context& context, const std::vector<message::StreamMessage>& messages) {
        if (messages.empty())
            co_return;
        auto transaction = co_await context.db().beginTransaction();
        for (const auto& message : messages) {
            const auto id = message.get("command_id");
            const auto deviceId = message.get("device_id");
            if (!common::isUuid(id) || !common::isUuid(deviceId))
                continue;
            const auto explicitState = message.get("result_state");
            const auto state = terminalState(explicitState)
                                   ? explicitState
                                   : collectorResultState(message.get("success") == "1",
                                                          message.get("reason"));
            const auto actual = actualValuesJson(message);
            using Op = ruvia::DbBinaryOperator;
            ruvia::DbQuery outcome;
            outcome.update("command_operation").set("status", outcome.value(state)).set("reason", outcome.value(message.get("reason")))
                .set("actual_values", outcome.cast(outcome.value(actual), ruvia::DbDataType::kJsonb)).set("completed_at", outcome.call("now"))
                .andWhere(outcome.binary(outcome.column("id"), Op::kEqual, outcome.cast(outcome.value(id), ruvia::DbDataType::kUuid)))
                .andWhere(outcome.binary(outcome.column("device_id"), Op::kEqual, outcome.cast(outcome.value(deviceId), ruvia::DbDataType::kUuid)))
                .andWhere(outcome.binary(outcome.binary(outcome.column("status"), Op::kIn,
                    outcome.list({ outcome.value("DISPATCHING"), outcome.value("AWAITING_RESULT") })), Op::kOr,
                    outcome.binary(outcome.binary(outcome.column("status"), Op::kEqual, outcome.value("UNKNOWN")), Op::kAnd,
                        outcome.binary(outcome.value(state), Op::kNotEqual, outcome.value("UNKNOWN")))))
                .returning({ outcome.cast(outcome.column("id"), ruvia::DbDataType::kText) });
            const auto updated = co_await transaction.query(outcome);
            if (!updated.empty())
                co_await repository::event(transaction, id, "device.command.updated");
        }
        co_await transaction.commit();
    }

  private:
    static std::size_t actualValueCount(const message::StreamMessage& message) {
        const auto value = message.get("actual_value_count");
        std::size_t count{};
        const auto [end, error] =
            std::from_chars(value.data(), value.data() + value.size(), count);
        if (error != std::errc{} || end != value.data() + value.size())
            return 0;
        return std::min<std::size_t>(count, 8);
    }

    static std::string actualValueField(std::size_t index, std::string_view name) {
        return "actual_value_" + std::to_string(index) + "_" + std::string(name);
    }

    static std::string actualValuesJson(const message::StreamMessage& message) {
        std::string output{"["};
        for (std::size_t index = 0; index < actualValueCount(message); ++index) {
            if (index != 0)
                output.push_back(',');
            output += "{\"elementId\":" + service::utils::jsonQuoted(
                          message.get(actualValueField(index, "element_id"))) +
                      ",\"name\":" + service::utils::jsonQuoted(
                          message.get(actualValueField(index, "name"))) +
                      ",\"kind\":" + service::utils::jsonQuoted(
                          message.get(actualValueField(index, "kind"))) +
                      ",\"value\":" + service::utils::jsonQuoted(
                          message.get(actualValueField(index, "value"))) +
                      ",\"unit\":" + service::utils::jsonQuoted(
                          message.get(actualValueField(index, "unit"))) + "}";
        }
        output.push_back(']');
        return output;
    }
};

} // namespace service::command

#include "service/features/configuration/configuration.service.h"
#include "service/features/collector/collector.protocol.h"
#include "service/features/edge/edge.protocol.h"

namespace service::command {

class PreparationService final {
    struct Prepared {
        std::string queue;
        PendingQueueKind kind;
        std::vector<PendingDispatch> dispatches;
        std::size_t maximum;
        std::string nodeId;
    };

public:
    static ruvia::Task<std::string> prepare(ruvia::WebWorkerContext& context,
                                           std::string_view payload) {
        const auto request = ruvia::JsonValue::parse(payload);
        if (!request) common::fail(18010, "Invalid command preparation request", 400);
        const auto id = request->get<ruvia::String>("deviceId");
        const auto elements = service::utils::jsonField(*request, "elements");
        if (!id || !common::isUuid(id->view()) || !elements)
            common::fail(18010, "Invalid command preparation parameters", 400);
        auto remaining = elements->view();
        const auto values = ruvia::detail::parseJsonValue<ruvia::Array<ruvia::Array<ruvia::String>>>(
            remaining, context.resource());
        if (!values || values->empty() || values->size() > 256)
            common::fail(18010, "Invalid command preparation elements", 400);
        std::vector<service::collector::CommandElementValue> requested;
        for (const auto& pair : *values) {
            if (pair.size() != 2)
                common::fail(18010, "Invalid command preparation element", 400);
            requested.push_back({std::string(pair[0].view()), std::string(pair[1].view())});
        }
        auto transaction = co_await context.db("control").beginTransaction();
        const auto prepared = co_await compileDevice(context, id->view(), std::move(requested), transaction);
        co_await transaction.commit();
        co_return encode(prepared);
    }

private:
    static std::string encode(const Prepared& prepared) {
        using service::utils::jsonQuoted;
        std::string result = "{\"queue\":" + jsonQuoted(prepared.queue) +
            ",\"kind\":" + jsonQuoted(prepared.kind == PendingQueueKind::List ? "list" : "stream") +
            ",\"maximum\":" + std::to_string(prepared.maximum) +
            ",\"nodeId\":" + jsonQuoted(prepared.nodeId) + ",\"commands\":[";
        bool firstCommand = true;
        for (const auto& dispatch : prepared.dispatches) {
            if (!firstCommand) result += ',';
            firstCommand = false;
            const auto& task = dispatch.task;
            result += "{\"id\":" + jsonQuoted(task.messageId) +
                ",\"deviceId\":" + jsonQuoted(task.deviceId) +
                ",\"deviceCode\":" + jsonQuoted(task.deviceCode) +
                ",\"protocol\":" + jsonQuoted(task.protocol) + ",\"elements\":[";
            bool first = true;
            for (const auto& [id, value] : task.elements) {
                if (!first) result += ',';
                first = false;
                result += "{\"elementId\":" + jsonQuoted(id) + ",\"value\":" + jsonQuoted(value) + "}";
            }
            result += "],\"payload\":[";
            if (prepared.kind == PendingQueueKind::List) {
                const auto* begin = reinterpret_cast<const std::uint8_t*>(dispatch.listPayload.data());
                result += jsonQuoted(message::toHex(
                    std::vector<std::uint8_t>(begin, begin + dispatch.listPayload.size())));
            } else {
                first = true;
                for (const auto& field : dispatch.streamFields) {
                    if (!first) result += ',';
                    first = false;
                    result += jsonQuoted(field.name) + ',' + jsonQuoted(field.value);
                }
            }
            result += "]}";
        }
        return result + "]}";
    }
    static ruvia::Task<Prepared> compileDevice(ruvia::WebWorkerContext& context,
                  std::string_view deviceId, std::vector<service::collector::CommandElementValue> requested,
                  ruvia::DbTransaction& transaction) {

        // 调用方在命令事务中持有设备共享锁，准备事务只读取，避免跨连接重复加锁。

        using Op = ruvia::DbBinaryOperator;
        using Type = ruvia::DbDataType;
        ruvia::DbQuery routeQuery;
        const auto parameters = routeQuery.column("protocol_params", "d");
        const auto text = [&](ruvia::DbExpression object, std::string_view key) {
            return routeQuery.binary(object, Op::kJsonGetText, routeQuery.value(key));
        };
        const auto boolean = [&](ruvia::DbExpression value) {
            return routeQuery.binary(routeQuery.call("lower", { routeQuery.coalesce({ value, routeQuery.value("") }) }), Op::kIn,
                routeQuery.list({ routeQuery.value("true"), routeQuery.value("t"), routeQuery.value("1"), routeQuery.value("yes"), routeQuery.value("y"), routeQuery.value("on") }));
        };
        const auto nodeConfig = routeQuery.binary(routeQuery.column("status", "n"), Op::kJsonGet, routeQuery.value("config"));
        const auto version = [&](std::string_view key) {
            const auto value = text(nodeConfig, key);
            return routeQuery.coalesce({ routeQuery.caseWhen({ { routeQuery.binary(routeQuery.coalesce({ value, routeQuery.value("") }), Op::kRegex,
                routeQuery.value("^-?[0-9]{1,18}$")), routeQuery.cast(value, Type::kBigInt) } }), routeQuery.value(0) });
        };
        auto applied = routeQuery.binary(routeQuery.column("status", "l"), Op::kEqual, routeQuery.value("enabled"));
        applied = routeQuery.binary(applied, Op::kAnd, routeQuery.binary(routeQuery.column("enrollment_status", "n"), Op::kEqual, routeQuery.value("approved")));
        applied = routeQuery.binary(applied, Op::kAnd, boolean(text(routeQuery.column("capability", "n"), "deviceConfig")));
        applied = routeQuery.binary(applied, Op::kAnd, routeQuery.binary(routeQuery.coalesce({ text(nodeConfig, "state"), routeQuery.value("idle") }), Op::kEqual, routeQuery.value("applied")));
        applied = routeQuery.binary(applied, Op::kAnd, routeQuery.binary(version("activeVersion"), Op::kEqual, version("desiredVersion")));
        routeQuery.select({ routeQuery.coalesce({ routeQuery.cast(routeQuery.column("edge_node_id", "l"), Type::kText), routeQuery.value("") }),
                text(parameters, "device_code"), routeQuery.column("protocol", "p"),
                routeQuery.caseWhen({ { routeQuery.binary(parameters, Op::kJsonHasKey, routeQuery.value("remote_control")), boolean(text(parameters, "remote_control")) } }, routeQuery.value(true)),
                routeQuery.coalesce({ applied, routeQuery.value(false) }),
                routeQuery.coalesce({ routeQuery.nullIf(text(routeQuery.column("config", "p"), "commandFastReadDuration"), routeQuery.value("")), routeQuery.value("60") }),
                routeQuery.coalesce({ routeQuery.nullIf(text(routeQuery.column("config", "p"), "commandFastReadInterval"), routeQuery.value("")), routeQuery.value("1") }) })
            .from("device", "d")
            .join(ruvia::DbJoinType::kInner, "link", routeQuery.binary(routeQuery.column("id", "l"), Op::kEqual, routeQuery.column("link_id", "d")), "l")
            .join(ruvia::DbJoinType::kInner, "device_model", routeQuery.binary(routeQuery.column("device_id", "p"), Op::kEqual, routeQuery.column("id", "d")), "p")
            .join(ruvia::DbJoinType::kLeft, "edge_node", routeQuery.binary(routeQuery.column("id", "n"), Op::kEqual, routeQuery.column("edge_node_id", "l")), "n")
            .andWhere(routeQuery.binary(routeQuery.column("id", "d"), Op::kEqual, routeQuery.cast(routeQuery.value(deviceId), Type::kUuid)))
            .andWhere(routeQuery.unary(ruvia::DbUnaryOperator::kIsNull, routeQuery.column("deleted_at", "d")))
            .andWhere(routeQuery.binary(routeQuery.column("status", "d"), Op::kEqual, routeQuery.value("enabled")))
            .andWhere(routeQuery.unary(ruvia::DbUnaryOperator::kIsNull, routeQuery.column("deleted_at", "l")))
            .andWhere(routeQuery.unary(ruvia::DbUnaryOperator::kIsNull, routeQuery.column("deleted_at", "p")))
            .andWhere(routeQuery.column("enabled", "p")).limit(1);
        const auto edge = co_await transaction.query(routeQuery);
        if (!edge.empty() && !edge.front()[0].value().value_or(std::string_view{}).empty()) {
            if (edge.front()[4].value().value_or(std::string_view{}) != "t")
                service::common::fail(18013, "边缘节点设备配置尚未生效", 409);
            co_return co_await enqueueEdgeDevice(context, deviceId, std::move(requested),
                                                 edge.front()[0].value().value_or(std::string_view{}),
                                                 edge.front()[1].value().value_or(std::string_view{}),
                                                 edge.front()[2].value().value_or(std::string_view{}),
                                                 edge.front()[3].value().value_or(std::string_view{}) == "t",
                                                 boundedUnsigned(
                                                     edge.front()[5].value().value_or(std::string_view{}),
                                                     60, 0),
                                                 boundedUnsigned(
                                                     edge.front()[6].value().value_or(std::string_view{}),
                                                     1, 1), transaction);
        }

        const auto snapshot =
            co_await service::runtime::repository::loadRuntimeSnapshot(transaction);
        const auto device =
            std::find_if(snapshot.devices.begin(), snapshot.devices.end(),
                         [deviceId](const auto& current) { return current.id == deviceId; });
        if (device == snapshot.devices.end())
            service::common::fail(18011, "设备、链路或协议配置未启用", 409);
        try {
            (void)service::collector::command::resolve(*device, requested);
        } catch (const std::invalid_argument& error) {
            service::common::fail(18010, error.what(), 400);
        }

        DeviceRoute route;
        try {
            route = co_await deviceRoute(context.redis(), device->id);
        } catch (const DeviceRouteError&) {
            service::common::fail(18013, "设备离线或没有可用的南桥连接", 409);
        }
        std::vector<std::vector<service::collector::CommandElementValue>> tasks;
        if (device->protocol == "SL651")
            tasks.push_back(std::move(requested));
        else {
            tasks.reserve(requested.size());
            for (auto& element : requested)
                tasks.push_back({std::move(element)});
        }

        std::vector<PendingDispatch> dispatches;
        dispatches.reserve(tasks.size());
        for (const auto& elements : tasks) {
            PendingDispatch dispatch;
            auto& task = dispatch.task;
            task.messageId = service::common::nextUuidV7();
            task.groupKey = "device:" + device->id;
            task.protocol = device->protocol;
            task.transport = device->protocol == "Modbus" ? device->modbusMode : "RAW";
            task.kind = "command";
            task.linkId = device->linkId;
            task.deviceId = device->id;
            task.deviceCode = device->code;
            task.responseTimeoutMs = 5000;
            task.maxAttempts = 1;
            task.connectionId = route.connectionId;
            task.sessionEpoch = route.sessionEpoch;
            task.createdAtMs = message::utcNowMilliseconds();
            for (const auto& element : elements)
                task.elements.emplace_back(element.elementId, element.value);
            dispatch.streamFields = message::protocolTaskFields(task);
            dispatches.push_back(std::move(dispatch));
        }
        co_return Prepared{
            .queue = message::commandStream(route.workerIndex, true, route.instanceId),
            .kind = PendingQueueKind::Stream, .dispatches = std::move(dispatches),
            .maximum = 10000};

    }

    static ruvia::Task<Prepared>
    enqueueEdgeDevice(ruvia::WebWorkerContext& context, std::string_view deviceId,
                      std::vector<service::collector::CommandElementValue> requested,
                      std::string_view nodeId, std::string_view deviceCode,
                      std::string_view protocol, bool remoteControl,
                      std::uint32_t fastReadDurationSec,
                      std::uint32_t fastReadIntervalSec,
                      ruvia::DbTransaction& transaction) {
        if (!remoteControl)
            service::common::fail(18005, "设备未开启远程控制", 403);
        if (!co_await context.redis().get("iot:edge:session:" + std::string(nodeId)))
            service::common::fail(18013, "边缘节点离线，无法下发设备命令", 409);

        service::collector::DeviceDefinition device;
        device.id = std::string(deviceId);
        device.code = std::string(deviceCode);
        device.protocol = std::string(protocol);
        co_await loadEdgeElements(transaction, device);
        service::collector::command::ResolvedCommand resolved;
        try {
            resolved = service::collector::command::resolve(device, requested);
        } catch (const std::invalid_argument& error) {
            service::common::fail(18010, error.what(), 400);
        }
        if (device.protocol == "SL651" && resolved.elements.size() > 8)
            service::common::fail(18010, "SL651 边缘命令最多包含 8 个要素", 400);

        std::vector<std::vector<service::collector::CommandElementValue>> tasks;
        if (device.protocol == "SL651") {
            tasks.push_back(std::move(requested));
        } else {
            tasks.reserve(requested.size());
            for (auto& element : requested)
                tasks.push_back({std::move(element)});
        }

        std::vector<PendingDispatch> dispatches;
        dispatches.reserve(tasks.size());
        for (const auto& elements : tasks) {
            PendingDispatch dispatch;
            auto& task = dispatch.task;
            task.messageId = service::common::nextUuidV7();
            task.groupKey = "device:" + device.id;
            task.protocol = device.protocol;
            task.transport = "EDGE";
            task.kind = "command";
            task.deviceId = device.id;
            task.deviceCode = device.code;
            task.responseTimeoutMs = 5000;
            task.maxAttempts = 1;
            task.createdAtMs = message::utcNowMilliseconds();
            for (const auto& element : elements)
                task.elements.emplace_back(element.elementId, element.value);

            auto envelope = service::edge::protocol::outbound(nodeId);
            auto* command = envelope.mutable_command_request();
            if (!setUuid(command->mutable_command_id(), task.messageId) ||
                !setUuid(command->mutable_device_id(), task.deviceId))
                service::common::fail(18010, "边缘命令标识无效", 500);
            command->set_timeout_ms(5000);
            command->set_readback_count(1);
            command->set_fast_read_duration_sec(fastReadDurationSec);
            command->set_fast_read_interval_sec(fastReadIntervalSec);
            for (const auto& element : elements) {
                if (element.elementId.size() > 64 || element.value.size() > 128)
                    service::common::fail(
                        18010, "边缘命令要素 ID 最长 64 字符、值最长 128 字符", 400);
                auto* value = command->add_values();
                value->set_element_id(element.elementId);
                auto* expected = value->mutable_expected();
                expected->set_kind(service::edge::pb::VALUE_STRING);
                expected->set_string_value(element.value);
            }
            dispatch.listPayload = service::edge::protocol::encode(envelope);
            if (dispatch.listPayload.empty())
                service::common::fail(18010, "边缘命令编码失败", 500);
            dispatches.push_back(std::move(dispatch));
        }
        co_return Prepared{
            .queue = "iot:v2:edge:commands:" + std::string(nodeId),
            .kind = PendingQueueKind::List, .dispatches = std::move(dispatches),
            .maximum = 1024, .nodeId = std::string(nodeId)};

    }

    static ruvia::Task<void> loadEdgeElements(ruvia::DbTransaction& transaction,
                                               service::collector::DeviceDefinition& device) {
        using Op = ruvia::DbBinaryOperator;
        using Type = ruvia::DbDataType;
        if (device.protocol != "Modbus" && device.protocol != "S7" && device.protocol != "SL651")
            service::common::fail(18010, "边缘节点不支持该设备协议", 400);
        ruvia::DbQuery elements;
        const auto field = [&](std::string_view key) {
            return elements.binary(elements.column("item"), Op::kJsonGetText, elements.value(key));
        };
        const auto integer = [&](std::string_view key, int fallback) {
            const auto value = field(key);
            return elements.coalesce({ elements.caseWhen({ { elements.binary(elements.coalesce({ value, elements.value("") }),
                Op::kRegex, elements.value("^-?[0-9]{1,18}$")), elements.cast(value, Type::kInteger) } }), elements.value(fallback) });
        };
        const auto functionText = [&](std::string_view key) {
            return elements.binary(elements.column("func"), Op::kJsonGetText, elements.value(key));
        };
        const auto isSl651 = device.protocol == "SL651";
        const auto isS7 = device.protocol == "S7";
        const auto writable = elements.binary(elements.call("lower", { elements.coalesce({ field("writable"), elements.value("") }) }), Op::kIn,
            elements.list({ elements.value("true"), elements.value("t"), elements.value("1"), elements.value("yes"), elements.value("y"), elements.value("on") }));
        elements.select({ field("id"), field("name"), elements.coalesce({ field("unit"), elements.value("") }),
                isSl651 ? elements.value("") : isS7 ? elements.coalesce({ field("dataType"), elements.value("BOOL") }) : field("dataType"),
                isSl651 ? functionText("dir") : elements.value(""),
                isSl651 ? integer("length", 1) : isS7 ? integer("size", 1) : elements.value(0),
                isSl651 ? integer("digits", 0) : elements.value(0),
                isSl651 ? elements.binary(functionText("dir"), Op::kEqual, elements.value("DOWN")) : writable,
                isSl651 ? elements.column("response_element") : elements.cast(elements.value(false), Type::kBoolean),
                isSl651 ? functionText("funcCode") : elements.value(""), isSl651 ? field("encode") : elements.value("") })
            .from("device", "d")
            .join(ruvia::DbJoinType::kInner, "link", elements.binary(elements.column("id", "l"), Op::kEqual, elements.column("link_id", "d")), "l")
            .join(ruvia::DbJoinType::kInner, "device_model", elements.binary(elements.column("device_id", "p"), Op::kEqual, elements.column("id", "d")), "p")
            .joinFunction(ruvia::DbJoinType::kCross, elements.call("jsonb_array_elements", { elements.coalesce({
                elements.binary(elements.column("config", "p"), Op::kJsonGet, elements.value(isSl651 ? "funcs" : isS7 ? "areas" : "registers")),
                elements.cast(elements.value("[]"), Type::kJsonb) }) }), {}, isSl651 ? "func" : "item", { .lateral = true })
            .andWhere(elements.binary(elements.column("id", "d"), Op::kEqual, elements.cast(elements.value(device.id), Type::kUuid)))
            .andWhere(elements.binary(elements.column("protocol", "p"), Op::kEqual, elements.value(device.protocol)))
            .andWhere(elements.binary(elements.column("execution", "l"), Op::kEqual, elements.value("edge")))
            .andWhere(elements.unary(ruvia::DbUnaryOperator::kIsNull, elements.column("deleted_at", "l")));
        if (isSl651) {
            const auto functionFields = [](std::string_view key, bool response) {
                ruvia::DbQuery query;
                query.select({ query.alias(query.column("value"), "item"), query.alias(query.cast(query.value(response), Type::kBoolean), "response_element") })
                    .fromFunction(query.call("jsonb_array_elements", { query.coalesce({
                        query.binary(query.column("func"), Op::kJsonGet, query.value(key)), query.cast(query.value("[]"), Type::kJsonb) }) }), "field");
                return query;
            };
            auto fields = functionFields("elements", false);
            const auto response = functionFields("responseElements", true);
            fields.combine(ruvia::DbSetOperation::kUnionAll, response);
            elements.join(ruvia::DbJoinType::kCross, fields, {}, "configured", { .lateral = true });
        }
        const auto rows = co_await transaction.query(elements);
        for (const auto& row : rows) {
            service::collector::ElementDefinition element;
            element.id = std::string(row[0].value().value_or(std::string_view{}));
            element.name = std::string(row[1].value().value_or(std::string_view{}));
            element.unit = std::string(row[2].value().value_or(std::string_view{}));
            element.dataType = std::string(row[3].value().value_or(std::string_view{}));
            element.direction = std::string(row[4].value().value_or(std::string_view{}));
            element.size = parseInteger(row[5].value().value_or(std::string_view{}));
            element.length = parseInteger(row[5].value().value_or(std::string_view{}));
            element.digits = parseInteger(row[6].value().value_or(std::string_view{}));
            element.writable = row[7].value().value_or(std::string_view{}) == "t";
            element.responseElement = row[8].value().value_or(std::string_view{}) == "t";
            element.functionCode = std::string(row[9].value().value_or(std::string_view{}));
            element.encoding = std::string(row[10].value().value_or(std::string_view{}));
            device.elements.push_back(std::move(element));
        }
    }

    static std::int64_t parseInteger(std::string_view value) {
        std::int64_t output{};
        const auto [end, error] =
            std::from_chars(value.data(), value.data() + value.size(), output);
        return error == std::errc{} && end == value.data() + value.size() ? output : 0;
    }

    static bool setUuid(std::string* output, std::string_view text) {
        std::uint8_t value[16]{};
        if (!service::edge::protocol::uuidBytes(text, value))
            return false;
        output->assign(service::edge::protocol::bytes(value, sizeof(value)));
        return true;
    }



    static std::uint32_t boundedUnsigned(std::string_view value, std::uint32_t fallback,
                                         std::uint32_t minimum) {
        std::uint32_t result{};
        const auto [end, error] =
            std::from_chars(value.data(), value.data() + value.size(), result);
        if (error != std::errc{} || end != value.data() + value.size() ||
            result < minimum || result > 3600U)
            return fallback;
        return result;
    }


};

} // namespace service::command
