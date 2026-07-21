#pragma once

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "service/common/bridge/message.contract.h"
#include "service/modules/southbridge/queue/redis_stream.h"

namespace service::southbridge {

class RedisProtocolTaskQueue {
  public:
    explicit RedisProtocolTaskQueue(bridge::RedisEndpoint endpoint)
        : connection_(std::move(endpoint)) {}

    [[nodiscard]] std::optional<std::string> enqueue(bridge::ProtocolTask task,
                                                     bridge::ProtocolTaskPriority priority) {
        if (task.messageId.empty())
            task.messageId = bridge::nextMessageId();
        if (task.createdAtMs == 0)
            task.createdAtMs = bridge::utcNowMilliseconds();
        const auto stream = streamFor(priority);
        const auto capacity =
            priority == bridge::ProtocolTaskPriority::High ? kHighCapacity : kNormalCapacity;
        std::lock_guard lock(mutex_);
        return connection_.addGroupedBounded(stream, bridge::protocolTaskFields(task), capacity,
                                             bridge::protocolTaskDepthKey(task.groupKey),
                                             kGroupCapacity);
    }

    static constexpr std::size_t kHighCapacity = 2048;
    static constexpr std::size_t kNormalCapacity = 10000;
    static constexpr std::size_t kGroupCapacity = 256;

  private:
    static std::string_view streamFor(bridge::ProtocolTaskPriority priority) noexcept {
        return priority == bridge::ProtocolTaskPriority::High ? bridge::kProtocolHighTaskStream
                                                              : bridge::kProtocolNormalTaskStream;
    }

    bridge::RedisStreamConnection connection_;
    std::mutex mutex_;
};

} // namespace service::southbridge
