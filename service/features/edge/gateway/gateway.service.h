#pragma once

#include "service/features/edge/gateway/gateway.entity.h"

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
        claim.update(service::edge::gateway::persistence::CommandAttemptEntity::tableName(), "a")
            .set(service::edge::gateway::persistence::CommandAttemptEntity::columnName<"sent_at">(), claim.call("now"))
            .updateFrom(service::edge::gateway::persistence::CommandOperationEntity::tableName(), "o")
            .andWhere(claim.binary(claim.column(service::edge::gateway::persistence::CommandAttemptEntity::columnName<"operation_id">(), "a"), Binary::kEqual,
                claim.cast(claim.value(operationId), ruvia::DbDataType::kUuid)))
            .andWhere(claim.binary(claim.column(service::edge::gateway::persistence::CommandOperationEntity::columnName<"id">(), "o"), Binary::kEqual,
                claim.column(service::edge::gateway::persistence::CommandAttemptEntity::columnName<"operation_id">(), "a")))
            .andWhere(claim.binary(claim.column(service::edge::gateway::persistence::CommandAttemptEntity::columnName<"node_id">(), "a"), Binary::kEqual,
                claim.value(nodeId)))
            .andWhere(claim.unary(ruvia::DbUnaryOperator::kIsNull,
                claim.column(service::edge::gateway::persistence::CommandAttemptEntity::columnName<"sent_at">(), "a")))
            .andWhere(claim.binary(claim.column(service::edge::gateway::persistence::CommandAttemptEntity::columnName<"deadline">(), "a"), Binary::kGreater,
                claim.call("now")))
            .andWhere(claim.binary(claim.column(service::edge::gateway::persistence::CommandOperationEntity::columnName<"status">(), "o"), Binary::kIn,
                claim.list({claim.value("DISPATCHING"), claim.value("AWAITING_RESULT")})))
            .returning({claim.column(service::edge::gateway::persistence::CommandAttemptEntity::columnName<"operation_id">(), "a")});
        const auto claimed = co_await context.db().query(claim);
        co_return !claimed.empty();
    }

    template <typename Context>
    static ruvia::Task<std::optional<FirmwareSource>> loadFirmwareSource(
        Context& context, std::string_view requestId, std::string_view nodeId) {
        ruvia::DbQuery firmware(context.pool());
        using Binary = ruvia::DbBinaryOperator;
        firmware.select({firmware.column(service::edge::gateway::persistence::EdgeFirmwareEntity::columnName<"storage_path">(), "firmware"),
                         firmware.column(service::edge::gateway::persistence::EdgeFirmwareEntity::columnName<"size_bytes">(), "firmware")})
            .from(service::edge::gateway::persistence::EdgeTaskEntity::tableName(), "task")
            .join(ruvia::DbJoinType::kInner, service::edge::gateway::persistence::EdgeFirmwareEntity::tableName(),
                firmware.binary(
                    firmware.cast(firmware.column(service::edge::gateway::persistence::EdgeFirmwareEntity::columnName<"id">(), "firmware"), ruvia::DbDataType::kText),
                    Binary::kEqual,
                    firmware.binary(firmware.column(service::edge::gateway::persistence::EdgeTaskEntity::columnName<"request">(), "task"), Binary::kJsonGetText,
                        firmware.value("firmware_id"))), "firmware")
            .andWhere(firmware.binary(firmware.column(service::edge::gateway::persistence::EdgeTaskEntity::columnName<"id">(), "task"), Binary::kEqual,
                firmware.cast(firmware.value(requestId), ruvia::DbDataType::kUuid)))
            .andWhere(firmware.binary(firmware.column(service::edge::gateway::persistence::EdgeTaskEntity::columnName<"node_id">(), "task"), Binary::kEqual,
                firmware.cast(firmware.value(nodeId), ruvia::DbDataType::kUuid)))
            .andWhere(firmware.binary(firmware.column(service::edge::gateway::persistence::EdgeTaskEntity::columnName<"task_type">(), "task"), Binary::kEqual,
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
