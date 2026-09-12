#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <ruvia/core/Task.h>
#include <ruvia/web/db/DbQuery.h>

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
        ruvia::DbQuery claim(context.pool());
        using Binary = ruvia::DbBinaryOperator;
        claim.update("command_attempt", "a")
            .set("sent_at", claim.call("now"))
            .updateFrom("command_operation", "o")
            .andWhere(claim.binary(claim.column("operation_id", "a"), Binary::kEqual,
                claim.cast(claim.value(operationId), ruvia::DbDataType::kUuid)))
            .andWhere(claim.binary(claim.column("id", "o"), Binary::kEqual,
                claim.column("operation_id", "a")))
            .andWhere(claim.binary(claim.column("node_id", "a"), Binary::kEqual,
                claim.value(nodeId)))
            .andWhere(claim.unary(ruvia::DbUnaryOperator::kIsNull,
                claim.column("sent_at", "a")))
            .andWhere(claim.binary(claim.column("deadline", "a"), Binary::kGreater,
                claim.call("now")))
            .andWhere(claim.binary(claim.column("status", "o"), Binary::kIn,
                claim.list({claim.value("DISPATCHING"), claim.value("AWAITING_RESULT")})))
            .returning({claim.column("operation_id", "a")});
        const auto claimed = co_await context.db().query(claim);
        co_return !claimed.empty();
    }

    template <typename Context>
    static ruvia::Task<std::optional<FirmwareSource>> loadFirmwareSource(
        Context& context, std::string_view requestId, std::string_view nodeId) {
        ruvia::DbQuery firmware(context.pool());
        using Binary = ruvia::DbBinaryOperator;
        firmware.select({firmware.column("storage_path", "firmware"),
                         firmware.column("size_bytes", "firmware")})
            .from("edge_task", "task")
            .join(ruvia::DbJoinType::kInner, "edge_firmware",
                firmware.binary(
                    firmware.cast(firmware.column("id", "firmware"), ruvia::DbDataType::kText),
                    Binary::kEqual,
                    firmware.binary(firmware.column("request", "task"), Binary::kJsonGetText,
                        firmware.value("firmware_id"))), "firmware")
            .andWhere(firmware.binary(firmware.column("id", "task"), Binary::kEqual,
                firmware.cast(firmware.value(requestId), ruvia::DbDataType::kUuid)))
            .andWhere(firmware.binary(firmware.column("node_id", "task"), Binary::kEqual,
                firmware.cast(firmware.value(nodeId), ruvia::DbDataType::kUuid)))
            .andWhere(firmware.binary(firmware.column("task_type", "task"), Binary::kEqual,
                firmware.value("firmware")))
            .limit(1);
        const auto rows = co_await context.db().query(firmware);
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
