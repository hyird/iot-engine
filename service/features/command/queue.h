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

#include "service/common/message/contract.h"
#include "service/features/collector/stream.h"
#include "service/features/telemetry/latest.h"

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

// The caller owns authorization, remote-control validation and protocol-frame compilation. This
// producer owns only worker-affine routing and the Redis task contract.
template <typename Context>
ruvia::Task<std::string> enqueue(Context& context, message::ProtocolTask task,
                                 const DeviceRoute& route, bool highPriority = true) {
    if (task.deviceCode.empty() || task.protocol.empty() || task.linkId.empty() ||
        (task.payload.empty() && task.elements.empty()))
        throw std::invalid_argument("protocol command task is incomplete");
    task.messageId = task.messageId.empty() ? message::nextMessageId() : task.messageId;
    task.groupKey = task.groupKey.empty() ? "device:" + task.deviceId : task.groupKey;
    task.connectionId = route.connectionId;
    task.sessionEpoch = route.sessionEpoch;
    task.createdAtMs = task.createdAtMs == 0 ? message::utcNowMilliseconds() : task.createdAtMs;
    task.attempt = std::max<std::int64_t>(1, task.attempt);
    task.maxAttempts = std::max(task.attempt, task.maxAttempts);
    (void)co_await message::redis::publish(
        context.redis(), message::commandStream(route.workerIndex, highPriority, route.instanceId),
        message::protocolTaskFields(task), 10000);
    co_return task.messageId;
}

template <typename Context>
ruvia::Task<std::string> enqueue(Context& context, message::ProtocolTask task,
                                 bool highPriority = true) {
    const auto route = co_await deviceRoute(context.redis(), task.deviceId);
    co_return co_await enqueue(context, std::move(task), route, highPriority);
}

} // namespace service::command
