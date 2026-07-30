#pragma once

#include "device/DeviceRegistry.h"
#include "media/StreamRegistry.h"
#include "service/common/http.h"
#include "service/common/timestamp.h"
#include "service/features/collector/stream.h"

#include <ruvia/web/WebWorker.h>

#include <atomic>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace service::gb28181 {

class Projector final {
public:
  struct Snapshot {
    std::vector<Device> devices;
    std::vector<StreamStatus> streams;
  };

  Projector() = default;
  Projector(const Projector &) = delete;
  Projector &operator=(const Projector &) = delete;

  Snapshot start(ruvia::WebWorkerHandle worker) {
    // Supervisor-only startup barrier. Projection I/O itself runs as a
    // coroutine on the selected Service Worker.
    if (running_.exchange(true))
      return snapshot_;
    worker_ = std::move(worker);
    auto ready = std::make_shared<std::promise<Snapshot>>();
    auto future = ready->get_future();
    const auto posted = worker_.post(
        [ready](ruvia::WebWorkerContext &context) -> ruvia::Task<void> {
          try {
            auto snapshot = co_await hydrate(context);
            for (const auto &device : snapshot.devices)
              co_await publishDevice(context, device);
            for (const auto &stream : snapshot.streams)
              co_await publishStream(context, stream);
            ready->set_value(std::move(snapshot));
          } catch (...) {
            ready->set_exception(std::current_exception());
          }
          co_return;
        });
    if (!posted.accepted()) {
      running_.store(false);
      throw std::runtime_error(
          "service worker rejected GB28181 projection hydration");
    }
    try {
      snapshot_ = future.get();
      return snapshot_;
    } catch (...) {
      running_.store(false);
      worker_ = {};
      throw;
    }
  }

  void stop() noexcept {
    running_.store(false);
    worker_ = {};
  }

  void project(Device device, DeviceRegistry::Change change) {
    if (!running_.load())
      return;
    const auto posted = worker_.post(
        [device = std::move(device),
         change](ruvia::WebWorkerContext &context) -> ruvia::Task<void> {
          try {
            co_await saveDevice(context, device, change);
          } catch (const std::exception &error) {
            std::cerr << "GB28181 device projection failed: " << error.what()
                      << '\n';
          }
        });
    if (!posted.accepted())
      std::cerr << "GB28181 device projection was rejected\n";
  }

  void project(StreamStatus stream) {
    if (!running_.load())
      return;
    const auto posted = worker_.post(
        [stream = std::move(stream)](
            ruvia::WebWorkerContext &context) -> ruvia::Task<void> {
          try {
            co_await saveStream(context, stream);
          } catch (const std::exception &error) {
            std::cerr << "GB28181 stream projection failed: " << error.what()
                      << '\n';
          }
        });
    if (!posted.accepted())
      std::cerr << "GB28181 stream projection was rejected\n";
  }

private:
  static bool boolean(std::string_view value) {
    return value == "t" || value == "true" || value == "1";
  }

  static int integer(std::string_view value, int fallback = 0) {
    try {
      return std::stoi(std::string(value));
    } catch (...) {
      return fallback;
    }
  }

  static ruvia::Task<Snapshot> hydrate(ruvia::WebWorkerContext &context) {
    Snapshot snapshot;
    std::unordered_map<std::string, std::size_t> deviceIndexes;
    const auto devices = co_await context.db().query(R"sql(
SELECT id, name, manufacturer, remote_address, registration_source, online,
       iot_utc_timestamp(last_seen_at), COALESCE(mapped_device_id::text, '')
FROM gb28181_device
ORDER BY id)sql");
    snapshot.devices.reserve(devices.rows().size());
    for (const auto &row : devices.rows()) {
      Device device;
      device.id = std::string(row[0].text());
      device.name = std::string(row[1].text());
      device.manufacturer = std::string(row[2].text());
      device.remoteAddress = std::string(row[3].text());
      device.registrationSource = std::string(row[4].text());
      device.online = boolean(row[5].text());
      if (const auto parsed = service::common::parseUtcTimestamp(row[6].text()))
        device.lastSeen = *parsed;
      device.mappedDeviceId = std::string(row[7].text());
      deviceIndexes.emplace(device.id, snapshot.devices.size());
      snapshot.devices.push_back(std::move(device));
    }

    const auto channels = co_await context.db().query(R"sql(
SELECT device_id, id, name, manufacturer, online, ptz_type
FROM gb28181_channel
ORDER BY device_id, id)sql");
    for (const auto &row : channels.rows()) {
      const auto device = deviceIndexes.find(std::string(row[0].text()));
      if (device == deviceIndexes.end())
        continue;
      snapshot.devices[device->second].channels.push_back(Channel{
          .id = std::string(row[1].text()),
          .name = std::string(row[2].text()),
          .manufacturer = std::string(row[3].text()),
          .online = boolean(row[4].text()),
          .ptzType = integer(row[5].text(), -1),
      });
    }

    const auto records = co_await context.db().query(R"sql(
SELECT device_id, channel_id, name, file_path, address,
       iot_utc_timestamp(start_time), iot_utc_timestamp(end_time),
       record_type, recorder_id
FROM gb28181_record
ORDER BY device_id, start_time DESC)sql");
    for (const auto &row : records.rows()) {
      const auto device = deviceIndexes.find(std::string(row[0].text()));
      if (device == deviceIndexes.end())
        continue;
      snapshot.devices[device->second].records.push_back(RecordItem{
          .deviceId = std::string(row[1].text()),
          .name = std::string(row[2].text()),
          .filePath = std::string(row[3].text()),
          .address = std::string(row[4].text()),
          .startTime = std::string(row[5].text()),
          .endTime = std::string(row[6].text()),
          .type = std::string(row[7].text()),
          .recorderId = std::string(row[8].text()),
      });
    }

    const auto streams = co_await context.db().query(R"sql(
SELECT app, stream, schema, online, reader_count
FROM gb28181_stream
ORDER BY app, stream, schema)sql");
    snapshot.streams.reserve(streams.rows().size());
    for (const auto &row : streams.rows()) {
      snapshot.streams.push_back(StreamStatus{
          .app = std::string(row[0].text()),
          .stream = std::string(row[1].text()),
          .schema = std::string(row[2].text()),
          .online = boolean(row[3].text()),
          .readerCount = integer(row[4].text()),
      });
    }
    co_return snapshot;
  }

  template <typename Transaction>
  static ruvia::Task<void> saveDeviceBase(Transaction &transaction,
                                          const Device &device) {
    const auto lastSeen = service::common::utcTimestamp(device.lastSeen);
    (void)co_await transaction.execute(
        R"sql(
INSERT INTO gb28181_device(
    id, name, manufacturer, remote_address, registration_source, online,
    last_seen_at, mapped_device_id, updated_at)
VALUES ($1, $2, $3, $4, $5, $6, $7::timestamptz,
        NULLIF($8, '')::uuid, NOW())
ON CONFLICT (id) DO UPDATE
SET name = EXCLUDED.name,
    manufacturer = EXCLUDED.manufacturer,
    remote_address = EXCLUDED.remote_address,
    registration_source = EXCLUDED.registration_source,
    online = EXCLUDED.online,
    last_seen_at = EXCLUDED.last_seen_at,
    mapped_device_id = EXCLUDED.mapped_device_id,
    updated_at = NOW())sql",
        service::common::dbParams(device.id, device.name, device.manufacturer,
                                  device.remoteAddress,
                                  device.registrationSource, device.online,
                                  lastSeen, device.mappedDeviceId));
  }

  template <typename Transaction>
  static ruvia::Task<void> syncChannels(Transaction &transaction,
                                        const Device &device) {
    if (device.channels.empty()) {
      (void)co_await transaction.execute(
          "DELETE FROM gb28181_channel WHERE device_id = $1",
          service::common::dbParams(device.id));
      co_return;
    }

    std::string sql = R"sql(
WITH raw_incoming(ordinal, id, name, manufacturer, online, ptz_type)
AS (VALUES )sql";
    std::vector<ruvia::DbValue> params;
    params.reserve(1 + device.channels.size() * 5);
    params.emplace_back(std::string_view(device.id));
    for (const auto &channel : device.channels) {
      if (params.size() != 1)
        sql.push_back(',');
      const auto base = params.size() + 1;
      sql += "(" + std::to_string((params.size() - 1) / 5) + "::bigint,$" +
             std::to_string(base) + "::varchar,$" + std::to_string(base + 1) +
             "::varchar,$" + std::to_string(base + 2) + "::varchar,$" +
             std::to_string(base + 3) + "::boolean,$" +
             std::to_string(base + 4) + "::integer)";
      params.emplace_back(std::string_view(channel.id));
      params.emplace_back(std::string_view(channel.name));
      params.emplace_back(std::string_view(channel.manufacturer));
      params.emplace_back(channel.online);
      params.emplace_back(channel.ptzType);
    }
    sql += R"sql(), incoming AS (
  SELECT DISTINCT ON (id) id, name, manufacturer, online, ptz_type
  FROM raw_incoming
  ORDER BY id, ordinal DESC
), upserted AS (
  INSERT INTO gb28181_channel(
      device_id, id, name, manufacturer, online, ptz_type, updated_at)
  SELECT $1, incoming.id, incoming.name, incoming.manufacturer,
         incoming.online, incoming.ptz_type, NOW()
  FROM incoming
  ON CONFLICT (device_id, id) DO UPDATE SET
      name = EXCLUDED.name,
      manufacturer = EXCLUDED.manufacturer,
      online = EXCLUDED.online,
      ptz_type = EXCLUDED.ptz_type,
      updated_at = NOW()
  WHERE (gb28181_channel.name, gb28181_channel.manufacturer,
         gb28181_channel.online, gb28181_channel.ptz_type)
        IS DISTINCT FROM
        (EXCLUDED.name, EXCLUDED.manufacturer, EXCLUDED.online,
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
  static ruvia::Task<void> syncRecords(Transaction &transaction,
                                       const Device &device) {
    if (device.records.empty()) {
      (void)co_await transaction.execute(
          "DELETE FROM gb28181_record WHERE device_id = $1",
          service::common::dbParams(device.id));
      co_return;
    }

    std::string sql = R"sql(
WITH raw_incoming(
    ordinal, channel_id, name, file_path, address, start_time, end_time,
    record_type, recorder_id) AS (VALUES )sql";
    std::vector<ruvia::DbValue> params;
    params.reserve(1 + device.records.size() * 8);
    params.emplace_back(std::string_view(device.id));
    for (const auto &record : device.records) {
      if (params.size() != 1)
        sql.push_back(',');
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

  static ruvia::Task<void> saveDevice(ruvia::WebWorkerContext &context,
                                      const Device &device,
                                      DeviceRegistry::Change change) {
    auto transaction = co_await context.db().beginTransaction();
    // Observer callbacks are fire-and-forget, but every snapshot for the
    // same SIP device must reach PostgreSQL in actor order.
    (void)co_await transaction.execute(
        "SELECT pg_advisory_xact_lock(hashtextextended($1, 28181))",
        service::common::dbParams(device.id));
    co_await saveDeviceBase(transaction, device);
    if (change == DeviceRegistry::Change::Catalog)
      co_await syncChannels(transaction, device);
    else if (change == DeviceRegistry::Change::Records)
      co_await syncRecords(transaction, device);
    co_await transaction.commit();

    co_await publishDevice(context, device);
  }

  static ruvia::Task<void> publishDevice(ruvia::WebWorkerContext &context,
                                         const Device &device) {
    const auto lastSeen = service::common::utcTimestamp(device.lastSeen);
    const auto key = "iot:state:gb28181:device:" + device.id;
    co_await service::message::redis::setHash(
        context.redis(), key,
        {{"id", device.id},
         {"name", device.name},
         {"online", device.online ? "1" : "0"},
         {"remote_address", device.remoteAddress},
         {"registration_source", device.registrationSource},
         {"mapped_device_id", device.mappedDeviceId},
         {"last_seen_at", lastSeen},
         {"channel_count", std::to_string(device.channels.size())},
         {"record_count", std::to_string(device.records.size())}});
    (void)co_await service::message::redis::command(
        context.redis(), {"SADD", "iot:state:gb28181:devices", device.id});
  }

  static ruvia::Task<void> saveStream(ruvia::WebWorkerContext &context,
                                      const StreamStatus &stream) {
    (void)co_await context.db().execute(
        R"sql(
INSERT INTO gb28181_stream(app, stream, schema, online, reader_count, updated_at)
VALUES ($1, $2, $3, $4, $5, NOW())
ON CONFLICT (app, stream, schema) DO UPDATE
SET online = EXCLUDED.online,
    reader_count = EXCLUDED.reader_count,
    updated_at = NOW()
WHERE (gb28181_stream.online, gb28181_stream.reader_count)
      IS DISTINCT FROM (EXCLUDED.online, EXCLUDED.reader_count))sql",
        service::common::dbParams(stream.app, stream.stream, stream.schema,
                                  stream.online, stream.readerCount));
    co_await publishStream(context, stream);
  }

  static ruvia::Task<void> publishStream(ruvia::WebWorkerContext &context,
                                         const StreamStatus &stream) {
    const auto identity =
        StreamRegistry::identity(stream.app, stream.stream, stream.schema);
    const auto key = "iot:state:gb28181:stream:" + identity;
    co_await service::message::redis::setHash(
        context.redis(), key,
        {{"id", identity},
         {"app", stream.app},
         {"stream", stream.stream},
         {"schema", stream.schema},
         {"online", stream.online ? "1" : "0"},
         {"reader_count", std::to_string(stream.readerCount)}});
    (void)co_await service::message::redis::command(
        context.redis(), {"SADD", "iot:state:gb28181:streams", identity});
  }

  std::atomic_bool running_{false};
  ruvia::WebWorkerHandle worker_;
  Snapshot snapshot_;
};

} // namespace service::gb28181
