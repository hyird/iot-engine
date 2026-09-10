#pragma once

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/web/db/Db.h>
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
            R"sql(SELECT CASE
              WHEN protocol_params ? 'remote_control' THEN
                CASE lower(COALESCE(protocol_params->>'remote_control', ''))
                  WHEN 'true' THEN TRUE WHEN 't' THEN TRUE WHEN '1' THEN TRUE
                  WHEN 'yes' THEN TRUE WHEN 'y' THEN TRUE WHEN 'on' THEN TRUE
                  ELSE FALSE END
              ELSE TRUE END
            FROM device WHERE id = $1 AND deleted_at IS NULL LIMIT 1)sql",
            service::common::dbParams(deviceId));
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
            R"sql(SELECT CASE
              WHEN protocol_params ? 'remote_control' THEN
                CASE lower(COALESCE(protocol_params->>'remote_control', ''))
                  WHEN 'true' THEN TRUE WHEN 't' THEN TRUE WHEN '1' THEN TRUE
                  WHEN 'yes' THEN TRUE WHEN 'y' THEN TRUE WHEN 'on' THEN TRUE
                  ELSE FALSE END
              ELSE TRUE END
            FROM device WHERE id = $1::uuid AND deleted_at IS NULL LIMIT 1)sql",
            service::common::dbParams(deviceId));
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

        service::device::DeviceCommandStatusDto result(context);
        fillStatus(result, commandId, fields);
        co_return result;
    }

    ruvia::Task<service::device::DeviceCommandStatusesDto>
    statuses(ruvia::Context& context) {
        const auto ids = context.req().query("ids").value_or("");
        if (ids.empty() || ids.size() > 256 * 37)
            service::common::fail(18012, "Provide between 1 and 256 command IDs", 400);
        ruvia::BoxedArray<service::device::DeviceCommandStatusDto> statuses(
            ruvia::ModelOptions{.resource = context.resource()});
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
        service::device::DeviceCommandStatusesDto result(context);
        result.set<"complete">(complete).set<"statuses">(std::move(statuses));
        co_return result;
    }

  private:
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
        (void)co_await transaction.query("SELECT pg_advisory_xact_lock(hashtextextended($1,0))",
                                         common::dbParams(lockKey));
        const auto prior = co_await transaction.query(
            "SELECT id::text,device_id=$3::uuid AND payload=$4::jsonb FROM command_request "
            "WHERE actor=$1 AND idempotency_key=$2::uuid",
            common::dbParams(submittedBy,key,deviceId,payload));
        if (!prior.empty()) {
            if (prior.front()[1].value().value_or(std::string_view{}) != "t")
                common::fail(18014, "幂等键已用于不同的指令请求", 409);
            const auto commands = co_await transaction.query(
                "SELECT id::text FROM command_operation WHERE request_id=$1::uuid ORDER BY ordinal",
                common::dbParams(prior.front()[0].value().value_or(std::string_view{})));
            ruvia::BoxedArray<ruvia::String> ids(ruvia::ModelOptions{.resource=context.resource()});
            for (const auto& row : commands)
                ids.emplace(row[0].value().value_or(std::string_view{}),
                            ruvia::ModelOptions{.resource=context.resource()});
            co_await transaction.commit();
            service::device::DeviceCommandCreateDto result(context);
            result.set<"commandIds">(std::move(ids)).set<"status">("ACCEPTED");
            co_return result;
        }
        const auto requestId = common::nextUuidV7();
        (void)co_await transaction.execute(
            "INSERT INTO command_request(id,actor,idempotency_key,device_id,payload) "
            "VALUES($1::uuid,$2,$3::uuid,$4::uuid,$5::jsonb)",
            common::dbParams(requestId,submittedBy,key,deviceId,payload));
        (void)co_await transaction.query("SELECT id FROM device WHERE id=$1::uuid FOR SHARE",
                                         common::dbParams(deviceId));
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
        const auto batch = ruvia::fromJson<PreparedBatch>(preparedJson, {.resource = context.resource()});
        if (!batch || !batch->get<"queue">() || !batch->get<"kind">() ||
            !batch->get<"maximum">() || !batch->get<"nodeId">() || !batch->get<"commands">() ||
            batch->get<"commands">()->empty() || batch->get<"commands">()->size() > 256)
            common::fail(10004, "Invalid prepared command batch", 502);
        const auto kind = batch->get<"kind">()->view();
        const auto maximum = static_cast<std::int64_t>(*batch->get<"maximum">());
        if ((kind != "list" && kind != "stream") || maximum <= 0 || maximum > 10000)
            common::fail(10004, "Invalid prepared command queue", 502);
        ruvia::BoxedArray<ruvia::String> commandIds(ruvia::ModelOptions{.resource = context.resource()});
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
            (void)co_await transaction.execute(R"sql(
INSERT INTO command_operation(id,request_id,ordinal,device_id,device_code,protocol,status,elements,model_id,model_revision)
SELECT $1::uuid,$2::uuid,$3,$4::uuid,$5,$6,'ACCEPTED',$7::jsonb,protocol_config_id,protocol_revision
FROM device WHERE id=$4::uuid)sql",
                common::dbParams(id, requestId, ordinal++, deviceId,
                    command.get<"deviceCode">()->view(), command.get<"protocol">()->view(), elements));
            (void)co_await transaction.execute(R"sql(
INSERT INTO command_attempt(operation_id,queue_key,queue_kind,payload,submitted_by,node_id,max_length)
VALUES($1::uuid,$2,$3,$4::jsonb,$5,$6,$7))sql",
                common::dbParams(id, batch->get<"queue">()->view(), kind, payload, submittedBy,
                    batch->get<"nodeId">()->view(), maximum));
            const auto eventId = common::nextUuidV7();
            (void)co_await transaction.execute(R"sql(
INSERT INTO outbox_event(id,event_type,aggregate_type,aggregate_id,action,schema_version,payload)
SELECT $1::uuid,$3,'command',device_id::text,'updated',2,
 jsonb_build_object('device_code',device_code,'data',jsonb_build_object(
 'commandId',id::text,'status',status,'reason',reason,'elements',elements,'actualValues',actual_values))
FROM command_operation WHERE id=$2::uuid)sql",
                common::dbParams(eventId, id, "device.command.accepted"));
            commandIds.emplace(id, ruvia::ModelOptions{.resource = context.resource()});
        }
        service::device::DeviceCommandCreateDto result(context);
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
    const auto rows = co_await context.db().query(R"sql(
SELECT device_id::text,device_code,protocol,status,reason,
 (extract(epoch FROM created_at)*1000)::bigint::text,
 COALESCE((extract(epoch FROM completed_at)*1000)::bigint::text,'0')
FROM command_operation WHERE id=$1::uuid)sql", common::dbParams(id));
    std::vector<message::StreamField> fields;
    if (rows.empty()) co_return fields;
    const std::string_view names[]{"device_id","device_code","protocol","status",
                                   "reason","created_at_ms","completed_at_ms"};
    for (std::size_t index = 0; index < 7; ++index)
        fields.push_back({std::string(names[index]),
                          std::string(rows.front()[index].value().value_or(std::string_view{}))});
    const auto actual = co_await context.db().query(R"sql(
SELECT value->>'elementId',value->>'name',value->>'kind',value->>'value',value->>'unit'
FROM command_operation, jsonb_array_elements(actual_values) WITH ORDINALITY a(value,idx)
WHERE id=$1::uuid ORDER BY idx)sql", common::dbParams(id));
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
