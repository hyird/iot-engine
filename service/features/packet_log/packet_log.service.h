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
            .where(q.binary(q.column(DebugHistoryEntity::columnName<"id">(), "h"), Op::kIn, q.list(ids)));
        const auto rows = co_await context.db("telemetry-history").query(q);
        for (const auto& input : messages) {
            const auto found = std::find_if(rows.begin(), rows.end(),
                [&](const auto& row) { return row[0].value().value_or("") == input.messageId; });
            const bool stored = found != rows.end();
            co_await updateHistoryState(context.redis(), input, stored ? "stored" : "skipped",
                stored ? std::string_view(input.messageId) : std::string_view{},
                stored ? (*found)[3].value().value_or("{}") : std::string_view(input.valuesJson));
        }
    }

    template <typename Redis>
    static ruvia::Task<void> updateHistoryState(const Redis& redis, const message::ParsedDeviceMessage& input,
        std::string_view status, std::string_view historyId = {}, std::string_view parsedJson = {}) {
        // 旧固件没有关联标识，不能推测关联，也不能另造一条入库报文。
        if (input.rawPacketIds.size() != input.rawPayloads.size()) co_return;
        std::size_t index = 0;
        for (const auto& bytes : input.rawPayloads) {
            const auto& identity = input.rawPacketIds[index];
            ++index;
            co_await recordPacket(redis, input.linkId, input.deviceId, "RX",
                input.source == "edge" ? "edge" : "collector", "", bytes, input.occurredAtMs, false,
                identity, status, {}, historyId,
                parsedJson.empty() ? std::string_view(input.valuesJson) : parsedJson, {}, true);
        }
    }

    template <typename Redis>
    static ruvia::Task<void> recordPacket(const Redis& redis, std::string_view linkId,
        std::string_view deviceId, std::string_view direction, std::string_view source,
        std::string_view address, std::span<const std::uint8_t> payload, std::int64_t time,
        bool deviceOnly = false, std::string_view eventId = {},
        std::string_view status = {}, std::string_view reason = {},
        std::string_view historyId = {}, std::string_view parsedJson = {},
        std::string_view replyToPacketId = {}, bool updateOnly = false, std::size_t baseOffset = 0) {
        if (linkId.empty() || payload.empty()) co_return;
        // 单个 Hash 保存报文；两个索引仅保存 packet_id，不复制正文。
        static constexpr std::string_view script = R"lua(
local prefix = 'iot:debug:v2:packet:'
local fields = {}
for i=1,#ARGV,2 do fields[ARGV[i]] = ARGV[i+1] end
local id = fields.event_id
local packet = prefix .. id
for _,key in ipairs(KEYS) do
    local kind = redis.call('TYPE',key).ok
    if kind ~= 'none' and kind ~= 'zset' then return redis.error_reply('debug index must be a sorted set') end
end
local kind = redis.call('TYPE',packet).ok
if kind ~= 'none' and kind ~= 'hash' then return redis.error_reply('debug packet must be a hash') end
if kind == 'none' and fields.update_only == '1' then return 0 end
fields.update_only = nil
if kind == 'hash' then
    for _,name in ipairs({'link_id','device_id','direction','payload_hex'}) do
        local previous=redis.call('HGET',packet,name)
        if previous and previous~='' and fields[name] and fields[name]~='' and previous~=fields[name] then
            return redis.error_reply('packet identity conflict')
        end
    end
end
local clock = redis.call('TIME')
local now = tonumber(clock[1])*1000 + math.floor(tonumber(clock[2])/1000)
local created = tonumber(redis.call('HGET',packet,'created_ms')) or now
local ranks = {
 transport_status={sending=1, sent=2, received=2, failed=3},
 response_status={waiting=1, success=2, failed=2, not_applicable=2},
 parse_status={pending=1, success=2, failed=2, not_applicable=2},
 storage_status={pending=1, failed=2, skipped=3, stored=4, not_applicable=4}
}
local changed = false
local stored = redis.call('HGET',packet,'storage_status') == 'stored'
for name,value in pairs(fields) do
    if value ~= '' then
        local previous = redis.call('HGET',packet,name)
        local allowed = true
        if name == 'parsed_json' and stored and (not fields.history_id or fields.history_id == '') then
            allowed = false
        elseif ranks[name] and previous and previous ~= value then
            allowed = (ranks[name][value] or 0) > (ranks[name][previous] or 0)
        elseif (name == 'payload_hex' or name == 'time_ms' or name == 'direction') and previous then
            allowed = previous == value
        end
        if allowed and previous ~= value then redis.call('HSET',packet,name,value); changed = true end
    end
end
redis.call('HSET',packet,'created_ms',created)
if changed then redis.call('HINCRBY',packet,'revision',1) end
redis.call('PEXPIREAT',packet,created+86400000)
local function collect(candidate)
    local item=prefix..candidate
    local link=redis.call('HGET',item,'link_index')
    local device=redis.call('HGET',item,'device_index')
    if (not link or not redis.call('ZSCORE',link,candidate)) and
       (not device or not redis.call('ZSCORE',device,candidate)) then redis.call('DEL',item) end
end
for _,key in ipairs(KEYS) do
    redis.call('HSET',packet,string.find(key,':device:',1,true) and 'device_index' or 'link_index',key)
    redis.call('ZADD',key,'NX',created,id)
    local expired=redis.call('ZRANGEBYSCORE',key,'-inf',now-86400000)
    redis.call('ZREMRANGEBYSCORE',key,'-inf',now-86400000)
    for _,candidate in ipairs(expired) do collect(candidate) end
    local excess=redis.call('ZCARD',key)-500
    if excess>0 then
        local removed=redis.call('ZRANGE',key,0,excess-1)
        redis.call('ZREMRANGEBYRANK',key,0,excess-1)
        for _,candidate in ipairs(removed) do collect(candidate) end
    end
    redis.call('EXPIRE',key,86400)
end
return changed and 1 or 0
)lua";
        std::vector<std::string> storage;
        if (!deviceOnly) storage.push_back(DebugPacketStorage::key("link", linkId));
        if (!deviceId.empty()) storage.push_back(DebugPacketStorage::key("device", deviceId));
        if (storage.empty()) co_return;
        const std::vector<std::string_view> keys(storage.begin(), storage.end());
        const auto identity = eventId.empty() ? message::nextMessageId() : std::string(eventId);
        for (std::size_t offset = 0; offset < payload.size(); offset += 4096) {
            const auto chunk = payload.subspan(offset, std::min<std::size_t>(4096, payload.size() - offset));
            const auto hex = message::toHex(std::vector<std::uint8_t>(chunk.begin(), chunk.end()));
            const auto timestamp = std::to_string(time);
            const auto offsetText = std::to_string(baseOffset + offset);
            const auto stableId = identity + ":" + offsetText;
            const auto transportStatus = direction == "RX" ? "received" :
                status == "failed" && reason == "socket_write_failed" ? "failed" :
                status == "sending" ? "sending" : "sent";
            const auto responseStatus = status == "waiting" || status == "success" ? status :
                status == "failed" && reason != "socket_write_failed" ? std::string_view("failed") : std::string_view{};
            const auto parseStatus = !parsedJson.empty() ? "success" : status == "parse_failed" ? "failed" :
                status == "transport_only" ? "not_applicable" : direction == "RX" ? "pending" : "not_applicable";
            const auto storageStatus = !historyId.empty() ? "stored" : status == "storage_failed" ? "failed" :
                status == "skipped" ? "skipped" : status == "not_applicable" || status == "transport_only" || status == "parse_failed" ? "not_applicable" : direction == "RX" ? "pending" : "not_applicable";
            const std::vector<std::string_view> args{DebugPacketHash::columnName<"link_id">(), linkId, DebugPacketHash::columnName<"device_id">(), deviceId,
                DebugPacketHash::columnName<"direction">(), direction, DebugPacketHash::columnName<"source">(), source, DebugPacketHash::columnName<"address">(), address,
                DebugPacketHash::columnName<"payload_hex">(), hex, DebugPacketHash::columnName<"time_ms">(), timestamp, DebugPacketHash::columnName<"offset">(), offsetText,
                DebugPacketHash::columnName<"event_id">(), stableId, DebugPacketHash::columnName<"transport_status">(), transportStatus, DebugPacketHash::columnName<"response_status">(), responseStatus,
                DebugPacketHash::columnName<"parse_status">(), parseStatus, DebugPacketHash::columnName<"storage_status">(), storageStatus, DebugPacketHash::columnName<"reason">(), reason, DebugPacketHash::columnName<"reply_to_packet_id">(), replyToPacketId,
                DebugPacketHash::columnName<"history_id">(), historyId, DebugPacketHash::columnName<"parsed_json">(), parsedJson, "update_only", updateOnly ? "1" : "0"};
            const auto result = co_await redis.eval(script, keys, args);
            if (result.kind() == ruvia::RedisValue::Kind::kError)
                message::redis::throwValue("append debug packet", result);
        }
        co_await service::live::publish(redis, "packet-debug");
    }
};
}
