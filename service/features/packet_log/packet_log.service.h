#pragma once
#include <ruvia/web/db/DbQuery.h>
#include "service/common/message.h"
#include "service/features/packet_log/packet_log.entity.h"
#include "service/utils/redis.h"
#include "service/features/live/live.service.h"

namespace service::packet_log {
class DebugPacketService {
  public:
    template <typename Redis>
    static ruvia::Task<void> finishAcquisition(const Redis& redis, std::string_view acquisitionId,
        std::string_view state) {
        if (acquisitionId.empty() || (state != "success" && state != "partial" && state != "failed"))
            throw std::invalid_argument("invalid acquisition completion");
        static constexpr std::string_view script = R"lua(
if redis.call('EXISTS',KEYS[1])==0 then return 0 end
local previous=redis.call('HGET',KEYS[1],'state')
if previous and previous~='running' then return 0 end
local clock=redis.call('TIME')
local now=tonumber(clock[1])*1000+math.floor(tonumber(clock[2])/1000)
redis.call('HSET',KEYS[1],'state',ARGV[1],'finished_at_ms',now)
if ARGV[1]~='success' and redis.call('HGET',KEYS[1],'storage_status')~='stored' then redis.call('HSET',KEYS[1],'storage_status','skipped') end
return 1
)lua";
        const std::string key = std::string(DebugAcquisitionStorage::prefix) + std::string(acquisitionId);
        const std::vector<std::string_view> keys{key}, args{state};
        const auto result = co_await redis.eval(script, keys, args);
        if (result.kind() == ruvia::RedisValue::Kind::kError)
            message::redis::throwValue("finish debug acquisition", result);
        co_await service::live::publish(redis, "packet-debug");
    }
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
        if (input.acquisitionId.empty() || input.rawPacketIds.size() != input.rawPayloads.size())
            throw std::invalid_argument("acquisition identity and packet identities are required");
        std::size_t index = 0;
        for (const auto& bytes : input.rawPayloads) {
            const auto& identity = input.rawPacketIds[index];
            ++index;
            co_await recordPacket(redis, input.linkId, input.deviceId, "RX",
                input.source == "edge" ? "edge" : "collector", "", bytes, input.occurredAtMs, false,
                identity, status, {}, historyId,
                parsedJson.empty() ? std::string_view(input.valuesJson) : parsedJson, {}, true, 0, input.acquisitionId);
        }
    }

    template <typename Redis>
    static ruvia::Task<void> recordPacket(const Redis& redis, std::string_view linkId,
        std::string_view deviceId, std::string_view direction, std::string_view source,
        std::string_view address, std::span<const std::uint8_t> payload, std::int64_t time,
        bool deviceOnly = false, std::string_view eventId = {},
        std::string_view status = {}, std::string_view reason = {},
        std::string_view historyId = {}, std::string_view parsedJson = {},
        std::string_view replyToPacketId = {}, bool updateOnly = false, std::size_t baseOffset = 0,
        std::string_view acquisitionId = {}) {
        if (acquisitionId.empty()) throw std::invalid_argument("debug packet requires acquisition ID");
        if (linkId.empty()) co_return;
        // 索引保存轮次 ID；每轮持有独立报文集合，状态更新只修改原记录。
        static constexpr std::string_view script = R"lua(
local fields={}
for i=1,#ARGV,2 do fields[ARGV[i]]=ARGV[i+1] end
local prefix='iot:debug:v3:'
local acquisition=fields.acquisition_id
if not acquisition or acquisition=='' then return redis.error_reply('missing acquisition ID') end
local round=fields.acquisition_prefix..acquisition
fields.acquisition_prefix=nil
local members=round..':packets'
local packet=prefix..'packet:'..fields.event_id
local hasPacket=fields.payload_hex and fields.payload_hex~=''
local clock=redis.call('TIME')
local now=tonumber(clock[1])*1000+math.floor(tonumber(clock[2])/1000)
if redis.call('EXISTS',round..':retired')==1 then return 0 end
local function validate(key,expected)
    local kind=redis.call('TYPE',key).ok
    return kind=='none' or kind==expected
end
if not validate(round,'hash') or not validate(packet,'hash') or not validate(members,'zset') then
    return redis.error_reply('invalid acquisition storage type')
end
for _,key in ipairs(KEYS) do
    if not validate(key,'zset') then return redis.error_reply('invalid acquisition index type') end
end
local exists=redis.call('EXISTS',packet)==1
if fields.update_only=='1' and ((hasPacket and not exists) or redis.call('EXISTS',round)==0) then return 0 end
fields.update_only=nil
for _,name in ipairs({'acquisition_id','link_id','device_id','direction'}) do
    local previous=redis.call('HGET',packet,name)
    if previous and previous~='' and fields[name] and fields[name]~='' and previous~=fields[name] then
        return redis.error_reply('packet identity conflict')
    end
end
for _,name in ipairs({'link_id','device_id'}) do
    local previous=redis.call('HGET',round,name)
    if previous and previous~='' and fields[name] and fields[name]~='' and previous~=fields[name] then
        return redis.error_reply('acquisition scope conflict')
    end
end
if hasPacket and not exists and redis.call('ZCARD',members)>=4096 then return redis.error_reply('acquisition packet limit exceeded') end
local created=tonumber(redis.call('HGET',round,'created_ms')) or now
local expires=created+86400000
local ranks={
    transport_status={sending=1,sent=2,received=2,failed=3},
    response_status={waiting=1,success=2,failed=2,not_applicable=2},
    parse_status={pending=1,success=2,failed=2,not_applicable=2},
    storage_status={pending=1,failed=2,skipped=3,stored=4,not_applicable=4}
}
local changed=false
local fragment='fragment:'..(fields.offset or '0')
if hasPacket then
    local offset=tonumber(fields.offset)
    if not offset or offset<0 or offset%4096~=0 or offset>1048576 or #fields.payload_hex>8192 then
        return redis.error_reply('invalid packet fragment')
    end
    local previous=redis.call('HGET',packet,fragment)
    if previous and previous~=fields.payload_hex then return redis.error_reply('packet fragment conflict') end
    redis.call('HSET',packet,fragment,fields.payload_hex)
    local offsets={}
    local fragments={}
    local values=redis.call('HGETALL',packet)
    for i=1,#values,2 do
        local position=string.match(values[i],'^fragment:(%d+)$')
        if position then offsets[#offsets+1]=tonumber(position); fragments[tonumber(position)]=values[i+1] end
    end
    table.sort(offsets)
    local payload={}
    for _,position in ipairs(offsets) do payload[#payload+1]=fragments[position] end
    fields.payload_hex=table.concat(payload)
end
local stored=redis.call('HGET',round,'storage_status')=='stored'
if hasPacket then
for name,value in pairs(fields) do
    if value~='' then
        local previous=redis.call('HGET',packet,name)
        local allowed=true
        if name=='parsed_json' and stored and (not fields.history_id or fields.history_id=='') then
            allowed=false
        elseif ranks[name] and previous and previous~=value then
            allowed=(ranks[name][value] or 0)>(ranks[name][previous] or 0)
        elseif (name=='time_ms' or name=='direction') and previous then
            allowed=previous==value
        end
        if allowed and previous~=value then redis.call('HSET',packet,name,value); changed=true end
    end
end
redis.call('HSET',packet,'created_ms',created)
if changed then redis.call('HINCRBY',packet,'revision',1) end
redis.call('PEXPIREAT',packet,expires)
end
redis.call('HSET',round,'acquisition_id',acquisition,'created_ms',created)
local state=fields.acquisition_state
local previousState=redis.call('HGET',round,'state')
local terminal=previousState and previousState~='running'
if state and state~='' and (not previousState or previousState=='running') then
    redis.call('HSET',round,'state',state)
    if state~='running' then redis.call('HSET',round,'finished_at_ms',now) end
end
for _,name in ipairs({'link_id','device_id','source','history_id','parsed_json'}) do
    local value=fields[name]
    if value and value~='' and (name~='parsed_json' or fields.storage_status=='stored' or fields.storage_status=='skipped' or (not stored and (not terminal or fields.acquisition_state~='running'))) then
        redis.call('HSET',round,name,value)
    end
end
if fields.storage_status=='stored' or fields.storage_status=='skipped' or fields.storage_status=='failed' or fields.storage_status=='pending' then
    local previous=redis.call('HGET',round,'storage_status')
    if (ranks.storage_status[fields.storage_status] or 0)>(ranks.storage_status[previous] or 0) then
        redis.call('HSET',round,'storage_status',fields.storage_status)
    end
end
local timestamp=tonumber(fields.time_ms) or now
local started=tonumber(redis.call('HGET',round,'started_at_ms')) or timestamp
local ended=tonumber(redis.call('HGET',round,'last_packet_at_ms')) or timestamp
redis.call('HSET',round,'started_at_ms',math.min(started,timestamp),'last_packet_at_ms',math.max(ended,timestamp))
if hasPacket then
    redis.call('ZADD',members,'NX',timestamp,fields.event_id)
    redis.call('PEXPIREAT',members,expires)
end
redis.call('PEXPIREAT',round,expires)
local function retire(id)
    local value=prefix..'acquisition:'..id
    for _,name in ipairs({'link_index','device_index'}) do
        local index=redis.call('HGET',value,name)
        if index and redis.call('ZSCORE',index,id) then return end
    end
    for _,packetId in ipairs(redis.call('ZRANGE',value..':packets',0,-1)) do
        redis.call('DEL',prefix..'packet:'..packetId)
    end
    redis.call('SET',value..':retired','1','PX',86400000)
    redis.call('DEL',value..':packets',value)
end
for _,key in ipairs(KEYS) do
    redis.call('HSET',round,string.find(key,':device:',1,true) and 'device_index' or 'link_index',key)
    redis.call('ZADD',key,'NX',created,acquisition)
    local expired=redis.call('ZRANGEBYSCORE',key,'-inf',now-86400000)
    redis.call('ZREMRANGEBYSCORE',key,'-inf',now-86400000)
    for _,id in ipairs(expired) do retire(id) end
    local excess=redis.call('ZCARD',key)-100
    if excess>0 then
        local removed=redis.call('ZRANGE',key,0,excess-1)
        redis.call('ZREMRANGEBYRANK',key,0,excess-1)
        for _,id in ipairs(removed) do retire(id) end
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
        for (std::size_t offset = 0; offset < std::max<std::size_t>(1, payload.size()); offset += 4096) {
            const auto chunk = payload.subspan(offset, std::min<std::size_t>(4096, payload.size() - offset));
            const auto hex = message::toHex(std::vector<std::uint8_t>(chunk.begin(), chunk.end()));
            const auto timestamp = std::to_string(time);
            const auto offsetText = std::to_string(baseOffset + offset);
            const auto& stableId = identity;
            const auto transportStatus = direction == "RX" ? "received" :
                status == "failed" && reason == "socket_write_failed" ? "failed" :
                status == "sending" ? "sending" : "sent";
            const auto responseStatus = status == "waiting" || status == "success" ? status :
                status == "failed" && reason != "socket_write_failed" ? std::string_view("failed") : std::string_view{};
            const auto parseStatus = !parsedJson.empty() ? "success" : status == "parse_failed" ? "failed" :
                status == "transport_only" ? "not_applicable" : direction == "RX" ? "pending" : "not_applicable";
            const auto storageStatus = !historyId.empty() ? "stored" : status == "storage_failed" ? "failed" :
                status == "skipped" ? "skipped" : status == "not_applicable" || status == "transport_only" || status == "parse_failed" ? "not_applicable" : direction == "RX" ? "pending" : "not_applicable";
            const auto acquisitionState = status == "acquisition_success" ? "success" :
                status == "acquisition_partial" ? "partial" : status == "acquisition_failed" ? "failed" : "running";
            const std::vector<std::string_view> args{"acquisition_prefix", DebugAcquisitionStorage::prefix, DebugAcquisitionHash::columnName<"acquisition_id">(), acquisitionId, DebugPacketHash::columnName<"link_id">(), linkId, DebugPacketHash::columnName<"device_id">(), deviceId,
                DebugPacketHash::columnName<"direction">(), direction, DebugPacketHash::columnName<"source">(), source, DebugPacketHash::columnName<"address">(), address,
                DebugPacketHash::columnName<"payload_hex">(), hex, DebugPacketHash::columnName<"time_ms">(), timestamp, DebugPacketHash::columnName<"offset">(), offsetText,
                DebugPacketHash::columnName<"event_id">(), stableId, DebugPacketHash::columnName<"transport_status">(), transportStatus, DebugPacketHash::columnName<"response_status">(), responseStatus,
                DebugPacketHash::columnName<"parse_status">(), parseStatus, DebugPacketHash::columnName<"storage_status">(), storageStatus, DebugPacketHash::columnName<"reason">(), reason, DebugPacketHash::columnName<"reply_to_packet_id">(), replyToPacketId,
                DebugPacketHash::columnName<"history_id">(), historyId, DebugPacketHash::columnName<"parsed_json">(), parsedJson,
                "acquisition_state", acquisitionState, "update_only", updateOnly ? "1" : "0"};
            const auto result = co_await redis.eval(script, keys, args);
            if (result.kind() == ruvia::RedisValue::Kind::kError)
                message::redis::throwValue("append debug packet", result);
        }
        co_await service::live::publish(redis, "packet-debug");
    }
};
}
