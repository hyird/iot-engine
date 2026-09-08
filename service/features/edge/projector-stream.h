#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <ruvia/core/Task.h>

#include "service/common/message/contract.h"
#include "service/features/collector/stream.h"

namespace service::edge::projector_stream {

inline constexpr std::string_view kIngressKind{"ingress"};
inline constexpr std::string_view kMetadataKind{"metadata"};

inline std::string stream(std::size_t workerIndex) {
    return "iot:v2:edge:projector:" + std::to_string(workerIndex);
}

template <typename Redis>
ruvia::Task<void> publishIngress(const Redis& redis, std::size_t workerIndex,
                                 std::string_view wire,
                                 std::int64_t receivedAtMs) {
    const auto partition = workerIndex;
    (void)co_await service::message::redis::publishAndWake(
        redis, stream(partition),
        {{"kind", std::string(kIngressKind)}, {"wire", std::string(wire)},
         {"received_at_ms", std::to_string(receivedAtMs)}},
        service::message::workerForPartition(partition), service::message::WorkerStreamTask::EdgeProjector, 100000);
}

template <typename Redis>
ruvia::Task<void> publishMetadata(const Redis& redis, std::size_t workerIndex,
                                  std::string_view nodeId) {
    (void)workerIndex;
    const auto partition = service::message::shard::index(nodeId);
    (void)co_await service::message::redis::publishAndWake(
        redis, stream(partition),
        {{"kind", std::string(kMetadataKind)}, {"node_id", std::string(nodeId)}},
        service::message::workerForPartition(partition), service::message::WorkerStreamTask::EdgeProjector, 100000);
}

} // namespace service::edge::projector_stream
