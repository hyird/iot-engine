#pragma once
#include "service/features/collector/stream.h"
#include "service/features/event/worker-wake.h"
#include "service/features/telemetry/contract.h"

namespace service::telemetry {
enum class Consumer { Dispatch, History, Latest, Alerts, Delivery };
inline constexpr std::array<std::string_view,5> consumerNames{"dispatch","history","latest","alerts","delivery"};
inline std::string consumerStream(std::size_t partition, Consumer consumer) {
    auto stream = service::message::parsedStream(partition);
    if (consumer != Consumer::Dispatch) stream += ':' + std::string(consumerNames[static_cast<std::size_t>(consumer)]);
    return stream;
}
// The ingress receipt and all four durable copies are committed atomically.
// Consumer streams never trim pending entries. Each consumer owns XACK/XDEL.
inline constexpr std::string_view kFanoutScript = R"lua(
if redis.call('EXISTS',KEYS[1]) ~= 0 then return 0 end
for i=2,5 do
 local kind = redis.call('TYPE',KEYS[i]).ok
 if kind ~= 'none' and kind ~= 'stream' then
  return redis.error_reply('telemetry consumer key must be a stream')
 end
end
local args = {'*'}
for i=1,#ARGV do args[#args+1]=ARGV[i] end
for i=2,5 do redis.call('XADD',KEYS[i],unpack(args)) end
redis.call('SET',KEYS[1],'1','EX',604800)
return 1
)lua";
template<class Redis>
ruvia::Task<void> fanout(const Redis& redis, const std::vector<service::message::StreamMessage>& messages) {
    for (const auto& input : messages) {
        auto parsed = service::message::parsedFrom(input);
        contract::normalize(parsed);
        const auto fields = service::message::parsedFields(parsed);
        const auto partition = service::message::shard::index(parsed.deviceId);
        const std::vector<std::string> store{
            "iot:telemetry:fanout:" + parsed.messageId,
            consumerStream(partition,Consumer::History),consumerStream(partition,Consumer::Latest),
            consumerStream(partition,Consumer::Alerts),consumerStream(partition,Consumer::Delivery)};
        const std::vector<std::string_view> keys(store.begin(),store.end());
        std::vector<std::string_view> args;
        for (const auto& field : fields) { args.push_back(field.name); args.push_back(field.value); }
        const auto reply = co_await redis.eval(kFanoutScript,keys,args);
        if (reply.kind() != ruvia::RedisValue::Kind::kInteger)
            service::message::redis::throwValue("telemetry fanout",reply);
    }
}
}
