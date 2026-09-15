#pragma once
#include "service/features/packet_log/packet_log.entity.h"
#include "service/utils/redis.h"
#include "service/features/live/live.service.h"

namespace service::packet_log {
class DebugPacketService {
  public:
    template <typename Redis>
    static ruvia::Task<void> append(const Redis& redis, std::string_view linkId,
        std::string_view deviceId, std::string_view direction, std::string_view source,
        std::string_view address, std::span<const std::uint8_t> payload, std::int64_t time,
        bool deviceOnly = false) {
        if (linkId.empty() || payload.empty()) co_return;
        // 每条记录最多 4 KiB；较大的传输片段完整拆开，保留原始顺序。
        static constexpr std::string_view script = R"lua(
for _, key in ipairs(KEYS) do
    redis.call('XADD', key, 'MAXLEN', '=', 500, '*', unpack(ARGV))
    redis.call('EXPIRE', key, 86400)
end
return 1
)lua";
        std::vector<std::string> storage;
        if (!deviceOnly) storage.push_back(DebugPacketStream::key("link", linkId));
        if (!deviceId.empty()) storage.push_back(DebugPacketStream::key("device", deviceId));
        if (storage.empty()) co_return;
        const std::vector<std::string_view> keys(storage.begin(), storage.end());
        for (std::size_t offset = 0; offset < payload.size(); offset += 4096) {
            const auto chunk = payload.subspan(offset, std::min<std::size_t>(4096, payload.size() - offset));
            const auto hex = message::toHex(std::vector<std::uint8_t>(chunk.begin(), chunk.end()));
            const auto timestamp = std::to_string(time);
            const auto offsetText = std::to_string(offset);
            const std::vector<std::string_view> args{"link_id", linkId, "device_id", deviceId,
                "direction", direction, "source", source, "address", address,
                "payload_hex", hex, "time_ms", timestamp, "offset", offsetText};
            const auto result = co_await redis.eval(script, keys, args);
            if (result.kind() == ruvia::RedisValue::Kind::kError)
                message::redis::throwValue("append debug packet", result);
        }
        co_await service::live::publish(redis, "packet-debug");
    }
};
}
