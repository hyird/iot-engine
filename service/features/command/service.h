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
#include "service/features/edge/dispatch.h"
#include "service/features/edge/protocol.h"
#include "service/features/command/queue.h"
#include "service/features/runtime/repository.h"
#include "service/domains/device/device.service.h"
#include "service/domains/device/device.types.h"
#include "service/features/access/contract.h"
#include "service/features/access/event.h"
#include "service/features/collector/command.h"

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
            co_await message::redis::hashEntries(context.redis(), stateKey(commandId));
        if (fields.empty())
            service::common::fail(18012, "下发记录不存在或已过期", 404);
        const auto deviceId = field(fields, "device_id");
        if (deviceId.empty())
            service::common::fail(18012, "下发状态数据无效", 500);
        (void)co_await accessService_.require(
            context, deviceId, service::device::DeviceAccessLevel::operate);

        service::device::DeviceCommandStatusDto result(context);
        result.set<"commandId">(commandId)
            .set<"deviceId">(deviceId)
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
        co_return result;
    }

    ruvia::Task<service::device::DeviceCommandWaitDto>
    wait(ruvia::Context& context, const service::device::DeviceCommandWaitBody& body) {
        std::vector<std::string> commandIds;
        commandIds.reserve(body.get<"commandIds">()->size());
        std::set<std::string, std::less<>> authorizedDevices;
        for (const auto& value : *body.get<"commandIds">()) {
            const std::string commandId(value.view());
            if (!service::common::isUuid(commandId))
                service::common::fail(18012, "指令 ID 无效", 400);
            const auto fields =
                co_await message::redis::hashEntries(context.redis(), stateKey(commandId));
            if (fields.empty())
                service::common::fail(18012, "下发记录不存在或已过期", 404);
            const std::string deviceId(field(fields, "device_id"));
            if (deviceId.empty())
                service::common::fail(18012, "下发状态数据无效", 500);
            if (authorizedDevices.insert(deviceId).second)
                (void)co_await accessService_.require(
                    context, deviceId, service::device::DeviceAccessLevel::operate);
            commandIds.push_back(std::move(commandId));
        }

        const auto timeout = std::chrono::milliseconds(
            body.get<"timeoutMs">()
                ? std::clamp<std::int64_t>(static_cast<std::int64_t>(*body.get<"timeoutMs">()),
                                           0, 60000)
                : 60000);
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (true) {
            ruvia::BoxedArray<service::device::DeviceCommandStatusDto> statuses(
                ruvia::ModelOptions{.resource = context.resource()});
            bool complete = true;
            for (const auto& commandId : commandIds) {
                const auto fields =
                    co_await message::redis::hashEntries(context.redis(), stateKey(commandId));
                if (fields.empty())
                    service::common::fail(18012, "下发记录不存在或已过期", 404);
                auto& result = statuses.emplace(context);
                fillStatus(result, commandId, fields);
                complete = complete && field(fields, "status") != "PENDING";
            }
            if (complete || std::chrono::steady_clock::now() >= deadline) {
                service::device::DeviceCommandWaitDto result(context);
                result.set<"complete">(complete).set<"statuses">(std::move(statuses));
                co_return result;
            }
            (void)co_await ruvia::sleepFor(context.worker(), std::chrono::milliseconds(100));
        }
    }

  private:
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
    }

    ruvia::Task<service::device::DeviceCommandCreateDto>
    enqueueDevice(ruvia::Context& context, std::string_view deviceId,
                  const service::device::DeviceCommandBody& body, std::string submittedBy) {

        const auto edge = co_await context.db().query(R"sql(
SELECT COALESCE(l.edge_node_id::text, ''), d.protocol_params->>'device_code', p.protocol,
       CASE
         WHEN d.protocol_params ? 'remote_control' THEN
           CASE lower(COALESCE(d.protocol_params->>'remote_control', ''))
             WHEN 'true' THEN TRUE WHEN 't' THEN TRUE WHEN '1' THEN TRUE
             WHEN 'yes' THEN TRUE WHEN 'y' THEN TRUE WHEN 'on' THEN TRUE
             ELSE FALSE END
         ELSE TRUE END,
       COALESCE(l.status = 'enabled'
                AND n.enrollment_status = 'approved'
                AND CASE lower(COALESCE(n.capability->>'deviceConfig', ''))
                      WHEN 'true' THEN TRUE WHEN 't' THEN TRUE WHEN '1' THEN TRUE
                      WHEN 'yes' THEN TRUE WHEN 'y' THEN TRUE WHEN 'on' THEN TRUE
                      ELSE FALSE END
                AND COALESCE(n.status->'config'->>'state', 'idle') = 'applied'
                AND COALESCE(
                      CASE WHEN COALESCE(n.status->'config'->>'activeVersion', '') ~
                                '^-?[0-9]{1,18}$'
                           THEN (n.status->'config'->>'activeVersion')::bigint END, 0)
                    = COALESCE(
                      CASE WHEN COALESCE(n.status->'config'->>'desiredVersion', '') ~
                                '^-?[0-9]{1,18}$'
                           THEN (n.status->'config'->>'desiredVersion')::bigint END, 0), false),
       COALESCE(NULLIF(p.config->>'commandFastReadDuration', ''), '60'),
       COALESCE(NULLIF(p.config->>'commandFastReadInterval', ''), '1')
FROM device d
JOIN link l ON l.id = d.link_id AND l.deleted_at IS NULL
JOIN protocol_config p ON p.id = d.protocol_config_id
             AND p.deleted_at IS NULL AND p.enabled
LEFT JOIN edge_node n ON n.id = l.edge_node_id
WHERE d.id = $1::uuid AND d.deleted_at IS NULL AND d.status = 'enabled' LIMIT 1)sql",
                                                      service::common::dbParams(deviceId));
        if (!edge.empty() && !edge.front()[0].value().value_or(std::string_view{}).empty()) {
            if (edge.front()[4].value().value_or(std::string_view{}) != "t")
                service::common::fail(18013, "边缘节点设备配置尚未生效", 409);
            co_return co_await enqueueEdgeDevice(context, deviceId, body, std::move(submittedBy),
                                                 edge.front()[0].value().value_or(std::string_view{}),
                                                 edge.front()[1].value().value_or(std::string_view{}),
                                                 edge.front()[2].value().value_or(std::string_view{}),
                                                 edge.front()[3].value().value_or(std::string_view{}) == "t",
                                                 boundedUnsigned(
                                                     edge.front()[5].value().value_or(std::string_view{}),
                                                     60, 0),
                                                 boundedUnsigned(
                                                     edge.front()[6].value().value_or(std::string_view{}),
                                                     1, 1));
        }

        auto requested = normalize(body);
        auto database = context.db();
        const auto snapshot =
            co_await service::runtime::repository::loadRuntimeSnapshot(database);
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
            route = co_await deviceRoute(context.redis(), device->code);
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
            task.groupKey = "device:" + device->code;
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
        (void)co_await dispatchPendingBatch(
            context.redis(), message::commandStream(route.workerIndex, true),
            PendingQueueKind::Stream, dispatches, submittedBy, 10000);

        ruvia::BoxedArray<ruvia::String> commandIds(
            ruvia::ModelOptions{.resource = context.resource()});
        for (const auto& dispatch : dispatches) {
            const auto& task = dispatch.task;
            std::string data = "{\"commandId\":\"" + task.messageId + "\",\"elements\":{";
            for (std::size_t index = 0; index < task.elements.size(); ++index) {
                if (index != 0)
                    data.push_back(',');
                data += "\"" + task.elements[index].first + "\":\"" +
                        service::access::jsonEscape(task.elements[index].second) + "\"";
            }
            data += "}}";
            try {
                co_await service::access::event::publish(
                    context.redis(), task.messageId, "device.command.dispatched", task.deviceId,
                    task.deviceCode, message::utcNowMilliseconds(), data);
            } catch (const std::exception& error) {
                std::cerr << "open access command event publish failed: " << error.what() << '\n';
            }
            commandIds.emplace(
                task.messageId, ruvia::ModelOptions{.resource = context.resource()});
        }

        service::device::DeviceCommandCreateDto result(context);
        result.set<"commandIds">(std::move(commandIds)).set<"status">("PENDING");
        co_return result;
    }

    ruvia::Task<service::device::DeviceCommandCreateDto>
    enqueueEdgeDevice(ruvia::Context& context, std::string_view deviceId,
                      const service::device::DeviceCommandBody& body, std::string submittedBy,
                      std::string_view nodeId, std::string_view deviceCode,
                      std::string_view protocol, bool remoteControl,
                      std::uint32_t fastReadDurationSec,
                      std::uint32_t fastReadIntervalSec) {
        if (!remoteControl)
            service::common::fail(18005, "设备未开启远程控制", 403);
        if (!co_await context.redis().get("iot:edge:session:" + std::string(nodeId)))
            service::common::fail(18013, "边缘节点离线，无法下发设备命令", 409);

        auto requested = normalize(body);
        service::collector::DeviceDefinition device;
        device.id = std::string(deviceId);
        device.code = std::string(deviceCode);
        device.protocol = std::string(protocol);
        co_await loadEdgeElements(context, device);
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
            task.groupKey = "edge-device:" + device.code;
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
        (void)co_await dispatchPendingBatch(
            context.redis(), "iot:edge:egress:" + std::string(nodeId), PendingQueueKind::List,
            dispatches, submittedBy, 1024);
        co_await service::edge::dispatch::notifyNode(context.redis(), nodeId);

        ruvia::BoxedArray<ruvia::String> commandIds(
            ruvia::ModelOptions{.resource = context.resource()});
        for (const auto& dispatch : dispatches) {
            const auto& task = dispatch.task;
            std::string data = "{\"commandId\":\"" + task.messageId + "\",\"elements\":{";
            for (std::size_t index = 0; index < task.elements.size(); ++index) {
                if (index != 0)
                    data.push_back(',');
                data += "\"" + task.elements[index].first + "\":\"" +
                        service::access::jsonEscape(task.elements[index].second) + "\"";
            }
            data += "}}";
            try {
                co_await service::access::event::publish(
                    context.redis(), task.messageId, "device.command.dispatched", task.deviceId,
                    task.deviceCode, message::utcNowMilliseconds(), data);
            } catch (const std::exception& error) {
                std::cerr << "open access edge command event publish failed: " << error.what()
                          << '\n';
            }
            commandIds.emplace(
                task.messageId, ruvia::ModelOptions{.resource = context.resource()});
        }

        service::device::DeviceCommandCreateDto result(context);
        result.set<"commandIds">(std::move(commandIds)).set<"status">("PENDING");
        co_return result;
    }

    static ruvia::Task<void> loadEdgeElements(ruvia::Context& context,
                                               service::collector::DeviceDefinition& device) {
        std::string sql;
        if (device.protocol == "Modbus") {
            sql = R"sql(
SELECT item->>'id', item->>'name', COALESCE(item->>'unit', ''), item->>'dataType',
       '', 0, 0,
       CASE lower(COALESCE(item->>'writable', ''))
         WHEN 'true' THEN TRUE WHEN 't' THEN TRUE WHEN '1' THEN TRUE
         WHEN 'yes' THEN TRUE WHEN 'y' THEN TRUE WHEN 'on' THEN TRUE
         ELSE FALSE END,
       false, '', ''
FROM device d
JOIN link l ON l.id = d.link_id AND l.execution = 'edge' AND l.deleted_at IS NULL
JOIN protocol_config p ON p.id = d.protocol_config_id
CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'registers', '[]')) item
WHERE d.id = $1::uuid AND p.protocol = 'Modbus')sql";
        } else if (device.protocol == "S7") {
            sql = R"sql(
SELECT item->>'id', item->>'name', COALESCE(item->>'unit', ''),
       COALESCE(item->>'dataType', 'BOOL'), '',
       COALESCE(CASE WHEN COALESCE(item->>'size', '') ~ '^-?[0-9]{1,18}$'
                     THEN (item->>'size')::integer END, 1),
       0,
       CASE lower(COALESCE(item->>'writable', ''))
         WHEN 'true' THEN TRUE WHEN 't' THEN TRUE WHEN '1' THEN TRUE
         WHEN 'yes' THEN TRUE WHEN 'y' THEN TRUE WHEN 'on' THEN TRUE
         ELSE FALSE END,
       false, '', ''
FROM device d
JOIN link l ON l.id = d.link_id AND l.execution = 'edge' AND l.deleted_at IS NULL
JOIN protocol_config p ON p.id = d.protocol_config_id
CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'areas', '[]')) item
WHERE d.id = $1::uuid AND p.protocol = 'S7')sql";
        } else if (device.protocol == "SL651") {
            sql = R"sql(
SELECT item->>'id', item->>'name', COALESCE(item->>'unit', ''), '',
       func->>'dir',
       COALESCE(CASE WHEN COALESCE(item->>'length', '') ~ '^-?[0-9]{1,18}$'
                     THEN (item->>'length')::integer END, 1),
       COALESCE(CASE WHEN COALESCE(item->>'digits', '') ~ '^-?[0-9]{1,18}$'
                     THEN (item->>'digits')::integer END, 0),
       func->>'dir' = 'DOWN', response_element,
       func->>'funcCode', item->>'encode'
FROM device d
JOIN link l ON l.id = d.link_id AND l.execution = 'edge' AND l.deleted_at IS NULL
JOIN protocol_config p ON p.id = d.protocol_config_id
CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'funcs', '[]')) func
CROSS JOIN LATERAL (
  SELECT value AS item, false AS response_element
  FROM jsonb_array_elements(COALESCE(func->'elements', '[]'))
  UNION ALL
  SELECT value AS item, true AS response_element
  FROM jsonb_array_elements(COALESCE(func->'responseElements', '[]'))
) configured
WHERE d.id = $1::uuid AND p.protocol = 'SL651')sql";
        } else {
            service::common::fail(18010, "边缘节点不支持该设备协议", 400);
        }
        const auto rows = co_await context.db().query(sql, service::common::dbParams(device.id));
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

    static std::string stateKey(std::string_view commandId) {
        return "iot:state:command:" + std::string(commandId);
    }

    static std::vector<service::collector::CommandElementValue>
    normalize(const service::device::DeviceCommandBody& body) {
        if (!body.get<"elements">() || body.get<"elements">()->empty() || body.get<"elements">()->size() > 256)
            service::common::fail(18010, "下发要素数量必须在 1 - 256 之间", 400);
        std::vector<service::collector::CommandElementValue> result;
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

    service::device::DeviceAccessService& accessService_;
};

inline CommandService& commandService() {
    static CommandService service(service::device::deviceAccessService());
    return service;
}

} // namespace service::command
