#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include "service/common/instance.h"

namespace service::message {

enum class WorkerStreamTask : std::uint8_t {
    Telemetry,
    TelemetryHistory,
    TelemetryLatest,
    TelemetryAlerts,
    TelemetryDelivery,
    Freshness,
    FreshnessAlerts,
    CommandResult,
    Webhook,
    Reconciler,
    EdgeProjector,
    EdgeDispatcher,
    Count,
};

inline constexpr std::string_view kWorkerWakeStreamPrefix{"iot:service:worker:"};
inline constexpr std::size_t kWorkerWakeCapacity = 100000;

inline std::string workerWakeStream(std::size_t workerIndex,
                                    std::string_view instance = service::runtime::instanceId()) {
    return std::string(kWorkerWakeStreamPrefix) + std::string(instance) + ":" + std::to_string(workerIndex) + ":wake";
}

inline constexpr std::array<std::string_view,
                            static_cast<std::size_t>(WorkerStreamTask::Count)>
    kWorkerStreamTaskNames{"telemetry", "telemetry-history", "telemetry-latest", "telemetry-alerts", "telemetry-delivery", "freshness", "freshness-alerts", "command-result", "webhook",
                           "reconciler", "edge-projector", "edge-dispatcher"};

inline constexpr std::string_view workerStreamTaskName(WorkerStreamTask task) {
    return kWorkerStreamTaskNames.at(static_cast<std::size_t>(task));
}

inline std::optional<WorkerStreamTask> workerStreamTask(std::string_view name) {
    for (std::size_t index = 0; index < kWorkerStreamTaskNames.size(); ++index)
        if (kWorkerStreamTaskNames[index] == name)
            return static_cast<WorkerStreamTask>(index);
    return std::nullopt;
}

inline std::atomic_size_t workerWakeRoutingCount{1};

inline void configureWorkerWakeRouting(std::size_t workerCount) {
    if (workerCount == 0)
        throw std::invalid_argument("Worker wake routing requires Service Workers");
    workerWakeRoutingCount.store(workerCount, std::memory_order_release);
}

inline std::size_t workerForPartition(std::size_t partition) noexcept {
    return partition % workerWakeRoutingCount.load(std::memory_order_acquire);
}

} // namespace service::message
