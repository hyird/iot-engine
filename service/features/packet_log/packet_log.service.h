#pragma once
#include <ruvia/web/db/DbQuery.h>
#include "service/common/message.h"
#include "service/features/packet_log/packet_log.entity.h"
#include "service/utils/redis.h"
#include "service/features/live/live.service.h"

namespace service::packet_log {
class DebugPacketService {
  public:
    template <typename Context>
    static ruvia::Task<void> publishStoredHistory(Context& context,
        const std::vector<message::ParsedDeviceMessage>& messages) {
        if (messages.empty()) co_return;
        using Op = ruvia::DbBinaryOperator;
        ruvia::DbQuery q;
        std::vector<ruvia::DbExpression> ids;
        for (const auto& message : messages)
            ids.push_back(q.cast(q.value(message.messageId), ruvia::DbDataType::kUuid));
        q.select({q.column(DebugHistoryEntity::columnName<"id">(), "h"),
                  q.column(DebugHistoryEntity::columnName<"device_id">(), "h"),
                  q.column(DebugHistoryEntity::columnName<"link_id">(), "h"),
                  q.column(DebugHistoryEntity::columnName<"data">(), "h"),
                  q.column(DebugHistoryEntity::columnName<"raw_payload_hex">(), "h"),
                  q.column(DebugHistoryEntity::columnName<"source">(), "h")})
            .from(DebugHistoryEntity::tableName(), "h")
            .join(ruvia::DbJoinType::kInner, DebugDeviceEntity::tableName(),
                q.binary(q.column(DebugDeviceEntity::columnName<"id">(), "d"), Op::kEqual,
                         q.column(DebugHistoryEntity::columnName<"device_id">(), "h")), "d")
            .join(ruvia::DbJoinType::kInner, DebugLinkEntity::tableName(),
                q.binary(q.column(DebugLinkEntity::columnName<"id">(), "l"), Op::kEqual,
                         q.column(DebugHistoryEntity::columnName<"link_id">(), "h")), "l")
            .where(q.binary(q.column(DebugHistoryEntity::columnName<"id">(), "h"), Op::kIn, q.list(ids)))
            .andWhere(q.binary(q.column(DebugDeviceEntity::columnName<"debug_enabled">(), "d"), Op::kOr,
                               q.column(DebugLinkEntity::columnName<"debug_enabled">(), "l")));
        const auto rows = co_await context.db("telemetry-history").query(q);
        for (const auto& row : rows) {
            const auto id = row[0].value().value_or("");
            const auto found = std::find_if(messages.begin(), messages.end(),
                [&](const auto& value) { return value.messageId == id; });
            if (found == messages.end()) continue;
            const auto raw = message::rawPayloadsFromJson(row[4].value().value_or("[]"));
            std::size_t index = 0;
            for (const auto& bytes : raw) {
                co_await append(context.redis(), row[2].value().value_or(""),
                    row[1].value().value_or(""), "RX",
                    row[5].value().value_or("") == "edge" ? "edge" : "collector", "", bytes,
                    found->occurredAtMs, false, "history:" + std::string(id) + ":" + std::to_string(index++),
                    "stored", "", id, row[3].value().value_or("{}"));
            }
        }
    }

    template <typename Redis>
    static ruvia::Task<void> append(const Redis& redis, std::string_view linkId,
        std::string_view deviceId, std::string_view direction, std::string_view source,
        std::string_view address, std::span<const std::uint8_t> payload, std::int64_t time,
        bool deviceOnly = false, std::string_view eventId = {},
        std::string_view status = {}, std::string_view reason = {},
        std::string_view historyId = {}, std::string_view parsedJson = {}) {
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
            const auto stableId = eventId.empty() ? std::string{} : std::string(eventId) + ":" + offsetText;
            const std::vector<std::string_view> args{"link_id", linkId, "device_id", deviceId,
                "direction", direction, "source", source, "address", address,
                "payload_hex", hex, "time_ms", timestamp, "offset", offsetText,
                "event_id", stableId, "status", status, "reason", reason,
                "history_id", historyId, "parsed_json", parsedJson};
            const auto result = co_await redis.eval(script, keys, args);
            if (result.kind() == ruvia::RedisValue::Kind::kError)
                message::redis::throwValue("append debug packet", result);
        }
        co_await service::live::publish(redis, "packet-debug");
    }
};
}
