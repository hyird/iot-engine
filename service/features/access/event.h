#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/core/Task.h>

#include "service/common/message/contract.h"
#include "service/features/access/stream.h"
#include "service/features/collector/stream.h"

namespace service::access::event {

inline constexpr std::int64_t kPublicationTtlSeconds = 7 * 24 * 60 * 60;

inline std::string publicationKey(std::string_view eventId, std::string_view eventType) {
    return "iot:open-access:event:published:" + std::string(eventType) + ":" +
           std::string(eventId);
}

inline constexpr std::string_view kPublishScript = R"lua(
if redis.call('EXISTS', KEYS[2]) ~= 0 then return false end
local arguments = {'MAXLEN', '~', ARGV[1], '*'}
for index = 5, #ARGV do arguments[#arguments + 1] = ARGV[index] end
local id = redis.call('XADD', KEYS[1], unpack(arguments))
redis.call('SET', KEYS[2], '1', 'EX', ARGV[2])
redis.call('XADD', KEYS[3], 'MAXLEN', '~', ARGV[3], '*', 'task', ARGV[4])
return id
)lua";

template <typename Pipeline>
inline void queue(Pipeline& pipeline, std::string_view scriptSha,
                  std::string_view eventId,
                  std::string_view eventType, std::string_view deviceId,
                  std::string_view deviceCode, std::int64_t occurredAtMs,
                  std::string_view dataJson) {
    const auto publishedKey = publicationKey(eventId, eventType);
    const auto occurredAt = std::to_string(occurredAtMs);
    const auto partition = stream::partition(deviceId);
    const auto outputStream = stream::event(partition);
    const auto wakeStream = service::message::workerWakeStream(
        service::message::workerForPartition(partition));
    const std::array<std::string_view, 3> keys{outputStream, publishedKey,
                                                wakeStream};
    const std::array<std::string_view, 16> arguments{
        "100000",
        "604800",
        "100000",
        "webhook",
        "event_id",
        eventId,
        "event_type",
        eventType,
        "device_id",
        deviceId,
        "device_code",
        deviceCode,
        "occurred_at_ms",
        occurredAt,
        "data_json",
        dataJson,
    };
    message::redis::queueEvalSha(pipeline, scriptSha, keys, arguments);
}

template <typename Redis>
inline ruvia::Task<void> publish(const Redis& redis, std::string_view eventId,
                                 std::string_view eventType, std::string_view deviceId,
                                 std::string_view deviceCode, std::int64_t occurredAtMs,
                                 std::string_view dataJson) {
    const auto partition = stream::partition(deviceId);
    const std::vector<std::string> keyStore{
        stream::event(partition), publicationKey(eventId, eventType),
        service::message::workerWakeStream(
            service::message::workerForPartition(partition))};
    const std::vector<std::string> argumentStore{
        "100000",
        std::to_string(kPublicationTtlSeconds),
        std::to_string(service::message::kWorkerWakeCapacity),
        std::string(service::message::workerStreamTaskName(
            service::message::WorkerStreamTask::Webhook)),
        "event_id",
        std::string(eventId),
        "event_type",
        std::string(eventType),
        "device_id",
        std::string(deviceId),
        "device_code",
        std::string(deviceCode),
        "occurred_at_ms",
        std::to_string(occurredAtMs),
        "data_json",
        std::string(dataJson),
    };
    const std::vector<std::string_view> keys(keyStore.begin(), keyStore.end());
    const std::vector<std::string_view> arguments(argumentStore.begin(), argumentStore.end());
    const auto reply = co_await redis.eval(kPublishScript, keys, arguments);
    if (!reply.null() && reply.kind() != ruvia::RedisValue::Kind::kString)
        message::redis::throwValue("publish open-access event", reply);
}

template <typename Redis>
inline ruvia::Task<void> publishMany(
    const Redis& redis, const std::vector<message::ParsedDeviceMessage>& messages) {
    if (messages.empty())
        co_return;
    const auto scriptSha = co_await redis.scriptLoad(kPublishScript);
    auto pipeline = redis.pipeline();
    for (const auto& parsed : messages) {
        const auto eventType =
            parsed.valuesJson.find("\"type\":\"JPEG\"") != std::string::npos
                ? "device.image.reported"
                : "device.data.reported";
        queue(pipeline, scriptSha, parsed.messageId, eventType, parsed.deviceId,
              parsed.deviceCode, parsed.observedAtMs, parsed.valuesJson);
    }
    const auto replies = co_await std::move(pipeline).exec();
    message::redis::requirePipelineSuccess("publish open-access events", replies);
}

} // namespace service::access::event
