#pragma once

#include "service/common/uuid.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <ruvia/core/StopToken.h>
#include <ruvia/core/Timer.h>
#include "service/features/gb28181/media/media.transport.h"

#include "service/features/gb28181/gb28181.config.h"
#include "service/features/gb28181/gb28181.protocol.h"

#include "service/features/gb28181/gb28181.entity.h"

#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <future>
#include <iostream>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ruvia/web/WebWorker.h>
#include <ruvia/web/db/DbQuery.h>

#include "service/common/http.h"
#include "service/common/message.h"
#include "service/common/timestamp.h"
#include "service/features/messaging/messaging.transport.h"
#include "service/features/gb28181/gb28181.types.h"

namespace service::gb28181 {

class GbProjectionService {
  public:
    static ruvia::Task<bool> projectionOwnerMatches(const ruvia::RedisHandle& redis, std::string_view aggregateId, bool online, std::string_view ownerToken) {
        if (ownerToken.empty()) {
            co_return false;
        }
        const auto current = co_await redis.get(
            control_protocol::stream::owner(aggregateId)
        );
        if (online) {
            co_return current&& std::string_view(*current) == ownerToken;
        }
        // A legitimate offline event removes its owner key.  An old offline event
        // must not overwrite a newer online owner.
        co_return !current || std::string_view(*current) == ownerToken;
    }

    template <typename Redis>
    static ruvia::Task<void> publishConfig(const Redis& redis, const AppConfig& config) {
        // Configuration is immutable for the process lifetime.  Keeping one
        // Redis projection lets Service Workers answer config queries without a
        // process-global GB runtime or a cross-worker callback.
        const ConfigProjectionRecord record{
            .enabled = config.enabled,
            .domain = config.sip.domain,
            .id = config.sip.id,
            .host = config.sip.host,
            .publicIp = config.sip.publicIp,
            .port = config.sip.port,
            .transport = config.sip.transport,
            .registrationTimeoutSeconds = config.sip.registrationTimeoutSeconds,
            .commandTimeoutSeconds = config.sip.commandTimeoutSeconds,
            .inviteTimeoutSeconds = config.sip.inviteTimeoutSeconds,
            .viewerLeaseTimeoutSeconds = config.sip.viewerLeaseTimeoutSeconds,
        };
        co_await service::message::redis::setHash(redis, control_protocol::stream::kConfigKey, record.encode());
        co_return;
    }

    static ruvia::Task<bool> configuredEnabled(const ruvia::RedisHandle& redis) {
        const auto value = co_await redis.hget(control_protocol::stream::kConfigKey, "enabled");
        co_return value && ConfigProjectionRecord::enabledValue(std::string_view(*value));
    }

    static ruvia::Task<std::optional<ConfigProjectionRecord>> loadConfig(const ruvia::RedisHandle& redis) {
        const auto fields = co_await redis.hgetAll(control_protocol::stream::kConfigKey);
        if (fields.empty()) co_return std::nullopt;
        co_return ConfigProjectionRecord::decode(fields);
    }
#ifdef IOT_ENGINE_TESTING
    static int integerForTest(std::string_view value, int fallback = 0) {
        return integer(value, fallback);
    }
#endif

    // Northbound queries read the durable Service projection directly.  The
    // Collector never exposes its in-memory registry to HTTP workers.
    static ruvia::Task<ProjectionSnapshot> loadSnapshot(ruvia::WebWorkerContext& context) {
        co_return co_await hydrate(context);
    }

    // Every Service Worker runs the same reconciliation pass.  There is no
    // worker-index partition: the PostgreSQL advisory lock serializes two
    // workers that notice the same expired lease, while the second Redis GET
    // is performed after that lock to avoid clearing a newly acquired owner.
    static ruvia::Task<void>
    reconcileExpiredOwners(ruvia::WebWorkerContext& context) {
        ruvia::DbQuery deviceQuery(context.resource());
        deviceQuery.select(deviceQuery.column(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"id">()))
            .from(service::gb28181::persistence::Gb28181DeviceEntity::tableName())
            .where(deviceQuery.binary(
                deviceQuery.column(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"online">()), ruvia::DbBinaryOperator::kEqual,
                deviceQuery.value(true)))
            .orderBy(deviceQuery.column(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"id">()));
        const auto devices = co_await context.db().query(deviceQuery);
        for (const auto& row : devices) {
            const auto id = std::string(row[0].value().value_or(std::string_view{}));
            if (id.empty()) {
                continue;
            }
            const auto ownerKey = "iot:gb28181:owner:" + id;
            if (co_await context.redis().get(ownerKey)) {
                continue;
            }

            auto transaction = co_await context.db().beginTransaction();
            (void)co_await transaction.query(advisoryLockQuery(context.resource(), id));
            ruvia::DbQuery currentQuery(context.resource());
            currentQuery.select(currentQuery.column(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"online">()))
                .from(service::gb28181::persistence::Gb28181DeviceEntity::tableName())
                .where(currentQuery.binary(
                    currentQuery.column(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual,
                    currentQuery.value(id)))
                .lock({.mode = ruvia::DbRowLock::kUpdate});
            const auto current = co_await transaction.query(currentQuery);
            if (current.empty() ||
                !boolean(current.front()[0].value().value_or(std::string_view{}))) {
                co_await transaction.commit();
                continue;
            }
            if (co_await context.redis().get(ownerKey)) {
                co_await transaction.commit();
                continue;
            }
            ruvia::DbQuery updateQuery(context.resource());
            updateQuery.update(service::gb28181::persistence::Gb28181DeviceEntity::tableName())
                .set(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"online">(), updateQuery.value(false))
                .set(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"updated_at">(), updateQuery.call("now"))
                .where(updateQuery.binary(
                    updateQuery.column(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual,
                    updateQuery.value(id)))
                .andWhere(updateQuery.binary(
                    updateQuery.column(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"online">()), ruvia::DbBinaryOperator::kEqual,
                    updateQuery.value(true)));
            (void)co_await transaction.execute(updateQuery);
            // Keep durable metadata untouched.  This is a targeted cache update so
            // a fresh REGISTER projection can restore the full metadata hash.
            co_await service::message::redis::setHash(
                context.redis(),
                "iot:state:gb28181:device:" + id,
                { { "online", "0" } }
            );
            co_await transaction.commit();
        }

        ruvia::DbQuery streamQuery(context.resource());
        streamQuery
            .select({streamQuery.column(service::gb28181::persistence::Gb28181StreamEntity::columnName<"app">()), streamQuery.column(service::gb28181::persistence::Gb28181StreamEntity::columnName<"stream">()),
                     streamQuery.column(service::gb28181::persistence::Gb28181StreamEntity::columnName<"schema">())})
            .from(service::gb28181::persistence::Gb28181StreamEntity::tableName())
            .where(streamQuery.binary(
                streamQuery.column(service::gb28181::persistence::Gb28181StreamEntity::columnName<"online">()), ruvia::DbBinaryOperator::kEqual,
                streamQuery.value(true)))
            .orderBy(streamQuery.column(service::gb28181::persistence::Gb28181StreamEntity::columnName<"app">()))
            .addOrderBy(streamQuery.column(service::gb28181::persistence::Gb28181StreamEntity::columnName<"stream">()))
            .addOrderBy(streamQuery.column(service::gb28181::persistence::Gb28181StreamEntity::columnName<"schema">()));
        const auto streams = co_await context.db().query(streamQuery);
        for (const auto& row : streams) {
            const auto app = std::string(row[0].value().value_or(std::string_view{}));
            const auto stream =
                std::string(row[1].value().value_or(std::string_view{}));
            const auto schema =
                std::string(row[2].value().value_or(std::string_view{}));
            if (app.empty() || stream.empty() || schema.empty()) {
                continue;
            }
            const auto identity = StreamStatus::identity(app, stream, schema);
            const auto ownerKey = "iot:gb28181:owner:" + identity;
            if (co_await context.redis().get(ownerKey)) {
                continue;
            }

            auto transaction = co_await context.db().beginTransaction();
            (void)co_await transaction.query(
                advisoryLockQuery(context.resource(), identity));
            ruvia::DbQuery currentQuery(context.resource());
            currentQuery.select(currentQuery.column(service::gb28181::persistence::Gb28181StreamEntity::columnName<"online">()))
                .from(service::gb28181::persistence::Gb28181StreamEntity::tableName())
                .where(currentQuery.binary(
                    currentQuery.column(service::gb28181::persistence::Gb28181StreamEntity::columnName<"app">()),
                    ruvia::DbBinaryOperator::kEqual,
                    currentQuery.value(app)))
                .andWhere(currentQuery.binary(
                    currentQuery.column(service::gb28181::persistence::Gb28181StreamEntity::columnName<"stream">()),
                    ruvia::DbBinaryOperator::kEqual,
                    currentQuery.value(stream)))
                .andWhere(currentQuery.binary(
                    currentQuery.column(service::gb28181::persistence::Gb28181StreamEntity::columnName<"schema">()),
                    ruvia::DbBinaryOperator::kEqual,
                    currentQuery.value(schema)))
                .lock({.mode = ruvia::DbRowLock::kUpdate});
            const auto current = co_await transaction.query(currentQuery);
            if (current.empty() ||
                !boolean(current.front()[0].value().value_or(std::string_view{}))) {
                co_await transaction.commit();
                continue;
            }
            if (co_await context.redis().get(ownerKey)) {
                co_await transaction.commit();
                continue;
            }
            ruvia::DbQuery updateQuery(context.resource());
            updateQuery.update(service::gb28181::persistence::Gb28181StreamEntity::tableName())
                .set(service::gb28181::persistence::Gb28181StreamEntity::columnName<"online">(), updateQuery.value(false))
                .set(service::gb28181::persistence::Gb28181StreamEntity::columnName<"reader_count">(), updateQuery.value(0))
                .set(service::gb28181::persistence::Gb28181StreamEntity::columnName<"updated_at">(), updateQuery.call("now"))
                .where(updateQuery.binary(
                    updateQuery.column(service::gb28181::persistence::Gb28181StreamEntity::columnName<"app">()),
                    ruvia::DbBinaryOperator::kEqual,
                    updateQuery.value(app)))
                .andWhere(updateQuery.binary(
                    updateQuery.column(service::gb28181::persistence::Gb28181StreamEntity::columnName<"stream">()),
                    ruvia::DbBinaryOperator::kEqual,
                    updateQuery.value(stream)))
                .andWhere(updateQuery.binary(
                    updateQuery.column(service::gb28181::persistence::Gb28181StreamEntity::columnName<"schema">()),
                    ruvia::DbBinaryOperator::kEqual,
                    updateQuery.value(schema)))
                .andWhere(updateQuery.binary(
                    updateQuery.column(service::gb28181::persistence::Gb28181StreamEntity::columnName<"online">()),
                    ruvia::DbBinaryOperator::kEqual,
                    updateQuery.value(true)));
            (void)co_await transaction.execute(updateQuery);
            co_await service::message::redis::setHash(
                context.redis(),
                "iot:state:gb28181:stream:" + identity,
                { { "online", "0" }, { "reader_count", "0" } }
            );
            co_await transaction.commit();
        }
        co_return;
    }

  protected:
    static ruvia::DbQuery advisoryLockQuery(
        std::pmr::memory_resource* resource, std::string_view key) {
        ruvia::DbQuery query(resource);
        query.select(query.call(
            "pg_advisory_xact_lock",
            {query.call("hashtextextended",
                        {query.value(key), query.value(std::int64_t{28181})})}));
        return query;
    }

    static ruvia::DbQuery::Expr projectionCursor(
        ruvia::DbQuery& query, std::string_view streamMessageId) {
        const auto messageId = query.value(streamMessageId);
        const auto separator = query.value(std::string_view{"-"});
        const auto radix = query.cast(
            query.value(std::string_view{"18446744073709551616"}),
            ruvia::DbDataType::kNumeric);
        const auto milliseconds = query.cast(
            query.call("split_part",
                       {messageId, separator,
                        query.cast(query.value(std::int64_t{1}),
                                   ruvia::DbDataType::kInteger)}),
            ruvia::DbDataType::kNumeric);
        const auto sequence = query.cast(
            query.call("split_part",
                       {messageId, separator,
                        query.cast(query.value(std::int64_t{2}),
                                   ruvia::DbDataType::kInteger)}),
            ruvia::DbDataType::kNumeric);
        return query.binary(
            query.binary(milliseconds, ruvia::DbBinaryOperator::kMultiply,
                         radix),
            ruvia::DbBinaryOperator::kAdd, sequence);
    }

    static bool boolean(std::string_view value) {
        return value == "t" || value == "true" || value == "1";
    }

    static int integer(std::string_view value, int fallback = 0) {
        int parsed = 0;
        const auto* begin = value.data();
        const auto* end = begin + value.size();
        const auto result = std::from_chars(begin, end, parsed);
        if (result.ec != std::errc{} || result.ptr != end) {
            return fallback;
        }
        return parsed;
    }

    static ruvia::Task<ProjectionSnapshot> hydrate(ruvia::WebWorkerContext& context) {
        ProjectionSnapshot snapshot;
        std::unordered_map<std::string, std::size_t> deviceIndexes;
        ruvia::DbQuery deviceQuery(context.resource());
        deviceQuery
            .select({
                deviceQuery.column(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"id">()), deviceQuery.column(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"name">()),
                deviceQuery.coalesce({deviceQuery.column(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"custom_name">()),
                                      deviceQuery.value(std::string_view{})}),
                deviceQuery.column(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"manufacturer">()),
                deviceQuery.column(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"remote_address">()),
                deviceQuery.column(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"registration_source">()),
                deviceQuery.column(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"online">()),
                deviceQuery.call("iot_utc_timestamp",
                                 {deviceQuery.column(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"last_seen_at">())}),
                deviceQuery.coalesce(
                    {deviceQuery.cast(deviceQuery.column(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"mapped_device_id">()),
                                      ruvia::DbDataType::kText),
                     deviceQuery.value(std::string_view{})}),
            })
            .from(service::gb28181::persistence::Gb28181DeviceEntity::tableName())
            .orderBy(deviceQuery.column(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"id">()));
        const auto devices = co_await context.db().query(deviceQuery);
        snapshot.devices.reserve(devices.size());
        for (const auto& row : devices) {
            Device device;
            device.id = std::string(row[0].value().value_or(std::string_view{}));
            device.name = std::string(row[1].value().value_or(std::string_view{}));
            device.customName =
                std::string(row[2].value().value_or(std::string_view{}));
            device.manufacturer =
                std::string(row[3].value().value_or(std::string_view{}));
            device.remoteAddress =
                std::string(row[4].value().value_or(std::string_view{}));
            device.registrationSource =
                std::string(row[5].value().value_or(std::string_view{}));
            device.online = boolean(row[6].value().value_or(std::string_view{}));
            if (const auto parsed = service::common::parseUtcTimestamp(
                    row[7].value().value_or(std::string_view{})
                )) {
                device.lastSeen = *parsed;
            }
            device.mappedDeviceId =
                std::string(row[8].value().value_or(std::string_view{}));
            deviceIndexes.emplace(device.id, snapshot.devices.size());
            snapshot.devices.push_back(std::move(device));
        }

        ruvia::DbQuery channelQuery(context.resource());
        channelQuery
            .select({channelQuery.column(service::gb28181::persistence::Gb28181ChannelEntity::columnName<"device_id">()),
                     channelQuery.column(service::gb28181::persistence::Gb28181ChannelEntity::columnName<"id">()), channelQuery.column(service::gb28181::persistence::Gb28181ChannelEntity::columnName<"name">()),
                     channelQuery.coalesce(
                         {channelQuery.column(service::gb28181::persistence::Gb28181ChannelEntity::columnName<"custom_name">()),
                          channelQuery.value(std::string_view{})}),
                     channelQuery.column(service::gb28181::persistence::Gb28181ChannelEntity::columnName<"manufacturer">()),
                     channelQuery.column(service::gb28181::persistence::Gb28181ChannelEntity::columnName<"online">()),
                     channelQuery.column(service::gb28181::persistence::Gb28181ChannelEntity::columnName<"ptz_type">())})
            .from(service::gb28181::persistence::Gb28181ChannelEntity::tableName())
            .orderBy(channelQuery.column(service::gb28181::persistence::Gb28181ChannelEntity::columnName<"device_id">()))
            .addOrderBy(channelQuery.column(service::gb28181::persistence::Gb28181ChannelEntity::columnName<"id">()));
        const auto channels = co_await context.db().query(channelQuery);
        for (const auto& row : channels) {
            const auto device = deviceIndexes.find(
                std::string(row[0].value().value_or(std::string_view{}))
            );
            if (device == deviceIndexes.end()) {
                continue;
            }
            snapshot.devices[device->second].channels.push_back(Channel{
                .id = std::string(row[1].value().value_or(std::string_view{})),
                .name = std::string(row[2].value().value_or(std::string_view{})),
                .customName = std::string(row[3].value().value_or(std::string_view{})),
                .manufacturer = std::string(row[4].value().value_or(std::string_view{})),
                .online = boolean(row[5].value().value_or(std::string_view{})),
                .ptzType = integer(row[6].value().value_or(std::string_view{}), -1),
            });
        }

        ruvia::DbQuery recordQuery(context.resource());
        recordQuery
            .select({recordQuery.column(service::gb28181::persistence::Gb28181RecordEntity::columnName<"device_id">()),
                     recordQuery.column(service::gb28181::persistence::Gb28181RecordEntity::columnName<"channel_id">()),
                     recordQuery.column(service::gb28181::persistence::Gb28181RecordEntity::columnName<"name">()),
                     recordQuery.column(service::gb28181::persistence::Gb28181RecordEntity::columnName<"file_path">()),
                     recordQuery.column(service::gb28181::persistence::Gb28181RecordEntity::columnName<"address">()),
                     recordQuery.call("iot_utc_timestamp",
                                      {recordQuery.column(service::gb28181::persistence::Gb28181RecordEntity::columnName<"start_time">())}),
                     recordQuery.call("iot_utc_timestamp",
                                      {recordQuery.column(service::gb28181::persistence::Gb28181RecordEntity::columnName<"end_time">())}),
                     recordQuery.column(service::gb28181::persistence::Gb28181RecordEntity::columnName<"record_type">()),
                     recordQuery.column(service::gb28181::persistence::Gb28181RecordEntity::columnName<"recorder_id">())})
            .from(service::gb28181::persistence::Gb28181RecordEntity::tableName())
            .orderBy(recordQuery.column(service::gb28181::persistence::Gb28181RecordEntity::columnName<"device_id">()))
            .addOrderBy(recordQuery.column(service::gb28181::persistence::Gb28181RecordEntity::columnName<"start_time">()),
                        ruvia::DbOrderDirection::kDesc);
        const auto records = co_await context.db().query(recordQuery);
        for (const auto& row : records) {
            const auto device = deviceIndexes.find(
                std::string(row[0].value().value_or(std::string_view{}))
            );
            if (device == deviceIndexes.end()) {
                continue;
            }
            snapshot.devices[device->second].records.push_back(RecordItem{
                .deviceId = std::string(row[1].value().value_or(std::string_view{})),
                .name = std::string(row[2].value().value_or(std::string_view{})),
                .filePath = std::string(row[3].value().value_or(std::string_view{})),
                .address = std::string(row[4].value().value_or(std::string_view{})),
                .startTime = std::string(row[5].value().value_or(std::string_view{})),
                .endTime = std::string(row[6].value().value_or(std::string_view{})),
                .type = std::string(row[7].value().value_or(std::string_view{})),
                .recorderId = std::string(row[8].value().value_or(std::string_view{})),
            });
        }

        ruvia::DbQuery streamQuery(context.resource());
        streamQuery
            .select({streamQuery.column(service::gb28181::persistence::Gb28181StreamEntity::columnName<"app">()), streamQuery.column(service::gb28181::persistence::Gb28181StreamEntity::columnName<"stream">()),
                     streamQuery.column(service::gb28181::persistence::Gb28181StreamEntity::columnName<"schema">()),
                     streamQuery.column(service::gb28181::persistence::Gb28181StreamEntity::columnName<"online">()),
                     streamQuery.column(service::gb28181::persistence::Gb28181StreamEntity::columnName<"reader_count">())})
            .from(service::gb28181::persistence::Gb28181StreamEntity::tableName())
            .orderBy(streamQuery.column(service::gb28181::persistence::Gb28181StreamEntity::columnName<"app">()))
            .addOrderBy(streamQuery.column(service::gb28181::persistence::Gb28181StreamEntity::columnName<"stream">()))
            .addOrderBy(streamQuery.column(service::gb28181::persistence::Gb28181StreamEntity::columnName<"schema">()));
        const auto streams = co_await context.db().query(streamQuery);
        snapshot.streams.reserve(streams.size());
        for (const auto& row : streams) {
            const auto app = std::string(row[0].value().value_or(std::string_view{}));
            const auto stream = std::string(row[1].value().value_or(std::string_view{}));
            const auto schema = std::string(row[2].value().value_or(std::string_view{}));
            snapshot.streams.push_back(StreamStatus{
                .app = std::move(app),
                .stream = std::move(stream),
                .schema = std::move(schema),
                .online = boolean(row[3].value().value_or(std::string_view{})),
                .readerCount = integer(row[4].value().value_or(std::string_view{})),
            });
        }
        co_return snapshot;
    }

    template <typename Transaction>
    static ruvia::Task<void> syncChannels(Transaction& transaction, const Device& device, bool updateCustomNames = true) {
        if (device.channels.empty()) {
            ruvia::DbQuery removal;
            removal.deleteFrom(service::gb28181::persistence::Gb28181ChannelEntity::tableName())
                .where(removal.binary(
                    removal.column(service::gb28181::persistence::Gb28181ChannelEntity::columnName<"device_id">()),
                    ruvia::DbBinaryOperator::kEqual,
                    removal.value(device.id)));
            (void)co_await transaction.execute(removal);
            co_return;
        }

        // CTE 参数使用 TEXT，目标列仍按实体和数据库约束验证长度，避免 VARCHAR(n) 转换静默截断。
        ruvia::DbQuery rawIncoming;
        std::size_t ordinal = 0;
        for (const auto& channel : device.channels) {
            rawIncoming.values({
                rawIncoming.cast(
                    rawIncoming.value(static_cast<std::int64_t>(ordinal)),
                    ruvia::DbDataType::kBigInt),
                rawIncoming.cast(rawIncoming.value(channel.id),
                                 ruvia::DbDataType::kText),
                rawIncoming.cast(rawIncoming.value(channel.name),
                                 ruvia::DbDataType::kText),
                rawIncoming.cast(rawIncoming.value(channel.customName),
                                 ruvia::DbDataType::kText),
                rawIncoming.cast(rawIncoming.value(channel.manufacturer),
                                 ruvia::DbDataType::kText),
                rawIncoming.cast(rawIncoming.value(channel.online),
                                 ruvia::DbDataType::kBoolean),
                rawIncoming.cast(rawIncoming.value(channel.ptzType),
                                 ruvia::DbDataType::kInteger),
            });
            ++ordinal;
        }

        ruvia::DbQuery incoming;
        incoming
            .select({incoming.column("id"), incoming.column("name"),
                     incoming.column("custom_name"),
                     incoming.column("manufacturer"),
                     incoming.column("online"), incoming.column("ptz_type")})
            .distinctOn({incoming.column("id")})
            .from("raw_incoming")
            .orderBy(incoming.column("id"))
            .addOrderBy(incoming.column("ordinal"),
                        ruvia::DbOrderDirection::kDesc);

        ruvia::DbQuery insertSource;
        insertSource
            .select({
                insertSource.value(device.id),
                insertSource.column("id", "incoming"),
                insertSource.column("name", "incoming"),
                insertSource.nullIf(insertSource.column("custom_name", "incoming"),
                                    insertSource.value(std::string_view{})),
                insertSource.column("manufacturer", "incoming"),
                insertSource.column("online", "incoming"),
                insertSource.column("ptz_type", "incoming"),
                insertSource.call("now"),
            })
            .from("incoming", "incoming");

        ruvia::DbQuery upsert;
        upsert
            .insertInto(service::gb28181::persistence::Gb28181ChannelEntity::tableName(),
                        {"device_id", "id", "name", "custom_name",
                         "manufacturer", "online", "ptz_type", "updated_at"})
            .insertFrom(insertSource);
        ruvia::DbConflictOptions conflict;
        conflict.columns = {"device_id", "id"};
        conflict.update = {
            {"name", upsert.excluded(service::gb28181::persistence::Gb28181ChannelEntity::columnName<"name">())},
            {"custom_name", updateCustomNames
                                 ? upsert.excluded(service::gb28181::persistence::Gb28181ChannelEntity::columnName<"custom_name">())
                                 : upsert.coalesce(
                                       {upsert.excluded(service::gb28181::persistence::Gb28181ChannelEntity::columnName<"custom_name">()),
                                        upsert.column(service::gb28181::persistence::Gb28181ChannelEntity::columnName<"custom_name">(),
                                                      "gb28181_channel")})},
            {"manufacturer", upsert.excluded(service::gb28181::persistence::Gb28181ChannelEntity::columnName<"manufacturer">())},
            {"online", upsert.excluded(service::gb28181::persistence::Gb28181ChannelEntity::columnName<"online">())},
            {"ptz_type", upsert.excluded(service::gb28181::persistence::Gb28181ChannelEntity::columnName<"ptz_type">())},
            {"updated_at", upsert.call("now")},
        };
        conflict.updateWhere = upsert.binary(
            upsert.tuple({
                upsert.column(service::gb28181::persistence::Gb28181ChannelEntity::columnName<"name">(), "gb28181_channel"),
                upsert.column(service::gb28181::persistence::Gb28181ChannelEntity::columnName<"custom_name">(), "gb28181_channel"),
                upsert.column(service::gb28181::persistence::Gb28181ChannelEntity::columnName<"manufacturer">(), "gb28181_channel"),
                upsert.column(service::gb28181::persistence::Gb28181ChannelEntity::columnName<"online">(), "gb28181_channel"),
                upsert.column(service::gb28181::persistence::Gb28181ChannelEntity::columnName<"ptz_type">(), "gb28181_channel"),
            }),
            ruvia::DbBinaryOperator::kIsDistinctFrom,
            upsert.tuple({upsert.excluded(service::gb28181::persistence::Gb28181ChannelEntity::columnName<"name">()),
                          upsert.excluded(service::gb28181::persistence::Gb28181ChannelEntity::columnName<"custom_name">()),
                          upsert.excluded(service::gb28181::persistence::Gb28181ChannelEntity::columnName<"manufacturer">()),
                          upsert.excluded(service::gb28181::persistence::Gb28181ChannelEntity::columnName<"online">()),
                          upsert.excluded(service::gb28181::persistence::Gb28181ChannelEntity::columnName<"ptz_type">())}));
        upsert.onConflict(conflict)
            .returning({upsert.column(service::gb28181::persistence::Gb28181ChannelEntity::columnName<"id">())});

        ruvia::DbQuery incomingIds;
        incomingIds
            .select(incomingIds.value(1))
            .from("incoming", "incoming")
            .where(incomingIds.binary(
                incomingIds.column("id", "incoming"),
                ruvia::DbBinaryOperator::kEqual,
                incomingIds.column(service::gb28181::persistence::Gb28181ChannelEntity::columnName<"id">(), "stored")));

        ruvia::DbQuery removal;
        removal
            .with("raw_incoming", rawIncoming,
                  {.columns = {"ordinal", "id", "name", "custom_name",
                               "manufacturer", "online", "ptz_type"}})
            .with("incoming", incoming)
            .with("upserted", upsert)
            .deleteFrom(service::gb28181::persistence::Gb28181ChannelEntity::tableName(), "stored")
            .where(removal.binary(
                removal.column(service::gb28181::persistence::Gb28181ChannelEntity::columnName<"device_id">(), "stored"),
                ruvia::DbBinaryOperator::kEqual,
                removal.value(device.id)))
            .andWhere(removal.unary(
                ruvia::DbUnaryOperator::kNot,
                removal.exists(incomingIds)));
        (void)co_await transaction.execute(removal);
    }

    template <typename Transaction>
    static ruvia::Task<void> syncRecords(Transaction& transaction, const Device& device) {
        if (device.records.empty()) {
            ruvia::DbQuery removal;
            removal.deleteFrom(service::gb28181::persistence::Gb28181RecordEntity::tableName())
                .where(removal.binary(
                    removal.column(service::gb28181::persistence::Gb28181RecordEntity::columnName<"device_id">()),
                    ruvia::DbBinaryOperator::kEqual,
                    removal.value(device.id)));
            (void)co_await transaction.execute(removal);
            co_return;
        }

        // CTE 参数使用 TEXT，目标列仍按实体和数据库约束验证长度，避免 VARCHAR(n) 转换静默截断。
        ruvia::DbQuery rawIncoming;
        std::size_t ordinal = 0;
        for (const auto& record : device.records) {
            rawIncoming.values({
                rawIncoming.cast(
                    rawIncoming.value(static_cast<std::int64_t>(ordinal)),
                    ruvia::DbDataType::kBigInt),
                rawIncoming.cast(rawIncoming.value(record.deviceId),
                                 ruvia::DbDataType::kText),
                rawIncoming.cast(rawIncoming.value(record.name),
                                 ruvia::DbDataType::kText),
                rawIncoming.cast(rawIncoming.value(record.filePath),
                                 ruvia::DbDataType::kText),
                rawIncoming.cast(rawIncoming.value(record.address),
                                 ruvia::DbDataType::kText),
                rawIncoming.cast(rawIncoming.value(record.startTime),
                                 ruvia::DbDataType::kTimestampTz),
                rawIncoming.cast(rawIncoming.value(record.endTime),
                                 ruvia::DbDataType::kTimestampTz),
                rawIncoming.cast(rawIncoming.value(record.type),
                                 ruvia::DbDataType::kText),
                rawIncoming.cast(rawIncoming.value(record.recorderId),
                                 ruvia::DbDataType::kText),
            });
            ++ordinal;
        }

        ruvia::DbQuery incoming;
        incoming
            .select({incoming.column("channel_id"), incoming.column("name"),
                     incoming.column("file_path"),
                     incoming.column("address"),
                     incoming.column("start_time"),
                     incoming.column("end_time"),
                     incoming.column("record_type"),
                     incoming.column("recorder_id")})
            .distinctOn({incoming.column("channel_id"),
                         incoming.column("start_time"),
                         incoming.column("end_time"),
                         incoming.column("file_path")})
            .from("raw_incoming")
            .orderBy(incoming.column("channel_id"))
            .addOrderBy(incoming.column("start_time"))
            .addOrderBy(incoming.column("end_time"))
            .addOrderBy(incoming.column("file_path"))
            .addOrderBy(incoming.column("ordinal"),
                        ruvia::DbOrderDirection::kDesc);

        ruvia::DbQuery insertSource;
        insertSource
            .select({insertSource.value(device.id),
                     insertSource.column("channel_id", "incoming"),
                     insertSource.column("name", "incoming"),
                     insertSource.column("file_path", "incoming"),
                     insertSource.column("address", "incoming"),
                     insertSource.column("start_time", "incoming"),
                     insertSource.column("end_time", "incoming"),
                     insertSource.column("record_type", "incoming"),
                     insertSource.column("recorder_id", "incoming")})
            .from("incoming", "incoming");

        ruvia::DbQuery upsert;
        upsert
            .insertInto(service::gb28181::persistence::Gb28181RecordEntity::tableName(),
                        {"device_id", "channel_id", "name", "file_path",
                         "address", "start_time", "end_time", "record_type",
                         "recorder_id"})
            .insertFrom(insertSource);
        ruvia::DbConflictOptions conflict;
        conflict.columns = {"device_id", "channel_id", "start_time",
                            "end_time", "file_path"};
        conflict.update = {
            {"name", upsert.excluded(service::gb28181::persistence::Gb28181RecordEntity::columnName<"name">())},
            {"address", upsert.excluded(service::gb28181::persistence::Gb28181RecordEntity::columnName<"address">())},
            {"record_type", upsert.excluded(service::gb28181::persistence::Gb28181RecordEntity::columnName<"record_type">())},
            {"recorder_id", upsert.excluded(service::gb28181::persistence::Gb28181RecordEntity::columnName<"recorder_id">())},
        };
        conflict.updateWhere = upsert.binary(
            upsert.tuple({upsert.column(service::gb28181::persistence::Gb28181RecordEntity::columnName<"name">(), "gb28181_record"),
                          upsert.column(service::gb28181::persistence::Gb28181RecordEntity::columnName<"address">(), "gb28181_record"),
                          upsert.column(service::gb28181::persistence::Gb28181RecordEntity::columnName<"record_type">(), "gb28181_record"),
                          upsert.column(service::gb28181::persistence::Gb28181RecordEntity::columnName<"recorder_id">(), "gb28181_record")}),
            ruvia::DbBinaryOperator::kIsDistinctFrom,
            upsert.tuple({upsert.excluded(service::gb28181::persistence::Gb28181RecordEntity::columnName<"name">()),
                          upsert.excluded(service::gb28181::persistence::Gb28181RecordEntity::columnName<"address">()),
                          upsert.excluded(service::gb28181::persistence::Gb28181RecordEntity::columnName<"record_type">()),
                          upsert.excluded(service::gb28181::persistence::Gb28181RecordEntity::columnName<"recorder_id">())}));
        upsert.onConflict(conflict)
            .returning({upsert.column(service::gb28181::persistence::Gb28181RecordEntity::columnName<"channel_id">()), upsert.column(service::gb28181::persistence::Gb28181RecordEntity::columnName<"start_time">()),
                        upsert.column(service::gb28181::persistence::Gb28181RecordEntity::columnName<"end_time">()), upsert.column(service::gb28181::persistence::Gb28181RecordEntity::columnName<"file_path">())});

        ruvia::DbQuery incomingRecords;
        incomingRecords
            .select(incomingRecords.value(1))
            .from("incoming", "incoming")
            .where(incomingRecords.binary(
                incomingRecords.binary(
                    incomingRecords.binary(
                        incomingRecords.binary(
                            incomingRecords.column("channel_id", "incoming"),
                            ruvia::DbBinaryOperator::kEqual,
                            incomingRecords.column(service::gb28181::persistence::Gb28181RecordEntity::columnName<"channel_id">(), "stored")),
                        ruvia::DbBinaryOperator::kAnd,
                        incomingRecords.binary(
                            incomingRecords.column("start_time", "incoming"),
                            ruvia::DbBinaryOperator::kEqual,
                            incomingRecords.column(service::gb28181::persistence::Gb28181RecordEntity::columnName<"start_time">(), "stored"))),
                    ruvia::DbBinaryOperator::kAnd,
                    incomingRecords.binary(
                        incomingRecords.column("end_time", "incoming"),
                        ruvia::DbBinaryOperator::kEqual,
                        incomingRecords.column(service::gb28181::persistence::Gb28181RecordEntity::columnName<"end_time">(), "stored"))),
                ruvia::DbBinaryOperator::kAnd,
                incomingRecords.binary(
                    incomingRecords.column("file_path", "incoming"),
                    ruvia::DbBinaryOperator::kEqual,
                    incomingRecords.column(service::gb28181::persistence::Gb28181RecordEntity::columnName<"file_path">(), "stored"))));

        ruvia::DbQuery removal;
        removal
            .with("raw_incoming", rawIncoming,
                  {.columns = {"ordinal", "channel_id", "name", "file_path",
                               "address", "start_time", "end_time",
                               "record_type", "recorder_id"}})
            .with("incoming", incoming)
            .with("upserted", upsert)
            .deleteFrom(service::gb28181::persistence::Gb28181RecordEntity::tableName(), "stored")
            .where(removal.binary(
                removal.column(service::gb28181::persistence::Gb28181RecordEntity::columnName<"device_id">(), "stored"),
                ruvia::DbBinaryOperator::kEqual,
                removal.value(device.id)))
            .andWhere(removal.unary(
                ruvia::DbUnaryOperator::kNot,
                removal.exists(incomingRecords)));
        (void)co_await transaction.execute(removal);
    }

    // Projection events carry a complete Collector-side snapshot, but a new
    // Collector registry does not hydrate Service metadata before REGISTER.
    // Apply only the fields owned by the event change so an empty in-memory
    // custom name, mapping, channel list, or record list cannot erase durable
    // state from an earlier session.
    static ruvia::Task<bool> applyDeviceProjection(
        ruvia::WebWorkerContext& context,
        const Device& device,
        DeviceChange change,
        std::string ownerKey,
        std::string ownerToken,
        std::string streamMessageId
    ) {
        auto transaction = co_await context.db().beginTransaction();
        (void)co_await transaction.query(
            advisoryLockQuery(context.resource(), device.id));
        ruvia::DbQuery cursorQuery(context.resource());
        cursorQuery
            .select(cursorQuery.binary(
                cursorQuery.column(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"projection_cursor">()),
                ruvia::DbBinaryOperator::kGreaterEqual,
                projectionCursor(cursorQuery, streamMessageId)))
            .from(service::gb28181::persistence::Gb28181DeviceEntity::tableName())
            .where(cursorQuery.binary(
                cursorQuery.column(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual,
                cursorQuery.value(device.id)))
            .lock({.mode = ruvia::DbRowLock::kUpdate});
        const auto cursor = co_await transaction.query(cursorQuery);
        if (!cursor.empty() &&
            boolean(cursor.front()[0].value().value_or(std::string_view{}))) {
            co_await transaction.commit();
            co_return true;
        }
        const auto currentOwner = co_await context.redis().get(ownerKey);
        const bool ownerMatches = device.online
            ? currentOwner &&
                std::string_view(*currentOwner) ==
                    ownerToken
            : !currentOwner ||
                std::string_view(*currentOwner) ==
                    ownerToken;
        if (!ownerMatches) {
            co_await transaction.commit();
            co_return false;
        }
        const auto lastSeen = service::common::utcTimestamp(device.lastSeen);

        const auto buildDeviceUpsert = [&]() {
            ruvia::DbQuery query(context.resource());
            query
                .insertInto(
                    service::gb28181::persistence::Gb28181DeviceEntity::tableName(),
                    {"id", "name", "custom_name", "manufacturer",
                     "remote_address", "registration_source", "online",
                     "last_seen_at", "mapped_device_id", "updated_at"})
                .values({
                    query.value(device.id),
                    query.value(device.name),
                    query.nullIf(query.value(device.customName),
                                 query.value(std::string_view{})),
                    query.value(device.manufacturer),
                    query.value(device.remoteAddress),
                    query.value(device.registrationSource),
                    query.value(device.online),
                    query.cast(query.value(lastSeen),
                               ruvia::DbDataType::kTimestampTz),
                    query.cast(query.nullIf(query.value(device.mappedDeviceId),
                                            query.value(std::string_view{})),
                               ruvia::DbDataType::kUuid),
                    query.call("now"),
                });

            ruvia::DbConflictOptions conflict;
            conflict.columns = {"id"};
            switch (change) {
                case DeviceChange::Status:
                case DeviceChange::Records:
                    conflict.update = {
                        {"remote_address", query.excluded(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"remote_address">())},
                        {"registration_source",
                         query.excluded(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"registration_source">())},
                        {"online", query.excluded(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"online">())},
                        {"last_seen_at", query.excluded(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"last_seen_at">())},
                        {"updated_at", query.call("now")},
                    };
                    break;
                case DeviceChange::Catalog:
                    conflict.update = {
                        {"name", query.excluded(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"name">())},
                        {"manufacturer", query.excluded(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"manufacturer">())},
                        {"remote_address", query.excluded(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"remote_address">())},
                        {"registration_source",
                         query.excluded(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"registration_source">())},
                        {"online", query.excluded(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"online">())},
                        {"last_seen_at", query.excluded(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"last_seen_at">())},
                        {"updated_at", query.call("now")},
                    };
                    break;
                case DeviceChange::Mapping:
                    conflict.update = {
                        {"mapped_device_id", query.excluded(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"mapped_device_id">())},
                        {"updated_at", query.call("now")},
                    };
                    break;
                case DeviceChange::DeviceName:
                    conflict.update = {
                        {"custom_name", query.excluded(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"custom_name">())},
                        {"updated_at", query.call("now")},
                    };
                    break;
                case DeviceChange::ChannelName:
                    break;
            }
            query.onConflict(conflict);
            return query;
        };

        switch (change) {
            case DeviceChange::Status:
                (void)co_await transaction.execute(buildDeviceUpsert());
                break;
            case DeviceChange::Catalog:
                (void)co_await transaction.execute(buildDeviceUpsert());
                co_await syncChannels(transaction, device, false);
                break;
            case DeviceChange::Records:
                (void)co_await transaction.execute(buildDeviceUpsert());
                co_await syncRecords(transaction, device);
                break;
            case DeviceChange::Mapping:
                (void)co_await transaction.execute(buildDeviceUpsert());
                break;
            case DeviceChange::DeviceName:
                (void)co_await transaction.execute(buildDeviceUpsert());
                break;
            case DeviceChange::ChannelName:
                for (const auto& channel : device.channels) {
                    if (channel.customName.empty()) {
                        continue;
                    }
                    ruvia::DbQuery updateQuery(context.resource());
                    updateQuery.update(service::gb28181::persistence::Gb28181ChannelEntity::tableName())
                        .set(service::gb28181::persistence::Gb28181ChannelEntity::columnName<"custom_name">(), updateQuery.value(channel.customName))
                        .set(service::gb28181::persistence::Gb28181ChannelEntity::columnName<"updated_at">(), updateQuery.call("now"))
                        .where(updateQuery.binary(
                            updateQuery.column(service::gb28181::persistence::Gb28181ChannelEntity::columnName<"device_id">()),
                            ruvia::DbBinaryOperator::kEqual,
                            updateQuery.value(device.id)))
                        .andWhere(updateQuery.binary(
                            updateQuery.column(service::gb28181::persistence::Gb28181ChannelEntity::columnName<"id">()),
                            ruvia::DbBinaryOperator::kEqual,
                            updateQuery.value(channel.id)));
                    (void)co_await transaction.execute(updateQuery);
                }
                break;
        }
        co_await publishDevice(context, transaction, device.id);
        ruvia::DbQuery cursorUpdate(context.resource());
        cursorUpdate
            .update(service::gb28181::persistence::Gb28181DeviceEntity::tableName())
            .set(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"projection_cursor">(),
                 projectionCursor(cursorUpdate, streamMessageId))
            .where(cursorUpdate.binary(
                cursorUpdate.column(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual,
                cursorUpdate.value(device.id)));
        (void)co_await transaction.execute(cursorUpdate);
        co_await transaction.commit();
        co_return true;
    }

    template <typename Transaction>
    static ruvia::Task<void> publishDevice(ruvia::WebWorkerContext& context, Transaction& transaction, const std::string& id) {
        ruvia::DbQuery channelCount;
        channelCount
            .select(channelCount.aggregate("count", {channelCount.star()}))
            .from(service::gb28181::persistence::Gb28181ChannelEntity::tableName())
            .where(channelCount.binary(
                channelCount.column(service::gb28181::persistence::Gb28181ChannelEntity::columnName<"device_id">()),
                ruvia::DbBinaryOperator::kEqual,
                channelCount.column(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"id">(), "d")));
        ruvia::DbQuery recordCount;
        recordCount
            .select(recordCount.aggregate("count", {recordCount.star()}))
            .from(service::gb28181::persistence::Gb28181RecordEntity::tableName())
            .where(recordCount.binary(
                recordCount.column(service::gb28181::persistence::Gb28181RecordEntity::columnName<"device_id">()),
                ruvia::DbBinaryOperator::kEqual,
                recordCount.column(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"id">(), "d")));
        ruvia::DbQuery publishQuery(context.resource());
        publishQuery
            .select({
                publishQuery.column(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"id">(), "d"),
                publishQuery.column(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"name">(), "d"),
                publishQuery.coalesce(
                    {publishQuery.column(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"custom_name">(), "d"),
                     publishQuery.value(std::string_view{})}),
                publishQuery.column(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"manufacturer">(), "d"),
                publishQuery.column(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"remote_address">(), "d"),
                publishQuery.column(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"registration_source">(), "d"),
                publishQuery.column(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"online">(), "d"),
                publishQuery.call(
                    "iot_utc_timestamp",
                    {publishQuery.column(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"last_seen_at">(), "d")}),
                publishQuery.coalesce(
                    {publishQuery.cast(
                         publishQuery.column(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"mapped_device_id">(), "d"),
                         ruvia::DbDataType::kText),
                     publishQuery.value(std::string_view{})}),
                publishQuery.subquery(channelCount),
                publishQuery.subquery(recordCount),
            })
            .from(service::gb28181::persistence::Gb28181DeviceEntity::tableName(), "d")
            .where(publishQuery.binary(
                publishQuery.column(service::gb28181::persistence::Gb28181DeviceEntity::columnName<"id">(), "d"),
                ruvia::DbBinaryOperator::kEqual,
                publishQuery.value(id)));
        const auto rows = co_await transaction.query(publishQuery);
        if (rows.empty()) {
            co_return;
        }
        const auto& row = rows.front();
        const auto value = [&](std::size_t index) {
            return std::string(row[index].value().value_or(std::string_view{}));
        };
        const auto customName = value(2);
        co_await service::message::redis::setHash(context.redis(), "iot:state:gb28181:device:" + id, { { "id", id }, { "name", customName.empty() ? value(1) : customName }, { "reported_name", value(1) }, { "custom_name", customName }, { "online", boolean(value(6)) ? "1" : "0" }, { "remote_address", value(4) }, { "registration_source", value(5) }, { "mapped_device_id", value(8) }, { "last_seen_at", value(7) }, { "channel_count", value(9) }, { "record_count", value(10) } });
        (void)co_await service::message::redis::command(context.redis(), { "SADD", "iot:state:gb28181:devices", id });
    }

    static ruvia::Task<void> publishStream(ruvia::WebWorkerContext& context, const StreamStatus& stream) {
        const auto identity =
            StreamStatus::identity(stream.app, stream.stream, stream.schema);
        const auto key = "iot:state:gb28181:stream:" + identity;
        co_await service::message::redis::setHash(
            context.redis(),
            key,
            { { "id", identity },
              { "app", stream.app },
              { "stream", stream.stream },
              { "schema", stream.schema },
              { "online", stream.online ? "1" : "0" },
              { "reader_count", std::to_string(stream.readerCount) } }
        );
        (void)co_await service::message::redis::command(
            context.redis(),
            { "SADD", "iot:state:gb28181:streams", identity }
        );
    }

    static ruvia::Task<bool> applyStreamProjection(
        ruvia::WebWorkerContext& context,
        const StreamStatus& stream,
        std::string ownerKey,
        std::string ownerToken,
        std::string streamMessageId
    ) {
        const auto identity =
            StreamStatus::identity(stream.app, stream.stream, stream.schema);
        auto transaction = co_await context.db().beginTransaction();
        (void)co_await transaction.query(
            advisoryLockQuery(context.resource(), identity));
        ruvia::DbQuery cursorQuery(context.resource());
        cursorQuery
            .select(cursorQuery.binary(
                cursorQuery.column(service::gb28181::persistence::Gb28181StreamEntity::columnName<"projection_cursor">()),
                ruvia::DbBinaryOperator::kGreaterEqual,
                projectionCursor(cursorQuery, streamMessageId)))
            .from(service::gb28181::persistence::Gb28181StreamEntity::tableName())
            .where(cursorQuery.binary(
                cursorQuery.column(service::gb28181::persistence::Gb28181StreamEntity::columnName<"app">()), ruvia::DbBinaryOperator::kEqual,
                cursorQuery.value(stream.app)))
            .andWhere(cursorQuery.binary(
                cursorQuery.column(service::gb28181::persistence::Gb28181StreamEntity::columnName<"stream">()), ruvia::DbBinaryOperator::kEqual,
                cursorQuery.value(stream.stream)))
            .andWhere(cursorQuery.binary(
                cursorQuery.column(service::gb28181::persistence::Gb28181StreamEntity::columnName<"schema">()), ruvia::DbBinaryOperator::kEqual,
                cursorQuery.value(stream.schema)))
            .lock({.mode = ruvia::DbRowLock::kUpdate});
        const auto cursor = co_await transaction.query(cursorQuery);
        if (!cursor.empty() &&
            boolean(cursor.front()[0].value().value_or(std::string_view{}))) {
            co_await transaction.commit();
            co_return true;
        }
        const auto currentOwner = co_await context.redis().get(ownerKey);
        const bool ownerMatches = stream.online
            ? currentOwner &&
                std::string_view(*currentOwner) ==
                    ownerToken
            : !currentOwner ||
                std::string_view(*currentOwner) ==
                    ownerToken;
        if (!ownerMatches) {
            co_await transaction.commit();
            co_return false;
        }
        ruvia::DbQuery upsert(context.resource());
        upsert
            .insertInto(service::gb28181::persistence::Gb28181StreamEntity::tableName(),
                        {"app", "stream", "schema", "online", "reader_count",
                         "updated_at"})
            .values({upsert.value(stream.app), upsert.value(stream.stream),
                     upsert.value(stream.schema), upsert.value(stream.online),
                     upsert.value(stream.readerCount), upsert.call("now")});
        ruvia::DbConflictOptions conflict;
        conflict.columns = {"app", "stream", "schema"};
        conflict.update = {
            {"online", upsert.excluded(service::gb28181::persistence::Gb28181StreamEntity::columnName<"online">())},
            {"reader_count", upsert.excluded(service::gb28181::persistence::Gb28181StreamEntity::columnName<"reader_count">())},
            {"updated_at", upsert.call("now")},
        };
        conflict.updateWhere = upsert.binary(
            upsert.tuple({upsert.column(service::gb28181::persistence::Gb28181StreamEntity::columnName<"online">(), "gb28181_stream"),
                          upsert.column(service::gb28181::persistence::Gb28181StreamEntity::columnName<"reader_count">(), "gb28181_stream")}),
            ruvia::DbBinaryOperator::kIsDistinctFrom,
            upsert.tuple({upsert.excluded(service::gb28181::persistence::Gb28181StreamEntity::columnName<"online">()),
                          upsert.excluded(service::gb28181::persistence::Gb28181StreamEntity::columnName<"reader_count">())}));
        upsert.onConflict(conflict);
        (void)co_await transaction.execute(upsert);
        co_await publishStream(context, stream);
        ruvia::DbQuery cursorUpdate(context.resource());
        cursorUpdate
            .update(service::gb28181::persistence::Gb28181StreamEntity::tableName())
            .set(service::gb28181::persistence::Gb28181StreamEntity::columnName<"projection_cursor">(),
                 projectionCursor(cursorUpdate, streamMessageId))
            .where(cursorUpdate.binary(
                cursorUpdate.column(service::gb28181::persistence::Gb28181StreamEntity::columnName<"app">()), ruvia::DbBinaryOperator::kEqual,
                cursorUpdate.value(stream.app)))
            .andWhere(cursorUpdate.binary(
                cursorUpdate.column(service::gb28181::persistence::Gb28181StreamEntity::columnName<"stream">()), ruvia::DbBinaryOperator::kEqual,
                cursorUpdate.value(stream.stream)))
            .andWhere(cursorUpdate.binary(
                cursorUpdate.column(service::gb28181::persistence::Gb28181StreamEntity::columnName<"schema">()), ruvia::DbBinaryOperator::kEqual,
                cursorUpdate.value(stream.schema)));
        (void)co_await transaction.execute(cursorUpdate);
        co_await transaction.commit();
        co_return true;
    }
};

} // namespace service::gb28181

namespace service::gb28181 {

class GbControlService final {
  public:
    static void requireEnabled(bool enabled) {
        if (!enabled) {
            service::common::fail(10004, "GB28181 功能未启用", 404);
        }
    }

    static ruvia::Task<void> publishControlCommand(
        const ruvia::RedisHandle& redis, const std::string& controlKey,
        const std::string& ownerKey, std::string_view ownerToken,
        std::span<const service::message::StreamField> fields) {
        std::vector<std::string_view> args{ ownerToken };
        for (const auto& field : fields) {
            args.push_back(field.name);
            args.push_back(field.value);
        }
        const std::string_view keys[]{ controlKey, ownerKey };
        static constexpr std::string_view publishScript = R"lua(
if redis.call('GET',KEYS[2])~=ARGV[1] then return 0 end
redis.call('XADD',KEYS[1],'*',unpack(ARGV,2))
redis.call('EXPIRE',KEYS[1],600)
return 1
)lua";
        const auto published = co_await redis.eval(publishScript, keys, args);
        if (published.kind() != ruvia::RedisValue::Kind::kInteger) {
            service::message::redis::throwValue("GB28181 command publish", published);
        }
        if (published.integer() != 1) {
            service::common::fail(10003, "GB28181 connection owner expired", 409);
        }
    }

    static ruvia::Task<ControlClaimResult> claimControlExecution(
        const ruvia::RedisHandle& redis, const std::string& ownerKey,
        const std::string& claimKey, const std::string& cancelKey,
        const std::string& ownerToken, std::string_view deadline) {
        // Claim and owner/cancel/deadline checks are atomic in Redis.  The
        // claim lives longer than the mandatory maximum command lifetime.
        static constexpr std::string_view script = R"lua(
if redis.call('GET', KEYS[1]) ~= ARGV[1] then return -1 end
if redis.call('EXISTS', KEYS[3]) == 1 then return -2 end
local t = redis.call('TIME')
if tonumber(t[1])*1000 + math.floor(tonumber(t[2])/1000) >= tonumber(ARGV[2]) then return -3 end
if redis.call('SET', KEYS[2], ARGV[1], 'NX', 'EX', 600) then return 1 end
return 0
)lua";
        const std::string_view keys[]{ ownerKey, claimKey, cancelKey };
        const std::string_view args[]{ ownerToken, deadline };
        const auto claimed = co_await redis.eval(script, keys, args);
        if (claimed.kind() != ruvia::RedisValue::Kind::kInteger) {
            service::message::redis::throwValue("GB28181 command claim", claimed);
        }
        co_return static_cast<ControlClaimResult>(claimed.integer());
    }

    static ruvia::Task<bool> controlCancelled(
        const ruvia::RedisHandle& redis, const std::string& cancelKey) {
        const auto cancelled = co_await redis.get(cancelKey);
        co_return static_cast<bool>(cancelled);
    }

    static ruvia::Task<ProjectionPublishResult> publishProjection(
        const ruvia::RedisHandle& redis, const std::string& projectionId,
        std::string_view projectionOrder, std::string_view currentEntryId,
        std::span<const service::message::StreamField> fields) {
    // The first stream ID is the actor's ordering cursor.  A later repair may
    // need a new Redis entry after the old one has been acknowledged/deleted,
    // but it must carry this original order so a retry cannot move the DB
    // cursor backwards or reapply an older event over a newer one.
    static constexpr std::string_view script = R"lua(
local function valid_id(id)
  return id ~= nil and string.match(id, '^%d+%-%d+$') ~= nil
end

local marker = redis.call('GET', KEYS[2])
local order = ARGV[1]
local current = ARGV[2]
if order ~= '' and not valid_id(order) then
  return redis.error_reply('invalid local GB28181 projection order')
end
if current ~= '' and not valid_id(current) then
  return redis.error_reply('invalid local GB28181 projection entry')
end

if marker then
  local separator = string.find(marker, '|', 1, true)
  if not separator or string.find(marker, '|', separator + 1, true) then
    return redis.error_reply('invalid GB28181 projection sent marker')
  end
  local marker_order = string.sub(marker, 1, separator - 1)
  local marker_current = string.sub(marker, separator + 1)
  if not valid_id(marker_order) or not valid_id(marker_current) then
    return redis.error_reply('invalid GB28181 projection sent marker IDs')
  end
  if order ~= '' and order ~= marker_order then
    return redis.error_reply('GB28181 projection order changed')
  end
  order = marker_order
  -- If the previous XADD succeeded but its reply was lost, the marker's
  -- current ID is authoritative and avoids a duplicate repair entry.
  current = marker_current
end

if current ~= '' then
  local existing = redis.call('XRANGE', KEYS[1], current, current)
  if #existing > 0 then
    -- A marker may have expired while this coroutine was waiting.  Restore
    -- it once, without refreshing an existing marker on every poll.
    if not marker then
      redis.call('SET', KEYS[2], order .. '|' .. current, 'EX', 86400)
    end
    return { order, current }
  end
end

local values = {}
for index = 3, #ARGV do
  values[#values + 1] = ARGV[index]
end
if order ~= '' then
  values[#values + 1] = 'projection_order'
  values[#values + 1] = order
end
local id = redis.call('XADD', KEYS[1], '*', unpack(values))
if order == '' then
  order = id
end
redis.call('SET', KEYS[2], order .. '|' .. id, 'EX', 86400)
return { order, id }
)lua";
    const auto sentKey = "iot:gb28181:projection:sent:" + projectionId;
    const std::string_view keys[]{ control_protocol::stream::kProjection, sentKey };
        std::vector<std::string_view> args;
        args.reserve(2 + fields.size() * 2);
        args.push_back(projectionOrder);
        args.push_back(currentEntryId);
        for (const auto& field : fields) {
            args.push_back(field.name);
            args.push_back(field.value);
        }
        const auto published = co_await redis.eval(script, keys, args);
        co_return parseProjectionPublishResult(published);
    }

    static ruvia::Task<std::optional<bool>> projectionReceipt(
        const ruvia::RedisHandle& redis, std::string_view projectionId) {
        const auto done = co_await redis.get(control_protocol::stream::projectionDone(projectionId));
        if (!done) co_return std::nullopt;
        if (*done != "0" && *done != "1") {
            throw std::runtime_error("invalid GB28181 projection receipt");
        }
        co_return std::string_view(*done) == "1";
    }

    static ProjectionPublishResult parseProjectionPublishResult(
    const ruvia::RedisValue& reply
) {
    if (reply.kind() != ruvia::RedisValue::Kind::kArray ||
        reply.array().size() != 2 ||
        reply.array()[0].kind() != ruvia::RedisValue::Kind::kString ||
        reply.array()[1].kind() != ruvia::RedisValue::Kind::kString) {
        service::message::redis::throwValue(
            "GB28181 projection publish result",
            reply
        );
    }
    ProjectionPublishResult result{
        std::string(reply.array()[0].string()),
        std::string(reply.array()[1].string())
    };
    if (!control_protocol::validProjectionStreamId(result.order) ||
        !control_protocol::validProjectionStreamId(result.current)) {
        throw std::runtime_error("GB28181 projection publish returned invalid stream ID");
    }
    return result;
}

    static ruvia::Task<std::optional<ControlResultRecord>> loadControlResult(
        const ruvia::RedisHandle& redis, std::string_view key) {
        const auto value = co_await redis.get(key);
        if (!value) co_return std::nullopt;
        co_return ControlResultRecord::decode(*value);
    }

    static ruvia::Task<bool> renewOwnerLease(const ruvia::RedisHandle& connection, std::string_view key, std::string_view token) {
        static constexpr std::string_view script = R"lua(
if redis.call('GET', KEYS[1]) ~= ARGV[1] then return 0 end
return redis.call('PEXPIRE', KEYS[1], ARGV[2])
)lua";

        const auto redis = connection.withOptions({ .timeout = std::chrono::seconds(3) });
        const std::string_view leaseTtl = "15000";
        const std::string_view keys[]{ key };
        const std::string_view arguments[]{ token, leaseTtl };
        const auto reply = co_await redis.eval(script, keys, arguments);
        if (reply.kind() != ruvia::RedisValue::Kind::kInteger) {
            service::message::redis::throwValue("renew GB28181 owner lease", reply);
        }
        co_return reply.integer() == 1;
    }

    static ruvia::Task<bool> claimOwnerLease(const ruvia::RedisHandle& connection, std::string_view key, std::string_view token, std::string_view previousToken) {
        static constexpr std::string_view script = R"lua(
local current=redis.call('GET',KEYS[1])
if current==ARGV[1] then
  redis.call('PEXPIRE',KEYS[1],ARGV[3])
  return 1
end
if ARGV[2]~='' then
  if current~=ARGV[2] then return 0 end
  redis.call('SET',KEYS[1],ARGV[1],'PX',ARGV[3])
  return 1
end
if current then return 0 end
local claimed=redis.call('SET',KEYS[1],ARGV[1],'PX',ARGV[3],'NX')
return claimed and 1 or 0
)lua";

        const auto redis = connection.withOptions({ .timeout = std::chrono::seconds(3) });
        const std::string_view leaseTtl = "15000";
        const std::string_view keys[]{ key };
        const std::string_view arguments[]{ token, previousToken, leaseTtl };
        const auto reply = co_await redis.eval(script, keys, arguments);
        if (reply.kind() != ruvia::RedisValue::Kind::kInteger) {
            service::message::redis::throwValue("claim GB28181 owner lease", reply);
        }
        co_return reply.integer() == 1;
    }

    static ruvia::Task<void> releaseOwnerLease(const ruvia::RedisHandle& connection, std::string_view key, std::string_view token) {
        static constexpr std::string_view script = R"lua(
if redis.call('GET',KEYS[1])==ARGV[1] then
  return redis.call('DEL',KEYS[1])
end
return 0
)lua";

        const auto redis = connection.withOptions({ .timeout = std::chrono::seconds(3) });
        const std::string_view keys[]{ key };
        const std::string_view arguments[]{ token };
        const auto reply = co_await redis.eval(script, keys, arguments);
        if (reply.kind() != ruvia::RedisValue::Kind::kInteger) {
            service::message::redis::throwValue("release GB28181 owner lease", reply);
        }
        co_return;
    }

    static ruvia::Task<void> publishReplyAndAcknowledge(
    const ruvia::RedisHandle& redis,
    std::string_view replyStream,
    std::string_view controlStream,
    std::string_view group,
    std::string_view messageId,
    std::string_view requestId,
    std::string_view operation,
    std::string_view status,
    std::string_view payload,
    std::string_view resultKey,
    std::string_view claimKey,
    std::string_view claimOwner
) {
    // Reply publication and PEL removal are one Redis transaction.  A worker
    // crash between the two cannot acknowledge a command whose response was
    // never made visible to the Service Worker.
    static constexpr std::string_view script = R"lua(
 if ARGV[7] ~= '' and redis.call('EXISTS', KEYS[3]) == 0 then
   redis.call('SET', KEYS[3], ARGV[7], 'EX', ARGV[8])
 end
 local reply = redis.call('XADD', KEYS[1], '*',
   'request_id', ARGV[1], 'operation', ARGV[2], 'status', ARGV[3],
   'payload', ARGV[4])
 redis.call('EXPIRE', KEYS[1], ARGV[9])
 redis.call('EXPIRE', KEYS[2], ARGV[10])
 local acknowledged = redis.call('XACK', KEYS[2], ARGV[5], ARGV[6])
 redis.call('XDEL', KEYS[2], ARGV[6])
 if ARGV[11] ~= '' and redis.call('GET', KEYS[4]) == ARGV[11] then
   redis.call('DEL', KEYS[4])
 end
return acknowledged
)lua";
    const std::string replyKey(replyStream);
    const std::string controlKey(controlStream);
    const std::string resultKeyValue(resultKey);
    const std::string claimKeyValue(claimKey);
    const std::string request(requestId);
    const std::string op(operation);
    const std::string state(status);
    const std::string body(payload);
    const std::string groupValue(group);
    const std::string id(messageId);
    const std::string result = ControlResultRecord{std::string(status), std::string(payload)}.encode();
    const std::string claim(claimOwner);
    const std::string resultTtl{ "600" };
    const std::string replyTtl{ "600" };
    const std::string controlTtl{ "600" };
    const std::string_view keys[]{ replyKey, controlKey, resultKeyValue, claimKeyValue };
    const std::string_view arguments[]{ request, op, state, body, groupValue, id, result, resultTtl, replyTtl, controlTtl, claim };
    const auto reply = co_await redis.eval(script, keys, arguments);
    if (reply.kind() != ruvia::RedisValue::Kind::kInteger) {
        service::message::redis::throwValue("GB28181 reply/ack", reply);
    }
    co_return;
}

    static ruvia::Task<void> markControlCancelled(const ruvia::RedisHandle& redis, std::string_view cancelKey) {
    ruvia::RedisSetOptions options;
    options.expiration =
        ruvia::RedisSetExpiration::expiresAfter(std::chrono::minutes(10));
    co_await redis.set(std::string(cancelKey), "1", std::move(options));
    co_return;
}

    static ruvia::Task<std::optional<std::string>> ownerTokenFor(
    const ruvia::RedisHandle& redis,
    std::string_view key
) {
    const auto value = co_await redis.get(key);
    if (!value) {
        co_return std::nullopt;
    }
    co_return std::optional<std::string>(std::string(*value));
}

    static ruvia::Task<void> markProjectionDoneAndAcknowledge(
    const ruvia::RedisHandle& redis,
    std::string_view stream,
    std::string_view group,
    std::string_view messageId,
    std::string_view projectionId,
    bool applied
) {
    if (projectionId.empty()) {
        throw std::runtime_error("GB28181 projection has no projection_id");
    }
    static constexpr std::string_view script = R"lua(
 redis.call('SET', KEYS[1], ARGV[1], 'EX', ARGV[2])
 local acknowledged = redis.call('XACK', KEYS[2], ARGV[3], ARGV[4])
 redis.call('XDEL', KEYS[2], ARGV[4])
return acknowledged
)lua";
    const std::string doneKey(
        control_protocol::stream::projectionDone(projectionId)
    );
    const std::string streamKey(stream);
    const std::string appliedValue(applied ? "1" : "0");
    const std::string ttl{ "600" };
    const std::string groupValue(group);
    const std::string idValue(messageId);
    const std::string_view keys[]{ doneKey, streamKey };
    const std::string_view arguments[]{ appliedValue, ttl, groupValue, idValue };
    const auto reply = co_await redis.eval(script, keys, arguments);
    if (reply.kind() != ruvia::RedisValue::Kind::kInteger) {
        service::message::redis::throwValue("GB28181 projection done", reply);
    }
    co_return;
}

static ruvia::Task<std::string> executeOperation(
    ruvia::WebWorkerContext& context,
    std::string_view operation,
    std::string_view payload,
    ruvia::StopToken stop
) {
    if (stop.stopRequested()) {
        service::common::fail(10004, "GB28181 operation cancelled", 503);
    }

    std::pmr::monotonic_buffer_resource resource;
    auto request = control_protocol::parseRequest(
        payload.empty() ? std::string_view{ "{}" } : payload,
        &resource
    );
    if (!request) {
        service::common::fail(10001, "GB28181 RPC 请求体无效", 400);
    }
    const auto& input = *request;
    const auto enabled = co_await GbProjectionService::configuredEnabled(context.redis());

    if (operation == control_protocol::kHealthOperation) {
        co_return control_protocol::successJson(
            healthJson(context, enabled),
            context.resource()
        );
    }
    if (operation == control_protocol::kSipConfigOperation) {
        GbControlService::requireEnabled(enabled);
        const auto config = co_await GbProjectionService::loadConfig(context.redis());
        if (!config) {
            service::common::fail(10004, "GB28181 配置投影不可用", 503);
        }
        co_return control_protocol::successJson(
            control_protocol::sipConfigJson(context.resource(), config->domain, config->id, config->host, config->publicIp, config->port, config->transport),
            context.resource()
        );
    }

    if (operation == control_protocol::kDevicesOperation) {
        GbControlService::requireEnabled(enabled);
        const auto snapshot = co_await GbProjectionService::loadSnapshot(context);
        ruvia::BoxedArray<control_protocol::DeviceJson> items(
            ruvia::ModelOptions{ .resource = context.resource() }
        );
        for (const auto& device : snapshot.devices) {
            items.emplace(control_protocol::deviceJson(context.resource(), device));
        }
        control_protocol::DeviceListJson result(context);
        result.set<"items">(std::move(items));
        co_return control_protocol::successJson(result, context.resource());
    }
    if (operation == control_protocol::kDeviceOperation) {
        GbControlService::requireEnabled(enabled);
        const auto id = control_protocol::requiredText(input.get<"deviceId">(), "设备编号不能为空");
        const auto snapshot = co_await GbProjectionService::loadSnapshot(context);
        const auto value = std::find_if(
            snapshot.devices.begin(),
            snapshot.devices.end(),
            [&id](const Device& device) {
                return device.id == id;
            }
        );
        if (value == snapshot.devices.end()) {
            service::common::fail(10003, "设备不存在", 404);
        }
        co_return control_protocol::successJson(control_protocol::deviceJson(context.resource(), *value), context.resource());
    }
    if (operation == control_protocol::kStreamsOperation) {
        GbControlService::requireEnabled(enabled);
        const auto snapshot = co_await GbProjectionService::loadSnapshot(context);
        ruvia::BoxedArray<control_protocol::StreamJson> items(
            ruvia::ModelOptions{ .resource = context.resource() }
        );
        for (const auto& stream : snapshot.streams) {
            items.emplace(control_protocol::streamJson(context.resource(), stream));
        }
        control_protocol::StreamListJson result(context);
        result.set<"items">(std::move(items));
        co_return control_protocol::successJson(result, context.resource());
    }
    if (operation == control_protocol::kStreamOperation) {
        GbControlService::requireEnabled(enabled);
        const auto id = control_protocol::requiredText(input.get<"streamId">(), "流编号不能为空");
        const auto snapshot = co_await GbProjectionService::loadSnapshot(context);
        const auto value = std::find_if(
            snapshot.streams.begin(),
            snapshot.streams.end(),
            [&id](const StreamStatus& stream) {
                return StreamStatus::identity(stream.app, stream.stream, stream.schema) == id;
            }
        );
        if (value == snapshot.streams.end()) {
            service::common::fail(10003, "流不存在", 404);
        }
        co_return control_protocol::successJson(control_protocol::streamJson(context.resource(), *value), context.resource());
    }

    if (operation == control_protocol::kCatalogOperation ||
        operation == control_protocol::kRenameDeviceOperation ||
        operation == control_protocol::kRenameChannelOperation ||
        operation == control_protocol::kMapOperation ||
        operation == control_protocol::kUnmapOperation ||
        operation == control_protocol::kPreviewStartOperation ||
        operation == control_protocol::kPtzOperation ||
        operation == control_protocol::kPtzPositionOperation ||
        operation == control_protocol::kRecordsOperation ||
        operation == control_protocol::kPlaybackStartOperation) {
        GbControlService::requireEnabled(enabled);
        const auto deviceId = control_protocol::requiredText(input.get<"deviceId">(), "设备编号不能为空");
        if (operation == control_protocol::kRenameDeviceOperation) {
            (void)control_protocol::requiredText(input.get<"name">(), "名称不能为空");
        }
        if (operation == control_protocol::kRenameChannelOperation) {
            (void)control_protocol::requiredText(input.get<"channelId">(), "通道编号不能为空");
            (void)control_protocol::requiredText(input.get<"name">(), "名称不能为空");
        }
        if (operation == control_protocol::kPreviewStartOperation ||
            operation == control_protocol::kPtzOperation ||
            operation == control_protocol::kPtzPositionOperation ||
            operation == control_protocol::kRecordsOperation ||
            operation == control_protocol::kPlaybackStartOperation) {
            (void)control_protocol::requiredText(input.get<"channelId">(), "通道编号不能为空");
        }
        if (operation == control_protocol::kPtzOperation) {
            const auto action = control_protocol::requiredText(input.get<"action">(), "云台动作不能为空");
            control_protocol::requireAction(action);
            const auto speedValue = input.get<"speed">();
            const auto speed = speedValue ? control_protocol::requiredInteger(speedValue, "speed 无效")
                                          : 80;
            if (speed < 0 || speed > 255) {
                service::common::fail(10001, "speed 必须是 0 - 255 的整数", 400);
            }
        }
        if (operation == control_protocol::kPtzPositionOperation) {
            (void)control_protocol::requiredFinite(input.get<"pan">(), "pan", 0.0, 360.0);
            (void)control_protocol::requiredFinite(input.get<"tilt">(), "tilt", -30.0, 90.0);
            (void)control_protocol::requiredFinite(input.get<"zoom">(), "zoom", 1.0, 1000.0);
        }
        if (operation == control_protocol::kRecordsOperation ||
            operation == control_protocol::kPlaybackStartOperation) {
            (void)control_protocol::requiredText(input.get<"startTime">(), "开始时间不能为空");
            (void)control_protocol::requiredText(input.get<"endTime">(), "结束时间不能为空");
        }
        if (operation == control_protocol::kMapOperation) {
            (void)control_protocol::requiredText(input.get<"mappedDeviceId">(), "映射设备编号不能为空");
        }
        co_return co_await dispatchByOwnerKey(
            context,
            operation,
            payload,
            control_protocol::stream::owner(deviceId),
            stop
        );
    }

    if (operation == control_protocol::kPreviewStopOperation ||
        operation == control_protocol::kPreviewHeartbeatOperation) {
        GbControlService::requireEnabled(enabled);
        const auto sessionId = control_protocol::requiredText(input.get<"sessionId">(), "会话编号不能为空");
        co_return co_await dispatchByOwnerKey(
            context,
            operation,
            payload,
            control_protocol::stream::sessionOwner(sessionId),
            stop
        );
    }

    if (operation == control_protocol::kRecordingOperation ||
        operation == control_protocol::kRecordingStartOperation ||
        operation == control_protocol::kRecordingStopOperation) {
        GbControlService::requireEnabled(enabled);
        const auto streamId = control_protocol::requiredText(input.get<"streamId">(), "流编号不能为空");
        co_return co_await dispatchByOwnerKey(
            context,
            operation,
            payload,
            control_protocol::stream::owner(streamId),
            stop
        );
    }

    service::common::fail(10002, "不支持的 GB28181 操作", 400);
}

  private:
static control_protocol::HealthJson healthJson(ruvia::WebWorkerContext& context, bool enabled, std::string_view error = {}) {
    const auto started = sdkSupervisor().started();
    control_protocol::HealthJson result(context);
    result.set<"status">(started ? "ok" : (enabled ? "error" : "disabled"))
        .set<"service">("iot-engine-gb28181")
        .set<"enabled">(enabled)
        .set<"started">(started)
        .set<"error">(error)
        .set<"mediaPorts">(control_protocol::mediaPortsJson(context.resource(), sdkSupervisor().ports()))
        .set<"mediaCapabilities">(
            control_protocol::mediaCapabilitiesJson(context.resource(), sdkSupervisor().capabilities())
        );
    return result;
}

static ruvia::Task<std::optional<service::message::StreamMessage>> readControlReply(
    const ruvia::WorkerHandle& worker,
    const ruvia::RedisHandle& redis,
    std::string_view stream,
    std::string_view requestId,
    ruvia::StopToken stop,
    std::chrono::system_clock::time_point deadline
) {
    const auto boundedRedis = redis.withOptions({ .stopToken = stop });
    while (!stop.stopRequested() &&
           std::chrono::system_clock::now() < deadline) {
        const auto reply = co_await service::message::redis::command(
            boundedRedis,
            { "XREAD", "COUNT", "1", "STREAMS", std::string(stream), "0-0" }
        );
        if (reply.null()) {
            (void)co_await ruvia::sleepFor(worker, std::chrono::milliseconds(100), stop);
            continue;
        }
        if (reply.kind() != ruvia::RedisValue::Kind::kArray) {
            service::message::redis::throwValue("XREAD GB28181 reply", reply);
        }
        for (const auto& streamReply : reply.array()) {
            if (streamReply.kind() != ruvia::RedisValue::Kind::kArray ||
                streamReply.array().size() != 2) {
                continue;
            }
            const auto& entries = streamReply.array()[1];
            if (entries.kind() != ruvia::RedisValue::Kind::kArray) {
                continue;
            }
            for (const auto& entry : entries.array()) {
                if (entry.kind() != ruvia::RedisValue::Kind::kArray ||
                    entry.array().size() != 2) {
                    continue;
                }
                const auto& id = entry.array()[0];
                const auto& fields = entry.array()[1];
                if (id.kind() != ruvia::RedisValue::Kind::kString ||
                    fields.kind() != ruvia::RedisValue::Kind::kArray) {
                    continue;
                }
                service::message::StreamMessage message;
                message.id = std::string(id.string());
                for (std::size_t index = 0; index + 1 < fields.array().size();
                     index += 2) {
                    const auto& name = fields.array()[index];
                    const auto& value = fields.array()[index + 1];
                    if (name.kind() != ruvia::RedisValue::Kind::kString ||
                        value.kind() != ruvia::RedisValue::Kind::kString) {
                        continue;
                    }
                    message.fields.push_back(
                        { std::string(name.string()), std::string(value.string()) }
                    );
                }
                if (message.get("request_id") == requestId) {
                    co_return message;
                }
            }
        }
    }
    if (stop.stopRequested()) {
        throw std::runtime_error("GB28181 operation was cancelled");
    }
    co_return std::nullopt;
}

static ruvia::Task<std::string> dispatchToCollector(
    ruvia::WebWorkerContext& context,
    std::string_view operation,
    std::string_view payload,
    std::string ownerKey,
    std::string_view ownerToken,
    ruvia::StopToken stop
) {
    const auto owner = control_protocol::stream::ownerIndex(ownerToken);
    const auto instance = control_protocol::stream::ownerInstance(ownerToken);
    if (!owner || !instance || !control_protocol::stream::completeOwnerToken(ownerToken)) {
        throw std::runtime_error("GB28181 owner token is invalid");
    }
    if (stop.stopRequested()) {
        throw std::runtime_error("GB28181 operation was cancelled");
    }
    const auto currentOwner = co_await GbControlService::ownerTokenFor(context.redis(), ownerKey);
    if (!currentOwner || std::string_view(*currentOwner) != ownerToken) {
        service::common::fail(10003, "GB28181 连接归属已变更或已过期", 404);
    }

    const auto requestId = context.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next();
    const auto replyStream = control_protocol::stream::reply(requestId);
    const auto resultKey = control_protocol::stream::result(requestId);
    const auto cancelKey = control_protocol::stream::cancel(requestId);
    const auto now = std::chrono::system_clock::now();
    const auto deadline = now + std::chrono::seconds(15);
    const auto deadlineMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                deadline.time_since_epoch()
    )
                                .count();
    std::exception_ptr publishFailure;
    try {
        const auto controlKey = control_protocol::stream::control(*owner, *instance);
        std::vector<service::message::StreamField> fields{
            { "request_id", requestId },
            { "owner_key", std::string(ownerKey) },
            { "owner_token", std::string(ownerToken) },
            { "operation", std::string(operation) },
            { "payload", std::string(payload) },
            { "reply_stream", replyStream },
            { "result_key", resultKey },
            { "cancel_key", cancelKey },
            { "deadline_ms", std::to_string(deadlineMs) }
        };
        co_await GbControlService::publishControlCommand(
            context.redis(), controlKey, ownerKey, ownerToken, fields);
    } catch (...) {
        publishFailure = std::current_exception();
    }
    if (publishFailure) {
        // XADD may have reached Redis before the client observed a transport
        // error.  Mark the request cancelled so an uncertain delivery cannot
        // execute a device side effect after the Service has failed it.
        try {
            co_await GbControlService::markControlCancelled(context.redis(), cancelKey);
        } catch (...) {
        }
        std::rethrow_exception(publishFailure);
    }

    std::optional<service::message::StreamMessage> response;
    std::exception_ptr failure;
    try {
        response = co_await readControlReply(context.worker(), context.redis(), replyStream, requestId, stop, deadline);
    } catch (...) {
        failure = std::current_exception();
    }
    if (failure) {
        // The Collector may have claimed the command before this wait was
        // cancelled.  Leave a short lived fence for it to check before any
        // device side effect.
        try {
            co_await GbControlService::markControlCancelled(context.redis(), cancelKey);
        } catch (...) {
        }
        try {
            (void)co_await context.redis().del(replyStream);
        } catch (...) {
        }
        std::rethrow_exception(failure);
    }
    if (!response) {
        try {
            co_await GbControlService::markControlCancelled(context.redis(), cancelKey);
        } catch (...) {
        }
        try {
            (void)co_await context.redis().del(replyStream);
        } catch (...) {
        }
        throw std::runtime_error(
            "GB28181 Collector did not reply before deadline"
        );
    }
    (void)co_await context.redis().del(replyStream);
    const auto status = response->get("status");
    const auto result = response->get("payload");
    if (status != "ok") {
        throw std::runtime_error(result.empty() ? "GB28181 Collector operation failed" : std::string(result));
    }
    co_return std::string(result);
}

static ruvia::Task<std::string> dispatchByOwnerKey(
    ruvia::WebWorkerContext& context,
    std::string_view operation,
    std::string_view payload,
    std::string ownerKey,
    ruvia::StopToken stop
) {
    const auto token = co_await GbControlService::ownerTokenFor(context.redis(), ownerKey);
    if (!token) {
        service::common::fail(10003, "GB28181 连接归属不存在或已过期", 404);
    }
    try {
        co_return co_await dispatchToCollector(context, operation, payload, ownerKey, *token, stop);
    } catch (const ruvia::HttpError&) {
        throw;
    } catch (const std::exception& error) {
        service::common::fail(10004, error.what(), 502);
    }
}

};

} // namespace service::gb28181
