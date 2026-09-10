#pragma once

#include <atomic>
#include <charconv>
#include <cstddef>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ruvia/web/WebWorker.h>

#include "service/common/http.h"
#include "service/common/message.h"
#include "service/common/timestamp.h"
#include "service/features/event/event.transport.h"
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
        const auto devices = co_await context.db().query(
            "SELECT id FROM gb28181_device WHERE online = TRUE ORDER BY id"
        );
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
            (void)co_await transaction.query(
                "SELECT pg_advisory_xact_lock(hashtextextended($1, 28181))",
                service::common::dbParams(id)
            );
            const auto current = co_await transaction.query(
                "SELECT online FROM gb28181_device WHERE id = $1 FOR UPDATE",
                service::common::dbParams(id)
            );
            if (current.empty() ||
                !boolean(current.front()[0].value().value_or(std::string_view{}))) {
                co_await transaction.commit();
                continue;
            }
            if (co_await context.redis().get(ownerKey)) {
                co_await transaction.commit();
                continue;
            }
            (void)co_await transaction.execute(
                "UPDATE gb28181_device SET online = FALSE, updated_at = NOW() "
                "WHERE id = $1 AND online = TRUE",
                service::common::dbParams(id)
            );
            // Keep durable metadata untouched.  This is a targeted cache update so
            // a fresh REGISTER projection can restore the full metadata hash.
            co_await service::message::redis::setHash(
                context.redis(),
                "iot:state:gb28181:device:" + id,
                { { "online", "0" } }
            );
            co_await transaction.commit();
        }

        const auto streams = co_await context.db().query(
            "SELECT app, stream, schema FROM gb28181_stream "
            "WHERE online = TRUE ORDER BY app, stream, schema"
        );
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
                "SELECT pg_advisory_xact_lock(hashtextextended($1, 28181))",
                service::common::dbParams(identity)
            );
            const auto current = co_await transaction.query(
                "SELECT online FROM gb28181_stream "
                "WHERE app = $1 AND stream = $2 AND schema = $3 FOR UPDATE",
                service::common::dbParams(app, stream, schema)
            );
            if (current.empty() ||
                !boolean(current.front()[0].value().value_or(std::string_view{}))) {
                co_await transaction.commit();
                continue;
            }
            if (co_await context.redis().get(ownerKey)) {
                co_await transaction.commit();
                continue;
            }
            (void)co_await transaction.execute(
                "UPDATE gb28181_stream SET online = FALSE, reader_count = 0, "
                "updated_at = NOW() WHERE app = $1 AND stream = $2 AND schema = $3 "
                "AND online = TRUE",
                service::common::dbParams(app, stream, schema)
            );
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
        const auto devices = co_await context.db().query(R"sql(
SELECT id, name, COALESCE(custom_name, ''), manufacturer, remote_address,
       registration_source, online, iot_utc_timestamp(last_seen_at),
       COALESCE(mapped_device_id::text, '')
FROM gb28181_device
ORDER BY id)sql");
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

        const auto channels = co_await context.db().query(R"sql(
SELECT device_id, id, name, COALESCE(custom_name, ''), manufacturer, online,
       ptz_type
FROM gb28181_channel
ORDER BY device_id, id)sql");
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

        const auto records = co_await context.db().query(R"sql(
SELECT device_id, channel_id, name, file_path, address,
       iot_utc_timestamp(start_time), iot_utc_timestamp(end_time),
       record_type, recorder_id
FROM gb28181_record
ORDER BY device_id, start_time DESC)sql");
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

        const auto streams = co_await context.db().query(R"sql(
SELECT app, stream, schema, online, reader_count
FROM gb28181_stream
ORDER BY app, stream, schema)sql");
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
            (void)co_await transaction.execute(
                "DELETE FROM gb28181_channel WHERE device_id = $1",
                service::common::dbParams(device.id)
            );
            co_return;
        }

        std::string sql = R"sql(
WITH raw_incoming(
    ordinal, id, name, custom_name, manufacturer, online, ptz_type)
AS (VALUES )sql";
        std::vector<ruvia::DbValue> params;
        params.reserve(1 + device.channels.size() * 6);
        params.emplace_back(std::string_view(device.id));
        for (const auto& channel : device.channels) {
            if (params.size() != 1) {
                sql.push_back(',');
            }
            const auto base = params.size() + 1;
            sql += "(" + std::to_string((params.size() - 1) / 6) + "::bigint,$" +
                std::to_string(base) + "::varchar,$" + std::to_string(base + 1) +
                "::varchar,$" + std::to_string(base + 2) + "::varchar,$" +
                std::to_string(base + 3) + "::varchar,$" +
                std::to_string(base + 4) + "::boolean,$" +
                std::to_string(base + 5) + "::integer)";
            params.emplace_back(std::string_view(channel.id));
            params.emplace_back(std::string_view(channel.name));
            params.emplace_back(std::string_view(channel.customName));
            params.emplace_back(std::string_view(channel.manufacturer));
            params.emplace_back(channel.online);
            params.emplace_back(channel.ptzType);
        }
        sql += R"sql(), incoming AS (
  SELECT DISTINCT ON (id) id, name, custom_name, manufacturer, online, ptz_type
  FROM raw_incoming
  ORDER BY id, ordinal DESC
), upserted AS (
  INSERT INTO gb28181_channel(
      device_id, id, name, custom_name, manufacturer, online, ptz_type,
      updated_at)
  SELECT $1, incoming.id, incoming.name, NULLIF(incoming.custom_name, ''),
         incoming.manufacturer, incoming.online, incoming.ptz_type, NOW()
  FROM incoming
  ON CONFLICT (device_id, id) DO UPDATE SET
      name = EXCLUDED.name,
      custom_name = )sql";
        sql += updateCustomNames
            ? "EXCLUDED.custom_name"
            : "COALESCE(EXCLUDED.custom_name, gb28181_channel.custom_name)";
        sql += R"sql(,
      manufacturer = EXCLUDED.manufacturer,
      online = EXCLUDED.online,
      ptz_type = EXCLUDED.ptz_type,
      updated_at = NOW()
  WHERE (gb28181_channel.name, gb28181_channel.custom_name,
         gb28181_channel.manufacturer,
         gb28181_channel.online, gb28181_channel.ptz_type)
        IS DISTINCT FROM
        (EXCLUDED.name, EXCLUDED.custom_name, EXCLUDED.manufacturer, EXCLUDED.online,
         EXCLUDED.ptz_type)
  RETURNING id
)
DELETE FROM gb28181_channel stored
WHERE stored.device_id = $1
  AND NOT EXISTS (
      SELECT 1 FROM incoming WHERE incoming.id = stored.id
  ))sql";
        (void)co_await transaction.execute(sql, params);
    }

    template <typename Transaction>
    static ruvia::Task<void> syncRecords(Transaction& transaction, const Device& device) {
        if (device.records.empty()) {
            (void)co_await transaction.execute(
                "DELETE FROM gb28181_record WHERE device_id = $1",
                service::common::dbParams(device.id)
            );
            co_return;
        }

        std::string sql = R"sql(
WITH raw_incoming(
    ordinal, channel_id, name, file_path, address, start_time, end_time,
    record_type, recorder_id) AS (VALUES )sql";
        std::vector<ruvia::DbValue> params;
        params.reserve(1 + device.records.size() * 8);
        params.emplace_back(std::string_view(device.id));
        for (const auto& record : device.records) {
            if (params.size() != 1) {
                sql.push_back(',');
            }
            const auto base = params.size() + 1;
            sql += "(" + std::to_string((params.size() - 1) / 8) + "::bigint,$" +
                std::to_string(base) + "::varchar,$" + std::to_string(base + 1) +
                "::varchar,$" + std::to_string(base + 2) + "::text,$" +
                std::to_string(base + 3) + "::text,$" + std::to_string(base + 4) +
                "::timestamptz,$" + std::to_string(base + 5) + "::timestamptz,$" +
                std::to_string(base + 6) + "::varchar,$" +
                std::to_string(base + 7) + "::varchar)";
            params.emplace_back(std::string_view(record.deviceId));
            params.emplace_back(std::string_view(record.name));
            params.emplace_back(std::string_view(record.filePath));
            params.emplace_back(std::string_view(record.address));
            params.emplace_back(std::string_view(record.startTime));
            params.emplace_back(std::string_view(record.endTime));
            params.emplace_back(std::string_view(record.type));
            params.emplace_back(std::string_view(record.recorderId));
        }
        sql += R"sql(), incoming AS (
  SELECT DISTINCT ON (channel_id, start_time, end_time, file_path)
         channel_id, name, file_path, address, start_time, end_time,
         record_type, recorder_id
  FROM raw_incoming
  ORDER BY channel_id, start_time, end_time, file_path, ordinal DESC
), upserted AS (
  INSERT INTO gb28181_record(
      device_id, channel_id, name, file_path, address, start_time, end_time,
      record_type, recorder_id)
  SELECT $1, incoming.channel_id, incoming.name, incoming.file_path,
         incoming.address, incoming.start_time, incoming.end_time,
         incoming.record_type, incoming.recorder_id
  FROM incoming
  ON CONFLICT (device_id, channel_id, start_time, end_time, file_path)
  DO UPDATE SET
      name = EXCLUDED.name,
      address = EXCLUDED.address,
      record_type = EXCLUDED.record_type,
      recorder_id = EXCLUDED.recorder_id
  WHERE (gb28181_record.name, gb28181_record.address,
         gb28181_record.record_type, gb28181_record.recorder_id)
        IS DISTINCT FROM
        (EXCLUDED.name, EXCLUDED.address, EXCLUDED.record_type,
         EXCLUDED.recorder_id)
  RETURNING channel_id, start_time, end_time, file_path
)
DELETE FROM gb28181_record stored
WHERE stored.device_id = $1
  AND NOT EXISTS (
      SELECT 1
      FROM incoming
      WHERE incoming.channel_id = stored.channel_id
        AND incoming.start_time = stored.start_time
        AND incoming.end_time = stored.end_time
        AND incoming.file_path = stored.file_path
  ))sql";
        (void)co_await transaction.execute(sql, params);
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
            "SELECT pg_advisory_xact_lock(hashtextextended($1, 28181))",
            service::common::dbParams(device.id)
        );
        const auto cursor = co_await transaction.query(
            R"sql(
SELECT projection_cursor >= (
    split_part($2, '-', 1)::numeric * 18446744073709551616
    + split_part($2, '-', 2)::numeric)
FROM gb28181_device
WHERE id = $1
FOR UPDATE)sql",
            service::common::dbParams(device.id, streamMessageId)
        );
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

        switch (change) {
            case DeviceChange::Status:
                (void)co_await transaction.execute(
                    R"sql(
INSERT INTO gb28181_device(
    id, name, custom_name, manufacturer, remote_address, registration_source,
    online, last_seen_at, mapped_device_id, updated_at)
VALUES ($1, $2, NULLIF($3, ''), $4, $5, $6, $7, $8::timestamptz,
        NULLIF($9, '')::uuid, NOW())
ON CONFLICT (id) DO UPDATE SET
    remote_address = EXCLUDED.remote_address,
    registration_source = EXCLUDED.registration_source,
    online = EXCLUDED.online,
    last_seen_at = EXCLUDED.last_seen_at,
    updated_at = NOW())sql",
                    service::common::dbParams(device.id, device.name, device.customName, device.manufacturer, device.remoteAddress, device.registrationSource, device.online, lastSeen, device.mappedDeviceId)
                );
                break;
            case DeviceChange::Catalog:
                (void)co_await transaction.execute(
                    R"sql(
INSERT INTO gb28181_device(
    id, name, custom_name, manufacturer, remote_address, registration_source,
    online, last_seen_at, mapped_device_id, updated_at)
VALUES ($1, $2, NULLIF($3, ''), $4, $5, $6, $7, $8::timestamptz,
        NULLIF($9, '')::uuid, NOW())
ON CONFLICT (id) DO UPDATE SET
    name = EXCLUDED.name,
    manufacturer = EXCLUDED.manufacturer,
    remote_address = EXCLUDED.remote_address,
    registration_source = EXCLUDED.registration_source,
    online = EXCLUDED.online,
    last_seen_at = EXCLUDED.last_seen_at,
    updated_at = NOW())sql",
                    service::common::dbParams(device.id, device.name, device.customName, device.manufacturer, device.remoteAddress, device.registrationSource, device.online, lastSeen, device.mappedDeviceId)
                );
                co_await syncChannels(transaction, device, false);
                break;
            case DeviceChange::Records:
                (void)co_await transaction.execute(
                    R"sql(
INSERT INTO gb28181_device(
    id, name, custom_name, manufacturer, remote_address, registration_source,
    online, last_seen_at, mapped_device_id, updated_at)
VALUES ($1, $2, NULLIF($3, ''), $4, $5, $6, $7, $8::timestamptz,
        NULLIF($9, '')::uuid, NOW())
ON CONFLICT (id) DO UPDATE SET
    remote_address = EXCLUDED.remote_address,
    registration_source = EXCLUDED.registration_source,
    online = EXCLUDED.online,
    last_seen_at = EXCLUDED.last_seen_at,
    updated_at = NOW())sql",
                    service::common::dbParams(device.id, device.name, device.customName, device.manufacturer, device.remoteAddress, device.registrationSource, device.online, lastSeen, device.mappedDeviceId)
                );
                co_await syncRecords(transaction, device);
                break;
            case DeviceChange::Mapping:
                (void)co_await transaction.execute(
                    R"sql(
INSERT INTO gb28181_device(
    id, name, custom_name, manufacturer, remote_address, registration_source,
    online, last_seen_at, mapped_device_id, updated_at)
VALUES ($1, $2, NULLIF($3, ''), $4, $5, $6, $7, $8::timestamptz,
        NULLIF($9, '')::uuid, NOW())
ON CONFLICT (id) DO UPDATE SET
    mapped_device_id = EXCLUDED.mapped_device_id,
    updated_at = NOW())sql",
                    service::common::dbParams(device.id, device.name, device.customName, device.manufacturer, device.remoteAddress, device.registrationSource, device.online, lastSeen, device.mappedDeviceId)
                );
                break;
            case DeviceChange::DeviceName:
                (void)co_await transaction.execute(
                    R"sql(
INSERT INTO gb28181_device(
    id, name, custom_name, manufacturer, remote_address, registration_source,
    online, last_seen_at, mapped_device_id, updated_at)
VALUES ($1, $2, NULLIF($3, ''), $4, $5, $6, $7, $8::timestamptz,
        NULLIF($9, '')::uuid, NOW())
ON CONFLICT (id) DO UPDATE SET
    custom_name = EXCLUDED.custom_name,
    updated_at = NOW())sql",
                    service::common::dbParams(device.id, device.name, device.customName, device.manufacturer, device.remoteAddress, device.registrationSource, device.online, lastSeen, device.mappedDeviceId)
                );
                break;
            case DeviceChange::ChannelName:
                for (const auto& channel : device.channels) {
                    if (channel.customName.empty()) {
                        continue;
                    }
                    (void)co_await transaction.execute(
                        "UPDATE gb28181_channel SET custom_name = $3, updated_at = NOW() WHERE device_id = $1 AND id = $2",
                        service::common::dbParams(device.id, channel.id, channel.customName)
                    );
                }
                break;
        }
        co_await publishDevice(context, transaction, device.id);
        (void)co_await transaction.execute(
            R"sql(
UPDATE gb28181_device
SET projection_cursor =
    split_part($2, '-', 1)::numeric * 18446744073709551616
    + split_part($2, '-', 2)::numeric
WHERE id = $1)sql",
            service::common::dbParams(device.id, streamMessageId)
        );
        co_await transaction.commit();
        co_return true;
    }

    template <typename Transaction>
    static ruvia::Task<void> publishDevice(ruvia::WebWorkerContext& context, Transaction& transaction, const std::string& id) {
        const auto rows = co_await transaction.query(R"sql(
SELECT id, name, COALESCE(custom_name,''), manufacturer, remote_address,
       registration_source, online, iot_utc_timestamp(last_seen_at),
       COALESCE(mapped_device_id::text,''),
       (SELECT count(*) FROM gb28181_channel WHERE device_id=d.id),
       (SELECT count(*) FROM gb28181_record WHERE device_id=d.id)
FROM gb28181_device d WHERE id=$1)sql",
                                                     service::common::dbParams(id));
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
            "SELECT pg_advisory_xact_lock(hashtextextended($1, 28181))",
            service::common::dbParams(identity)
        );
        const auto cursor = co_await transaction.query(
            R"sql(
SELECT projection_cursor >= (
    split_part($4, '-', 1)::numeric * 18446744073709551616
    + split_part($4, '-', 2)::numeric)
FROM gb28181_stream
WHERE app = $1 AND stream = $2 AND schema = $3
FOR UPDATE)sql",
            service::common::dbParams(stream.app, stream.stream, stream.schema, streamMessageId)
        );
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
        (void)co_await transaction.execute(
            R"sql(
INSERT INTO gb28181_stream(app, stream, schema, online, reader_count, updated_at)
VALUES ($1, $2, $3, $4, $5, NOW())
ON CONFLICT (app, stream, schema) DO UPDATE
SET online = EXCLUDED.online,
    reader_count = EXCLUDED.reader_count,
    updated_at = NOW()
WHERE (gb28181_stream.online, gb28181_stream.reader_count)
      IS DISTINCT FROM (EXCLUDED.online, EXCLUDED.reader_count))sql",
            service::common::dbParams(stream.app, stream.stream, stream.schema, stream.online, stream.readerCount)
        );
        co_await publishStream(context, stream);
        (void)co_await transaction.execute(
            R"sql(
UPDATE gb28181_stream
SET projection_cursor =
    split_part($1, '-', 1)::numeric * 18446744073709551616
    + split_part($1, '-', 2)::numeric
WHERE app = $2 AND stream = $3 AND schema = $4)sql",
            service::common::dbParams(streamMessageId, stream.app, stream.stream, stream.schema)
        );
        co_await transaction.commit();
        co_return true;
    }
};

} // namespace service::gb28181
