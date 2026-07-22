#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/core/Task.h>

#include "service/common/bridge/message.contract.h"
#include "service/modules/southbridge/queue/redis_stream_async.h"

namespace service::open_access::event {

inline constexpr std::string_view kStream = "iot:channel:open-access:event";

template <typename Redis>
inline ruvia::Task<void> publish(const Redis& redis, std::string_view eventId,
                                 std::string_view eventType, std::string_view deviceId,
                                 std::string_view deviceCode, std::int64_t occurredAtMs,
                                 std::string_view dataJson) {
    std::vector<bridge::StreamField> fields{
        {"event_id", std::string(eventId)},
        {"event_type", std::string(eventType)},
        {"device_id", std::string(deviceId)},
        {"device_code", std::string(deviceCode)},
        {"occurred_at_ms", std::to_string(occurredAtMs)},
        {"data_json", std::string(dataJson)},
    };
    (void)co_await bridge::redis_async::add(redis, kStream, fields, 100000);
}

} // namespace service::open_access::event
