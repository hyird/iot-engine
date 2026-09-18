#pragma once

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <memory_resource>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/core/Timer.h>
#include <ruvia/web/db/Db.h>
#include <ruvia/web/db/DbQuery.h>

#include "service/common/http.h"
#include "service/common/message.h"
#include "service/common/uuid.h"
#include "service/middleware/rpc.h"
#include "service/modules/command/command.entity.h"
#include "service/modules/command/command.types.h"
#include "service/modules/device/device.service.h"
#include "service/modules/device/device.types.h"
#include "service/utils/json.h"

namespace service::command {

class CommandService final {
  public:
    explicit CommandService(service::device::DeviceAccessService& accessService)
        : accessService_(accessService) {}

    template <typename Context>
    ruvia::Task<service::device::DeviceCommandCreateDto>
    create(Context& context, std::string_view deviceId, const SubmitCommandBody& body) {
        const auto access = co_await accessService_.require(
            context,
            deviceId,
            service::device::DeviceAccessLevel::operate,
            context.userId
        );
        const auto deviceRows = co_await context.db().query(
            deviceRemoteControlQuery(context.pool(), deviceId)
        );
        if (deviceRows.empty()) {
            service::common::fail(18001, "设备不存在", 404);
        }
        const auto capabilities = service::device::DeviceAccessService::capabilities(
            access.actor,
            access.level,
            deviceRows.front()[0].value().value_or(std::string_view{}) == "t"
        );
        if (!capabilities.canCommand) {
            service::common::fail(18005, "设备未开启远程控制或当前账号无下发权限", 403);
        }

        co_return co_await enqueueDevice(context, deviceId, body, access.actor.userId);
    }

    template <typename Context>
    ruvia::Task<service::device::DeviceCommandCreateDto>
    createExternal(Context& context, std::string_view deviceId, const service::device::DeviceCommandBody& body, std::string_view accessKeyId) {
        const auto deviceRows = co_await context.db().query(
            deviceRemoteControlQuery(context.pool(), deviceId)
        );
        if (deviceRows.empty()) {
            service::common::fail(18001, "设备不存在", 404);
        }
        if (deviceRows.front()[0].value().value_or(std::string_view{}) != "t") {
            service::common::fail(18005, "设备未开启远程控制", 403);
        }
        co_return co_await enqueueDevice(context, deviceId, body, "access-key:" + std::string(accessKeyId));
    }

  private:
    static ruvia::DbQuery deviceRemoteControlQuery(std::pmr::memory_resource* resource, std::string_view deviceId) {
        ruvia::DbQuery query(resource);
        const auto protocolParams = query.column(service::command::entities::DeviceEntity::columnName<"protocol_params">());
        const auto remoteControlKey = query.cast(
            query.value("remote_control"),
            ruvia::DbDataType::kText
        );
        const auto hasRemoteControl = query.binary(
            protocolParams,
            ruvia::DbBinaryOperator::kJsonHasKey,
            remoteControlKey
        );
        const auto remoteControl = query.binary(
            protocolParams,
            ruvia::DbBinaryOperator::kJsonGetText,
            remoteControlKey
        );
        const auto normalizedRemoteControl = query.call(
            "lower",
            { query.coalesce({ remoteControl, query.value("") }) }
        );
        const auto enabled = query.caseWhen(
            { { query.binary(normalizedRemoteControl, ruvia::DbBinaryOperator::kEqual, query.value("true")),
                query.cast(query.value(true), ruvia::DbDataType::kBoolean) },
              { query.binary(normalizedRemoteControl, ruvia::DbBinaryOperator::kEqual, query.value("t")),
                query.cast(query.value(true), ruvia::DbDataType::kBoolean) },
              { query.binary(normalizedRemoteControl, ruvia::DbBinaryOperator::kEqual, query.value("1")),
                query.cast(query.value(true), ruvia::DbDataType::kBoolean) },
              { query.binary(normalizedRemoteControl, ruvia::DbBinaryOperator::kEqual, query.value("yes")),
                query.cast(query.value(true), ruvia::DbDataType::kBoolean) },
              { query.binary(normalizedRemoteControl, ruvia::DbBinaryOperator::kEqual, query.value("y")),
                query.cast(query.value(true), ruvia::DbDataType::kBoolean) },
              { query.binary(normalizedRemoteControl, ruvia::DbBinaryOperator::kEqual, query.value("on")),
                query.cast(query.value(true), ruvia::DbDataType::kBoolean) } },
            query.cast(query.value(false), ruvia::DbDataType::kBoolean)
        );
        query
            .select(query.caseWhen({ { hasRemoteControl, enabled } }, query.cast(query.value(true), ruvia::DbDataType::kBoolean)))
            .from(service::command::entities::DeviceEntity::tableName())
            .where(query.binary(query.column(service::command::entities::DeviceEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual, query.cast(query.value(deviceId), ruvia::DbDataType::kUuid)))
            .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull, query.column(service::command::entities::DeviceEntity::columnName<"deleted_at">())))
            .limit(1);
        return query;
    }

    template <typename Context, typename Body>
    ruvia::Task<service::device::DeviceCommandCreateDto>
    enqueueDevice(Context& context, std::string_view deviceId, const Body& body, std::string submittedBy) {
        const auto& keyField = body.template get<"idempotencyKey">();
        if (!keyField || !service::common::isUuid(keyField->view())) {
            service::common::fail(18010, "idempotency_key 必须是 UUID", 400);
        }
        const std::string key(keyField->view());
        const auto elements = normalize(body);
        std::string payload = "[";
        for (const auto& [elementId, value] : elements) {
            if (payload.size() > 1) {
                payload += ',';
            }
            payload += "[" + service::utils::jsonQuoted(elementId) + "," +
                service::utils::jsonQuoted(value) + "]";
        }
        payload += ']';
        auto transaction = co_await context.db().beginTransaction();
        const std::string lockKey = submittedBy + ":" + key;
        ruvia::DbQuery advisory(context.pool());
        advisory.select(advisory.call(
            "pg_advisory_xact_lock",
            { advisory.call("hashtextextended", { advisory.value(lockKey), advisory.value(std::int64_t{ 0 }) }) }
        ));
        (void)co_await transaction.query(advisory);

        ruvia::DbQuery priorQuery(context.pool());
        const auto priorDevice = priorQuery.binary(
            priorQuery.column(service::command::entities::CommandRequestEntity::columnName<"device_id">()),
            ruvia::DbBinaryOperator::kEqual,
            priorQuery.cast(priorQuery.value(deviceId), ruvia::DbDataType::kUuid)
        );
        const auto priorPayload = priorQuery.binary(
            priorQuery.column(service::command::entities::CommandRequestEntity::columnName<"payload">()),
            ruvia::DbBinaryOperator::kEqual,
            priorQuery.cast(priorQuery.value(payload), ruvia::DbDataType::kJsonb)
        );
        priorQuery
            .select({ priorQuery.cast(priorQuery.column(service::command::entities::CommandRequestEntity::columnName<"id">()), ruvia::DbDataType::kText), priorQuery.binary(priorDevice, ruvia::DbBinaryOperator::kAnd, priorPayload) })
            .from(service::command::entities::CommandRequestEntity::tableName())
            .where(priorQuery.binary(
                priorQuery.binary(priorQuery.column(service::command::entities::CommandRequestEntity::columnName<"actor">()), ruvia::DbBinaryOperator::kEqual, priorQuery.value(submittedBy)),
                ruvia::DbBinaryOperator::kAnd,
                priorQuery.binary(priorQuery.column(service::command::entities::CommandRequestEntity::columnName<"idempotency_key">()), ruvia::DbBinaryOperator::kEqual, priorQuery.cast(priorQuery.value(key), ruvia::DbDataType::kUuid))
            ));
        const auto prior = co_await transaction.query(priorQuery);
        if (!prior.empty()) {
            if (prior.front()[1].value().value_or(std::string_view{}) != "t") {
                common::fail(18014, "幂等键已用于不同的指令请求", 409);
            }
            ruvia::DbQuery commandQuery(context.pool());
            commandQuery
                .select(commandQuery.cast(commandQuery.column(service::command::entities::CommandOperationEntity::columnName<"id">()), ruvia::DbDataType::kText))
                .from(service::command::entities::CommandOperationEntity::tableName())
                .where(commandQuery.binary(
                    commandQuery.column(service::command::entities::CommandOperationEntity::columnName<"request_id">()),
                    ruvia::DbBinaryOperator::kEqual,
                    commandQuery.cast(
                        commandQuery.value(prior.front()[0].value().value_or(std::string_view{})),
                        ruvia::DbDataType::kUuid
                    )
                ))
                .orderBy(commandQuery.column(service::command::entities::CommandOperationEntity::columnName<"ordinal">()));
            const auto commands = co_await transaction.query(commandQuery);
            ruvia::BoxedArray<ruvia::String> ids(ruvia::ModelOptions{ .resource = context.arena() });
            for (const auto& row : commands) {
                ids.emplace(row[0].value().value_or(std::string_view{}), ruvia::ModelOptions{ .resource = context.arena() });
            }
            co_await transaction.commit();
            service::device::DeviceCommandCreateDto result(ruvia::ModelOptions{ .resource = context.arena() });
            result.template set<"commandIds">(std::move(ids)).template set<"status">("ACCEPTED");
            co_return result;
        }
        const auto requestId = context.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next();
        ruvia::DbQuery requestQuery(context.pool());
        requestQuery
            .insertInto(service::command::entities::CommandRequestEntity::tableName(), { "id", "actor", "idempotency_key", "device_id", "payload" })
            .values({ requestQuery.cast(requestQuery.value(requestId), ruvia::DbDataType::kUuid), requestQuery.value(submittedBy), requestQuery.cast(requestQuery.value(key), ruvia::DbDataType::kUuid), requestQuery.cast(requestQuery.value(deviceId), ruvia::DbDataType::kUuid), requestQuery.cast(requestQuery.value(payload), ruvia::DbDataType::kJsonb) });
        (void)co_await transaction.execute(requestQuery);

        ruvia::DbQuery deviceLock(context.pool());
        deviceLock
            .select(deviceLock.column(service::command::entities::DeviceEntity::columnName<"id">()))
            .from(service::command::entities::DeviceEntity::tableName())
            .where(deviceLock.binary(deviceLock.column(service::command::entities::DeviceEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual, deviceLock.cast(deviceLock.value(deviceId), ruvia::DbDataType::kUuid)))
            .lock({ .mode = ruvia::DbRowLock::kShare });
        (void)co_await transaction.query(deviceLock);
        const auto prepared = co_await service::rpc::call(context, "command", "prepare", "{\"deviceId\":" + service::utils::jsonQuoted(deviceId) + ",\"elements\":" + payload + "}");
        auto result = co_await appendPrepared(context, transaction, requestId, deviceId, submittedBy, prepared);
        co_await transaction.commit();
        co_return result;
    }

    template <typename Context>
    static ruvia::Task<service::device::DeviceCommandCreateDto> appendPrepared(
        Context& context,
        ruvia::DbTransaction& transaction,
        std::string_view requestId,
        std::string_view deviceId,
        std::string_view submittedBy,
        std::string_view preparedJson
    ) {
        const auto batch = ruvia::fromJson<PreparedBatch>(preparedJson, { .resource = context.arena() });
        if (!batch || !batch->template get<"queue">() || !batch->template get<"kind">() ||
            !batch->template get<"maximum">() || !batch->template get<"nodeId">() || !batch->template get<"commands">() ||
            batch->template get<"commands">()->empty() || batch->template get<"commands">()->size() > 256) {
            common::fail(10004, "Invalid prepared command batch", 502);
        }
        const auto kind = batch->template get<"kind">()->view();
        const auto maximum = static_cast<std::int64_t>(*batch->template get<"maximum">());
        if ((kind != "list" && kind != "stream") || maximum <= 0 || maximum > 10000) {
            common::fail(10004, "Invalid prepared command queue", 502);
        }
        ruvia::BoxedArray<ruvia::String> commandIds(ruvia::ModelOptions{ .resource = context.arena() });
        std::int64_t ordinal = 0;
        for (const auto& command : *batch->template get<"commands">()) {
            if (!command.template get<"id">() || !common::isUuid(command.template get<"id">()->view()) ||
                !command.template get<"deviceId">() || command.template get<"deviceId">()->view() != deviceId ||
                !command.template get<"deviceCode">() || !command.template get<"protocol">() ||
                !command.template get<"elements">() || !command.template get<"payload">()) {
                common::fail(10004, "Invalid prepared command", 502);
            }
            std::string elements = "[";
            for (const auto& element : *command.template get<"elements">()) {
                if (!element.template get<"elementId">() || !element.template get<"value">()) {
                    common::fail(10004, "Invalid prepared command element", 502);
                }
                if (elements.size() > 1) {
                    elements += ',';
                }
                elements += "{\"elementId\":" + service::utils::jsonQuoted(element.template get<"elementId">()->view()) +
                    ",\"value\":" + service::utils::jsonQuoted(element.template get<"value">()->view()) + "}";
            }
            elements += ']';
            std::string payload = "[";
            for (const auto& value : *command.template get<"payload">()) {
                if (payload.size() > 1) {
                    payload += ',';
                }
                payload += service::utils::jsonQuoted(value.view());
            }
            payload += ']';
            const auto id = command.template get<"id">()->view();
            const auto operationOrdinal = ordinal++;
            ruvia::DbQuery operationSource(context.pool());
            operationSource
                .select({ operationSource.cast(operationSource.value(id), ruvia::DbDataType::kUuid), operationSource.cast(operationSource.value(requestId), ruvia::DbDataType::kUuid), operationSource.value(static_cast<std::int32_t>(operationOrdinal)), operationSource.cast(operationSource.value(deviceId), ruvia::DbDataType::kUuid), operationSource.value(command.template get<"deviceCode">()->view()), operationSource.value(command.template get<"protocol">()->view()), operationSource.value("ACCEPTED"), operationSource.cast(operationSource.value(elements), ruvia::DbDataType::kJsonb), operationSource.column(service::command::entities::DeviceEntity::columnName<"protocol_config_id">()) })
                .from(service::command::entities::DeviceEntity::tableName())
                .where(operationSource.binary(
                    operationSource.column(service::command::entities::DeviceEntity::columnName<"id">()),
                    ruvia::DbBinaryOperator::kEqual,
                    operationSource.cast(operationSource.value(deviceId), ruvia::DbDataType::kUuid)
                ));
            ruvia::DbQuery operationQuery(context.pool());
            operationQuery
                .insertInto(service::command::entities::CommandOperationEntity::tableName(), { "id", "request_id", "ordinal", "device_id", "device_code", "protocol", "status", "elements", "model_id" })
                .insertFrom(operationSource);
            (void)co_await transaction.execute(operationQuery);

            ruvia::DbQuery attemptQuery(context.pool());
            attemptQuery
                .insertInto(service::command::entities::CommandAttemptEntity::tableName(), { "operation_id", "queue_key", "queue_kind", "payload", "submitted_by", "node_id", "max_length" })
                .values({ attemptQuery.cast(attemptQuery.value(id), ruvia::DbDataType::kUuid), attemptQuery.value(batch->template get<"queue">()->view()), attemptQuery.value(kind), attemptQuery.cast(attemptQuery.value(payload), ruvia::DbDataType::kJsonb), attemptQuery.value(submittedBy), attemptQuery.value(batch->template get<"nodeId">()->view()), attemptQuery.value(static_cast<std::int32_t>(maximum)) });
            (void)co_await transaction.execute(attemptQuery);
            const auto eventId = context.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next();
            ruvia::DbQuery eventSource(context.pool());
            const auto jsonKey = [&](std::string_view key) {
                return eventSource.cast(eventSource.value(key), ruvia::DbDataType::kText);
            };
            eventSource
                .select({ eventSource.cast(eventSource.value(eventId), ruvia::DbDataType::kUuid), eventSource.value("device.command.accepted"), eventSource.value("command"), eventSource.cast(eventSource.column(service::command::entities::CommandOperationEntity::columnName<"device_id">()), ruvia::DbDataType::kText), eventSource.value("updated"), eventSource.value(std::int32_t{ 2 }), eventSource.call("jsonb_build_object", { jsonKey("device_code"), eventSource.column(service::command::entities::CommandOperationEntity::columnName<"device_code">()), jsonKey("data"), eventSource.call("jsonb_build_object", { jsonKey("commandId"), eventSource.cast(eventSource.column(service::command::entities::CommandOperationEntity::columnName<"id">()), ruvia::DbDataType::kText), jsonKey("status"), eventSource.column(service::command::entities::CommandOperationEntity::columnName<"status">()), jsonKey("reason"), eventSource.column(service::command::entities::CommandOperationEntity::columnName<"reason">()), jsonKey("elements"), eventSource.column(service::command::entities::CommandOperationEntity::columnName<"elements">()), jsonKey("actualValues"), eventSource.column(service::command::entities::CommandOperationEntity::columnName<"actual_values">()) }) }) })
                .from(service::command::entities::CommandOperationEntity::tableName())
                .where(eventSource.binary(
                    eventSource.column(service::command::entities::CommandOperationEntity::columnName<"id">()),
                    ruvia::DbBinaryOperator::kEqual,
                    eventSource.cast(eventSource.value(id), ruvia::DbDataType::kUuid)
                ));
            ruvia::DbQuery eventQuery(context.pool());
            eventQuery
                .insertInto(service::command::entities::OutboxEventEntity::tableName(), { "id", "event_type", "aggregate_type", "aggregate_id", "action", "schema_version", "payload" })
                .insertFrom(eventSource);
            (void)co_await transaction.execute(eventQuery);
            commandIds.emplace(id, ruvia::ModelOptions{ .resource = context.arena() });
        }
        service::device::DeviceCommandCreateDto result(ruvia::ModelOptions{ .resource = context.arena() });
        result.template set<"commandIds">(std::move(commandIds)).template set<"status">("ACCEPTED");
        co_return result;
    }

    template <typename Body>
    static std::vector<std::pair<std::string, std::string>> normalize(const Body& body) {
        const auto* elements = [&]() -> const ruvia::Array<service::device::DeviceCommandElementBody>* {
            if constexpr (std::is_same_v<Body, SubmitCommandBody>) {
                return &body.template get<"elements">();
            } else {
                const auto& values = body.template get<"elements">();
                return values ? &*values : nullptr;
            }
        }();
        if (!elements || elements->empty() || elements->size() > 256) {
            service::common::fail(18010, "下发要素数量必须在 1 - 256 之间", 400);
        }
        std::vector<std::pair<std::string, std::string>> result;
        std::set<std::string, std::less<>> seenElementIds;
        result.reserve(elements->size());
        for (const auto& element : *elements) {
            if (!element.template get<"elementId">() || !element.template get<"value">()) {
                service::common::fail(18010, "下发要素参数不完整", 400);
            }
            const auto id = element.template get<"elementId">()->view();
            const auto value = element.template get<"value">()->view();
            if (!service::common::isUuid(id)) {
                service::common::fail(18010, "下发要素 ID 必须是 UUID", 400);
            }
            if (!seenElementIds.emplace(id).second) {
                service::common::fail(18010, "下发要素不能重复", 400);
            }
            if (value.empty() || value.size() > 4096) {
                service::common::fail(18010, "下发要素值长度必须在 1 - 4096 之间", 400);
            }
            result.push_back({ std::string(id), std::string(value) });
        }
        return result;
    }

    service::device::DeviceAccessService& accessService_;
};

inline CommandService& commandService() {
    static thread_local CommandService service(service::device::deviceAccessService());
    return service;
}

} // namespace service::command
