#pragma once

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/web/db/Db.h>

#include "service/common/http.h"
#include "service/common/uuid.h"
#include "service/modules/northbridge/command/protocol_command.queue.h"
#include "service/modules/northbridge/config/runtime_config.repository.h"
#include "service/modules/northbridge/device/device.access.h"
#include "service/modules/northbridge/device/device.types.h"
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
            co_await setPending(context, task, access.actor.userId);
            (void)co_await enqueue(context, task, route, true);
            commandIds.emplace(task.messageId, context.resource());
        }

        service::device::DeviceCommandCreateDto result(context);
        result.commandIds(std::move(commandIds)).status("PENDING");
        co_return result;
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
        co_await bridge::redis_async::setHash(context.redis(), key,
                                              {{"command_id", task.messageId},
                                               {"device_id", task.deviceId},
                                               {"device_code", task.deviceCode},
                                               {"protocol", task.protocol},
                                               {"status", "PENDING"},
                                               {"submitted_by", std::string(userId)},
                                               {"created_at_ms", std::to_string(now)}});
        (void)co_await bridge::redis_async::command(
            context.redis(),
            {"PEXPIRE", key,
             std::to_string(
                 std::chrono::duration_cast<std::chrono::milliseconds>(kStateTtl).count())});
    }
};

inline DeviceCommandService& deviceCommandService() { return DeviceCommandService::instance(); }

} // namespace service::northbridge::command
