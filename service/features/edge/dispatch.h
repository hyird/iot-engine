#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include <ruvia/core/Task.h>

#include "service/common/message/contract.h"
#include "service/features/collector/stream.h"
#include "service/features/edge/session.h"

namespace service::edge::dispatch {

inline constexpr std::string_view kGroup{"iot-engine:edge-dispatch"};
inline constexpr std::string_view kNodeKind{"node"};

inline std::string stream(std::size_t workerIndex) {
    return "iot:edge:dispatch:" + std::to_string(workerIndex);
}

struct Event final {
    std::string kind;
    std::string nodeId;
};

template <typename Redis>
ruvia::Task<void> notifyNode(const Redis& redis, std::string_view nodeId) {
    if (nodeId.empty())
        co_return;
    const auto session = co_await redis.get(session_state::key(nodeId));
    if (!session)
        co_return;
    const auto owner = session_state::workerIndex(
        std::string_view(session->data(), session->size()));
    if (!owner)
        co_return;
    (void)co_await service::message::redis::publishAndWake(
        redis, stream(*owner),
        {{"kind", std::string(kNodeKind)}, {"node_id", std::string(nodeId)}},
        *owner, service::message::WorkerStreamTask::EdgeDispatcher, 10000);
}

inline Event eventFrom(const service::message::StreamMessage& message) {
    return {
        .kind = std::string(message.get("kind")),
        .nodeId = std::string(message.get("node_id")),
    };
}

} // namespace service::edge::dispatch
