#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <ruvia/core/Task.h>

#include "service/common/http.h"

namespace service::edge::gateway {

struct FirmwareSource final {
    std::string storagePath;
    std::uint64_t sizeBytes{};
};

class GatewayService final {
  public:
    template <typename Context>
    static ruvia::Task<bool> claimCommand(Context& context,
                                          std::string_view operationId,
                                          std::string_view nodeId) {
        const auto claimed = co_await context.db().query(R"sql(
UPDATE command_attempt a SET sent_at=NOW() FROM command_operation o
WHERE a.operation_id=$1::uuid AND o.id=a.operation_id AND a.node_id=$2
  AND a.sent_at IS NULL AND a.deadline>NOW()
  AND o.status IN ('DISPATCHING','AWAITING_RESULT') RETURNING a.operation_id)sql",
            service::common::dbParams(operationId, nodeId));
        co_return !claimed.empty();
    }

    template <typename Context>
    static ruvia::Task<std::optional<FirmwareSource>> loadFirmwareSource(
        Context& context, std::string_view requestId, std::string_view nodeId) {
        const auto rows = co_await context.db().query(R"sql(
SELECT firmware.storage_path, firmware.size_bytes
FROM edge_task task
JOIN edge_firmware firmware
  ON firmware.id::text = task.request->>'firmware_id'
WHERE task.id = $1::uuid AND task.node_id = $2::uuid
  AND task.task_type = 'firmware'
LIMIT 1)sql",
            service::common::dbParams(requestId, nodeId));
        if (rows.empty())
            co_return std::nullopt;

        FirmwareSource source;
        source.storagePath =
            std::string(rows.front()[0].value().value_or(std::string_view{}));
        const auto sizeText = rows.front()[1].value().value_or(std::string_view{});
        const auto parsed = std::from_chars(sizeText.data(), sizeText.data() + sizeText.size(),
                                            source.sizeBytes);
        if (source.storagePath.empty() || source.sizeBytes == 0 || parsed.ec != std::errc{} ||
            parsed.ptr != sizeText.data() + sizeText.size())
            co_return std::nullopt;
        co_return source;
    }
};

} // namespace service::edge::gateway
