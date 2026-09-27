#pragma once

#include "service/common/uuid.h"

#include <memory>

#include "service/features/edge/gateway/gateway.entity.h"

#include <charconv>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <ruvia/core/Task.h>
#include <ruvia/web/db/DbQuery.h>

#include "service/common/http.h"
#include "service/features/edge/gateway/gateway.types.h"
#include "service/features/edge/edge.protocol.h"
#include "service/features/live/live.service.h"

namespace service::edge::gateway {

class GatewayService final {
  public:
    static ruvia::Task<EnrollmentRecord> loadEnrollment(ruvia::Context& c,
        std::string_view imei, std::string_view pendingNodeId = {}) {
        const auto value = co_await c.redis().get(service::message::edge::authKey(imei));
        if (value) {
            const auto record = EnrollmentRecord::decode(std::string_view(value->data(), value->size()));
            if (record) co_return *record;
        }
        co_return EnrollmentRecord{pendingNodeId.empty() ? c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next() :
                                   std::string(pendingNodeId), "pending"};
    }

    static ruvia::Task<void> saveLogResult(ruvia::Context& c, std::string_view nodeId, const pb::LogResult& result) {
        if (result.request_id().size() != 16) {
            co_return;
        }
        LogResultRecord record;
        if (!result.SerializeToString(&record.protobufBytes)) {
            co_return;
        }
        const auto id = protocol::uuidText(result.request_id());
        ruvia::RedisSetOptions options;
        options.expiration =
            ruvia::RedisSetExpiration::expiresAfter(std::chrono::seconds(60));
        const auto key = LogResultRecord::responseKey(id);
        co_await c.redis().set(key, record.protobufBytes, std::move(options));
        co_await service::live::publish(c.redis(), key);
        if (result.success()) {
            const auto snapshotKey = LogResultRecord::snapshotKey(nodeId);
            co_await c.redis().set(snapshotKey, record.protobufBytes);
            co_await service::live::publish(c.redis(), snapshotKey);
        }
    }

    static ruvia::Task<void> saveLogLevelResult(ruvia::Context& c, const pb::LogLevelResult& result) {
        if (result.request_id().size() != 16) {
            co_return;
        }
        LogResultRecord record;
        if (!result.SerializeToString(&record.protobufBytes)) {
            co_return;
        }
        const auto id = protocol::uuidText(result.request_id());
        ruvia::RedisSetOptions options;
        options.expiration =
            ruvia::RedisSetExpiration::expiresAfter(std::chrono::seconds(60));
        const auto key = LogResultRecord::levelKey(id);
        co_await c.redis().set(key, record.protobufBytes, std::move(options));
        co_await service::live::publish(c.redis(), key);
    }


    template <typename Context>
    static ruvia::Task<std::int64_t> databaseTimeMilliseconds(Context& context) {
        ruvia::DbQuery sample(context.pool());
        const auto epochMilliseconds = sample.cast(
            sample.call("floor", {sample.binary(
                sample.extract(ruvia::DbDatePart::kEpoch, sample.call("clock_timestamp")),
                ruvia::DbBinaryOperator::kMultiply, sample.value(std::int64_t{1000}))}),
            ruvia::DbDataType::kBigInt);
        sample.select(sample.cast(epochMilliseconds, ruvia::DbDataType::kText));
        const auto rows = co_await context.db().query(sample);
        if (rows.empty())
            throw std::runtime_error("PostgreSQL clock sample is unavailable");
        const auto sampledValue = rows.front()[0].value();
        if (!sampledValue)
            throw std::runtime_error("PostgreSQL clock sample is unavailable");
        const auto value = *sampledValue;
        std::int64_t milliseconds{};
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), milliseconds);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
            throw std::runtime_error("PostgreSQL clock sample is invalid");
        co_return milliseconds;
    }

    template <typename Context>
    static ruvia::Task<std::optional<std::int64_t>> claimCommand(
        Context& context, std::string_view operationId, std::string_view nodeId) {
        ruvia::DbQuery claim(context.pool());
        using Binary = ruvia::DbBinaryOperator;
        const auto deadlineMs = claim.cast(
            claim.call("floor", {claim.binary(
                claim.extract(ruvia::DbDatePart::kEpoch,
                    claim.column(service::edge::gateway::persistence::CommandAttemptEntity::columnName<"deadline">(), "a")),
                Binary::kMultiply, claim.value(1000))}),
            ruvia::DbDataType::kBigInt);
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
            .returning({claim.cast(claim.column(service::edge::gateway::persistence::CommandAttemptEntity::columnName<"operation_id">(), "a"), ruvia::DbDataType::kText), deadlineMs});
        const auto claimed = co_await context.db().query(claim);
        if (claimed.empty())
            co_return std::nullopt;
        // sent_at is already committed. A bad returned value must never reopen the attempt;
        // the command deadline sweep will conservatively transition it to UNKNOWN.
        const auto value = claimed.front()[1].value();
        if (!value)
            throw std::runtime_error("claimed command attempt has no persisted deadline");
        std::int64_t milliseconds{};
        const auto parsed = std::from_chars(value->data(), value->data() + value->size(), milliseconds);
        if (parsed.ec != std::errc{} || parsed.ptr != value->data() + value->size())
            throw std::runtime_error("claimed command attempt deadline is invalid");
        co_return milliseconds;
    }

    static void applyCommandDeadline(pb::CommandRequest& request,
                                     bool supportsCommandStartBefore,
                                     std::optional<std::int64_t> deadlineMs) {
        if (!supportsCommandStartBefore) {
            if (request.has_start_before_ms())
                throw std::invalid_argument("command start deadline was not negotiated");
            return;
        }
        if (request.has_start_before_ms())
            throw std::invalid_argument("command start deadline must come from its persisted attempt");
        if (!deadlineMs)
            throw std::invalid_argument("negotiated command start deadline is unavailable");
        request.set_start_before_ms(*deadlineMs);
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
