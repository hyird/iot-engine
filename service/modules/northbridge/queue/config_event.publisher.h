#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/web/Context.h>
#include <ruvia/web/redis/Redis.h>

#include "service/common/bridge/message.contract.h"

namespace service::bridge {

inline ruvia::Task<void> publishConfigEvent(ruvia::Context& context, std::string_view aggregate,
                                            std::string_view action, std::string_view aggregateId) {
    const auto messageId = nextMessageId();
    const auto occurredAt = std::to_string(utcNowMilliseconds());
    const std::array<std::string_view, 16> args{
        "XADD",         kConfigStream, "MAXLEN",         "~",       "10000",  "*",
        "message_id",   messageId,     "aggregate",      aggregate, "action", action,
        "aggregate_id", aggregateId,   "occurred_at_ms", occurredAt};
    (void)co_await context.redis().command(args);
}

} // namespace service::bridge
