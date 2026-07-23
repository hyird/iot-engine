#pragma once

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/web/db/Db.h>

#include "service/common/http.h"
#include "service/common/uuid.h"
#include "service/modules/edge/edge.protocol.h"
#include "service/modules/northbridge/command/protocol_command.queue.h"
#include "service/modules/northbridge/config/runtime_config.repository.h"
#include "service/modules/northbridge/device/device.access.h"
#include "service/modules/northbridge/device/device.types.h"
#include "service/modules/northbridge/open/open_access.common.h"
#include "service/modules/northbridge/open/open_access.event.h"
#include "service/modules/southbridge/protocol/command_value.h"

namespace service::northbridge::command {

class DeviceCommandService final {
  public:
    static DeviceCommandService& instance() {
        static DeviceCommandService service;
        return service;
    }

    ruvia::Task<service::device::DeviceCommandCreateDto>
    create(ruvia::Context& context, std::string_view deviceId,
           const service::device::DeviceCommandBody& body) {
        const auto access = co_await service::device::deviceAccessService().require(
            context, deviceId, service::device::DeviceAccessLevel::operate);
        const auto deviceRows = co_await context.db().query(
            "SELECT COALESCE((protocol_params->>'remote_control')::boolean, TRUE) "
            "FROM device WHERE id = $1 AND deleted_at IS NULL LIMIT 1",
            service::common::dbParams(deviceId));
        if (deviceRows.rows().empty())
            service::common::fail(18001, "设备不存在", 404);
        const auto capabilities = service::device::DeviceAccessService::capabilities(
            access.actor, access.level, deviceRows.rows().front()[0].text() == "t");
        if (!capabilities.canCommand)
            service::common::fail(18005, "设备未开启远程控制或当前账号无下发权限", 403);

        co_return co_await enqueueDevice(context, deviceId, body, access.actor.userId);
    }

    ruvia::Task<service::device::DeviceCommandCreateDto>
    createExternal(ruvia::Context& context, std::string_view deviceId,
                   const service::device::DeviceCommandBody& body, std::string_view accessKeyId) {
        const auto deviceRows = co_await context.db().query(
            "SELECT COALESCE((protocol_params->>'remote_control')::boolean, TRUE) "
            "FROM device WHERE id = $1::uuid AND deleted_at IS NULL LIMIT 1",
            service::common::dbParams(deviceId));
        if (deviceRows.rows().empty())
            service::common::fail(18001, "设备不存在", 404);
        if (deviceRows.rows().front()[0].text() != "t")
            service::common::fail(18005, "设备未开启远程控制", 403);
        co_return co_await enqueueDevice(context, deviceId, body,
                                         "access-key:" + std::string(accessKeyId));
    }

    ruvia::Task<service::device::DeviceCommandStatusDto> status(ruvia::Context& context,
                                                                std::string_view commandId) {
        const auto fields =
            co_await bridge::redis_async::hashEntries(context.redis(), stateKey(commandId));
        if (fields.empty())
            service::common::fail(18012, "下发记录不存在或已过期", 404);
        const auto deviceId = field(fields, "device_id");
        if (deviceId.empty())
            service::common::fail(18012, "下发状态数据无效", 500);
        (void)co_await service::device::deviceAccessService().require(
            context, deviceId, service::device::DeviceAccessLevel::operate);

        service::device::DeviceCommandStatusDto result(context);
        result.commandId(commandId)
            .deviceId(deviceId)
            .deviceCode(field(fields, "device_code"))
            .protocol(field(fields, "protocol"))
            .status(field(fields, "status"));
        const auto reason = field(fields, "reason");
        if (!reason.empty())
            result.reason(reason);
        const auto createdAt = integer(fields, "created_at_ms");
        if (createdAt != 0)
            result.createdAtMs(createdAt);
        const auto completedAt = integer(fields, "completed_at_ms");
        if (completedAt != 0)
            result.completedAtMs(completedAt);
        co_return result;
    }

  private:
    static constexpr auto kStateTtl = std::chrono::hours(24);

    ruvia::Task<service::device::DeviceCommandCreateDto>
    enqueueDevice(ruvia::Context& context, std::string_view deviceId,
                  const service::device::DeviceCommandBody& body, std::string submittedBy) {

        const auto edge = co_await context.db().query(R"sql(
SELECT COALESCE(d.edge_node_id::text, ''), d.protocol_params->>'device_code', p.protocol,
       COALESCE((d.protocol_params->>'remote_control')::boolean, true),
       COALESCE(n.enrollment_status = 'approved' AND n.supports_device_config AND
                n.config_status = 'applied' AND
                n.active_config_version = n.desired_config_version, false)
FROM device d JOIN protocol_config p ON p.id = d.protocol_config_id
             AND p.deleted_at IS NULL AND p.enabled
LEFT JOIN edge_node n ON n.id = d.edge_node_id
WHERE d.id = $1::uuid AND d.deleted_at IS NULL AND d.status = 'enabled' LIMIT 1)sql",
                                                      service::common::dbParams(deviceId));
        if (!edge.rows().empty() && !edge.rows().front()[0].text().empty()) {
            if (edge.rows().front()[4].text() != "t")
                service::common::fail(18013, "边缘节点设备配置尚未生效", 409);
            co_return co_await enqueueEdgeDevice(context, deviceId, body, std::move(submittedBy),
                                                 edge.rows().front()[0].text(),
                                                 edge.rows().front()[1].text(),
                                                 edge.rows().front()[2].text(),
                                                 edge.rows().front()[3].text() == "t");
        }

        auto requested = normalize(body);
        auto database = context.db();
        const auto snapshot =
            co_await service::northbridge::config::repository::loadRuntimeSnapshot(database);
        const auto device =
            std::find_if(snapshot.devices.begin(), snapshot.devices.end(),
                         [deviceId](const auto& current) { return current.id == deviceId; });
        if (device == snapshot.devices.end())
            service::common::fail(18011, "设备、链路或协议配置未启用", 409);
        try {
            (void)service::southbridge::command_value::resolve(*device, requested);
        } catch (const std::invalid_argument& error) {
            service::common::fail(18010, error.what(), 400);
        }

        DeviceRoute route;
        try {
            route = co_await deviceRoute(context.redis(), device->code);
        } catch (const DeviceRouteError&) {
            service::common::fail(18013, "设备离线或没有可用的南桥连接", 409);
        }
        std::vector<std::vector<service::southbridge::CommandElementValue>> tasks;
        if (device->protocol == "SL651")
            tasks.push_back(std::move(requested));
        else {
            tasks.reserve(requested.size());
            for (auto& element : requested)
                tasks.push_back({std::move(element)});
        }

        ruvia::List<ruvia::String> commandIds(context.resource());
        for (const auto& elements : tasks) {
            bridge::ProtocolTask task;
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
            for (const auto& element : elements)
                task.elements.emplace_back(element.elementId, element.value);
            co_await setPending(context, task, submittedBy);
            (void)co_await enqueue(context, task, route, true);
            std::string data = "{\"commandId\":\"" + task.messageId + "\",\"elements\":{";
            for (std::size_t index = 0; index < task.elements.size(); ++index) {
                if (index != 0)
                    data.push_back(',');
                data += "\"" + task.elements[index].first + "\":\"" +
                        service::open_access::jsonEscape(task.elements[index].second) + "\"";
            }
            data += "}}";
            try {
                co_await service::open_access::event::publish(
                    context.redis(), task.messageId, "device.command.dispatched", task.deviceId,
                    task.deviceCode, bridge::utcNowMilliseconds(), data);
            } catch (const std::exception& error) {
                std::cerr << "open access command event publish failed: " << error.what() << '\n';
            }
            commandIds.emplace(task.messageId, context.resource());
        }

        service::device::DeviceCommandCreateDto result(context);
        result.commandIds(std::move(commandIds)).status("PENDING");
        co_return result;
    }

    ruvia::Task<service::device::DeviceCommandCreateDto>
    enqueueEdgeDevice(ruvia::Context& context, std::string_view deviceId,
                      const service::device::DeviceCommandBody& body, std::string submittedBy,
                      std::string_view nodeId, std::string_view deviceCode,
                      std::string_view protocol, bool remoteControl) {
        if (!remoteControl)
            service::common::fail(18005, "设备未开启远程控制", 403);
        if (!co_await context.redis().get("iot:edge:session:" + std::string(nodeId)))
            service::common::fail(18013, "边缘节点离线，无法下发设备命令", 409);

        auto requested = normalize(body);
        service::southbridge::DeviceDefinition device;
        device.id = std::string(deviceId);
        device.code = std::string(deviceCode);
        device.protocol = std::string(protocol);
        co_await loadEdgeElements(context, device);
        service::southbridge::command_value::ResolvedCommand resolved;
        try {
            resolved = service::southbridge::command_value::resolve(device, requested);
        } catch (const std::invalid_argument& error) {
            service::common::fail(18010, error.what(), 400);
        }
        if (device.protocol == "SL651" && resolved.elements.size() > 8)
            service::common::fail(18010, "SL651 边缘命令最多包含 8 个要素", 400);

        std::vector<std::vector<service::southbridge::CommandElementValue>> tasks;
        if (device.protocol == "SL651") {
            tasks.push_back(std::move(requested));
        } else {
            tasks.reserve(requested.size());
            for (auto& element : requested)
                tasks.push_back({std::move(element)});
        }

        ruvia::List<ruvia::String> commandIds(context.resource());
        for (const auto& elements : tasks) {
            bridge::ProtocolTask task;
            task.messageId = service::common::nextUuidV7();
            task.groupKey = "edge-device:" + device.code;
            task.protocol = device.protocol;
            task.transport = "EDGE";
            task.kind = "command";
            task.deviceId = device.id;
            task.deviceCode = device.code;
            task.responseTimeoutMs = 5000;
            task.maxAttempts = 1;
            for (const auto& element : elements)
                task.elements.emplace_back(element.elementId, element.value);
            co_await setPending(context, task, submittedBy);

            auto envelope = service::edge::protocol::outbound(nodeId);
            auto* command = envelope.mutable_command_request();
            if (!setUuid(command->mutable_command_id(), task.messageId) ||
                !setUuid(command->mutable_device_id(), task.deviceId))
                service::common::fail(18010, "边缘命令标识无效", 500);
            command->set_timeout_ms(5000);
            command->set_readback_count(1);
            command->set_fast_read_duration_sec(10);
            command->set_fast_read_interval_sec(1);
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
            co_await pushEdgeCommand(context, nodeId, envelope);

            std::string data = "{\"commandId\":\"" + task.messageId + "\",\"elements\":{";
            for (std::size_t index = 0; index < task.elements.size(); ++index) {
                if (index != 0)
                    data.push_back(',');
                data += "\"" + task.elements[index].first + "\":\"" +
                        service::open_access::jsonEscape(task.elements[index].second) + "\"";
            }
            data += "}}";
            try {
                co_await service::open_access::event::publish(
                    context.redis(), task.messageId, "device.command.dispatched", task.deviceId,
                    task.deviceCode, bridge::utcNowMilliseconds(), data);
            } catch (const std::exception& error) {
                std::cerr << "open access edge command event publish failed: " << error.what()
                          << '\n';
            }
            commandIds.emplace(task.messageId, context.resource());
        }

        service::device::DeviceCommandCreateDto result(context);
        result.commandIds(std::move(commandIds)).status("PENDING");
        co_return result;
    }

    static ruvia::Task<void> loadEdgeElements(ruvia::Context& context,
                                               service::southbridge::DeviceDefinition& device) {
        std::string sql;
        if (device.protocol == "Modbus") {
            sql = R"sql(
SELECT item->>'id', item->>'name', COALESCE(item->>'unit', ''), item->>'dataType',
       '', 0, 0, COALESCE((item->>'writable')::boolean, false), false, '', ''
FROM device d JOIN protocol_config p ON p.id = d.protocol_config_id
CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'registers', '[]')) item
WHERE d.id = $1::uuid AND d.edge_node_id IS NOT NULL AND p.protocol = 'Modbus')sql";
        } else if (device.protocol == "S7") {
            sql = R"sql(
SELECT item->>'id', item->>'name', COALESCE(item->>'unit', ''),
       COALESCE(item->>'dataType', 'BOOL'), '', COALESCE((item->>'size')::integer, 1), 0,
       COALESCE((item->>'writable')::boolean, false), false, '', ''
FROM device d JOIN protocol_config p ON p.id = d.protocol_config_id
CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'areas', '[]')) item
WHERE d.id = $1::uuid AND d.edge_node_id IS NOT NULL AND p.protocol = 'S7')sql";
        } else if (device.protocol == "SL651") {
            sql = R"sql(
SELECT item->>'id', item->>'name', COALESCE(item->>'unit', ''), '',
       func->>'dir', COALESCE((item->>'length')::integer, 1),
       COALESCE((item->>'digits')::integer, 0), func->>'dir' = 'DOWN', response_element,
       func->>'funcCode', item->>'encode'
FROM device d JOIN protocol_config p ON p.id = d.protocol_config_id
CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'funcs', '[]')) func
CROSS JOIN LATERAL (
  SELECT value AS item, false AS response_element
  FROM jsonb_array_elements(COALESCE(func->'elements', '[]'))
  UNION ALL
  SELECT value AS item, true AS response_element
  FROM jsonb_array_elements(COALESCE(func->'responseElements', '[]'))
) configured
WHERE d.id = $1::uuid AND d.edge_node_id IS NOT NULL AND p.protocol = 'SL651')sql";
        } else {
            service::common::fail(18010, "边缘节点不支持该设备协议", 400);
        }
        const auto rows = co_await context.db().query(sql, service::common::dbParams(device.id));
        for (const auto& row : rows.rows()) {
            service::southbridge::ElementDefinition element;
            element.id = std::string(row[0].text());
            element.name = std::string(row[1].text());
            element.unit = std::string(row[2].text());
            element.dataType = std::string(row[3].text());
            element.direction = std::string(row[4].text());
            element.size = parseInteger(row[5].text());
            element.length = parseInteger(row[5].text());
            element.digits = parseInteger(row[6].text());
            element.writable = row[7].text() == "t";
            element.responseElement = row[8].text() == "t";
            element.functionCode = std::string(row[9].text());
            element.encoding = std::string(row[10].text());
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

    static ruvia::Task<void> pushEdgeCommand(ruvia::Context& context, std::string_view nodeId,
                                              const service::edge::pb::Envelope& envelope) {
        const auto wire = service::edge::protocol::encode(envelope);
        if (wire.empty())
            service::common::fail(18010, "边缘命令编码失败", 500);
        const auto key = "iot:edge:egress:" + std::string(nodeId);
        (void)co_await context.redis().rpush(key, wire);
        co_await context.redis().ltrim(key, -1024, -1);
    }

    static std::string stateKey(std::string_view commandId) {
        return "iot:state:command:" + std::string(commandId);
    }

    static std::vector<service::southbridge::CommandElementValue>
    normalize(const service::device::DeviceCommandBody& body) {
        if (!body.elements() || body.elements()->empty() || body.elements()->size() > 256)
            service::common::fail(18010, "下发要素数量必须在 1 - 256 之间", 400);
        std::vector<service::southbridge::CommandElementValue> result;
        result.reserve(body.elements()->size());
        for (const auto& element : *body.elements()) {
            if (!element.elementId() || !element.value())
                service::common::fail(18010, "下发要素参数不完整", 400);
            const auto id = element.elementId()->view();
            const auto value = element.value()->view();
            if (!service::common::isUuid(id))
                service::common::fail(18010, "下发要素 ID 必须是 UUID", 400);
            if (value.empty() || value.size() > 4096)
                service::common::fail(18010, "下发要素值长度必须在 1 - 4096 之间", 400);
            result.push_back({std::string(id), std::string(value)});
        }
        return result;
    }

    static std::int64_t integer(const std::vector<bridge::StreamField>& fields,
                                std::string_view name) {
        const auto value = field(fields, name);
        std::int64_t result = 0;
        const auto [end, error] =
            std::from_chars(value.data(), value.data() + value.size(), result);
        return error == std::errc{} && end == value.data() + value.size() ? result : 0;
    }

    static ruvia::Task<void> setPending(ruvia::Context& context, const bridge::ProtocolTask& task,
                                        std::string_view userId) {
        const auto now = bridge::utcNowMilliseconds();
        const auto key = stateKey(task.messageId);
        co_await bridge::redis_async::eraseHash(context.redis(), key);
        std::vector<bridge::StreamField> fields;
        fields.reserve(7);
        fields.push_back({"command_id", task.messageId});
        fields.push_back({"device_id", task.deviceId});
        fields.push_back({"device_code", task.deviceCode});
        fields.push_back({"protocol", task.protocol});
        fields.push_back({"status", "PENDING"});
        fields.push_back({"submitted_by", std::string(userId)});
        fields.push_back({"created_at_ms", std::to_string(now)});
        co_await bridge::redis_async::setHash(context.redis(), key, fields);
        (void)co_await bridge::redis_async::command(
            context.redis(),
            {"PEXPIRE", key,
             std::to_string(
                 std::chrono::duration_cast<std::chrono::milliseconds>(kStateTtl).count())});
    }
};

inline DeviceCommandService& deviceCommandService() { return DeviceCommandService::instance(); }

} // namespace service::northbridge::command
