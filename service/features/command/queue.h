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
    std::vector<std::string> keys;
    keys.reserve(dispatches.size() + 1);
    keys.emplace_back(queueKey);
    std::vector<std::string> arguments{
        kind == PendingQueueKind::Stream ? "stream" : "list",
        std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::hours(24))
                           .count()),
        std::to_string(maxLength), std::to_string(dispatches.size())};
    for (const auto& dispatch : dispatches) {
        const auto& task = dispatch.task;
        if (task.messageId.empty() || task.deviceId.empty() || task.deviceCode.empty() ||
            task.protocol.empty() || !commandIds.emplace(task.messageId).second)
            throw std::invalid_argument("pending command state is incomplete");
        if ((kind == PendingQueueKind::Stream && dispatch.streamFields.empty()) ||
            (kind == PendingQueueKind::List && dispatch.listPayload.empty()))
            throw std::invalid_argument("pending command queue payload is incomplete");
        keys.push_back("iot:state:command:" + task.messageId);
        arguments.push_back(task.messageId);
        arguments.push_back(task.deviceId);
        arguments.push_back(task.deviceCode);
        arguments.push_back(task.protocol);
        arguments.emplace_back(submittedBy);
        arguments.push_back(std::to_string(task.createdAtMs));
        if (kind == PendingQueueKind::Stream) {
            arguments.push_back(std::to_string(dispatch.streamFields.size()));
            for (const auto& field : dispatch.streamFields) {
                arguments.push_back(field.name);
                arguments.push_back(field.value);
            }
        } else {
            arguments.emplace_back("1");
            arguments.push_back(dispatch.listPayload);
        }
    }

    static constexpr std::string_view script = R"lua(
local mode = ARGV[1]
local ttl = ARGV[2]
local maximum = ARGV[3]
local count = tonumber(ARGV[4])
local offset = 5
for task = 1, count do
  local command_id = ARGV[offset]
  local device_id = ARGV[offset + 1]
  local device_code = ARGV[offset + 2]
  local protocol = ARGV[offset + 3]
  local submitted_by = ARGV[offset + 4]
  local created_at = ARGV[offset + 5]
  local item_count = tonumber(ARGV[offset + 6])
  offset = offset + 7
  if mode == 'stream' then
    local values = {'MAXLEN', '~', maximum, '*'}
    for item = 1, item_count * 2 do
      values[#values + 1] = ARGV[offset]
      offset = offset + 1
    end
    redis.call('XADD', KEYS[1], unpack(values))
  else
    redis.call('RPUSH', KEYS[1], ARGV[offset])
    offset = offset + item_count
  end
  redis.call('HSET', KEYS[task + 1],
    'command_id', command_id, 'device_id', device_id,
    'device_code', device_code, 'protocol', protocol,
    'status', 'PENDING', 'submitted_by', submitted_by,
    'created_at_ms', created_at)
  redis.call('PEXPIRE', KEYS[task + 1], ttl)
end
if mode == 'list' then redis.call('LTRIM', KEYS[1], -tonumber(maximum), -1) end
return count
)lua";
    std::vector<std::string_view> keyViews(keys.begin(), keys.end());
    std::vector<std::string_view> argumentViews(arguments.begin(), arguments.end());
    const auto reply = co_await redis.eval(script, keyViews, argumentViews);
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
ruvia::Task<DeviceRoute> deviceRoute(const Redis& redis, std::string_view deviceCode) {
    const auto fields = co_await message::redis::hashEntries(
        redis, service::telemetry::latest::runtimeKey(deviceCode));
    const auto worker = field(fields, "worker_id");
    const auto connection = field(fields, "connection_id");
    const auto epoch = field(fields, "session_epoch");
    if (worker.empty() || connection.empty() || epoch.empty())
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
    task.groupKey = task.groupKey.empty() ? "device:" + task.deviceCode : task.groupKey;
    task.connectionId = route.connectionId;
    task.sessionEpoch = route.sessionEpoch;
    task.createdAtMs = task.createdAtMs == 0 ? message::utcNowMilliseconds() : task.createdAtMs;
    task.attempt = std::max<std::int64_t>(1, task.attempt);
    task.maxAttempts = std::max(task.attempt, task.maxAttempts);
    (void)co_await message::redis::publish(
        context.redis(), message::commandStream(route.workerIndex, highPriority),
        message::protocolTaskFields(task), 10000);
    co_return task.messageId;
}

template <typename Context>
ruvia::Task<std::string> enqueue(Context& context, message::ProtocolTask task,
                                 bool highPriority = true) {
    const auto route = co_await deviceRoute(context.redis(), task.deviceCode);
    co_return co_await enqueue(context, std::move(task), route, highPriority);
}

} // namespace service::command
