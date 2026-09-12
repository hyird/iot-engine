#pragma once

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
#include "service/features/gb28181/device/device.types.h"
#include "service/features/gb28181/media/media.types.h"

namespace service::gb28181 {

class GbProjectionService {
  public:
    struct Snapshot {
        std::vector<Device> devices;
        std::vector<StreamStatus> streams;
    };

#ifdef IOT_ENGINE_TESTING
    static int integerForTest(std::string_view value, int fallback = 0) {
        return integer(value, fallback);
    }
#endif

    // Northbound queries read the durable Service projection directly.  The
    // Collector never exposes its in-memory registry to HTTP workers.
    static ruvia::Task<Snapshot> loadSnapshot(ruvia::WebWorkerContext& context) {
        co_return co_await hydrate(context);
    }

    // Every Service Worker runs the same reconciliation pass.  There is no
    // worker-index partition: the PostgreSQL advisory lock serializes two
    // workers that notice the same expired lease, while the second Redis GET
    // is performed after that lock to avoid clearing a newly acquired owner.
    static ruvia::Task<void>
    reconcileExpiredOwners(ruvia::WebWorkerContext& context) {
        ruvia::DbQuery deviceQuery(context.resource());
        deviceQuery.select(deviceQuery.column("id"))
            .from("gb28181_device")
            .where(deviceQuery.binary(
                deviceQuery.column("online"), ruvia::DbBinaryOperator::kEqual,
                deviceQuery.value(true)))
            .orderBy(deviceQuery.column("id"));
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
            currentQuery.select(currentQuery.column("online"))
                .from("gb28181_device")
                .where(currentQuery.binary(
                    currentQuery.column("id"), ruvia::DbBinaryOperator::kEqual,
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
            updateQuery.update("gb28181_device")
                .set("online", updateQuery.value(false))
                .set("updated_at", updateQuery.call("now"))
                .where(updateQuery.binary(
                    updateQuery.column("id"), ruvia::DbBinaryOperator::kEqual,
                    updateQuery.value(id)))
                .andWhere(updateQuery.binary(
                    updateQuery.column("online"), ruvia::DbBinaryOperator::kEqual,
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
            .select({streamQuery.column("app"), streamQuery.column("stream"),
                     streamQuery.column("schema")})
            .from("gb28181_stream")
            .where(streamQuery.binary(
                streamQuery.column("online"), ruvia::DbBinaryOperator::kEqual,
                streamQuery.value(true)))
            .orderBy(streamQuery.column("app"))
            .addOrderBy(streamQuery.column("stream"))
            .addOrderBy(streamQuery.column("schema"));
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
            currentQuery.select(currentQuery.column("online"))
                .from("gb28181_stream")
                .where(currentQuery.binary(
                    currentQuery.column("app"),
                    ruvia::DbBinaryOperator::kEqual,
                    currentQuery.value(app)))
                .andWhere(currentQuery.binary(
                    currentQuery.column("stream"),
                    ruvia::DbBinaryOperator::kEqual,
                    currentQuery.value(stream)))
                .andWhere(currentQuery.binary(
                    currentQuery.column("schema"),
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
            updateQuery.update("gb28181_stream")
                .set("online", updateQuery.value(false))
                .set("reader_count", updateQuery.value(0))
                .set("updated_at", updateQuery.call("now"))
                .where(updateQuery.binary(
                    updateQuery.column("app"),
                    ruvia::DbBinaryOperator::kEqual,
                    updateQuery.value(app)))
                .andWhere(updateQuery.binary(
                    updateQuery.column("stream"),
                    ruvia::DbBinaryOperator::kEqual,
                    updateQuery.value(stream)))
                .andWhere(updateQuery.binary(
                    updateQuery.column("schema"),
                    ruvia::DbBinaryOperator::kEqual,
                    updateQuery.value(schema)))
                .andWhere(updateQuery.binary(
                    updateQuery.column("online"),
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

    static ruvia::Task<Snapshot> hydrate(ruvia::WebWorkerContext& context) {
        Snapshot snapshot;
        std::unordered_map<std::string, std::size_t> deviceIndexes;
        ruvia::DbQuery deviceQuery(context.resource());
        deviceQuery
            .select({
                deviceQuery.column("id"), deviceQuery.column("name"),
                deviceQuery.coalesce({deviceQuery.column("custom_name"),
                                      deviceQuery.value(std::string_view{})}),
                deviceQuery.column("manufacturer"),
                deviceQuery.column("remote_address"),
                deviceQuery.column("registration_source"),
                deviceQuery.column("online"),
                deviceQuery.call("iot_utc_timestamp",
                                 {deviceQuery.column("last_seen_at")}),
                deviceQuery.coalesce(
                    {deviceQuery.cast(deviceQuery.column("mapped_device_id"),
                                      ruvia::DbDataType::kText),
                     deviceQuery.value(std::string_view{})}),
            })
            .from("gb28181_device")
            .orderBy(deviceQuery.column("id"));
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
            .select({channelQuery.column("device_id"),
                     channelQuery.column("id"), channelQuery.column("name"),
                     channelQuery.coalesce(
                         {channelQuery.column("custom_name"),
                          channelQuery.value(std::string_view{})}),
                     channelQuery.column("manufacturer"),
                     channelQuery.column("online"),
                     channelQuery.column("ptz_type")})
            .from("gb28181_channel")
            .orderBy(channelQuery.column("device_id"))
            .addOrderBy(channelQuery.column("id"));
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
            .select({recordQuery.column("device_id"),
                     recordQuery.column("channel_id"),
                     recordQuery.column("name"),
                     recordQuery.column("file_path"),
                     recordQuery.column("address"),
                     recordQuery.call("iot_utc_timestamp",
                                      {recordQuery.column("start_time")}),
                     recordQuery.call("iot_utc_timestamp",
                                      {recordQuery.column("end_time")}),
                     recordQuery.column("record_type"),
                     recordQuery.column("recorder_id")})
            .from("gb28181_record")
            .orderBy(recordQuery.column("device_id"))
            .addOrderBy(recordQuery.column("start_time"),
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
            .select({streamQuery.column("app"), streamQuery.column("stream"),
                     streamQuery.column("schema"),
                     streamQuery.column("online"),
                     streamQuery.column("reader_count")})
            .from("gb28181_stream")
            .orderBy(streamQuery.column("app"))
            .addOrderBy(streamQuery.column("stream"))
            .addOrderBy(streamQuery.column("schema"));
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
            removal.deleteFrom("gb28181_channel")
                .where(removal.binary(
                    removal.column("device_id"),
                    ruvia::DbBinaryOperator::kEqual,
                    removal.value(device.id)));
            (void)co_await transaction.execute(removal);
            co_return;
        }

        ruvia::DbQuery rawIncoming;
        std::size_t ordinal = 0;
        for (const auto& channel : device.channels) {
            rawIncoming.values({
                rawIncoming.cast(
                    rawIncoming.value(static_cast<std::int64_t>(ordinal)),
                    ruvia::DbDataType::kBigInt),
                rawIncoming.cast(rawIncoming.value(channel.id),
                                 ruvia::DbDataType::kVarchar),
                rawIncoming.cast(rawIncoming.value(channel.name),
                                 ruvia::DbDataType::kVarchar),
                rawIncoming.cast(rawIncoming.value(channel.customName),
                                 ruvia::DbDataType::kVarchar),
                rawIncoming.cast(rawIncoming.value(channel.manufacturer),
                                 ruvia::DbDataType::kVarchar),
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
            .insertInto("gb28181_channel",
                        {"device_id", "id", "name", "custom_name",
                         "manufacturer", "online", "ptz_type", "updated_at"})
            .insertFrom(insertSource);
        ruvia::DbConflictOptions conflict;
        conflict.columns = {"device_id", "id"};
        conflict.update = {
            {"name", upsert.excluded("name")},
            {"custom_name", updateCustomNames
                                 ? upsert.excluded("custom_name")
                                 : upsert.coalesce(
                                       {upsert.excluded("custom_name"),
                                        upsert.column("custom_name",
                                                      "gb28181_channel")})},
            {"manufacturer", upsert.excluded("manufacturer")},
            {"online", upsert.excluded("online")},
            {"ptz_type", upsert.excluded("ptz_type")},
            {"updated_at", upsert.call("now")},
        };
        conflict.updateWhere = upsert.binary(
            upsert.tuple({
                upsert.column("name", "gb28181_channel"),
                upsert.column("custom_name", "gb28181_channel"),
                upsert.column("manufacturer", "gb28181_channel"),
                upsert.column("online", "gb28181_channel"),
                upsert.column("ptz_type", "gb28181_channel"),
            }),
            ruvia::DbBinaryOperator::kIsDistinctFrom,
            upsert.tuple({upsert.excluded("name"),
                          upsert.excluded("custom_name"),
                          upsert.excluded("manufacturer"),
                          upsert.excluded("online"),
                          upsert.excluded("ptz_type")}));
        upsert.onConflict(conflict)
            .returning({upsert.column("id")});

        ruvia::DbQuery incomingIds;
        incomingIds
            .select(incomingIds.value(1))
            .from("incoming", "incoming")
            .where(incomingIds.binary(
                incomingIds.column("id", "incoming"),
                ruvia::DbBinaryOperator::kEqual,
                incomingIds.column("id", "stored")));

        ruvia::DbQuery removal;
        removal
            .with("raw_incoming", rawIncoming,
                  {.columns = {"ordinal", "id", "name", "custom_name",
                               "manufacturer", "online", "ptz_type"}})
            .with("incoming", incoming)
            .with("upserted", upsert)
            .deleteFrom("gb28181_channel", "stored")
            .where(removal.binary(
                removal.column("device_id", "stored"),
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
            removal.deleteFrom("gb28181_record")
                .where(removal.binary(
                    removal.column("device_id"),
                    ruvia::DbBinaryOperator::kEqual,
                    removal.value(device.id)));
            (void)co_await transaction.execute(removal);
            co_return;
        }

        ruvia::DbQuery rawIncoming;
        std::size_t ordinal = 0;
        for (const auto& record : device.records) {
            rawIncoming.values({
                rawIncoming.cast(
                    rawIncoming.value(static_cast<std::int64_t>(ordinal)),
                    ruvia::DbDataType::kBigInt),
                rawIncoming.cast(rawIncoming.value(record.deviceId),
                                 ruvia::DbDataType::kVarchar),
                rawIncoming.cast(rawIncoming.value(record.name),
                                 ruvia::DbDataType::kVarchar),
                rawIncoming.cast(rawIncoming.value(record.filePath),
                                 ruvia::DbDataType::kText),
                rawIncoming.cast(rawIncoming.value(record.address),
                                 ruvia::DbDataType::kText),
                rawIncoming.cast(rawIncoming.value(record.startTime),
                                 ruvia::DbDataType::kTimestampTz),
                rawIncoming.cast(rawIncoming.value(record.endTime),
                                 ruvia::DbDataType::kTimestampTz),
                rawIncoming.cast(rawIncoming.value(record.type),
                                 ruvia::DbDataType::kVarchar),
                rawIncoming.cast(rawIncoming.value(record.recorderId),
                                 ruvia::DbDataType::kVarchar),
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
            .insertInto("gb28181_record",
                        {"device_id", "channel_id", "name", "file_path",
                         "address", "start_time", "end_time", "record_type",
                         "recorder_id"})
            .insertFrom(insertSource);
        ruvia::DbConflictOptions conflict;
        conflict.columns = {"device_id", "channel_id", "start_time",
                            "end_time", "file_path"};
        conflict.update = {
            {"name", upsert.excluded("name")},
            {"address", upsert.excluded("address")},
            {"record_type", upsert.excluded("record_type")},
            {"recorder_id", upsert.excluded("recorder_id")},
        };
        conflict.updateWhere = upsert.binary(
            upsert.tuple({upsert.column("name", "gb28181_record"),
                          upsert.column("address", "gb28181_record"),
                          upsert.column("record_type", "gb28181_record"),
                          upsert.column("recorder_id", "gb28181_record")}),
            ruvia::DbBinaryOperator::kIsDistinctFrom,
            upsert.tuple({upsert.excluded("name"),
                          upsert.excluded("address"),
                          upsert.excluded("record_type"),
                          upsert.excluded("recorder_id")}));
        upsert.onConflict(conflict)
            .returning({upsert.column("channel_id"), upsert.column("start_time"),
                        upsert.column("end_time"), upsert.column("file_path")});

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
                            incomingRecords.column("channel_id", "stored")),
                        ruvia::DbBinaryOperator::kAnd,
                        incomingRecords.binary(
                            incomingRecords.column("start_time", "incoming"),
                            ruvia::DbBinaryOperator::kEqual,
                            incomingRecords.column("start_time", "stored"))),
                    ruvia::DbBinaryOperator::kAnd,
                    incomingRecords.binary(
                        incomingRecords.column("end_time", "incoming"),
                        ruvia::DbBinaryOperator::kEqual,
                        incomingRecords.column("end_time", "stored"))),
                ruvia::DbBinaryOperator::kAnd,
                incomingRecords.binary(
                    incomingRecords.column("file_path", "incoming"),
                    ruvia::DbBinaryOperator::kEqual,
                    incomingRecords.column("file_path", "stored"))));

        ruvia::DbQuery removal;
        removal
            .with("raw_incoming", rawIncoming,
                  {.columns = {"ordinal", "channel_id", "name", "file_path",
                               "address", "start_time", "end_time",
                               "record_type", "recorder_id"}})
            .with("incoming", incoming)
            .with("upserted", upsert)
            .deleteFrom("gb28181_record", "stored")
            .where(removal.binary(
                removal.column("device_id", "stored"),
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
                cursorQuery.column("projection_cursor"),
                ruvia::DbBinaryOperator::kGreaterEqual,
                projectionCursor(cursorQuery, streamMessageId)))
            .from("gb28181_device")
            .where(cursorQuery.binary(
                cursorQuery.column("id"), ruvia::DbBinaryOperator::kEqual,
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
                    "gb28181_device",
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
                        {"remote_address", query.excluded("remote_address")},
                        {"registration_source",
                         query.excluded("registration_source")},
                        {"online", query.excluded("online")},
                        {"last_seen_at", query.excluded("last_seen_at")},
                        {"updated_at", query.call("now")},
                    };
                    break;
                case DeviceChange::Catalog:
                    conflict.update = {
                        {"name", query.excluded("name")},
                        {"manufacturer", query.excluded("manufacturer")},
                        {"remote_address", query.excluded("remote_address")},
                        {"registration_source",
                         query.excluded("registration_source")},
                        {"online", query.excluded("online")},
                        {"last_seen_at", query.excluded("last_seen_at")},
                        {"updated_at", query.call("now")},
                    };
                    break;
                case DeviceChange::Mapping:
                    conflict.update = {
                        {"mapped_device_id", query.excluded("mapped_device_id")},
                        {"updated_at", query.call("now")},
                    };
                    break;
                case DeviceChange::DeviceName:
                    conflict.update = {
                        {"custom_name", query.excluded("custom_name")},
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
                    updateQuery.update("gb28181_channel")
                        .set("custom_name", updateQuery.value(channel.customName))
                        .set("updated_at", updateQuery.call("now"))
                        .where(updateQuery.binary(
                            updateQuery.column("device_id"),
                            ruvia::DbBinaryOperator::kEqual,
                            updateQuery.value(device.id)))
                        .andWhere(updateQuery.binary(
                            updateQuery.column("id"),
                            ruvia::DbBinaryOperator::kEqual,
                            updateQuery.value(channel.id)));
                    (void)co_await transaction.execute(updateQuery);
                }
                break;
        }
        co_await publishDevice(context, transaction, device.id);
        ruvia::DbQuery cursorUpdate(context.resource());
        cursorUpdate
            .update("gb28181_device")
            .set("projection_cursor",
                 projectionCursor(cursorUpdate, streamMessageId))
            .where(cursorUpdate.binary(
                cursorUpdate.column("id"), ruvia::DbBinaryOperator::kEqual,
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
            .from("gb28181_channel")
            .where(channelCount.binary(
                channelCount.column("device_id"),
                ruvia::DbBinaryOperator::kEqual,
                channelCount.column("id", "d")));
        ruvia::DbQuery recordCount;
        recordCount
            .select(recordCount.aggregate("count", {recordCount.star()}))
            .from("gb28181_record")
            .where(recordCount.binary(
                recordCount.column("device_id"),
                ruvia::DbBinaryOperator::kEqual,
                recordCount.column("id", "d")));
        ruvia::DbQuery publishQuery(context.resource());
        publishQuery
            .select({
                publishQuery.column("id", "d"),
                publishQuery.column("name", "d"),
                publishQuery.coalesce(
                    {publishQuery.column("custom_name", "d"),
                     publishQuery.value(std::string_view{})}),
                publishQuery.column("manufacturer", "d"),
                publishQuery.column("remote_address", "d"),
                publishQuery.column("registration_source", "d"),
                publishQuery.column("online", "d"),
                publishQuery.call(
                    "iot_utc_timestamp",
                    {publishQuery.column("last_seen_at", "d")}),
                publishQuery.coalesce(
                    {publishQuery.cast(
                         publishQuery.column("mapped_device_id", "d"),
                         ruvia::DbDataType::kText),
                     publishQuery.value(std::string_view{})}),
                publishQuery.subquery(channelCount),
                publishQuery.subquery(recordCount),
            })
            .from("gb28181_device", "d")
            .where(publishQuery.binary(
                publishQuery.column("id", "d"),
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
                cursorQuery.column("projection_cursor"),
                ruvia::DbBinaryOperator::kGreaterEqual,
                projectionCursor(cursorQuery, streamMessageId)))
            .from("gb28181_stream")
            .where(cursorQuery.binary(
                cursorQuery.column("app"), ruvia::DbBinaryOperator::kEqual,
                cursorQuery.value(stream.app)))
            .andWhere(cursorQuery.binary(
                cursorQuery.column("stream"), ruvia::DbBinaryOperator::kEqual,
                cursorQuery.value(stream.stream)))
            .andWhere(cursorQuery.binary(
                cursorQuery.column("schema"), ruvia::DbBinaryOperator::kEqual,
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
            .insertInto("gb28181_stream",
                        {"app", "stream", "schema", "online", "reader_count",
                         "updated_at"})
            .values({upsert.value(stream.app), upsert.value(stream.stream),
                     upsert.value(stream.schema), upsert.value(stream.online),
                     upsert.value(stream.readerCount), upsert.call("now")});
        ruvia::DbConflictOptions conflict;
        conflict.columns = {"app", "stream", "schema"};
        conflict.update = {
            {"online", upsert.excluded("online")},
            {"reader_count", upsert.excluded("reader_count")},
            {"updated_at", upsert.call("now")},
        };
        conflict.updateWhere = upsert.binary(
            upsert.tuple({upsert.column("online", "gb28181_stream"),
                          upsert.column("reader_count", "gb28181_stream")}),
            ruvia::DbBinaryOperator::kIsDistinctFrom,
            upsert.tuple({upsert.excluded("online"),
                          upsert.excluded("reader_count")}));
        upsert.onConflict(conflict);
        (void)co_await transaction.execute(upsert);
        co_await publishStream(context, stream);
        ruvia::DbQuery cursorUpdate(context.resource());
        cursorUpdate
            .update("gb28181_stream")
            .set("projection_cursor",
                 projectionCursor(cursorUpdate, streamMessageId))
            .where(cursorUpdate.binary(
                cursorUpdate.column("app"), ruvia::DbBinaryOperator::kEqual,
                cursorUpdate.value(stream.app)))
            .andWhere(cursorUpdate.binary(
                cursorUpdate.column("stream"), ruvia::DbBinaryOperator::kEqual,
                cursorUpdate.value(stream.stream)))
            .andWhere(cursorUpdate.binary(
                cursorUpdate.column("schema"), ruvia::DbBinaryOperator::kEqual,
                cursorUpdate.value(stream.schema)));
        (void)co_await transaction.execute(cursorUpdate);
        co_await transaction.commit();
        co_return true;
    }
};

} // namespace service::gb28181
