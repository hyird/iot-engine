#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

#include <ruvia/core/Task.h>

#include "service/common/message.h"
#include "service/features/edge/session/session.service.h"
#include "service/features/messaging/messaging.transport.h"

namespace service::edge::dispatch {

inline constexpr std::string_view kGroup{ "iot-engine:edge-dispatch" };
inline constexpr std::string_view kNodeKind{ "node" };

inline std::string stream(std::size_t workerIndex, std::string_view instance = service::runtime::instanceId()) {
    return "iot:v2:edge:dispatch:" + std::string(instance) + ":" + std::to_string(workerIndex);
}

struct Event final {
    std::string kind;
    std::string nodeId;
};

template <typename Redis>
ruvia::Task<void> notifyNode(const Redis& redis, std::string_view nodeId) {
    if (nodeId.empty()) {
        co_return;
    }
    const auto session = co_await redis.get(session_state::key(nodeId));
    if (!session) {
        co_return;
    }
    const auto owner = session_state::parse(
        std::string_view(session->data(), session->size())
    );
    if (!owner) {
        co_return;
    }
    (void)co_await service::message::redis::publish(
        redis,
        stream(owner->workerIndex, owner->instanceId),
        { { "kind", std::string(kNodeKind) }, { "node_id", std::string(nodeId) } },
        10000
    );
    (void)co_await service::message::redis::publish(redis, service::message::workerWakeStream(owner->workerIndex, owner->instanceId), { { "task", "edge-dispatcher" } }, 10000);
}

template <typename Redis>
ruvia::Task<void> enqueue(const Redis& redis, std::string_view nodeId, std::string_view wire) {
    if (nodeId.empty() || wire.empty()) {
        co_return;
    }
    const auto key = "iot:edge:egress:" + std::string(nodeId);
    (void)co_await redis.rpush(key, wire);
    (void)co_await redis.ltrim(key, -100, -1);
    co_await notifyNode(redis, nodeId);
}

inline Event eventFrom(const service::message::StreamMessage& message) {
    return {
        .kind = std::string(message.get("kind")),
        .nodeId = std::string(message.get("node_id")),
    };
}

} // namespace service::edge::dispatch

#include <cstdint>

namespace service::edge::projector_stream {

inline constexpr std::string_view kIngressKind{ "ingress" };
inline constexpr std::string_view kMetadataKind{ "metadata" };
inline constexpr std::string_view kStreamPrefix{ "iot:v3:edge:projector:" };
inline constexpr std::string_view kLeasePrefix{ "iot:v3:edge:projector:lease:" };
inline constexpr std::string_view kStreamRegistry{ "iot:v3:edge:projector:streams" };
inline constexpr auto kLeaseTtl = std::chrono::milliseconds(15000);

inline std::string stream(std::size_t workerIndex, std::string_view instance = service::runtime::instanceId()) {
    return std::string(kStreamPrefix) + std::string(instance) + ":" + std::to_string(workerIndex);
}

inline std::string leaseKey(std::size_t workerIndex, std::string_view instance = service::runtime::instanceId()) {
    return std::string(kLeasePrefix) + std::string(instance) + ":" +
        std::to_string(workerIndex);
}

inline std::string ownerToken(std::size_t workerIndex, std::string_view instance = service::runtime::instanceId()) {
    return std::string(instance) + ":" + std::to_string(workerIndex);
}

inline constexpr std::string_view kFencedPublishScript = R"lua(
if redis.call('GET', KEYS[1]) ~= ARGV[1] then return 0 end
-- Data entries may not be trimmed before the projector persists them.  A full
-- queue rejects the device message atomically; the caller then withholds its
-- protocol acknowledgement and retries after reconnecting.
if redis.call('XLEN', KEYS[2]) >= tonumber(ARGV[2]) then return 0 end
local arguments = {'*'}
for index = 5, #ARGV do arguments[#arguments + 1] = ARGV[index] end
local id = redis.call('XADD', KEYS[2], unpack(arguments))
redis.call('SADD', KEYS[3], KEYS[2])
redis.call('XADD', KEYS[4], 'MAXLEN', '~', ARGV[3], '*', 'task', ARGV[4])
return id
)lua";

template <typename Redis>
ruvia::Task<bool> publishIngress(const Redis& redis, std::size_t workerIndex, std::string_view wire, std::int64_t receivedAtMs) {
    const auto instance = service::runtime::instanceId();
    const auto lease = leaseKey(workerIndex, instance);
    const auto streamName = stream(workerIndex, instance);
    const auto registry = std::string(kStreamRegistry);
    const auto wake = service::message::workerWakeStream(workerIndex, instance);
    const std::string maxLength = "100000";
    const std::string wakeCapacity = std::to_string(service::message::kWorkerWakeCapacity);
    const std::string task(
        service::message::workerStreamTaskName(service::message::WorkerStreamTask::EdgeProjector)
    );
    const auto token = ownerToken(workerIndex, instance);
    const std::string receivedAt = std::to_string(receivedAtMs);
    const std::string_view keys[]{ lease, streamName, registry, wake };
    const std::string_view arguments[]{ token, maxLength, wakeCapacity, task, "kind", kIngressKind, "wire", wire, "received_at_ms", receivedAt };
    const auto reply = co_await redis.eval(kFencedPublishScript, keys, arguments);
    if (reply.kind() == ruvia::RedisValue::Kind::kInteger && reply.integer() == 0) {
        co_return false;
    }
    if (reply.kind() != ruvia::RedisValue::Kind::kString) {
        service::message::redis::throwValue("fenced edge ingress", reply);
    }
    co_return true;
}

template <typename Redis>
ruvia::Task<bool> publishMetadata(const Redis& redis, std::size_t workerIndex, std::string_view nodeId, std::string_view instance = service::runtime::instanceId()) {
    const auto lease = leaseKey(workerIndex, instance);
    const auto streamName = stream(workerIndex, instance);
    const auto registry = std::string(kStreamRegistry);
    const auto wake = service::message::workerWakeStream(workerIndex, instance);
    const std::string maxLength = "100000";
    const std::string wakeCapacity = std::to_string(service::message::kWorkerWakeCapacity);
    const std::string task(
        service::message::workerStreamTaskName(service::message::WorkerStreamTask::EdgeProjector)
    );
    const auto token = ownerToken(workerIndex, instance);
    const std::string_view keys[]{ lease, streamName, registry, wake };
    const std::string_view arguments[]{ token, maxLength, wakeCapacity, task, "kind", kMetadataKind, "node_id", nodeId };
    const auto reply = co_await redis.eval(kFencedPublishScript, keys, arguments);
    if (reply.kind() == ruvia::RedisValue::Kind::kInteger && reply.integer() == 0) {
        co_return false;
    }
    if (reply.kind() != ruvia::RedisValue::Kind::kString) {
        service::message::redis::throwValue("fenced edge metadata", reply);
    }
    co_return true;
}

} // namespace service::edge::projector_stream
