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
    (void)co_await db.execute(R"sql(
INSERT INTO outbox_event(id,event_type,aggregate_type,aggregate_id,action,schema_version,payload)
SELECT $1::uuid,$3,'command',device_id::text,'updated',2,
 jsonb_build_object('device_code',device_code,'data',jsonb_build_object(
 'commandId',id::text,'status',status,'reason',reason,'elements',elements,'actualValues',actual_values))
FROM command_operation WHERE id=$2::uuid)sql",
        common::dbParams(eventId, commandId, type));
}


// Claim is committed BEFORE touching the physical delivery path. A crashed or ambiguous
// attempt is never replayed automatically: old EdgeNode cannot promise durable deduplication.
template <typename Context>
ruvia::Task<void> dispatch(Context& context) {
    auto tx = co_await context.db().beginTransaction();
    const auto rows = co_await tx.query(R"sql(
SELECT a.operation_id::text,a.queue_key,a.queue_kind,a.submitted_by,a.node_id,
 a.max_length::text,o.device_id::text,o.device_code,o.protocol,
 (extract(epoch FROM o.created_at)*1000)::bigint::text
FROM command_attempt a JOIN command_operation o ON o.id=a.operation_id
WHERE a.claimed_at IS NULL AND a.deadline>NOW() AND o.status='ACCEPTED'
ORDER BY o.created_at FOR UPDATE OF a,o SKIP LOCKED LIMIT 16)sql");
    for (const auto& row : rows) {
        const auto id = row[0].value().value_or(std::string_view{});
        (void)co_await tx.execute("UPDATE command_attempt SET claimed_at=NOW() WHERE operation_id=$1::uuid",
                                  common::dbParams(id));
        (void)co_await tx.execute("UPDATE command_operation SET status='DISPATCHING' WHERE id=$1::uuid",
                                  common::dbParams(id));
    }
    co_await tx.commit();
    for (const auto& row : rows) {
        const auto cell = [&](std::size_t index) { return row[index].value().value_or(std::string_view{}); };
        std::string failure;
        bool published = false;
        try {
            const auto values = co_await context.db().query(
                "SELECT value FROM command_attempt, jsonb_array_elements_text(payload) WITH ORDINALITY p(value,idx) "
                "WHERE operation_id=$1::uuid ORDER BY idx", common::dbParams(cell(0)));
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
        (void)co_await update.execute(R"sql(
UPDATE command_operation SET status=$2,reason=$3,
 completed_at=CASE WHEN $2='AWAITING_RESULT' THEN NULL ELSE NOW() END
WHERE id=$1::uuid AND status='DISPATCHING')sql", common::dbParams(cell(0),next,failure));
        if (published)
            (void)co_await update.execute("UPDATE command_attempt SET dispatched_at=NOW() WHERE operation_id=$1::uuid",
                                          common::dbParams(cell(0)));
        else co_await event(update,cell(0),"device.command.updated");
        co_await update.commit();
        if (published && !cell(4).empty())
            co_await edge::dispatch::notifyNode(context.redis(),cell(4));
    }
    auto expiry = co_await context.db().beginTransaction();
    const auto expired = co_await expiry.query(R"sql(
UPDATE command_operation o SET status=CASE WHEN a.claimed_at IS NULL THEN 'REJECTED' ELSE 'UNKNOWN' END,
 reason=CASE WHEN a.claimed_at IS NULL THEN 'dispatch_deadline_expired' ELSE 'result_not_confirmed' END,
 completed_at=NOW()
FROM command_attempt a WHERE o.id=a.operation_id AND a.deadline<=NOW()
 AND o.status IN ('ACCEPTED','DISPATCHING','AWAITING_RESULT') RETURNING o.id::text)sql");
    for (const auto& row : expired)
        co_await event(expiry,row[0].value().value_or(std::string_view{}),"device.command.updated");
    co_await expiry.commit();
}

template <typename Context>
ruvia::Task<std::optional<std::chrono::milliseconds>>
nextDispatchDelay(Context& context) {
    const auto rows = co_await context.db().query(R"sql(
SELECT CASE
 WHEN COUNT(*) = 0 THEN NULL
 WHEN BOOL_OR(o.status='ACCEPTED' AND a.claimed_at IS NULL AND a.deadline>NOW()) THEN 25
 ELSE GREATEST(25, CEIL(EXTRACT(EPOCH FROM (MIN(a.deadline)-NOW()))*1000)::bigint)
 END::text
FROM command_attempt a JOIN command_operation o ON o.id=a.operation_id
WHERE o.status IN ('ACCEPTED','DISPATCHING','AWAITING_RESULT')
  AND a.deadline IS NOT NULL)sql");
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
            const auto updated = co_await transaction.query(R"sql(
UPDATE command_operation SET status=$3,reason=$4,actual_values=$5::jsonb,completed_at=NOW()
WHERE id=$1::uuid AND device_id=$2::uuid
 AND (status IN ('DISPATCHING','AWAITING_RESULT') OR (status='UNKNOWN' AND $3<>'UNKNOWN'))
RETURNING id::text)sql",
                common::dbParams(id, deviceId, state, message.get("reason"), actual));
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

        const auto edge = co_await transaction.query(R"sql(
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
JOIN device_model p ON p.device_id = d.id
             AND p.deleted_at IS NULL AND p.enabled
LEFT JOIN edge_node n ON n.id = l.edge_node_id
WHERE d.id = $1::uuid AND d.deleted_at IS NULL AND d.status = 'enabled' LIMIT 1)sql",
                                                      service::common::dbParams(deviceId));
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
JOIN device_model p ON p.device_id = d.id
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
JOIN device_model p ON p.device_id = d.id
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
JOIN device_model p ON p.device_id = d.id
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
        const auto rows = co_await transaction.query(sql, service::common::dbParams(device.id));
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
