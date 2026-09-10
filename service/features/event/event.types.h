#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "service/common/uuid.h"

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

inline constexpr std::string_view kWorkerWakeStreamPrefix{ "iot:service:worker:" };
inline constexpr std::size_t kWorkerWakeCapacity = 100000;

inline std::string workerWakeStream(std::size_t workerIndex, std::string_view instance = service::runtime::instanceId()) {
    return std::string(kWorkerWakeStreamPrefix) + std::string(instance) + ":" + std::to_string(workerIndex) + ":wake";
}

inline std::string sharedWakeStream(std::string_view instance = service::runtime::instanceId()) {
    return "iot:service:" + std::string(instance) + ":work-available";
}

inline std::string workerWakeStream(std::optional<std::size_t> workerIndex, std::string_view instance = service::runtime::instanceId()) {
    return workerIndex ? workerWakeStream(*workerIndex, instance) : sharedWakeStream(instance);
}

inline constexpr std::array<std::string_view, static_cast<std::size_t>(WorkerStreamTask::Count)>
    kWorkerStreamTaskNames{ "telemetry", "telemetry-history", "telemetry-latest", "telemetry-alerts", "telemetry-delivery", "freshness", "freshness-alerts", "command-result", "webhook", "reconciler", "edge-projector", "edge-dispatcher" };

inline constexpr std::string_view workerStreamTaskName(WorkerStreamTask task) {
    return kWorkerStreamTaskNames.at(static_cast<std::size_t>(task));
}

inline std::optional<WorkerStreamTask> workerStreamTask(std::string_view name) {
    for (std::size_t index = 0; index < kWorkerStreamTaskNames.size(); ++index) {
        if (kWorkerStreamTaskNames[index] == name) {
            return static_cast<WorkerStreamTask>(index);
        }
    }
    return std::nullopt;
}

} // namespace service::message
