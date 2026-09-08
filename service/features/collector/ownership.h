#pragma once

#include <set>
#include "service/common/instance.h"
#include "service/features/collector/config.h"

namespace service::collector::ownership {

inline std::string key(std::string_view linkId) {
    return "iot:v2:owner:link:" + std::string(linkId);
}

template <typename Redis>
ruvia::Task<RuntimeSnapshot> retain(const Redis& redis, RuntimeSnapshot snapshot) {
    static constexpr std::string_view script = R"lua(
local owner=redis.call('GET',KEYS[1])
if owner and owner~=ARGV[1] then return 0 end
redis.call('SET',KEYS[1],ARGV[1],'PX',15000)
return 1
)lua";
    std::set<std::string,std::less<>> owned;
    for (const auto& link : snapshot.links) {
        const auto lease = key(link.id);
        const std::string_view keys[]{lease};
        const std::string_view args[]{service::runtime::instanceId()};
        const auto reply = co_await redis.eval(script,keys,args);
        if (reply.kind() != ruvia::RedisValue::Kind::kInteger)
            message::redis::throwValue("claim link ownership",reply);
        if (reply.integer() == 1) owned.insert(link.id);
    }
    std::erase_if(snapshot.links,[&](const auto& link) { return !owned.contains(link.id); });
    std::erase_if(snapshot.devices,[&](const auto& device) { return !owned.contains(device.linkId); });
    co_return snapshot;
}

} // namespace service::collector::ownership
