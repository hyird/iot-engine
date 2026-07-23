#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/core/Task.h>

#include "service/common/message/contract.h"
#include "service/features/collector/stream.h"

namespace service::access::event {

inline constexpr std::string_view kStream = "iot:channel:open-access:event";

template <typename Redis>
inline ruvia::Task<void> publish(const Redis& redis, std::string_view eventId,
                                 std::string_view eventType, std::string_view deviceId,
                                 std::string_view deviceCode, std::int64_t occurredAtMs,
                                 std::string_view dataJson) {
    std::vector<message::StreamField> fields{
        {"event_id", std::string(eventId)},
        {"event_type", std::string(eventType)},
        {"device_id", std::string(deviceId)},
        {"device_code", std::string(deviceCode)},
        {"occurred_at_ms", std::to_string(occurredAtMs)},
        {"data_json", std::string(dataJson)},
    };
    (void)co_await message::redis::add(redis, kStream, fields, 100000);
}

} // namespace service::access::event
