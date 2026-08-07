#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/core/Task.h>

#include "service/common/message/contract.h"
#include "service/common/uuid.h"
#include "service/features/collector/stream.h"

namespace service::access::audit {

inline constexpr std::string_view kStream{"iot:channel:open-access:audit"};
inline constexpr std::size_t kCapacity = 100000;

template <typename Redis>
ruvia::Task<void> publish(const Redis& redis, std::string_view action,
                          std::string_view accessKeyId, std::string_view method,
                          std::string_view target, std::string_view requestIp,
                          std::int64_t httpStatus, std::string_view deviceId = {},
                          std::string_view requestPayload = "{}",
                          std::string_view responsePayload = "{}") {
    const std::vector<service::message::StreamField> fields{
        {"log_id", service::common::nextUuidV7()},
        {"access_key_id", std::string(accessKeyId)},
        {"action", std::string(action)},
        {"http_method", std::string(method)},
        {"target", std::string(target)},
        {"request_ip", std::string(requestIp)},
        {"http_status", std::to_string(httpStatus)},
        {"device_id", std::string(deviceId)},
        {"request_payload", std::string(requestPayload)},
        {"response_payload", std::string(responsePayload)},
        {"used_at_ms", std::to_string(service::message::utcNowMilliseconds())},
    };
    (void)co_await service::message::redis::add(redis, kStream, fields, kCapacity);
}

} // namespace service::access::audit
