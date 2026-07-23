#pragma once

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include <ruvia/core/Task.h>

#include "service/common/message/contract.h"
#include "service/features/collector/stream.h"

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
        redis, "iot:state:device:" + std::string(deviceCode));
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
