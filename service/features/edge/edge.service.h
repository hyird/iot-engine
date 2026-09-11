#pragma once

#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/web/WebWorker.h>

#include "service/common/http.h"
#include "service/common/uuid.h"
#include "service/features/messaging/messaging.transport.h"
#include "service/features/edge/edge.transport.h"
#include "service/features/edge/session/session.service.h"
#include "service/features/vpn/vpn.service.h"

namespace service::edge::metadata {

inline constexpr std::string_view kStoreNodeScript = R"lua(
redis.call('DEL', KEYS[1])
for index = 1, #ARGV, 2 do
  redis.call('HSET', KEYS[1], ARGV[index], ARGV[index + 1])
end
return #ARGV / 2
)lua";

inline constexpr std::string_view kLoadNodeSql = R"sql(
SELECT d.id::text, d.link_id::text, d.protocol_params->>'device_code', p.protocol,
       COALESCE(NULLIF(p.config->>'storagePolicy', ''), 'report'),
       COALESCE(NULLIF(d.protocol_params->>'online_timeout', ''), '300')
FROM device d
JOIN link l ON l.id = d.link_id AND l.execution = 'edge' AND l.deleted_at IS NULL
JOIN device_model p ON p.device_id = d.id AND p.deleted_at IS NULL
WHERE l.edge_node_id = $1::uuid AND d.deleted_at IS NULL
ORDER BY d.id)sql";

inline constexpr std::string_view kLoadCatalogSql = R"sql(
SELECT n.id::text, d.id::text, d.link_id::text, d.protocol_params->>'device_code', p.protocol,
       COALESCE(NULLIF(p.config->>'storagePolicy', ''), 'report'),
       COALESCE(NULLIF(d.protocol_params->>'online_timeout', ''), '300')
FROM edge_node n
LEFT JOIN link l ON l.edge_node_id = n.id AND l.execution = 'edge' AND l.deleted_at IS NULL
LEFT JOIN device d ON d.link_id = l.id AND d.deleted_at IS NULL
LEFT JOIN device_model p ON p.device_id = d.id AND p.deleted_at IS NULL
ORDER BY n.id, d.id)sql";

struct Device final {
    std::string linkId;
    std::string deviceCode;
    std::string protocol;
    std::string storagePolicy{"report"};
    std::int64_t onlineWindowMs{300000};
};

using NodeSnapshot = std::unordered_map<std::string, Device>;
using Catalog = std::unordered_map<std::string, NodeSnapshot>;

inline std::string key(std::string_view nodeId) {
    return "iot:edge:metadata:" + std::string(nodeId);
}

inline void appendField(std::string& output, std::string_view value) {
    output += std::to_string(value.size());
    output.push_back(':');
    output.append(value);
}

inline std::string encode(const Device& device) {
    std::string output;
    output.reserve(device.linkId.size() + device.deviceCode.size() + device.protocol.size() + 48);
    appendField(output, device.linkId);
    appendField(output, device.deviceCode);
    appendField(output, device.protocol);
    appendField(output, device.storagePolicy);
    appendField(output, std::to_string(device.onlineWindowMs));
    return output;
}

inline std::optional<std::string_view> takeField(std::string_view value,
                                                 std::size_t& offset) noexcept {
    const auto colon = value.find(':', offset);
    if (colon == std::string_view::npos)
        return std::nullopt;
    std::size_t size{};
    const auto [end, error] =
        std::from_chars(value.data() + offset, value.data() + colon, size);
    if (error != std::errc{} || end != value.data() + colon || size > value.size() - colon - 1)
        return std::nullopt;
    const auto begin = colon + 1;
    offset = begin + size;
    return value.substr(begin, size);
}

inline std::optional<std::int64_t> integer(std::string_view value) noexcept {
    std::int64_t result{};
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size())
        return std::nullopt;
    return result;
}

inline bool validStoragePolicy(std::string_view value) noexcept {
    return value == "report" || value == "change";
}

inline std::int64_t onlineWindowMilliseconds(std::string_view value,
                                             std::int64_t fallbackSeconds = 300) noexcept {
    auto seconds = integer(value).value_or(fallbackSeconds);
    if (seconds < 1)
        seconds = fallbackSeconds;
    if (seconds > std::numeric_limits<std::int64_t>::max() / 1000)
        return std::numeric_limits<std::int64_t>::max();
    return seconds * 1000;
}

inline std::optional<Device> decode(std::string_view value) {
    std::size_t offset{};
    const auto linkId = takeField(value, offset);
    const auto deviceCode = takeField(value, offset);
    const auto protocol = takeField(value, offset);
    const auto storagePolicy = takeField(value, offset);
    const auto onlineWindowMs = takeField(value, offset);
    if (!linkId || !deviceCode || !protocol || !storagePolicy || !onlineWindowMs ||
        offset != value.size())
        return std::nullopt;
    const auto online = integer(*onlineWindowMs);
    if (!validStoragePolicy(*storagePolicy) || !online || *online < 1000)
        return std::nullopt;
    return Device{std::string(*linkId), std::string(*deviceCode), std::string(*protocol),
                  std::string(*storagePolicy), *online};
}

template <typename Pipeline>
void queueStoreNode(Pipeline& pipeline, std::string_view nodeId,
                     const NodeSnapshot& snapshot) {
    const std::vector<std::string> keys{key(nodeId)};
    std::vector<std::string> arguments;
    arguments.reserve(snapshot.size() * 2);
    for (const auto& [deviceId, device] : snapshot) {
        arguments.push_back(deviceId);
        arguments.push_back(encode(device));
    }
    const std::vector<std::string_view> keyViews(keys.begin(), keys.end());
    const std::vector<std::string_view> argumentViews(arguments.begin(), arguments.end());
    service::message::redis::queueEval(pipeline, kStoreNodeScript, keyViews, argumentViews);
}

template <typename Context>
ruvia::Task<NodeSnapshot> loadNodeFromDatabase(Context& context, std::string_view nodeId) {
    const auto rows = co_await context.db().query(kLoadNodeSql,
                                                    service::common::dbParams(nodeId));
    NodeSnapshot snapshot;
    snapshot.reserve(rows.size());
    for (const auto& row : rows) {
        snapshot.emplace(
            std::string(row[0].value().value_or(std::string_view{})),
            Device{std::string(row[1].value().value_or(std::string_view{})), std::string(row[2].value().value_or(std::string_view{})),
                   std::string(row[3].value().value_or(std::string_view{})), std::string(row[4].value().value_or(std::string_view{})),
                   onlineWindowMilliseconds(row[5].value().value_or(std::string_view{}))});
    }
    co_return snapshot;
}

template <typename Context>
ruvia::Task<Catalog> loadCatalogFromDatabase(Context& context) {
    const auto rows = co_await context.db().query(kLoadCatalogSql);
    Catalog catalog;
    for (const auto& row : rows) {
        auto& snapshot = catalog[std::string(row[0].value().value_or(std::string_view{}))];
        if (!row[1].value().has_value() || !row[2].value().has_value() || !row[3].value().has_value() || !row[4].value().has_value())
            continue;
        snapshot.emplace(
            std::string(row[1].value().value_or(std::string_view{})),
            Device{std::string(row[2].value().value_or(std::string_view{})), std::string(row[3].value().value_or(std::string_view{})),
                   std::string(row[4].value().value_or(std::string_view{})), std::string(row[5].value().value_or(std::string_view{})),
                   onlineWindowMilliseconds(row[6].value().value_or(std::string_view{}))});
    }
    co_return catalog;
}

template <typename Redis>
ruvia::Task<void> storeNode(const Redis& redis, std::string_view nodeId,
                             const NodeSnapshot& snapshot, bool notify) {
    const std::vector<std::string> keyStore{key(nodeId)};
    std::vector<std::string> argumentStore;
    argumentStore.reserve(snapshot.size() * 2);
    for (const auto& [deviceId, device] : snapshot) {
        argumentStore.push_back(deviceId);
        argumentStore.push_back(encode(device));
    }
    std::vector<std::string_view> keys(keyStore.begin(), keyStore.end());
    std::vector<std::string_view> arguments(argumentStore.begin(), argumentStore.end());
    const auto reply = co_await redis.eval(kStoreNodeScript,
                                           std::span<const std::string_view>(keys),
                                           std::span<const std::string_view>(arguments));
    if (reply.kind() != ruvia::RedisValue::Kind::kInteger)
        service::message::redis::throwValue("store edge metadata", reply);
    if (!notify)
        co_return;
    const auto session = co_await redis.get(session_state::key(nodeId));
    if (!session)
        co_return;
    const auto owner = session_state::parse(
        std::string_view(session->data(), session->size()));
    if (owner)
        co_await projector_stream::publishMetadata(redis, owner->workerIndex, nodeId, owner->instanceId);
}

template <typename Redis>
ruvia::Task<NodeSnapshot> loadNode(const Redis& redis, std::string_view nodeId) {
    const auto reply =
        co_await service::message::redis::command(redis, {"HGETALL", key(nodeId)});
    if (reply.kind() != ruvia::RedisValue::Kind::kArray)
        service::message::redis::throwValue("load edge metadata", reply);
    NodeSnapshot snapshot;
    const auto& fields = reply.array();
    snapshot.reserve(fields.size() / 2);
    for (std::size_t index = 0; index + 1 < fields.size(); index += 2) {
        if (fields[index].kind() != ruvia::RedisValue::Kind::kString ||
            fields[index + 1].kind() != ruvia::RedisValue::Kind::kString)
            throw std::runtime_error("edge metadata hash contains a non-string field");
        auto device = decode(fields[index + 1].string());
        if (!device)
            throw std::runtime_error("edge metadata hash contains an invalid device snapshot");
        snapshot.emplace(std::string(fields[index].string()), std::move(*device));
    }
    co_return snapshot;
}

template <typename Context>
ruvia::Task<void> publishNode(Context& context, std::string_view nodeId) {
    auto snapshot = co_await loadNodeFromDatabase(context, nodeId);
    co_await storeNode(context.redis(), nodeId, snapshot, true);
}

template <typename Context>
ruvia::Task<Catalog> hydrate(Context& context) {
    auto catalog = co_await loadCatalogFromDatabase(context);
    if (catalog.empty())
        co_return catalog;
    const auto redis = context.redis();
    auto pipeline = redis.pipeline();
    for (const auto& [nodeId, snapshot] : catalog)
        queueStoreNode(pipeline, nodeId, snapshot);
    const auto replies = co_await std::move(pipeline).exec();
    service::message::redis::requirePipelineSuccess("hydrate edge metadata", replies);
    co_return catalog;
}

} // namespace service::edge::metadata

#include <algorithm>
#include <array>
#include <cmath>
#include <set>

#include <openssl/evp.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <ruvia/web/Controller.h>

#include "service/features/edge/edge.protocol.h"
#include "service/middleware/auth.h"
#include "service/utils/number.h"

namespace service::edge {

namespace config::detail {
inline std::vector<std::uint8_t> packetBytes(std::string_view mode, std::string_view content,
                                             std::string_view name) {
    std::vector<std::uint8_t> output;
    if (mode == "OFF" || content.empty())
        return output;
    if (mode == "ASCII") {
        output.assign(content.begin(), content.end());
        return output;
    }
    if (mode != "HEX")
        throw std::runtime_error("invalid edge config packet mode: " + std::string(name));

    int high = -1;
    for (const char character : content) {
        if (character == ' ' || character == '\t' || character == '\r' || character == '\n')
            continue;
        const int digit = protocol::hexDigit(character);
        if (digit < 0)
            throw std::runtime_error("invalid edge config hex: " + std::string(name));
        if (high < 0)
            high = digit;
        else {
            output.push_back(static_cast<std::uint8_t>((high << 4U) | digit));
            high = -1;
        }
    }
    if (high >= 0)
        throw std::runtime_error("invalid edge config hex: " + std::string(name));
    return output;
}

inline void packet(std::string* output, std::string_view mode, std::string_view content,
                   std::string_view name) {
    const auto value = packetBytes(mode, content, name);
    output->assign(protocol::bytes(value.data(), value.size()));
}

inline double number(std::string_view value, double fallback = 0.0) {
    if (value.empty())
        return fallback;
    const auto result = service::utils::decimal(value);
    return result.value_or(fallback);
}

inline constexpr std::string_view kReplaceQueueScript = R"lua(
local incoming = tonumber(ARGV[1]) or 0
local current = tonumber(redis.call('GET', KEYS[2]) or '0') or 0
if current > incoming then return 0 end
redis.call('DEL', KEYS[1])
for index = 2, #ARGV do redis.call('RPUSH', KEYS[1], ARGV[index]) end
redis.call('EXPIRE', KEYS[1], 604800)
redis.call('SETEX', KEYS[2], 604800, ARGV[1])
return #ARGV - 1
)lua";

inline constexpr std::string_view kQueueSnapshotSql = R"sql(
WITH next AS (
    SELECT id,
           GREATEST(
               (EXTRACT(EPOCH FROM clock_timestamp()) * 1000)::bigint,
               COALESCE(CASE WHEN status->'config'->>'desiredVersion' ~ '^-?[0-9]{1,18}$'
                             THEN (status->'config'->>'desiredVersion')::bigint END, 0) + 1,
               COALESCE(CASE WHEN status->'config'->>'activeVersion' ~ '^-?[0-9]{1,18}$'
                             THEN (status->'config'->>'activeVersion')::bigint END, 0) + 1) AS revision
    FROM edge_node
    WHERE id = $1::uuid AND enrollment_status = 'approved'
      AND CASE lower(COALESCE(capability->>'deviceConfig', ''))
              WHEN 'true' THEN true WHEN 't' THEN true WHEN '1' THEN true
              ELSE false END
)
UPDATE edge_node node
SET status = jsonb_set(
        jsonb_set(
            jsonb_set(node.status, '{config,desiredVersion}', to_jsonb(next.revision), true),
            '{config,state}', to_jsonb('pending'::text), true),
        '{config,message}', to_jsonb(''::text), true),
    updated_at = NOW()
FROM next
WHERE node.id = next.id
RETURNING next.revision)sql";

inline constexpr std::string_view kRequeueDesiredSql = R"sql(
SELECT COALESCE(CASE WHEN status->'config'->>'desiredVersion' ~ '^-?[0-9]{1,18}$'
                     THEN (status->'config'->>'desiredVersion')::bigint END, 0),
       COALESCE(status->'config'->>'state', 'idle')
FROM edge_node
WHERE id = $1::uuid AND enrollment_status = 'approved'
  AND CASE lower(COALESCE(capability->>'deviceConfig', ''))
          WHEN 'true' THEN true WHEN 't' THEN true WHEN '1' THEN true
          ELSE false END)sql";

inline constexpr std::string_view kRequeuePendingSql = R"sql(
UPDATE edge_node
SET status = jsonb_set(
        jsonb_set(status, '{config,state}', to_jsonb('pending'::text), true),
        '{config,message}', to_jsonb(''::text), true),
    updated_at = NOW()
WHERE id = $1::uuid
  AND COALESCE(CASE WHEN status->'config'->>'desiredVersion' ~ '^-?[0-9]{1,18}$'
                    THEN (status->'config'->>'desiredVersion')::bigint END, 0) = $2
  AND COALESCE(status->'config'->>'state', 'idle') <> 'rejected')sql";

inline constexpr std::string_view kRejectBuildSql = R"sql(
UPDATE edge_node
SET status = jsonb_set(
        jsonb_set(status, '{config,state}', to_jsonb('rejected'::text), true),
        '{config,message}', to_jsonb($1::text), true),
    updated_at = NOW()
WHERE id = $2::uuid
  AND COALESCE(CASE WHEN status->'config'->>'desiredVersion' ~ '^-?[0-9]{1,18}$'
                    THEN (status->'config'->>'desiredVersion')::bigint END, 0) = $3)sql";

inline constexpr std::string_view kBuildItemsSql = R"sql(
SELECT d.id::text, d.name, d.protocol_params->>'device_code', p.protocol,
       COALESCE(NULLIF(d.protocol_params->>'timezone', ''), '+08:00'),
       COALESCE(NULLIF(p.config->>'readInterval', ''), '1'),
       COALESCE(NULLIF(d.protocol_params->>'online_timeout', ''), '300'),
       COALESCE(NULLIF(d.protocol_params->>'slave_id', ''), '1'),
       COALESCE(d.protocol_params->>'modbus_mode', 'TCP'),
       l.endpoint->>'transport', l.endpoint->>'interface',
       COALESCE(l.endpoint->>'mode', ''), COALESCE(l.endpoint->>'ip', ''),
       COALESCE(NULLIF(l.endpoint->>'port', ''), '0'),
       COALESCE(NULLIF(l.endpoint->>'baud_rate', ''), '9600'),
       COALESCE(NULLIF(l.endpoint->>'data_bits', ''), '8'),
       COALESCE(NULLIF(l.endpoint->>'stop_bits', ''), '1'),
       COALESCE(l.endpoint->>'parity', 'none'),
       CASE lower(COALESCE(l.endpoint->>'rs485', ''))
            WHEN 'true' THEN true WHEN 't' THEN true WHEN '1' THEN true
            ELSE false END,
       COALESCE(NULLIF(p.config->'packet'->>'mergeGap', ''), '0'),
       COALESCE(NULLIF(p.config->'packet'->>'maxQuantity', ''), '125'),
       COALESCE(p.config->'connection'->>'mode', 'RACK_SLOT'),
       COALESCE(p.config->'connection'->>'connectionType', 'PG'),
       COALESCE(NULLIF(p.config->'connection'->>'rack', ''), '0'),
       COALESCE(NULLIF(p.config->'connection'->>'slot', ''), '1'),
       COALESCE(p.config->'connection'->>'localTSAP', ''),
       COALESCE(p.config->'connection'->>'remoteTSAP', ''),
       COALESCE(d.protocol_params->'heartbeat'->>'mode', 'OFF'),
       COALESCE(d.protocol_params->'heartbeat'->>'content', ''),
       d.status = 'enabled' AND p.enabled AND l.status = 'enabled',
       d.link_id::text,
       COALESCE(NULLIF(p.config->>'commandFastReadDuration', ''), '60'),
       COALESCE(NULLIF(p.config->>'commandFastReadInterval', ''), '1'),
       l.name, l.status = 'enabled'
FROM device d
JOIN link l ON l.id = d.link_id AND l.execution = 'edge' AND l.deleted_at IS NULL
JOIN device_model p ON p.device_id = d.id AND p.deleted_at IS NULL
WHERE l.edge_node_id = $1::uuid AND d.deleted_at IS NULL
ORDER BY d.id)sql";

inline constexpr std::string_view kAppendModbusSql = R"sql(
SELECT d.id::text, item->>'id', item->>'name', COALESCE(item->>'unit', ''),
       item->>'registerType', item->>'dataType',
       COALESCE(item->>'byteOrder', p.config->>'byteOrder', 'BIG_ENDIAN'),
       COALESCE(NULLIF(item->>'address', ''), '0'),
       COALESCE(NULLIF(item->>'quantity', ''), '1'),
       COALESCE(NULLIF(item->>'scale', ''), '1'),
       COALESCE(NULLIF(item->>'decimals', ''), '-1'),
       CASE lower(COALESCE(item->>'writable', ''))
            WHEN 'true' THEN true WHEN 't' THEN true WHEN '1' THEN true
            ELSE false END
FROM device d
JOIN link l ON l.id = d.link_id AND l.execution = 'edge' AND l.deleted_at IS NULL
JOIN device_model p ON p.device_id = d.id AND p.protocol = 'Modbus'
CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'registers', '[]')) item
WHERE l.edge_node_id = $1::uuid AND d.deleted_at IS NULL
ORDER BY d.id, item->>'id')sql";

inline constexpr std::string_view kAppendS7Sql = R"sql(
SELECT d.id::text, item->>'id', item->>'name', COALESCE(item->>'unit', ''),
       item->>'area', COALESCE(NULLIF(item->>'dbNumber', ''), '0'),
       COALESCE(NULLIF(item->>'start', ''), '0'),
       COALESCE(NULLIF(item->>'startBit', ''), '0'),
       COALESCE(NULLIF(item->>'size', ''), '1'), COALESCE(item->>'dataType', 'BOOL'),
       COALESCE(NULLIF(item->>'decimals', ''), '-1'),
       CASE lower(COALESCE(item->>'writable', ''))
            WHEN 'true' THEN true WHEN 't' THEN true WHEN '1' THEN true
            ELSE false END
FROM device d
JOIN link l ON l.id = d.link_id AND l.execution = 'edge' AND l.deleted_at IS NULL
JOIN device_model p ON p.device_id = d.id AND p.protocol = 'S7'
CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'areas', '[]')) item
WHERE l.edge_node_id = $1::uuid AND d.deleted_at IS NULL
ORDER BY d.id, item->>'id')sql";

inline constexpr std::string_view kAppendSl651FunctionsSql = R"sql(
SELECT d.id::text, func->>'funcCode', func->>'name', func->>'dir'
FROM device d
JOIN link l ON l.id = d.link_id AND l.execution = 'edge' AND l.deleted_at IS NULL
JOIN device_model p ON p.device_id = d.id AND p.protocol = 'SL651'
CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'funcs', '[]')) func
WHERE l.edge_node_id = $1::uuid AND d.deleted_at IS NULL
ORDER BY d.id, func->>'funcCode')sql";

inline constexpr std::string_view kAppendSl651ElementsSql = R"sql(
SELECT d.id::text, func->>'funcCode', element->>'id', element->>'name',
       COALESCE(element->>'unit', ''), element->>'encode',
       COALESCE(NULLIF(element->>'length', ''), '0'),
       COALESCE(NULLIF(element->>'digits', ''), '0'),
       COALESCE(element->>'guideHex', ''), response_element,
       func->>'dir' = 'DOWN'
FROM device d
JOIN link l ON l.id = d.link_id AND l.execution = 'edge' AND l.deleted_at IS NULL
JOIN device_model p ON p.device_id = d.id AND p.protocol = 'SL651'
CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'funcs', '[]')) func
CROSS JOIN LATERAL (
  SELECT value AS element, false AS response_element
  FROM jsonb_array_elements(COALESCE(func->'elements', '[]'))
  UNION ALL
  SELECT value AS element, true AS response_element
  FROM jsonb_array_elements(COALESCE(func->'responseElements', '[]'))
) values
WHERE l.edge_node_id = $1::uuid AND d.deleted_at IS NULL
ORDER BY d.id, func->>'funcCode', response_element, element->>'id')sql";
} // namespace config::detail

class ConfigService final {
  public:
    static ConfigService& instance() {
        static ConfigService value;
        return value;
    }

    ruvia::Task<std::uint64_t> queueSnapshot(ruvia::WebWorkerContext& c,
                                             std::string_view nodeId,
                                             std::string_view actorId) {
        const auto version = co_await c.db().query(config::detail::kQueueSnapshotSql,
                                                   service::common::dbParams(nodeId));
        if (version.empty())
            service::common::fail(17011, "边缘节点未批准或不支持设备配置", 409);
        const auto revision = unsignedInteger(version.front()[0].value().value_or(std::string_view{}));

        auto snapshot = co_await buildSnapshot(c, nodeId, revision);
        if (!snapshot) {
            service::common::fail(17012, "边缘节点配置条目超过 512 条", 409);
        }

        if (!service::common::isUuid(actorId))
            service::common::fail(10002, "invalid edge snapshot actor", 400);
        (void)co_await c.db().execute(R"sql(
INSERT INTO edge_config_revision(node_id, revision, sha256, item_count, created_by)
VALUES ($1::uuid, $2, $3, $4, $5::uuid))sql",
                                      service::common::dbParams(
                                          nodeId, static_cast<std::int64_t>(revision),
                                          snapshot->digest,
                                          static_cast<std::int64_t>(snapshot->itemCount),
                                          actorId));
        co_await replaceQueue(c, nodeId, revision, snapshot->wires);
        co_await metadata::publishNode(c, nodeId);
        co_return revision;
    }

    ruvia::Task<bool> requeueIfStale(ruvia::Context& c, std::string_view nodeId,
                                     std::uint64_t activeRevision) {
        const auto desired = co_await c.db().query(config::detail::kRequeueDesiredSql,
                                                   service::common::dbParams(nodeId));
        if (desired.empty())
            co_return false;
        const auto revision = unsignedInteger(desired.front()[0].value().value_or(std::string_view{}));
        if (revision == 0 || revision == activeRevision ||
            desired.front()[1].value().value_or(std::string_view{}) == "rejected")
            co_return false;

        auto snapshot = co_await buildSnapshot(c, nodeId, revision);
        if (!snapshot)
            co_return false;
        (void)co_await c.db().execute(R"sql(
INSERT INTO edge_config_revision(node_id, revision, sha256, item_count, created_by)
VALUES ($1::uuid, $2, $3, $4, NULL)
ON CONFLICT (node_id, revision) DO UPDATE
SET sha256 = EXCLUDED.sha256, item_count = EXCLUDED.item_count,
    status = 'pending', message = '', completed_at = NULL)sql",
                                      service::common::dbParams(
                                          nodeId, static_cast<std::int64_t>(revision),
                                          snapshot->digest,
                                          static_cast<std::int64_t>(snapshot->itemCount)));
        (void)co_await c.db().execute(config::detail::kRequeuePendingSql,
                                      service::common::dbParams(
                                          nodeId, static_cast<std::int64_t>(revision)));
        co_await replaceQueue(c, nodeId, revision, snapshot->wires);
        co_await metadata::publishNode(c, nodeId);
        co_return true;
    }

  private:
    struct Snapshot {
        std::string digest;
        std::vector<std::string> wires;
        std::size_t itemCount{};
    };

    static std::uint64_t unsignedInteger(std::string_view value) {
        std::uint64_t result{};
        const auto [end, error] =
            std::from_chars(value.data(), value.data() + value.size(), result);
        if (error != std::errc{} || end != value.data() + value.size())
            throw std::runtime_error("invalid edge config integer");
        return result;
    }

    static std::int64_t integer(std::string_view value, std::int64_t fallback = 0) {
        if (value.empty())
            return fallback;
        std::int64_t result{};
        const auto [end, error] =
            std::from_chars(value.data(), value.data() + value.size(), result);
        return error == std::errc{} && end == value.data() + value.size() ? result : fallback;
    }

    static std::uint32_t positiveCeil(std::string_view value, double fallback = 1.0) {
        double parsed = config::detail::number(value, fallback);
        if (parsed < 1.0)
            parsed = 1.0;
        const auto maximum = static_cast<double>(std::numeric_limits<std::uint32_t>::max());
        if (parsed > maximum)
            return std::numeric_limits<std::uint32_t>::max();
        return static_cast<std::uint32_t>(std::ceil(parsed));
    }

    static pb::Protocol protocolValue(std::string_view value) {
        if (value == "Modbus")
            return pb::PROTOCOL_MODBUS;
        if (value == "S7")
            return pb::PROTOCOL_S7;
        if (value == "SL651")
            return pb::PROTOCOL_SL651;
        return pb::PROTOCOL_UNSPECIFIED;
    }

    static bool setUuid(std::string* field, std::string_view text) {
        std::uint8_t value[16]{};
        if (!protocol::uuidBytes(text, value))
            return false;
        field->assign(protocol::bytes(value, sizeof(value)));
        return true;
    }

    static void packet(std::string* output, std::string_view mode, std::string_view content,
                       std::string_view name) {
        config::detail::packet(output, mode, content, name);
    }

    static bool encodeItem(const pb::ConfigItem& item, std::string& output) {
        const auto size = item.ByteSizeLong();
        if (size > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            return false;
        output.assign(size, '\0');
        google::protobuf::io::ArrayOutputStream raw(output.data(), static_cast<int>(size));
        google::protobuf::io::CodedOutputStream coded(&raw);
        coded.SetSerializationDeterministic(true);
        return item.SerializeToCodedStream(&coded) && !coded.HadError();
    }

    static std::array<std::uint8_t, 32> sha256(std::string_view value) {
        std::array<std::uint8_t, 32> output{};
        unsigned size{};
        if (EVP_Digest(value.data(), value.size(), output.data(), &size, EVP_sha256(), nullptr) !=
                1 ||
            size != output.size())
            throw std::runtime_error("SHA-256 failed");
        return output;
    }

    static std::array<std::uint8_t, 32>
    digestList(const std::vector<std::array<std::uint8_t, 32>>& values) {
        EVP_MD_CTX* context = EVP_MD_CTX_new();
        if (context == nullptr)
            throw std::runtime_error("SHA-256 context allocation failed");
        std::array<std::uint8_t, 32> output{};
        unsigned size{};
        bool ok = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1;
        for (const auto& value : values)
            ok = ok && EVP_DigestUpdate(context, value.data(), value.size()) == 1;
        ok = ok && EVP_DigestFinal_ex(context, output.data(), &size) == 1 &&
             size == output.size();
        EVP_MD_CTX_free(context);
        if (!ok)
            throw std::runtime_error("configuration SHA-256 failed");
        return output;
    }

    static std::string hex(const std::array<std::uint8_t, 32>& value) {
        constexpr char digits[] = "0123456789abcdef";
        std::string result;
        result.reserve(64);
        for (const auto byte : value) {
            result.push_back(digits[byte >> 4U]);
            result.push_back(digits[byte & 0x0fU]);
        }
        return result;
    }

    static void appendWire(std::vector<std::string>& output, const pb::Envelope& envelope) {
        auto wire = protocol::encode(envelope);
        if (wire.empty())
            throw std::runtime_error("cannot encode edge configuration envelope");
        output.push_back(std::move(wire));
    }

    template <typename Context>
    static ruvia::Task<std::optional<Snapshot>> buildSnapshot(Context& c,
                                                               std::string_view nodeId,
                                                               std::uint64_t revision) {
        auto items = co_await buildItems(c, nodeId);
        if (items.size() > 512) {
            co_await rejectBuild(c, nodeId, revision, "配置条目超过 nanopb v1 的 512 条上限");
            co_return std::nullopt;
        }

        std::vector<std::array<std::uint8_t, 32>> itemDigests;
        itemDigests.reserve(items.size());
        for (std::size_t index = 0; index < items.size(); ++index) {
            auto& item = items[index];
            item.set_revision(revision);
            item.set_index(static_cast<std::uint32_t>(index));
            std::string canonical;
            if (!encodeItem(item, canonical))
                throw std::runtime_error("cannot encode edge config item");
            auto digest = sha256(canonical);
            item.set_sha256(protocol::bytes(digest.data(), digest.size()));
            itemDigests.push_back(digest);
        }
        const auto snapshotDigest = digestList(itemDigests);
        Snapshot snapshot;
        snapshot.digest = hex(snapshotDigest);
        snapshot.itemCount = items.size();
        snapshot.wires.reserve(items.size() + 2);

        auto begin = protocol::outbound(nodeId);
        auto* configBegin = begin.mutable_config_begin();
        configBegin->set_revision(revision);
        configBegin->set_item_count(static_cast<std::uint32_t>(items.size()));
        configBegin->set_sha256(
            protocol::bytes(snapshotDigest.data(), snapshotDigest.size()));
        appendWire(snapshot.wires, begin);
        for (const auto& item : items) {
            auto envelope = protocol::outbound(nodeId);
            *envelope.mutable_config_item() = item;
            appendWire(snapshot.wires, envelope);
        }
        auto commit = protocol::outbound(nodeId);
        auto* configCommit = commit.mutable_config_commit();
        configCommit->set_revision(revision);
        configCommit->set_sha256(
            protocol::bytes(snapshotDigest.data(), snapshotDigest.size()));
        appendWire(snapshot.wires, commit);
        co_return snapshot;
    }

    template <typename Context>
    static ruvia::Task<void> replaceQueue(Context& c, std::string_view nodeId,
                                          std::uint64_t revision,
                                          const std::vector<std::string>& wires) {
        const std::string key = "iot:edge:config:" + std::string(nodeId);
        const std::string revisionKey = "iot:edge:config-revision:" + std::string(nodeId);
        const std::array<std::string_view, 2> keys{key, revisionKey};
        const auto revisionText = std::to_string(revision);
        std::vector<std::string_view> values;
        values.reserve(wires.size() + 1);
        values.push_back(revisionText);
        for (const auto& wire : wires)
            values.push_back(wire);
        (void)co_await c.redis().eval(config::detail::kReplaceQueueScript,
                                      std::span<const std::string_view>(keys),
                                      std::span<const std::string_view>(values));
        co_await dispatch::notifyNode(c.redis(), nodeId);
    }

    template <typename Context>
    static ruvia::Task<void> rejectBuild(Context& c, std::string_view nodeId,
                                         std::uint64_t revision, std::string_view message) {
        (void)co_await c.db().execute(config::detail::kRejectBuildSql,
                                      service::common::dbParams(
                                          message, nodeId, static_cast<std::int64_t>(revision)));
    }

    template <typename Context>
    static ruvia::Task<std::vector<pb::ConfigItem>>
    buildItems(Context& c, std::string_view nodeId) {
        std::vector<pb::ConfigItem> items;
        std::set<std::string> endpoints;
        const auto devices = co_await c.db().query(config::detail::kBuildItemsSql,
                                                     service::common::dbParams(nodeId));
        for (const auto& row : devices) {
            const auto protocol = protocolValue(row[3].value().value_or(std::string_view{}));
            pb::ConfigItem endpoint;
            endpoint.set_kind(pb::CONFIG_ITEM_ENDPOINT);
            auto* endpointValue = endpoint.mutable_endpoint();
            if (!setUuid(endpointValue->mutable_endpoint_id(), row[30].value().value_or(std::string_view{})))
                throw std::runtime_error("invalid edge link UUID");
            endpointValue->set_name(row[33].value().value_or(std::string_view{}));
            endpointValue->set_interface_name(row[10].value().value_or(std::string_view{}));
            endpointValue->set_protocol(protocol);
            endpointValue->set_enabled(row[34].value().value_or(std::string_view{}) == "t");
            if (row[9].value().value_or(std::string_view{}) == "serial") {
                endpointValue->set_transport(pb::TRANSPORT_SERIAL);
                endpointValue->set_mode(pb::LINK_MODE_SERIAL);
                auto* serial = endpointValue->mutable_serial();
                serial->set_channel(row[10].value().value_or(std::string_view{}));
                serial->set_baud_rate(
                    static_cast<std::uint32_t>(integer(row[14].value().value_or(std::string_view{}), 9600)));
                serial->set_data_bits(
                    static_cast<std::uint32_t>(integer(row[15].value().value_or(std::string_view{}), 8)));
                serial->set_stop_bits(
                    static_cast<std::uint32_t>(integer(row[16].value().value_or(std::string_view{}), 1)));
                serial->set_parity(row[17].value().value_or(std::string_view{}));
                serial->set_rs485(row[18].value().value_or(std::string_view{}) == "t");
            } else {
                endpointValue->set_transport(pb::TRANSPORT_ETHERNET);
                endpointValue->set_mode(row[11].value().value_or(std::string_view{}) == "TCP Server"
                                            ? pb::LINK_MODE_TCP_SERVER
                                            : pb::LINK_MODE_TCP_CLIENT);
                endpointValue->set_ip(row[12].value().value_or(std::string_view{}));
                endpointValue->set_port(
                    static_cast<std::uint32_t>(integer(row[13].value().value_or(std::string_view{}))));
            }
            if (endpoints.emplace(row[30].value().value_or("")).second)
                items.push_back(std::move(endpoint));

            pb::ConfigItem device;
            device.set_kind(pb::CONFIG_ITEM_DEVICE);
            auto* deviceValue = device.mutable_device();
            setUuid(deviceValue->mutable_device_id(), row[0].value().value_or(std::string_view{}));
            setUuid(deviceValue->mutable_endpoint_id(), row[30].value().value_or(std::string_view{}));
            deviceValue->set_device_code(row[2].value().value_or(std::string_view{}));
            deviceValue->set_name(row[1].value().value_or(std::string_view{}));
            deviceValue->set_protocol(protocol);
            deviceValue->set_timezone(row[4].value().value_or(std::string_view{}));
            // Southbound acquisition is fixed at one second. The protocol's configured
            // read interval controls edge-to-platform reporting; storagePolicy remains
            // a platform-only persistence policy carried by telemetry metadata.
            deviceValue->set_io_interval_ms(1000);
            deviceValue->set_report_interval_sec(positiveCeil(row[5].value().value_or(std::string_view{})));
            deviceValue->set_online_timeout_sec(
                static_cast<std::uint32_t>(integer(row[6].value().value_or(std::string_view{}), 300)));
            deviceValue->set_modbus_slave_id(
                static_cast<std::uint32_t>(integer(row[7].value().value_or(std::string_view{}), 1)));
            deviceValue->set_modbus_mode(row[8].value().value_or(std::string_view{}));
            deviceValue->set_modbus_merge_gap(
                static_cast<std::uint32_t>(integer(row[19].value().value_or(std::string_view{}))));
            deviceValue->set_modbus_max_quantity(
                static_cast<std::uint32_t>(integer(row[20].value().value_or(std::string_view{}), 125)));
            deviceValue->set_s7_connection_mode(row[21].value().value_or(std::string_view{}));
            deviceValue->set_s7_connection_type(row[22].value().value_or(std::string_view{}));
            deviceValue->set_s7_rack(
                static_cast<std::uint32_t>(integer(row[23].value().value_or(std::string_view{}))));
            deviceValue->set_s7_slot(
                static_cast<std::uint32_t>(integer(row[24].value().value_or(std::string_view{}), 1)));
            deviceValue->set_s7_local_tsap(row[25].value().value_or(std::string_view{}));
            deviceValue->set_s7_remote_tsap(row[26].value().value_or(std::string_view{}));
            deviceValue->set_command_fast_read_duration_sec(static_cast<std::uint32_t>(
                std::clamp<std::int64_t>(integer(row[31].value().value_or(std::string_view{}), 60),
                                         0, 3600)));
            deviceValue->set_command_fast_read_interval_sec(static_cast<std::uint32_t>(
                std::clamp<std::int64_t>(integer(row[32].value().value_or(std::string_view{}), 1),
                                         1, 3600)));
            packet(deviceValue->mutable_heartbeat_payload(), row[27].value().value_or(std::string_view{}), row[28].value().value_or(std::string_view{}),
                   "heartbeat_payload");
            deviceValue->set_enabled(row[29].value().value_or(std::string_view{}) == "t");
            items.push_back(std::move(device));
        }

        co_await appendModbus(c, nodeId, items);
        co_await appendS7(c, nodeId, items);
        co_await appendSl651(c, nodeId, items);
        co_return items;
    }

    template <typename Context>
    static ruvia::Task<void> appendModbus(Context& c, std::string_view nodeId,
                                          std::vector<pb::ConfigItem>& items) {
        const auto rows = co_await c.db().query(config::detail::kAppendModbusSql,
                                                 service::common::dbParams(nodeId));
        for (const auto& row : rows) {
            pb::ConfigItem item;
            item.set_kind(pb::CONFIG_ITEM_MODBUS_REGISTER);
            auto* value = item.mutable_modbus_register();
            setUuid(value->mutable_device_id(), row[0].value().value_or(std::string_view{}));
            value->set_element_id(row[1].value().value_or(std::string_view{}));
            value->set_name(row[2].value().value_or(std::string_view{}));
            value->set_unit(row[3].value().value_or(std::string_view{}));
            value->set_register_type(row[4].value().value_or(std::string_view{}));
            value->set_data_type(row[5].value().value_or(std::string_view{}));
            value->set_byte_order(row[6].value().value_or(std::string_view{}));
            value->set_address(static_cast<std::uint32_t>(integer(row[7].value().value_or(std::string_view{}))));
            value->set_quantity(
                static_cast<std::uint32_t>(integer(row[8].value().value_or(std::string_view{}), 1)));
            value->set_scale(config::detail::number(row[9].value().value_or(std::string_view{}), 1.0));
            value->set_decimals(
                static_cast<std::int32_t>(integer(row[10].value().value_or(std::string_view{}), -1)));
            value->set_writable(row[11].value().value_or(std::string_view{}) == "t");
            items.push_back(std::move(item));
        }
    }

    template <typename Context>
    static ruvia::Task<void> appendS7(Context& c, std::string_view nodeId,
                                      std::vector<pb::ConfigItem>& items) {
        const auto rows = co_await c.db().query(config::detail::kAppendS7Sql,
                                                 service::common::dbParams(nodeId));
        for (const auto& row : rows) {
            pb::ConfigItem item;
            item.set_kind(pb::CONFIG_ITEM_S7_AREA);
            auto* value = item.mutable_s7_area();
            setUuid(value->mutable_device_id(), row[0].value().value_or(std::string_view{}));
            value->set_element_id(row[1].value().value_or(std::string_view{}));
            value->set_name(row[2].value().value_or(std::string_view{}));
            value->set_unit(row[3].value().value_or(std::string_view{}));
            value->set_area(row[4].value().value_or(std::string_view{}));
            value->set_db_number(
                static_cast<std::uint32_t>(integer(row[5].value().value_or(std::string_view{}))));
            value->set_start(static_cast<std::uint32_t>(integer(row[6].value().value_or(std::string_view{}))));
            value->set_start_bit(
                static_cast<std::uint32_t>(integer(row[7].value().value_or(std::string_view{}))));
            value->set_size(
                static_cast<std::uint32_t>(integer(row[8].value().value_or(std::string_view{}), 1)));
            value->set_data_type(row[9].value().value_or(std::string_view{}));
            value->set_scale(1.0);
            value->set_decimals(
                static_cast<std::int32_t>(integer(row[10].value().value_or(std::string_view{}), -1)));
            value->set_writable(row[11].value().value_or(std::string_view{}) == "t");
            items.push_back(std::move(item));
        }
    }

    template <typename Context>
    static ruvia::Task<void> appendSl651(Context& c, std::string_view nodeId,
                                         std::vector<pb::ConfigItem>& items) {
        const auto functions = co_await c.db().query(config::detail::kAppendSl651FunctionsSql,
                                                      service::common::dbParams(nodeId));
        for (const auto& row : functions) {
            pb::ConfigItem item;
            item.set_kind(pb::CONFIG_ITEM_SL651_FUNCTION);
            auto* value = item.mutable_sl651_function();
            setUuid(value->mutable_device_id(), row[0].value().value_or(std::string_view{}));
            value->set_function_code(row[1].value().value_or(std::string_view{}));
            value->set_name(row[2].value().value_or(std::string_view{}));
            value->set_direction(row[3].value().value_or(std::string_view{}));
            items.push_back(std::move(item));
        }

        const auto elements = co_await c.db().query(config::detail::kAppendSl651ElementsSql,
                                                     service::common::dbParams(nodeId));
        for (const auto& row : elements) {
            pb::ConfigItem item;
            item.set_kind(pb::CONFIG_ITEM_SL651_ELEMENT);
            auto* value = item.mutable_sl651_element();
            setUuid(value->mutable_device_id(), row[0].value().value_or(std::string_view{}));
            value->set_function_code(row[1].value().value_or(std::string_view{}));
            value->set_element_id(row[2].value().value_or(std::string_view{}));
            value->set_name(row[3].value().value_or(std::string_view{}));
            value->set_unit(row[4].value().value_or(std::string_view{}));
            value->set_encoding(row[5].value().value_or(std::string_view{}));
            value->set_length(
                static_cast<std::uint32_t>(integer(row[6].value().value_or(std::string_view{}))));
            value->set_digits(
                static_cast<std::uint32_t>(integer(row[7].value().value_or(std::string_view{}))));
            const auto guide =
                config::detail::packetBytes("HEX", row[8].value().value_or(std::string_view{}), "sl651_guide");
            value->set_guide(protocol::bytes(guide.data(), guide.size()));
            value->set_response_element(row[9].value().value_or(std::string_view{}) == "t");
            value->set_writable(row[10].value().value_or(std::string_view{}) == "t" && !value->response_element());
            items.push_back(std::move(item));
        }
    }
};

inline ConfigService& configService() { return ConfigService::instance(); }

} // namespace service::edge

#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <future>
#include <iostream>
#include <memory>

#include <ruvia/core/Timer.h>
#include <ruvia/web/WebWorker.h>

#include "service/common/message.h"
#include "service/features/telemetry/telemetry.service.h"

namespace service::edge {

inline constexpr std::string_view kEdgeIngressGroup{"iot-engine:edge-projector"};

inline bool validVpnPublicKey(std::string_view value) noexcept {
    return value.size() == 44 && value.back() == '=' &&
           std::all_of(value.begin(), value.end(), [](unsigned char character) {
               return (character >= 'A' && character <= 'Z') ||
                      (character >= 'a' && character <= 'z') ||
                      (character >= '0' && character <= '9') || character == '+' ||
                      character == '/' || character == '=';
           });
}

class EdgeProjectionService {
protected:
    static ruvia::Task<void> hydrateAuth(ruvia::WebWorkerContext& context) {
        const auto rows = co_await context.db().query(
            "SELECT imei, id::text, enrollment_status FROM edge_node");
        if (rows.empty())
            co_return;
        auto pipeline = context.redis().pipeline();
        for (const auto& row : rows) {
            const auto key = protocol::authKey(row[0].value().value_or(std::string_view{}));
            const auto value = std::string(row[1].value().value_or(std::string_view{})) + "|" + std::string(row[2].value().value_or(std::string_view{}));
            pipeline.set(key, value);
        }
        const auto replies = co_await std::move(pipeline).exec();
        service::message::redis::requirePipelineSuccess("hydrate edge authorization", replies);
    }

    ruvia::Task<void> project(
        ruvia::WebWorkerContext& context, metadata::Catalog& catalog,
        std::string_view wire,
        std::string_view receivedAtText,
        std::vector<service::message::StreamMessage>& telemetry) {
        pb::Envelope envelope;
        if (!protocol::decode(wire, envelope))
            co_return;
        const auto receivedAt = service::common::parseInt64(
            receivedAtText.empty() ? std::nullopt
                                   : std::optional<std::string_view>(receivedAtText));
        const auto receivedAtMs = receivedAt.value_or(protocol::nowMs());
        if (envelope.payload_case() == pb::Envelope::kHello) {
            co_await saveHello(context, envelope.hello());
            co_return;
        }
        if (envelope.node_id().size() != 16)
            co_return;
        const auto nodeId = protocol::uuidText(envelope.node_id());
        if (envelope.has_telemetry_batch() || envelope.has_command_result())
            catalog[nodeId] = co_await metadata::loadNode(context.redis(),nodeId);
        switch (envelope.payload_case()) {
        case pb::Envelope::kHeartbeat:
            co_await saveHeartbeat(context, nodeId, envelope.heartbeat());
            break;
        case pb::Envelope::kCapabilityReport:
            co_await saveCapabilities(context, nodeId, envelope.capability_report());
            break;
        case pb::Envelope::kNetworkConfigResult:
            co_await saveNetworkResult(context, nodeId, envelope.network_config_result());
            break;
        case pb::Envelope::kVpnConfigResult:
            co_await saveVpnResult(context, nodeId, envelope.vpn_config_result());
            break;
        case pb::Envelope::kFirmwareUpdateResult:
            co_await saveFirmwareResult(context, nodeId, envelope.firmware_update_result());
            break;
        case pb::Envelope::kModemControlResult:
            co_await saveModemResult(context, nodeId, envelope.modem_control_result());
            break;
        case pb::Envelope::kPlatformConfigResult:
            co_await savePlatformResult(context, nodeId, envelope.platform_config_result());
            break;
        case pb::Envelope::kConfigApplied:
            co_await saveConfigApplied(context, nodeId, envelope.config_applied());
            break;
        case pb::Envelope::kConfigRejected:
            co_await saveConfigRejected(context, nodeId, envelope.config_rejected());
            break;
        case pb::Envelope::kTelemetryBatch:
            for (const auto& record : envelope.telemetry_batch().records()) {
                if (record.has_device_status() &&
                    record.device_status().device_id() == record.device_id()) {
                    pb::DeviceStatusReport report;
                    *report.add_devices() = record.device_status();
                    co_await saveDeviceStatus(context, nodeId, report);
                }
            }
            co_await ensureMetadata(context, catalog, nodeId,
                                    envelope.telemetry_batch());
            collectTelemetry(catalog, nodeId, receivedAtMs,
                             envelope.telemetry_batch(), telemetry);
            break;
        case pb::Envelope::kCommandResult:
            if (envelope.command_result().device_id().size() == 16)
                co_await ensureMetadata(
                    context, catalog, nodeId,
                    protocol::uuidText(envelope.command_result().device_id()));
            co_await saveCommandResult(context, catalog, nodeId, receivedAtMs,
                                       envelope.command_result());
            break;
        case pb::Envelope::kDeviceStatusReport:
            co_await saveDeviceStatus(context, nodeId, envelope.device_status_report());
            break;
        default:
            break;
        }
    }

    ruvia::Task<void> ensureMetadata(ruvia::WebWorkerContext& context,
                                      metadata::Catalog& catalog,
                                      std::string_view nodeId,
                                      std::string_view deviceId) {
        const auto existingNode = catalog.find(std::string(nodeId));
        if (existingNode != catalog.end() &&
            existingNode->second.contains(std::string(deviceId)))
            co_return;
        auto snapshot = co_await metadata::loadNode(context.redis(), nodeId);
        if (!snapshot.contains(std::string(deviceId))) {
            snapshot = co_await metadata::loadNodeFromDatabase(context, nodeId);
            co_await metadata::storeNode(context.redis(), nodeId, snapshot, false);
        }
        catalog[std::string(nodeId)] = std::move(snapshot);
    }

    ruvia::Task<void> ensureMetadata(ruvia::WebWorkerContext& context,
                                      metadata::Catalog& catalog,
                                      std::string_view nodeId,
                                      const pb::TelemetryBatch& batch) {
        const auto containsAll = [&batch](const metadata::NodeSnapshot& snapshot) {
            for (const auto& record : batch.records()) {
                if (record.device_id().size() == 16 &&
                    !snapshot.contains(protocol::uuidText(record.device_id())))
                    return false;
            }
            return true;
        };
        const auto existingNode = catalog.find(std::string(nodeId));
        if (existingNode != catalog.end() && containsAll(existingNode->second))
            co_return;
        auto snapshot = co_await metadata::loadNode(context.redis(), nodeId);
        if (!containsAll(snapshot)) {
            snapshot = co_await metadata::loadNodeFromDatabase(context, nodeId);
            co_await metadata::storeNode(context.redis(), nodeId, snapshot, false);
        }
        catalog[std::string(nodeId)] = std::move(snapshot);
    }

    static std::string_view simState(pb::ModemSimState state) {
        switch (state) {
        case pb::MODEM_SIM_READY:
            return "ready";
        case pb::MODEM_SIM_NOT_INSERTED:
            return "not_inserted";
        case pb::MODEM_SIM_PIN_REQUIRED:
            return "pin_required";
        case pb::MODEM_SIM_PUK_REQUIRED:
            return "puk_required";
        case pb::MODEM_SIM_BLOCKED:
            return "blocked";
        default:
            return "unknown";
        }
    }

    static ruvia::Task<void> saveHello(ruvia::WebWorkerContext& context,
                                       const pb::Hello& hello) {
        if (!protocol::validImei(hello.imei()))
            co_return;
        const auto candidate = service::common::nextUuidV7();
        const auto rows = co_await context.db().query(R"sql(
INSERT INTO edge_node(id, platform_id, imei, model, software_version, hostname, architecture,
                      openwrt_release, capability, mobile, status, last_seen_at, updated_at)
VALUES ($1::uuid, $2::uuid, $3, $4, $5, $6, $7, $8,
        jsonb_build_object(
            'networkConfig', $9::boolean,
            'firmwareUpdate', $10::boolean,
            'firmwareStream', $29::boolean,
            'platformConfig', $11::boolean,
            'deviceConfig', $12::boolean,
            'networkConfigVersion', $13::bigint,
            'modemControl', $14::boolean,
            'logs', $15::boolean,
            'terminal', false,
            'vpn', jsonb_build_object('supportsVpn', false, 'wireguardVersion', '',
                                      'agentVersion', '', 'publicKey', '')),
        jsonb_build_object(
            'available', $16::boolean,
            'simState', $17::text,
            'iccid', $18::text,
            'signal', jsonb_build_object(
                'csq', $19::bigint,
                'rssiDbm', $20::bigint,
                'percent', $21::bigint),
            'registered', $22::boolean,
            'registrationStatus', $23::bigint,
            'apn', $24::text,
            'operator', $25::text,
            'connected', $26::boolean,
            'ipv4', $27::text),
        jsonb_build_object(
            'config', jsonb_build_object(
                'activeVersion', 0,
                'desiredVersion', 0,
                'state', 'idle',
                'message', ''),
            'outbox', jsonb_build_object('records', 0, 'bytes', 0),
            'log', jsonb_build_object('level', COALESCE(NULLIF($28::text, ''), 'info'))),
        NOW(), NOW())
ON CONFLICT (platform_id, imei) DO UPDATE
SET model = EXCLUDED.model, software_version = EXCLUDED.software_version,
    hostname = EXCLUDED.hostname, architecture = EXCLUDED.architecture,
	    openwrt_release = EXCLUDED.openwrt_release,
	    capability = EXCLUDED.capability || jsonb_build_object(
	        'terminal', CASE lower(COALESCE(edge_node.capability->>'terminal', ''))
	                        WHEN 'true' THEN true WHEN 't' THEN true WHEN '1' THEN true
	                        ELSE false END,
	        'vpn', COALESCE(edge_node.capability->'vpn', EXCLUDED.capability->'vpn')),
    mobile = jsonb_set(
        jsonb_set(
            EXCLUDED.mobile, '{apn}',
            to_jsonb(COALESCE(NULLIF(EXCLUDED.mobile->>'apn', ''),
                              edge_node.mobile->>'apn', '')::text), true),
        '{operator}',
        to_jsonb(COALESCE(NULLIF(EXCLUDED.mobile->>'operator', ''),
                          edge_node.mobile->>'operator', '')::text), true),
    status = jsonb_set(
        jsonb_set(edge_node.status, '{log}',
                  COALESCE(edge_node.status->'log', '{}'::jsonb), true),
        '{log,level}', to_jsonb(COALESCE(NULLIF($28::text, ''), 'info')::text), true),
    last_seen_at = NOW(),
    updated_at = NOW()
RETURNING id::text, enrollment_status)sql",
                                                     service::common::dbParams(
                                                         candidate, protocol::platformId(),
                                                         hello.imei(), hello.model(),
                                                         hello.software_version(), hello.hostname(),
                                                         hello.architecture(),
                                                         hello.openwrt_release(),
                                                         hello.supports_network_config(),
                                                         hello.supports_firmware_update(),
                                                         hello.supports_platform_config(),
                                                         hello.supports_device_config(),
                                                         hello.network_config_version(),
                                                         hello.supports_modem_control(),
                                                         hello.supports_logs(),
                                                         hello.modem_available(),
                                                         simState(hello.sim_state()),
                                                         hello.iccid(), hello.signal_csq(),
                                                         hello.signal_rssi_dbm(),
                                                         hello.signal_percent(),
                                                         hello.mobile_registered(),
                                                         hello.mobile_registration_status(),
                                                         hello.apn(), hello.mobile_operator(),
                                                         hello.mobile_connected(),
                                                         hello.mobile_ipv4(),
                                                         hello.log_level(),
                                                         hello.supports_firmware_stream()));
        const auto key = protocol::authKey(hello.imei());
        const auto nodeId = std::string(rows.front()[0].value().value_or(std::string_view{}));
        const auto enrollmentStatus = std::string(rows.front()[1].value().value_or(std::string_view{}));
        const auto value = nodeId + "|" + enrollmentStatus;
        co_await context.redis().set(key, value);
        if (enrollmentStatus == "approved") {
            (void)co_await context.db().execute(R"sql(
WITH target AS (
    SELECT task.id AS task_id, firmware.id AS firmware_id
    FROM edge_task task
    JOIN edge_firmware firmware
      ON firmware.id::text = task.request->>'firmware_id'
    WHERE task.node_id = $2::uuid
      AND task.task_type = 'firmware'
      AND task.status = 'running'
      AND task.result->>'state' = 'flashing'
    ORDER BY task.created_at DESC
    LIMIT 1
), completed AS (
    UPDATE edge_task task
    SET status = 'succeeded',
        result = task.result || jsonb_build_object(
            'state', 'rebooted',
            'message', 'firmware reboot confirmed',
            'softwareVersion', $1::text),
        updated_at = NOW(),
        completed_at = NOW()
    FROM target
    WHERE task.id = target.task_id
    RETURNING target.firmware_id
)
UPDATE edge_firmware firmware
SET version = $1::text
FROM completed
WHERE firmware.id = completed.firmware_id)sql",
                                                service::common::dbParams(
                                                    hello.software_version(), nodeId));
        }
    }

    static ruvia::Task<void> saveHeartbeat(ruvia::WebWorkerContext& context,
                                           std::string_view nodeId,
                                           const pb::Heartbeat& heartbeat) {
        (void)co_await context.db().execute(R"sql(
UPDATE edge_node
SET status = jsonb_build_object(
        'config', jsonb_build_object(
	            'activeVersion', GREATEST(
	                COALESCE(CASE WHEN status->'config'->>'activeVersion' ~ '^-?[0-9]{1,18}$'
	                              THEN (status->'config'->>'activeVersion')::bigint END, 0),
	                $1::bigint),
	            'desiredVersion',
	                COALESCE(CASE WHEN status->'config'->>'desiredVersion' ~ '^-?[0-9]{1,18}$'
	                              THEN (status->'config'->>'desiredVersion')::bigint END, 0),
	            'state', CASE
	                WHEN COALESCE(CASE WHEN status->'config'->>'desiredVersion' ~ '^-?[0-9]{1,18}$'
	                                   THEN (status->'config'->>'desiredVersion')::bigint END, 0) = $1
	                     AND $1 > 0 THEN 'applied'
	                ELSE COALESCE(status->'config'->>'state', 'idle') END,
	            'message', CASE
	                WHEN COALESCE(CASE WHEN status->'config'->>'desiredVersion' ~ '^-?[0-9]{1,18}$'
	                                   THEN (status->'config'->>'desiredVersion')::bigint END, 0) = $1
	                     AND $1 > 0 THEN ''
	                ELSE COALESCE(status->'config'->>'message', '') END),
        'outbox', jsonb_build_object('records', $2::bigint, 'bytes', $3::bigint),
        'log', jsonb_build_object('level', COALESCE(NULLIF($16::text, ''), 'info'))),
    mobile = jsonb_build_object(
        'available', $4::boolean,
        'simState', $5::text,
        'iccid', $6::text,
        'signal', jsonb_build_object('csq', $7::bigint, 'rssiDbm', $8::bigint,
                                     'percent', $9::bigint),
        'registered', $10::boolean,
        'registrationStatus', $11::bigint,
        'apn', COALESCE(NULLIF($12::text, ''), mobile->>'apn', ''),
        'operator', COALESCE(NULLIF($13::text, ''), mobile->>'operator', ''),
        'connected', $14::boolean,
        'ipv4', $15::text),
    capability = jsonb_set(capability, '{modemControl}', to_jsonb($17::boolean), true),
    last_seen_at = NOW(),
    updated_at = NOW()
WHERE id = $18::uuid)sql",
                                             service::common::dbParams(
                                                 heartbeat.active_config_version(),
                                                 heartbeat.outbox_records(),
                                                 heartbeat.outbox_bytes(),
                                                 heartbeat.modem_available(),
                                                 simState(heartbeat.sim_state()),
                                                 heartbeat.iccid(), heartbeat.signal_csq(),
                                                 heartbeat.signal_rssi_dbm(),
                                                 heartbeat.signal_percent(),
                                                 heartbeat.mobile_registered(),
                                                 heartbeat.mobile_registration_status(),
                                                 heartbeat.apn(), heartbeat.mobile_operator(),
                                                 heartbeat.mobile_connected(),
                                                 heartbeat.mobile_ipv4(),
                                                 heartbeat.log_level(),
                                                 heartbeat.supports_modem_control(), nodeId));
        if (heartbeat.active_config_version() != 0) {
            (void)co_await context.db().execute(R"sql(
UPDATE edge_config_revision revision
SET status = 'applied', message = '', completed_at = COALESCE(completed_at, NOW())
FROM edge_node node
WHERE revision.node_id = node.id AND node.id = $1::uuid
	  AND revision.revision = $2
	  AND COALESCE(CASE WHEN node.status->'config'->>'desiredVersion' ~ '^-?[0-9]{1,18}$'
	                    THEN (node.status->'config'->>'desiredVersion')::bigint END, 0) = $2)sql",
                                                service::common::dbParams(
                                                    nodeId,
                                                    heartbeat.active_config_version()));
        }
    }

    static std::string mac(const pb::InterfaceCapability& item) {
        if (item.mac().size() != 6)
            return {};
        constexpr char digits[] = "0123456789abcdef";
        std::string output;
        output.reserve(17);
        for (std::size_t index = 0; index < 6; ++index) {
            if (index != 0)
                output.push_back(':');
            const auto byte = static_cast<std::uint8_t>(item.mac()[index]);
            output.push_back(digits[byte >> 4U]);
            output.push_back(digits[byte & 0x0fU]);
        }
        return output;
    }

    static std::string
    jsonArray(const google::protobuf::RepeatedPtrField<std::string>& bridgePorts) {
        std::string output{"["};
        bool first = true;
        for (const auto& port : bridgePorts) {
            if (!first)
                output.push_back(',');
            output.push_back('"');
            output += jsonEscape(port);
            output.push_back('"');
            first = false;
        }
        output.push_back(']');
        return output;
    }

    static std::string addressMode(pb::NetworkAddressMode mode) {
        switch (mode) {
        case pb::NETWORK_ADDRESS_DHCP:
            return "dhcp";
        case pb::NETWORK_ADDRESS_STATIC:
            return "static";
        default:
            return "none";
        }
    }

    static ruvia::Task<void> saveCapabilities(
        ruvia::WebWorkerContext& context, std::string_view nodeId,
        const pb::CapabilityReport& report) {
        (void)co_await context.db().execute(
            "DELETE FROM edge_node_interface WHERE node_id = $1::uuid",
            service::common::dbParams(nodeId));
        for (const auto& item : report.interfaces()) {
            const auto macAddress = mac(item);
            const auto ports = jsonArray(item.bridge_ports());
            (void)co_await context.db().execute(R"sql(
INSERT INTO edge_node_interface(node_id, name, display_name, mac, is_up, is_bridge, ipv4,
                                prefix_length, gateway, bridge_ports)
VALUES ($1::uuid, $2, $3, NULLIF($4, ''), $5, $6, NULLIF($7, ''), $8,
        NULLIF($9, ''), $10::jsonb))sql",
                                                service::common::dbParams(
                                                    nodeId, item.name(), item.display_name(),
                                                    macAddress, item.up(), item.bridge(),
                                                    item.ipv4(), item.prefix_length(),
                                                    item.gateway(), ports));
        }
        (void)co_await context.db().execute(
            "DELETE FROM edge_node_network WHERE node_id = $1::uuid",
            service::common::dbParams(nodeId));
        for (const auto& item : report.networks()) {
            const auto ports = jsonArray(item.bridge_ports());
            const auto mode = addressMode(item.mode());
            (void)co_await context.db().execute(R"sql(
INSERT INTO edge_node_network(node_id, name, address_mode, device, is_up, is_bridge, ipv4,
                              prefix_length, gateway, bridge_ports)
VALUES ($1::uuid, $2, $3, $4, $5, $6, NULLIF($7, ''), $8, NULLIF($9, ''),
        $10::jsonb))sql",
                                                service::common::dbParams(
                                                    nodeId, item.name(), mode, item.device(),
                                                    item.up(), item.bridge(), item.ipv4(),
                                                    item.prefix_length(), item.gateway(), ports));
        }
        (void)co_await context.db().execute(
            "DELETE FROM edge_node_serial WHERE node_id = $1::uuid",
            service::common::dbParams(nodeId));
        for (const auto& item : report.serial_ports()) {
            (void)co_await context.db().execute(R"sql(
INSERT INTO edge_node_serial(node_id, path, display_name, available, rs485)
VALUES ($1::uuid, $2, $3, $4, $5))sql",
                                                service::common::dbParams(
                                                    nodeId, item.path(), item.display_name(),
                                                    item.available(), item.rs485()));
        }
        (void)co_await context.db().execute(
            "UPDATE edge_node SET capability = jsonb_set(capability, '{terminal}', "
            "to_jsonb($1::boolean), true), updated_at = NOW() WHERE id = $2::uuid",
            service::common::dbParams(report.ttyd_available(), nodeId));
        if (report.has_vpn()) {
            const auto vpnPublicKey = validVpnPublicKey(report.vpn().public_key())
                                          ? std::string(report.vpn().public_key())
                                          : std::string{};
            (void)co_await context.db().execute(
                "UPDATE edge_node SET capability = jsonb_set(capability, '{vpn}', "
                "jsonb_build_object('supportsVpn', $1::boolean, 'wireguardVersion', $2::text, "
                "'agentVersion', $3::text, 'publicKey', $4::text), true), "
                "updated_at = NOW() WHERE id = $5::uuid",
                service::common::dbParams(report.vpn().supports_vpn(),
                                          report.vpn().wireguard_version(),
                                          report.vpn().agent_version(), vpnPublicKey, nodeId));
        }
        if (report.has_vpn() && report.vpn().supports_vpn()) {
            const auto& publicKey = report.vpn().public_key();
            if (validVpnPublicKey(publicKey)) {
                const auto activated = co_await context.db().query(R"sql(
UPDATE vpn_peer p
SET public_key = $1, status = 'active', updated_at = NOW()
FROM vpn_network n
WHERE p.peer_type = 'edge' AND p.edge_node_id = $2::uuid AND p.status <> 'revoked'
  AND n.id = p.network_id
RETURNING p.id::text, p.network_id::text, n.created_by::text)sql",
                                                                   service::common::dbParams(
                                                                       publicKey, nodeId));
                for (const auto& row : activated) {
                    auto transaction = co_await context.db().beginTransaction();
                    (void)co_await transaction.query(
                        "SELECT pg_advisory_xact_lock(5282804697543808068::bigint)");
                    co_await service::vpn::feature::syncEdgeBridgeRoutes(
                        transaction, row[0].value().value_or(std::string_view{}),
                        row[1].value().value_or(std::string_view{}), nodeId,
                        row[2].value().value_or(std::string_view{}));
                    co_await transaction.commit();
                    co_await service::vpn::queueEdgeConfig(
                        context, row[0].value().value_or(std::string_view{}));
                }
            }
        }
    }

    static ruvia::Task<void> saveNetworkResult(
        ruvia::WebWorkerContext& context, std::string_view nodeId,
        const pb::NetworkConfigResult& result) {
        if (result.request_id().size() != 16)
            co_return;
        const auto id = protocol::uuidText(result.request_id());
        const std::string status = result.success() ? "succeeded" : "failed";
        const std::string json = "{\"message\":\"" + jsonEscape(result.message()) +
                                 "\",\"rolled_back\":" +
                                 (result.rolled_back() ? "true" : "false") + "}";
        (void)co_await context.db().execute(R"sql(
UPDATE edge_task SET status = $1, result = $2::jsonb, updated_at = NOW(), completed_at = NOW()
WHERE id = $3::uuid AND node_id = $4::uuid AND task_type = 'network'
  AND status NOT IN ('succeeded', 'failed'))sql",
                                            service::common::dbParams(status, json, id, nodeId));
    }

    static ruvia::Task<void> saveVpnResult(
        ruvia::WebWorkerContext& context, std::string_view nodeId,
        const pb::VpnConfigResult& result) {
        if (result.request_id().size() != 16)
            co_return;
        const auto id = protocol::uuidText(result.request_id());
        const bool applied = result.applied();
        const std::string status = applied ? "succeeded" : "failed";
        const std::string json = "{\"configVersion\":" +
                                 std::to_string(result.config_version()) +
                                 ",\"errorCode\":" + jsonQuoted(result.error_code()) +
                                 ",\"errorMessage\":" + jsonQuoted(result.error_message()) + "}";
        (void)co_await context.db().execute(R"sql(
WITH transitioned AS (
    UPDATE edge_task
    SET status = $1, result = $2::jsonb, updated_at = NOW(), completed_at = NOW()
    WHERE id = $3::uuid AND node_id = $4::uuid AND task_type = 'vpn'
      AND status NOT IN ('succeeded', 'failed')
    RETURNING request->>'peerId' AS peer_id,
              request->>'enabled' AS enabled,
              request->>'configVersion' AS config_version
)
UPDATE vpn_route route
SET status = CASE WHEN $5::boolean
                 THEN CASE WHEN COALESCE(task.enabled::boolean, true)
                                AND route.enabled THEN 'active' ELSE 'disabled' END
                 ELSE 'error' END,
    last_error = CASE WHEN $5::boolean THEN '' ELSE $6::text END,
    updated_at = NOW()
FROM transitioned task
WHERE route.edge_peer_id = task.peer_id::uuid
  AND (SELECT config_revision::text FROM vpn_peer peer
       WHERE peer.id = task.peer_id::uuid) = task.config_version)sql",
                                            service::common::dbParams(status, json, id, nodeId,
                                                                      applied, result.error_message()));
    }

    static ruvia::Task<void> saveFirmwareResult(
        ruvia::WebWorkerContext& context, std::string_view nodeId,
        const pb::FirmwareUpdateResult& result) {
        if (result.request_id().size() != 16)
            co_return;
        const auto id = protocol::uuidText(result.request_id());
        std::string status = "running";
        bool completed = false;
        if (result.state() == pb::FIRMWARE_UPDATE_ACCEPTED)
            status = "accepted";
        else if (result.state() == pb::FIRMWARE_UPDATE_FAILED) {
            status = "failed";
            completed = true;
        }
        std::string state = "running";
        if (result.state() == pb::FIRMWARE_UPDATE_ACCEPTED)
            state = "accepted";
        else if (result.state() == pb::FIRMWARE_UPDATE_DOWNLOADING)
            state = "downloading";
        else if (result.state() == pb::FIRMWARE_UPDATE_VERIFYING)
            state = "verifying";
        else if (result.state() == pb::FIRMWARE_UPDATE_FLASHING)
            state = "flashing";
        else if (result.state() == pb::FIRMWARE_UPDATE_FAILED)
            state = "failed";
        const std::string json =
            "{\"state\":\"" + state + "\",\"message\":\"" +
            jsonEscape(result.message()) +
            "\",\"progressPercent\":" + std::to_string(result.progress_percent()) +
            ",\"downloadedBytes\":" + std::to_string(result.downloaded_bytes()) +
            ",\"totalBytes\":" + std::to_string(result.total_bytes()) + "}";
        (void)co_await context.db().execute(R"sql(
UPDATE edge_task SET status = $1, result = $2::jsonb, updated_at = NOW(),
    completed_at = CASE WHEN $3 THEN NOW() ELSE NULL END
WHERE id = $4::uuid AND node_id = $5::uuid AND task_type = 'firmware'
  AND status NOT IN ('succeeded', 'failed'))sql",
                                             service::common::dbParams(status, json, completed, id,
                                                                       nodeId));
    }

    static ruvia::Task<void> saveModemResult(
        ruvia::WebWorkerContext& context, std::string_view nodeId,
        const pb::ModemControlResult& result) {
        if (result.request_id().size() != 16)
            co_return;
        const auto id = protocol::uuidText(result.request_id());
        std::string status = "running";
        bool completed = false;
        if (result.state() == pb::MODEM_CONTROL_ACCEPTED)
            status = "accepted";
        else if (result.state() == pb::MODEM_CONTROL_SUCCEEDED) {
            status = "succeeded";
            completed = true;
        } else if (result.state() == pb::MODEM_CONTROL_FAILED) {
            status = "failed";
            completed = true;
        }
        const std::string json = "{\"message\":\"" + jsonEscape(result.message()) +
                                 "\",\"apn\":\"" + jsonEscape(result.apn()) + "\"}";
        (void)co_await context.db().execute(R"sql(
UPDATE edge_task SET status = $1, result = $2::jsonb, updated_at = NOW(),
    completed_at = CASE WHEN $3 THEN NOW() ELSE NULL END
WHERE id = $4::uuid AND node_id = $5::uuid AND task_type = 'modem'
  AND status NOT IN ('succeeded', 'failed'))sql",
                                            service::common::dbParams(status, json, completed, id,
                                                                      nodeId));
    }

    static ruvia::Task<void> savePlatformResult(
        ruvia::WebWorkerContext& context, std::string_view nodeId,
        const pb::PlatformConfigResult& result) {
        if (result.request_id().size() != 16)
            co_return;
        const auto id = protocol::uuidText(result.request_id());
        const std::string status = result.success() ? "succeeded" : "failed";
        const std::string json = "{\"message\":\"" + jsonEscape(result.message()) + "\"}";
        (void)co_await context.db().execute(R"sql(
WITH transitioned AS (
    UPDATE edge_task
    SET status = $1, result = $2::jsonb, updated_at = NOW(), completed_at = NOW()
    WHERE id = $3::uuid AND node_id = $4::uuid
      AND task_type IN ('platform_upsert', 'platform_delete')
      AND status NOT IN ('succeeded', 'failed')
    RETURNING (request->>'platform_id')::uuid AS platform_id, task_type
), updated AS (
    UPDATE edge_node_platform target
    SET status = jsonb_build_object('state', $5::text, 'message', $6::text),
        updated_at = NOW()
    FROM transitioned task
    WHERE target.node_id = $4::uuid AND target.platform_id = task.platform_id
      AND NOT ($7::boolean AND task.task_type = 'platform_delete')
    RETURNING target.platform_id
)
DELETE FROM edge_node_platform target
USING transitioned task
WHERE $7::boolean AND task.task_type = 'platform_delete'
  AND target.node_id = $4::uuid AND target.platform_id = task.platform_id)sql",
                                            service::common::dbParams(
                                                status, json, id, nodeId,
                                                result.success() ? "applied" : "failed",
                                                result.message(), result.success()));
    }

    static std::string hex(std::string_view value) {
        constexpr char digits[] = "0123456789abcdef";
        std::string output;
        output.reserve(value.size() * 2);
        for (const char item : value) {
            const auto byte = static_cast<std::uint8_t>(item);
            output.push_back(digits[byte >> 4U]);
            output.push_back(digits[byte & 0x0fU]);
        }
        return output;
    }

    static ruvia::Task<void> saveConfigApplied(ruvia::WebWorkerContext& context,
                                                std::string_view nodeId,
                                                const pb::ConfigApplied& result) {
        if (result.revision() == 0 || result.sha256().size() != 32)
            co_return;
        const auto digest = hex(result.sha256());
        (void)co_await context.db().execute(R"sql(
UPDATE edge_config_revision
SET status = 'applied', message = '', completed_at = NOW()
WHERE node_id = $1::uuid AND revision = $2 AND sha256 = $3)sql",
                                            service::common::dbParams(
                                                nodeId,
                                                static_cast<std::int64_t>(result.revision()),
                                                digest));
        (void)co_await context.db().execute(R"sql(
UPDATE edge_node
SET status = jsonb_set(
        jsonb_set(
            jsonb_set(status, '{config,activeVersion}', to_jsonb(GREATEST(
                COALESCE(CASE WHEN status->'config'->>'activeVersion' ~ '^-?[0-9]{1,18}$'
                              THEN (status->'config'->>'activeVersion')::bigint END, 0),
                $1::bigint)), true),
            '{config,state}', to_jsonb(CASE
                WHEN COALESCE(CASE WHEN status->'config'->>'desiredVersion' ~ '^-?[0-9]{1,18}$'
                                   THEN (status->'config'->>'desiredVersion')::bigint END, 0) = $1
                THEN 'applied'
                ELSE COALESCE(status->'config'->>'state', 'idle') END::text), true),
        '{config,message}', to_jsonb(CASE
            WHEN COALESCE(CASE WHEN status->'config'->>'desiredVersion' ~ '^-?[0-9]{1,18}$'
                               THEN (status->'config'->>'desiredVersion')::bigint END, 0) = $1
            THEN ''
            ELSE COALESCE(status->'config'->>'message', '') END::text), true),
    updated_at = NOW()
WHERE id = $2::uuid)sql",
                                            service::common::dbParams(
                                                static_cast<std::int64_t>(result.revision()),
                                                nodeId));
    }

    static ruvia::Task<void> saveConfigRejected(ruvia::WebWorkerContext& context,
                                                 std::string_view nodeId,
                                                 const pb::ConfigRejected& result) {
        if (result.revision() == 0)
            co_return;
        const std::string message = result.code() + ": " + result.message();
        (void)co_await context.db().execute(R"sql(
UPDATE edge_config_revision
SET status = 'rejected', message = $1, completed_at = NOW()
WHERE node_id = $2::uuid AND revision = $3)sql",
                                            service::common::dbParams(
                                                message, nodeId,
                                                static_cast<std::int64_t>(result.revision())));
        (void)co_await context.db().execute(R"sql(
UPDATE edge_node
SET status = jsonb_set(
        jsonb_set(status, '{config,state}', to_jsonb(CASE
            WHEN COALESCE(CASE WHEN status->'config'->>'desiredVersion' ~ '^-?[0-9]{1,18}$'
                               THEN (status->'config'->>'desiredVersion')::bigint END, 0) = $1
            THEN 'rejected'
            ELSE COALESCE(status->'config'->>'state', 'idle') END::text), true),
        '{config,message}', to_jsonb(CASE
            WHEN COALESCE(CASE WHEN status->'config'->>'desiredVersion' ~ '^-?[0-9]{1,18}$'
                               THEN (status->'config'->>'desiredVersion')::bigint END, 0) = $1
            THEN $2::text
            ELSE COALESCE(status->'config'->>'message', '') END::text), true),
    updated_at = NOW()
WHERE id = $3::uuid)sql",
                                            service::common::dbParams(
                                                static_cast<std::int64_t>(result.revision()),
                                                message,
                                                nodeId));
    }

    static std::string protocolName(pb::Protocol value) {
        if (value == pb::PROTOCOL_MODBUS)
            return "Modbus";
        if (value == pb::PROTOCOL_S7)
            return "S7";
        if (value == pb::PROTOCOL_SL651)
            return "SL651";
        return {};
    }

    static std::string scalarJson(const pb::ScalarValue& value) {
        switch (value.value_case()) {
        case pb::ScalarValue::kBoolValue:
            return value.bool_value() ? "1" : "0";
        case pb::ScalarValue::kSignedValue:
            return std::to_string(value.signed_value());
        case pb::ScalarValue::kUnsignedValue:
            return std::to_string(value.unsigned_value());
        case pb::ScalarValue::kDoubleValue: {
            std::ostringstream output;
            output.precision(15);
            output << value.double_value();
            return output.str();
        }
        case pb::ScalarValue::kStringValue:
            return "\"" + jsonEscape(value.string_value()) + "\"";
        case pb::ScalarValue::kBytesValue:
            return "\"" + hex(value.bytes_value()) + "\"";
        default:
            return "null";
        }
    }

    static std::string scalarKind(const pb::ScalarValue& value) {
        switch (value.kind()) {
        case pb::VALUE_BOOL:
            return "BOOL";
        case pb::VALUE_SIGNED:
            return "SIGNED";
        case pb::VALUE_UNSIGNED:
            return "UNSIGNED";
        case pb::VALUE_DOUBLE:
            return "DOUBLE";
        case pb::VALUE_STRING:
            return "STRING";
        case pb::VALUE_BYTES:
            return "BYTES";
        default:
            return "UNSPECIFIED";
        }
    }

    static std::string scalarText(const pb::ScalarValue& value) {
        switch (value.value_case()) {
        case pb::ScalarValue::kBoolValue:
            return value.bool_value() ? "1" : "0";
        case pb::ScalarValue::kSignedValue:
            return std::to_string(value.signed_value());
        case pb::ScalarValue::kUnsignedValue:
            return std::to_string(value.unsigned_value());
        case pb::ScalarValue::kDoubleValue: {
            std::ostringstream output;
            output.precision(15);
            output << value.double_value();
            return output.str();
        }
        case pb::ScalarValue::kStringValue:
            return value.string_value();
        case pb::ScalarValue::kBytesValue:
            return hex(value.bytes_value());
        default:
            return {};
        }
    }

    static std::string telemetryJson(const pb::TelemetryRecord& record) {
        std::string output = "{\"function_code\":\"" +
                             jsonEscape(record.function_code()) +
                             "\",\"function_name\":\"" +
                             jsonEscape(record.function_name()) + "\",\"direction\":\"" +
                             jsonEscape(record.direction()) + "\",\"values\":{";
        bool first = true;
        for (const auto& item : record.values()) {
            if (!first)
                output.push_back(',');
            output += "\"" + jsonEscape(item.element_id()) + "\":{\"name\":\"" +
                      jsonEscape(item.name()) + "\",\"value\":" +
                      (item.has_value() ? scalarJson(item.value()) : "null") +
                      ",\"dataType\":\"" +
                      jsonEscape(item.has_value() ? scalarKind(item.value()) : "UNSPECIFIED") +
                      "\"" +
                      ",\"unit\":\"" + jsonEscape(item.unit()) + "\"}";
            first = false;
        }
        output += "}}";
        return output;
    }

    static void collectTelemetry(const metadata::Catalog& catalog,
                          std::string_view nodeId, std::int64_t receivedAtMs,
                          const pb::TelemetryBatch& batch,
                          std::vector<service::message::StreamMessage>& messages) {
        const auto node = catalog.find(std::string(nodeId));
        if (node == catalog.end())
            return;
        for (const auto& record : batch.records()) {
            if (record.record_id().size() != 16 || record.device_id().size() != 16)
                continue;
            const auto deviceId = protocol::uuidText(record.device_id());
            const auto device = node->second.find(deviceId);
            if (device == node->second.end())
                continue;
            message::ParsedDeviceMessage parsed;
            if (record.model_id().size() == 16 && record.model_revision() > 0 &&
                record.model_revision() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                parsed.modelId = protocol::uuidText(record.model_id());
                parsed.modelRevision = static_cast<std::int64_t>(record.model_revision());
            } else if (!record.model_id().empty() || record.model_revision() != 0) {
                throw std::runtime_error("invalid edge telemetry model reference");
            }
            parsed.messageId = protocol::uuidText(record.record_id());
            parsed.causationId = parsed.messageId;
            parsed.linkId = device->second.linkId;
            parsed.deviceId = deviceId;
            parsed.deviceCode = device->second.deviceCode;
            parsed.protocol = protocolName(record.protocol());
            if (parsed.protocol.empty())
                parsed.protocol = device->second.protocol;
            parsed.connectionId = std::string(nodeId);
            parsed.occurredAtMs = receivedAtMs;
            parsed.observedAtMs = record.observed_at_ms();
            parsed.storagePolicy = device->second.storagePolicy;
            parsed.onlineWindowMs = device->second.onlineWindowMs;
            parsed.source = "edge";
            parsed.valuesJson = telemetryJson(record);
            if (!record.raw_payload().empty())
                parsed.rawPayloads.emplace_back(record.raw_payload().begin(),
                                                record.raw_payload().end());
            service::message::StreamMessage streamMessage;
            streamMessage.fields = message::parsedFields(parsed);
            messages.push_back(std::move(streamMessage));
        }
    }

    ruvia::Task<void> saveCommandResult(ruvia::WebWorkerContext& context,
                                        const metadata::Catalog& catalog,
                                        std::string_view nodeId,
                                        std::int64_t receivedAtMs,
                                        const pb::CommandResult& result) {
        if (result.command_id().size() != 16 || result.device_id().size() != 16)
            co_return;
        if (!protocol::terminalCommandResultState(result.state()))
            co_return;
        const auto commandId = protocol::uuidText(result.command_id());
        const auto deviceId = protocol::uuidText(result.device_id());
        const auto node = catalog.find(std::string(nodeId));
        if (node == catalog.end())
            co_return;
        const auto device = node->second.find(deviceId);
        if (device == node->second.end())
            co_return;
        const bool success = result.state() == pb::COMMAND_STATE_SUCCEEDED;
        const std::string state = success ? "SUCCEEDED" :
            result.state() == pb::COMMAND_STATE_READBACK_MISMATCH ? "READBACK_MISMATCH" :
            result.state() == pb::COMMAND_STATE_DEVICE_OFFLINE ||
            result.state() == pb::COMMAND_STATE_REJECTED ? "REJECTED" :
            result.state() == pb::COMMAND_STATE_FAILED ? "FAILED" : "UNKNOWN";
        const auto completedAtMs = message::effectiveObservedAt(
            result.completed_at_ms(), receivedAtMs);
        std::vector<message::StreamField> fields{
            {"message_id", message::nextMessageId()},
            {"causation_id", commandId},
            {"command_id", commandId},
            {"device_id", deviceId},
            {"device_code", device->second.deviceCode},
            {"protocol", device->second.protocol},
            {"attempt", "1"},
            {"success", success ? "1" : "0"},
            {"result_state", state},
            {"reason", result.message()},
            {"worker_id", "0"},
            {"created_at_ms", std::to_string(message::utcNowMilliseconds())},
            {"completed_at_ms", std::to_string(completedAtMs)},
            {"actual_value_count", std::to_string(result.actual_values_size())}};
        for (int index = 0; index < result.actual_values_size(); ++index) {
            const auto& actual = result.actual_values(index);
            const auto prefix = "actual_value_" + std::to_string(index) + "_";
            fields.push_back({prefix + "element_id", actual.element_id()});
            fields.push_back({prefix + "name", actual.name()});
            fields.push_back({prefix + "kind",
                              actual.has_value() ? scalarKind(actual.value()) : "UNSPECIFIED"});
            fields.push_back({prefix + "value",
                              actual.has_value() ? scalarText(actual.value()) : std::string{}});
            fields.push_back({prefix + "unit", actual.unit()});
        }
        (void)co_await message::redis::publishAndWake(
            context.redis(), message::commandResultStream(), fields,
            std::nullopt,
            service::message::WorkerStreamTask::CommandResult, 10000);
    }

    static std::string deviceStatusKey(std::string_view nodeId, std::string_view deviceId) {
        return "iot:runtime:edge:" + std::string(nodeId) + ":device:" +
               std::string(deviceId);
    }

    static ruvia::Task<void> saveDeviceStatus(ruvia::WebWorkerContext& context,
                                               std::string_view nodeId,
                                               const pb::DeviceStatusReport& report) {
        bool updated = false;
        for (const auto& status : report.devices()) {
            if (status.device_id().size() != 16)
                continue;
            const auto deviceId = protocol::uuidText(status.device_id());
            std::string clients;
            for (const auto& client : status.clients()) {
                if (!clients.empty())
                    clients.push_back(',');
                clients += client;
            }
            const auto key = deviceStatusKey(nodeId, deviceId);
            co_await service::message::redis::eraseHash(context.redis(), key);
            co_await service::message::redis::setHash(
                context.redis(), key,
                {{"node_id", std::string(nodeId)},
                 {"device_id", deviceId},
                 {"state", status.state()},
                 {"reason", status.reason()},
                 {"client_count", std::to_string(status.client_count())},
                 {"clients", clients},
                 {"last_activity_at_ms", std::to_string(status.last_activity_at_ms())},
                 {"updated_at_ms", std::to_string(protocol::nowMs())}});
            (void)co_await service::message::redis::command(
                context.redis(), {"PEXPIRE", key, "900000"});
            updated = true;
        }
        if (updated)
            co_await service::telemetry::latest::publishRealtimeChange(context.redis());
    }

    static std::string jsonEscape(std::string_view value) {
        std::string output;
        output.reserve(value.size());
        for (const char ch : value) {
            if (ch == '"' || ch == '\\')
                output.push_back('\\');
            if (static_cast<unsigned char>(ch) >= 0x20U)
                output.push_back(ch);
        }
        return output;
    }

    static std::string jsonQuoted(std::string_view value) {
        return "\"" + jsonEscape(value) + "\"";
    }

};

} // namespace service::edge
