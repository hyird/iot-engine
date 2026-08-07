#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <ruvia/web/Context.h>

#include "service/common/message/contract.h"
#include "service/features/collector/stream.h"

namespace service::message {

inline constexpr std::string_view kRuntimeConfigChangesStream{
    "iot:channel:runtime:config-change"};
inline constexpr std::string_view kWebhookCatalogChangesStream{
    "iot:channel:open-access:config-change"};

inline ruvia::Task<void> publishConfigEvent(ruvia::Context& context, std::string_view aggregate,
                                            std::string_view action, std::string_view aggregateId) {
    const bool runtimeChange =
        aggregate == "link" || aggregate == "device" || aggregate == "protocol";
    const bool webhookChange = aggregate == "access_key" || aggregate == "webhook" ||
                               aggregate == "device" || aggregate == "protocol";
    if (!runtimeChange && !webhookChange)
        co_return;
    const auto messageId = service::message::nextMessageId();
    const std::vector<service::message::StreamField> fields{
        {"message_id", messageId},
        {"aggregate", std::string(aggregate)},
        {"action", std::string(action)},
        {"aggregate_id", std::string(aggregateId)},
        {"created_at_ms", std::to_string(service::message::utcNowMilliseconds())}};
    auto pipeline = context.redis().pipeline();
    if (runtimeChange)
        service::message::redis::queueAdd(
            pipeline, kRuntimeConfigChangesStream, fields, 10000);
    if (webhookChange)
        service::message::redis::queueAdd(
            pipeline, kWebhookCatalogChangesStream, fields, 10000);
    const auto replies = co_await std::move(pipeline).exec();
    service::message::redis::requirePipelineSuccess("publish config change", replies);
    co_return;
}

} // namespace service::message
