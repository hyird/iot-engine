#pragma once

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory_resource>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/web/db/Db.h>
#include <ruvia/web/db/DbQuery.h>
#include <ruvia/core/Timer.h>

#include "service/common/http.h"
#include "service/common/uuid.h"
#include "service/common/message.h"
#include "service/modules/command/command.types.h"
#include "service/middleware/rpc.h"
#include "service/modules/device/device.service.h"
#include "service/modules/device/device.types.h"
#include "service/utils/json.h"

namespace service::command {

class CommandService final {
  public:
    explicit CommandService(service::device::DeviceAccessService& accessService)
        : accessService_(accessService) {}

    ruvia::Task<service::device::DeviceCommandCreateDto>
    create(ruvia::Context& context, std::string_view deviceId,
           const service::device::DeviceCommandBody& body) {
        const auto access = co_await accessService_.require(
            context, deviceId, service::device::DeviceAccessLevel::operate);
        const auto deviceRows = co_await context.db().query(
            deviceRemoteControlQuery(context.pool(), deviceId));
        if (deviceRows.empty())
            service::common::fail(18001, "设备不存在", 404);
        const auto capabilities = service::device::DeviceAccessService::capabilities(
            access.actor, access.level, deviceRows.front()[0].value().value_or(std::string_view{}) == "t");
        if (!capabilities.canCommand)
            service::common::fail(18005, "设备未开启远程控制或当前账号无下发权限", 403);

        co_return co_await enqueueDevice(context, deviceId, body, access.actor.userId);
    }

    ruvia::Task<service::device::DeviceCommandCreateDto>
    createExternal(ruvia::Context& context, std::string_view deviceId,
                   const service::device::DeviceCommandBody& body, std::string_view accessKeyId) {
        const auto deviceRows = co_await context.db().query(
            deviceRemoteControlQuery(context.pool(), deviceId));
        if (deviceRows.empty())
            service::common::fail(18001, "设备不存在", 404);
        if (deviceRows.front()[0].value().value_or(std::string_view{}) != "t")
            service::common::fail(18005, "设备未开启远程控制", 403);
        co_return co_await enqueueDevice(context, deviceId, body,
                                         "access-key:" + std::string(accessKeyId));
    }

    ruvia::Task<service::device::DeviceCommandStatusDto> status(ruvia::Context& context,
                                                                std::string_view commandId) {
        const auto fields =
            co_await loadStatus(context, commandId);
        if (fields.empty())
            service::common::fail(18012, "下发记录不存在", 404);
        const auto deviceId = field(fields, "device_id");
        if (deviceId.empty())
            service::common::fail(18012, "下发状态数据无效", 500);
        (void)co_await accessService_.require(
            context, deviceId, service::device::DeviceAccessLevel::operate);

        service::device::DeviceCommandStatusDto result(ruvia::ModelOptions{.resource = context.arena()});
        fillStatus(result, commandId, fields);
        co_return result;
    }

    ruvia::Task<service::device::DeviceCommandStatusesDto>
    statuses(ruvia::Context& context) {
        const auto ids = context.req().query("ids").value_or("");
        if (ids.empty() || ids.size() > 256 * 37)
            service::common::fail(18012, "Provide between 1 and 256 command IDs", 400);
        ruvia::BoxedArray<service::device::DeviceCommandStatusDto> statuses(
            ruvia::ModelOptions{.resource = context.arena()});
        bool complete = true;
        std::size_t offset = 0, count = 0;
        for (;;) {
            const auto end = ids.find(',', offset);
            const auto commandId = ids.substr(offset, end == std::string_view::npos ? end : end - offset);
            if (!service::common::isUuid(commandId) || ++count > 256)
                service::common::fail(18012, "Invalid command ID list", 400);
            auto result = co_await status(context, commandId);
            complete = complete && terminalState(result.get<"status">()->view());
            statuses.emplace(std::move(result));
            if (end == std::string_view::npos) break;
            offset = end + 1;
        }
        service::device::DeviceCommandStatusesDto result(ruvia::ModelOptions{.resource = context.arena()});
        result.set<"complete">(complete).set<"statuses">(std::move(statuses));
        co_return result;
    }

  private:
    static ruvia::DbQuery deviceRemoteControlQuery(std::pmr::memory_resource* resource,
                                                   std::string_view deviceId) {
        ruvia::DbQuery query(resource);
        const auto protocolParams = query.column("protocol_params");
        const auto remoteControlKey = query.cast(
            query.value("remote_control"), ruvia::DbDataType::kText);
        const auto hasRemoteControl = query.binary(
            protocolParams, ruvia::DbBinaryOperator::kJsonHasKey,
            remoteControlKey);
        const auto remoteControl = query.binary(
            protocolParams, ruvia::DbBinaryOperator::kJsonGetText,
            remoteControlKey);
        const auto normalizedRemoteControl = query.call(
            "lower", {query.coalesce({remoteControl, query.value("")})});
        const auto enabled = query.caseWhen(
            {{query.binary(normalizedRemoteControl, ruvia::DbBinaryOperator::kEqual,
                          query.value("true")),
              query.cast(query.value(true), ruvia::DbDataType::kBoolean)},
             {query.binary(normalizedRemoteControl, ruvia::DbBinaryOperator::kEqual,
                          query.value("t")),
              query.cast(query.value(true), ruvia::DbDataType::kBoolean)},
             {query.binary(normalizedRemoteControl, ruvia::DbBinaryOperator::kEqual,
                          query.value("1")),
              query.cast(query.value(true), ruvia::DbDataType::kBoolean)},
             {query.binary(normalizedRemoteControl, ruvia::DbBinaryOperator::kEqual,
                          query.value("yes")),
              query.cast(query.value(true), ruvia::DbDataType::kBoolean)},
             {query.binary(normalizedRemoteControl, ruvia::DbBinaryOperator::kEqual,
                          query.value("y")),
              query.cast(query.value(true), ruvia::DbDataType::kBoolean)},
             {query.binary(normalizedRemoteControl, ruvia::DbBinaryOperator::kEqual,
                          query.value("on")),
              query.cast(query.value(true), ruvia::DbDataType::kBoolean)}},
            query.cast(query.value(false), ruvia::DbDataType::kBoolean));
        query
            .select(query.caseWhen({{hasRemoteControl, enabled}}, query.cast(query.value(true), ruvia::DbDataType::kBoolean)))
            .from("device")
            .where(query.binary(query.column("id"), ruvia::DbBinaryOperator::kEqual,
                                query.cast(query.value(deviceId), ruvia::DbDataType::kUuid)))
            .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull,
                                  query.column("deleted_at")))
            .limit(1);
        return query;
    }

    static std::string actualValueField(std::size_t index, std::string_view name) {
        return "actual_value_" + std::to_string(index) + "_" + std::string(name);
    }

    static void fillStatus(service::device::DeviceCommandStatusDto& result,
                           std::string_view commandId,
                           const std::vector<message::StreamField>& fields) {
        result.set<"commandId">(commandId)
            .set<"deviceId">(field(fields, "device_id"))
            .set<"deviceCode">(field(fields, "device_code"))
            .set<"protocol">(field(fields, "protocol"))
            .set<"status">(field(fields, "status"));
        const auto reason = field(fields, "reason");
        if (!reason.empty())
            result.set<"reason">(reason);
        const auto createdAt = integer(fields, "created_at_ms");
        if (createdAt != 0)
            result.set<"createdAtMs">(createdAt);
        const auto completedAt = integer(fields, "completed_at_ms");
        if (completedAt != 0)
            result.set<"completedAtMs">(completedAt);
        const auto actualCount =
            std::clamp<std::int64_t>(integer(fields, "actual_value_count"), 0, 8);
        if (actualCount != 0) {
            ruvia::BoxedArray<service::device::DeviceCommandActualValueDto> actualValues(
                ruvia::ModelOptions{.resource = result.resource()});
            for (std::int64_t index = 0; index < actualCount; ++index) {
                auto& actual = actualValues.emplace();
                actual.set<"elementId">(
                          field(fields, actualValueField(index, "element_id")))
                    .set<"name">(field(fields, actualValueField(index, "name")))
                    .set<"kind">(field(fields, actualValueField(index, "kind")))
                    .set<"value">(field(fields, actualValueField(index, "value")))
                    .set<"unit">(field(fields, actualValueField(index, "unit")));
            }
            result.set<"actualValues">(std::move(actualValues));
        }
    }

    ruvia::Task<service::device::DeviceCommandCreateDto>
    enqueueDevice(ruvia::Context& context, std::string_view deviceId,
                  const service::device::DeviceCommandBody& body, std::string submittedBy) {
        const auto& keyField = body.get<"idempotencyKey">();
        if (!keyField || !service::common::isUuid(keyField->view()))
            service::common::fail(18010, "idempotency_key 必须是 UUID", 400);
        const std::string key(keyField->view());
        const auto elements = normalize(body);
        std::string payload = "[";
        for (const auto& [elementId, value] : elements) {
            if (payload.size() > 1) payload += ',';
            payload += "[" + service::utils::jsonQuoted(elementId) + "," +
                       service::utils::jsonQuoted(value) + "]";
        }
        payload += ']';
        auto transaction = co_await context.db().beginTransaction();
        const std::string lockKey = submittedBy + ":" + key;
        ruvia::DbQuery advisory(context.pool());
        advisory.select(advisory.call(
            "pg_advisory_xact_lock",
            {advisory.call("hashtextextended", {advisory.value(lockKey), advisory.value(std::int64_t{0})})}));
        (void)co_await transaction.query(advisory);

        ruvia::DbQuery priorQuery(context.pool());
        const auto priorDevice = priorQuery.binary(
            priorQuery.column("device_id"), ruvia::DbBinaryOperator::kEqual,
            priorQuery.cast(priorQuery.value(deviceId), ruvia::DbDataType::kUuid));
        const auto priorPayload = priorQuery.binary(
            priorQuery.column("payload"), ruvia::DbBinaryOperator::kEqual,
            priorQuery.cast(priorQuery.value(payload), ruvia::DbDataType::kJsonb));
        priorQuery
            .select({priorQuery.cast(priorQuery.column("id"), ruvia::DbDataType::kText),
                     priorQuery.binary(priorDevice, ruvia::DbBinaryOperator::kAnd, priorPayload)})
            .from("command_request")
            .where(priorQuery.binary(
                priorQuery.binary(priorQuery.column("actor"), ruvia::DbBinaryOperator::kEqual,
                                  priorQuery.value(submittedBy)),
                ruvia::DbBinaryOperator::kAnd,
                priorQuery.binary(priorQuery.column("idempotency_key"),
                                  ruvia::DbBinaryOperator::kEqual,
                                  priorQuery.cast(priorQuery.value(key), ruvia::DbDataType::kUuid))));
        const auto prior = co_await transaction.query(priorQuery);
        if (!prior.empty()) {
            if (prior.front()[1].value().value_or(std::string_view{}) != "t")
                common::fail(18014, "幂等键已用于不同的指令请求", 409);
            ruvia::DbQuery commandQuery(context.pool());
            commandQuery
                .select(commandQuery.cast(commandQuery.column("id"), ruvia::DbDataType::kText))
                .from("command_operation")
                .where(commandQuery.binary(
                    commandQuery.column("request_id"), ruvia::DbBinaryOperator::kEqual,
                    commandQuery.cast(
                        commandQuery.value(prior.front()[0].value().value_or(std::string_view{})),
                        ruvia::DbDataType::kUuid)))
                .orderBy(commandQuery.column("ordinal"));
            const auto commands = co_await transaction.query(commandQuery);
            ruvia::BoxedArray<ruvia::String> ids(ruvia::ModelOptions{.resource=context.arena()});
            for (const auto& row : commands)
                ids.emplace(row[0].value().value_or(std::string_view{}),
                            ruvia::ModelOptions{.resource=context.arena()});
            co_await transaction.commit();
            service::device::DeviceCommandCreateDto result(ruvia::ModelOptions{.resource = context.arena()});
            result.set<"commandIds">(std::move(ids)).set<"status">("ACCEPTED");
            co_return result;
        }
        const auto requestId = common::nextUuidV7();
        ruvia::DbQuery requestQuery(context.pool());
        requestQuery
            .insertInto("command_request", {"id", "actor", "idempotency_key", "device_id", "payload"})
            .values({requestQuery.cast(requestQuery.value(requestId), ruvia::DbDataType::kUuid),
                     requestQuery.value(submittedBy),
                     requestQuery.cast(requestQuery.value(key), ruvia::DbDataType::kUuid),
                     requestQuery.cast(requestQuery.value(deviceId), ruvia::DbDataType::kUuid),
                     requestQuery.cast(requestQuery.value(payload), ruvia::DbDataType::kJsonb)});
        (void)co_await transaction.execute(requestQuery);

        ruvia::DbQuery deviceLock(context.pool());
        deviceLock
            .select(deviceLock.column("id"))
            .from("device")
            .where(deviceLock.binary(deviceLock.column("id"), ruvia::DbBinaryOperator::kEqual,
                                     deviceLock.cast(deviceLock.value(deviceId),
                                                     ruvia::DbDataType::kUuid)))
            .lock({.mode = ruvia::DbRowLock::kShare});
        (void)co_await transaction.query(deviceLock);
        const auto prepared = co_await service::rpc::call(context, "command", "prepare",
            "{\"deviceId\":" + service::utils::jsonQuoted(deviceId) + ",\"elements\":" + payload + "}");
        auto result = co_await appendPrepared(context, transaction, requestId, deviceId, submittedBy, prepared);
        co_await transaction.commit();
        co_return result;
    }


    static ruvia::Task<service::device::DeviceCommandCreateDto> appendPrepared(
        ruvia::Context& context, ruvia::DbTransaction& transaction,
        std::string_view requestId, std::string_view deviceId,
        std::string_view submittedBy, std::string_view preparedJson) {
        const auto batch = ruvia::fromJson<PreparedBatch>(preparedJson, {.resource = context.arena()});
        if (!batch || !batch->get<"queue">() || !batch->get<"kind">() ||
            !batch->get<"maximum">() || !batch->get<"nodeId">() || !batch->get<"commands">() ||
            batch->get<"commands">()->empty() || batch->get<"commands">()->size() > 256)
            common::fail(10004, "Invalid prepared command batch", 502);
        const auto kind = batch->get<"kind">()->view();
        const auto maximum = static_cast<std::int64_t>(*batch->get<"maximum">());
        if ((kind != "list" && kind != "stream") || maximum <= 0 || maximum > 10000)
            common::fail(10004, "Invalid prepared command queue", 502);
        ruvia::BoxedArray<ruvia::String> commandIds(ruvia::ModelOptions{.resource = context.arena()});
        std::int64_t ordinal = 0;
        for (const auto& command : *batch->get<"commands">()) {
            if (!command.get<"id">() || !common::isUuid(command.get<"id">()->view()) ||
                !command.get<"deviceId">() || command.get<"deviceId">()->view() != deviceId ||
                !command.get<"deviceCode">() || !command.get<"protocol">() ||
                !command.get<"elements">() || !command.get<"payload">())
                common::fail(10004, "Invalid prepared command", 502);
            std::string elements = "[";
            for (const auto& element : *command.get<"elements">()) {
                if (!element.get<"elementId">() || !element.get<"value">())
                    common::fail(10004, "Invalid prepared command element", 502);
                if (elements.size() > 1) elements += ',';
                elements += "{\"elementId\":" + service::utils::jsonQuoted(element.get<"elementId">()->view()) +
                    ",\"value\":" + service::utils::jsonQuoted(element.get<"value">()->view()) + "}";
            }
            elements += ']';
            std::string payload = "[";
            for (const auto& value : *command.get<"payload">()) {
                if (payload.size() > 1) payload += ',';
                payload += service::utils::jsonQuoted(value.view());
            }
            payload += ']';
            const auto id = command.get<"id">()->view();
            const auto operationOrdinal = ordinal++;
            ruvia::DbQuery operationSource(context.pool());
            operationSource
                .select({operationSource.cast(operationSource.value(id), ruvia::DbDataType::kUuid),
                         operationSource.cast(operationSource.value(requestId),
                                              ruvia::DbDataType::kUuid),
                         operationSource.value(static_cast<std::int32_t>(operationOrdinal)),
                         operationSource.cast(operationSource.value(deviceId),
                                              ruvia::DbDataType::kUuid),
                         operationSource.value(command.get<"deviceCode">()->view()),
                         operationSource.value(command.get<"protocol">()->view()),
                         operationSource.value("ACCEPTED"),
                         operationSource.cast(operationSource.value(elements),
                                              ruvia::DbDataType::kJsonb),
                         operationSource.column("protocol_config_id"),
                         operationSource.column("protocol_revision")})
                .from("device")
                .where(operationSource.binary(
                    operationSource.column("id"), ruvia::DbBinaryOperator::kEqual,
                    operationSource.cast(operationSource.value(deviceId),
                                         ruvia::DbDataType::kUuid)));
            ruvia::DbQuery operationQuery(context.pool());
            operationQuery
                .insertInto("command_operation",
                            {"id", "request_id", "ordinal", "device_id", "device_code",
                             "protocol", "status", "elements", "model_id", "model_revision"})
                .insertFrom(operationSource);
            (void)co_await transaction.execute(operationQuery);

            ruvia::DbQuery attemptQuery(context.pool());
            attemptQuery
                .insertInto("command_attempt",
                            {"operation_id", "queue_key", "queue_kind", "payload", "submitted_by",
                             "node_id", "max_length"})
                .values({attemptQuery.cast(attemptQuery.value(id), ruvia::DbDataType::kUuid),
                         attemptQuery.value(batch->get<"queue">()->view()),
                         attemptQuery.value(kind),
                         attemptQuery.cast(attemptQuery.value(payload), ruvia::DbDataType::kJsonb),
                         attemptQuery.value(submittedBy),
                         attemptQuery.value(batch->get<"nodeId">()->view()),
                         attemptQuery.value(static_cast<std::int32_t>(maximum))});
            (void)co_await transaction.execute(attemptQuery);
            const auto eventId = common::nextUuidV7();
            ruvia::DbQuery eventSource(context.pool());
            const auto jsonKey = [&](std::string_view key) {
                return eventSource.cast(eventSource.value(key), ruvia::DbDataType::kText);
            };
            eventSource
                .select({eventSource.cast(eventSource.value(eventId), ruvia::DbDataType::kUuid),
                         eventSource.value("device.command.accepted"),
                         eventSource.value("command"),
                         eventSource.cast(eventSource.column("device_id"),
                                          ruvia::DbDataType::kText),
                         eventSource.value("updated"), eventSource.value(std::int32_t{2}),
                         eventSource.call(
                             "jsonb_build_object",
                             {jsonKey("device_code"), eventSource.column("device_code"),
                              jsonKey("data"),
                              eventSource.call(
                                  "jsonb_build_object",
                                  {jsonKey("commandId"),
                                   eventSource.cast(eventSource.column("id"),
                                                    ruvia::DbDataType::kText),
                                   jsonKey("status"), eventSource.column("status"),
                                   jsonKey("reason"), eventSource.column("reason"),
                                   jsonKey("elements"), eventSource.column("elements"),
                                   jsonKey("actualValues"),
                                   eventSource.column("actual_values")})})})
                .from("command_operation")
                .where(eventSource.binary(
                    eventSource.column("id"), ruvia::DbBinaryOperator::kEqual,
                    eventSource.cast(eventSource.value(id), ruvia::DbDataType::kUuid)));
            ruvia::DbQuery eventQuery(context.pool());
            eventQuery
                .insertInto("outbox_event",
                            {"id", "event_type", "aggregate_type", "aggregate_id", "action",
                             "schema_version", "payload"})
                .insertFrom(eventSource);
            (void)co_await transaction.execute(eventQuery);
            commandIds.emplace(id, ruvia::ModelOptions{.resource = context.arena()});
        }
        service::device::DeviceCommandCreateDto result(ruvia::ModelOptions{.resource = context.arena()});
        result.set<"commandIds">(std::move(commandIds)).set<"status">("ACCEPTED");
        co_return result;
    }

    static std::string_view field(const std::vector<message::StreamField>& fields,
                                   std::string_view name) {
        for (const auto& value : fields) if (value.name == name) return value.value;
        return {};
    }

    static bool terminalState(std::string_view state) {
        return state == "SUCCEEDED" || state == "REJECTED" || state == "UNKNOWN" ||
               state == "READBACK_MISMATCH" || state == "FAILED";
    }

static ruvia::Task<std::vector<message::StreamField>> loadStatus(ruvia::Context& context,
                                                     std::string_view id) {
    ruvia::DbQuery statusQuery(context.pool());
    const auto createdAtMs = statusQuery.cast(
        statusQuery.binary(statusQuery.extract(ruvia::DbDatePart::kEpoch,
                                                statusQuery.column("created_at")),
                           ruvia::DbBinaryOperator::kMultiply, statusQuery.value(std::int64_t{1000})),
        ruvia::DbDataType::kBigInt);
    const auto completedAtMs = statusQuery.cast(
        statusQuery.binary(statusQuery.extract(ruvia::DbDatePart::kEpoch,
                                                statusQuery.column("completed_at")),
                           ruvia::DbBinaryOperator::kMultiply, statusQuery.value(std::int64_t{1000})),
        ruvia::DbDataType::kBigInt);
    statusQuery
        .select({statusQuery.cast(statusQuery.column("device_id"), ruvia::DbDataType::kText),
                 statusQuery.column("device_code"), statusQuery.column("protocol"),
                 statusQuery.column("status"), statusQuery.column("reason"),
                 statusQuery.cast(createdAtMs, ruvia::DbDataType::kText),
                 statusQuery.coalesce({statusQuery.cast(completedAtMs, ruvia::DbDataType::kText),
                                       statusQuery.value("0")})})
        .from("command_operation")
        .where(statusQuery.binary(statusQuery.column("id"), ruvia::DbBinaryOperator::kEqual,
                                  statusQuery.cast(statusQuery.value(id), ruvia::DbDataType::kUuid)));
    const auto rows = co_await context.db().query(statusQuery);
    std::vector<message::StreamField> fields;
    if (rows.empty()) co_return fields;
    const std::string_view names[]{"device_id","device_code","protocol","status",
                                   "reason","created_at_ms","completed_at_ms"};
    for (std::size_t index = 0; index < 7; ++index)
        fields.push_back({std::string(names[index]),
                          std::string(rows.front()[index].value().value_or(std::string_view{}))});
    ruvia::DbQuery actualQuery(context.pool());
    const auto jsonKey = [&](std::string_view key) {
        return actualQuery.cast(actualQuery.value(key), ruvia::DbDataType::kText);
    };
    actualQuery
        .select({actualQuery.binary(actualQuery.column("value", "a"),
                                    ruvia::DbBinaryOperator::kJsonGetText,
                                    jsonKey("elementId")),
                 actualQuery.binary(actualQuery.column("value", "a"),
                                    ruvia::DbBinaryOperator::kJsonGetText,
                                    jsonKey("name")),
                 actualQuery.binary(actualQuery.column("value", "a"),
                                    ruvia::DbBinaryOperator::kJsonGetText,
                                    jsonKey("kind")),
                 actualQuery.binary(actualQuery.column("value", "a"),
                                    ruvia::DbBinaryOperator::kJsonGetText,
                                    jsonKey("value")),
                 actualQuery.binary(actualQuery.column("value", "a"),
                                    ruvia::DbBinaryOperator::kJsonGetText,
                                    jsonKey("unit"))})
        .from("command_operation", "operation")
        .joinFunction(
            ruvia::DbJoinType::kCross,
            actualQuery.call("jsonb_array_elements",
                             {actualQuery.column("actual_values", "operation")}),
            {}, "a", {.lateral = true, .withOrdinality = true,
                        .columns = {{.name = "value"}, {.name = "idx"}}})
        .where(actualQuery.binary(actualQuery.column("id", "operation"),
                                  ruvia::DbBinaryOperator::kEqual,
                                  actualQuery.cast(actualQuery.value(id),
                                                   ruvia::DbDataType::kUuid)))
        .orderBy(actualQuery.column("idx", "a"));
    const auto actual = co_await context.db().query(actualQuery);
    fields.push_back({"actual_value_count",std::to_string(actual.size())});
    const std::string_view actualNames[]{"element_id","name","kind","value","unit"};
    for (std::size_t index = 0; index < actual.size(); ++index)
        for (std::size_t col = 0; col < 5; ++col)
            fields.push_back({"actual_value_" + std::to_string(index) + "_" +
                              std::string(actualNames[col]),
                              std::string(actual[index][col].value().value_or(std::string_view{}))});
    co_return fields;
}



    static std::vector<std::pair<std::string, std::string>>
    normalize(const service::device::DeviceCommandBody& body) {
        if (!body.get<"elements">() || body.get<"elements">()->empty() || body.get<"elements">()->size() > 256)
            service::common::fail(18010, "下发要素数量必须在 1 - 256 之间", 400);
        std::vector<std::pair<std::string, std::string>> result;
        std::set<std::string, std::less<>> seenElementIds;
        result.reserve(body.get<"elements">()->size());
        for (const auto& element : *body.get<"elements">()) {
            if (!element.get<"elementId">() || !element.get<"value">())
                service::common::fail(18010, "下发要素参数不完整", 400);
            const auto id = element.get<"elementId">()->view();
            const auto value = element.get<"value">()->view();
            if (!service::common::isUuid(id))
                service::common::fail(18010, "下发要素 ID 必须是 UUID", 400);
            if (!seenElementIds.emplace(id).second)
                service::common::fail(18010, "下发要素不能重复", 400);
            if (value.empty() || value.size() > 4096)
                service::common::fail(18010, "下发要素值长度必须在 1 - 4096 之间", 400);
            result.push_back({std::string(id), std::string(value)});
        }
        return result;
    }

    static std::int64_t integer(const std::vector<message::StreamField>& fields,
                                std::string_view name) {
        const auto value = field(fields, name);
        std::int64_t result = 0;
        const auto [end, error] =
            std::from_chars(value.data(), value.data() + value.size(), result);
        return error == std::errc{} && end == value.data() + value.size() ? result : 0;
    }


    service::device::DeviceAccessService& accessService_;
};

inline CommandService& commandService() {
    static CommandService service(service::device::deviceAccessService());
    return service;
}

} // namespace service::command
