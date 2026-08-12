#pragma once

#include <charconv>
#include <cmath>
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

#include "service/common/http.h"
#include "service/features/collector/stream.h"
#include "service/utils/number.h"

namespace service::edge::metadata {

inline constexpr std::string_view kChangesStream{"iot:edge:metadata:changes"};
inline constexpr std::string_view kStoreNodeScript = R"lua(
redis.call('DEL', KEYS[1])
for index = 3, #ARGV, 2 do
  redis.call('HSET', KEYS[1], ARGV[index], ARGV[index + 1])
end
if ARGV[1] == '1' then
  redis.call('XADD', KEYS[2], 'MAXLEN', '~', 10000, '*', 'node_id', ARGV[2])
end
return (#ARGV - 2) / 2
)lua";

inline constexpr std::string_view kLoadNodeSql = R"sql(
SELECT d.id::text, d.link_id::text, d.protocol_params->>'device_code', p.protocol,
       COALESCE(NULLIF(p.config->>'storageInterval', ''), '1'),
       COALESCE(NULLIF(d.protocol_params->>'online_timeout', ''), '300')
FROM device d
JOIN link l ON l.id = d.link_id AND l.execution = 'edge' AND l.deleted_at IS NULL
JOIN protocol_config p ON p.id = d.protocol_config_id AND p.deleted_at IS NULL
WHERE l.edge_node_id = $1::uuid AND d.deleted_at IS NULL
ORDER BY d.id)sql";

inline constexpr std::string_view kLoadCatalogSql = R"sql(
SELECT n.id::text, d.id::text, d.link_id::text, d.protocol_params->>'device_code', p.protocol,
       COALESCE(NULLIF(p.config->>'storageInterval', ''), '1'),
       COALESCE(NULLIF(d.protocol_params->>'online_timeout', ''), '300')
FROM edge_node n
LEFT JOIN link l ON l.edge_node_id = n.id AND l.execution = 'edge' AND l.deleted_at IS NULL
LEFT JOIN device d ON d.link_id = l.id AND d.deleted_at IS NULL
LEFT JOIN protocol_config p ON p.id = d.protocol_config_id AND p.deleted_at IS NULL
ORDER BY n.id, d.id)sql";

struct Device final {
    std::string linkId;
    std::string deviceCode;
    std::string protocol;
    std::int64_t storageInterval{1};
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
    appendField(output, std::to_string(device.storageInterval));
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

inline double number(std::string_view value, double fallback = 0.0) noexcept {
    if (value.empty())
        return fallback;
    const auto result = service::utils::decimal(value);
    return result.value_or(fallback);
}

inline std::int64_t positiveCeil(std::string_view value, double fallback = 1.0) noexcept {
    double parsed = number(value, fallback);
    if (parsed < 1.0)
        parsed = 1.0;
    const auto maximum = static_cast<double>(std::numeric_limits<std::int64_t>::max());
    if (parsed > maximum)
        return std::numeric_limits<std::int64_t>::max();
    return static_cast<std::int64_t>(std::ceil(parsed));
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
    const auto storageInterval = takeField(value, offset);
    const auto onlineWindowMs = takeField(value, offset);
    if (!linkId || !deviceCode || !protocol || !storageInterval || !onlineWindowMs ||
        offset != value.size())
        return std::nullopt;
    const auto storage = integer(*storageInterval);
    const auto online = integer(*onlineWindowMs);
    if (!storage || !online || *storage < 1 || *online < 1000)
        return std::nullopt;
    return Device{std::string(*linkId), std::string(*deviceCode), std::string(*protocol),
                  *storage, *online};
}

template <typename Pipeline>
void queueStoreNode(Pipeline& pipeline, std::string_view nodeId,
                    const NodeSnapshot& snapshot, bool notify) {
    const std::vector<std::string> keys{key(nodeId), std::string(kChangesStream)};
    std::vector<std::string> arguments{notify ? "1" : "0", std::string(nodeId)};
    arguments.reserve(2 + snapshot.size() * 2);
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
    snapshot.reserve(rows.rows().size());
    for (const auto& row : rows.rows()) {
        snapshot.emplace(
            std::string(row[0].text()),
            Device{std::string(row[1].text()), std::string(row[2].text()),
                   std::string(row[3].text()), positiveCeil(row[4].text()),
                   onlineWindowMilliseconds(row[5].text())});
    }
    co_return snapshot;
}

template <typename Context>
ruvia::Task<Catalog> loadCatalogFromDatabase(Context& context) {
    const auto rows = co_await context.db().query(kLoadCatalogSql);
    Catalog catalog;
    for (const auto& row : rows.rows()) {
        auto& snapshot = catalog[std::string(row[0].text())];
        if (row[1].isNull() || row[2].isNull() || row[3].isNull() || row[4].isNull())
            continue;
        snapshot.emplace(
            std::string(row[1].text()),
            Device{std::string(row[2].text()), std::string(row[3].text()),
                   std::string(row[4].text()), positiveCeil(row[5].text()),
                   onlineWindowMilliseconds(row[6].text())});
    }
    co_return catalog;
}

template <typename Redis>
ruvia::Task<void> storeNode(const Redis& redis, std::string_view nodeId,
                            const NodeSnapshot& snapshot, bool notify) {
    const std::vector<std::string> keyStore{key(nodeId), std::string(kChangesStream)};
    std::vector<std::string> argumentStore{notify ? "1" : "0", std::string(nodeId)};
    argumentStore.reserve(2 + snapshot.size() * 2);
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
        queueStoreNode(pipeline, nodeId, snapshot, false);
    const auto replies = co_await std::move(pipeline).exec();
    service::message::redis::requirePipelineSuccess("hydrate edge metadata", replies);
    co_return catalog;
}

} // namespace service::edge::metadata
