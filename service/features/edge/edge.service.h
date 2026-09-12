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
#include <ruvia/web/db/DbQuery.h>

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
    ruvia::DbQuery query;
    const auto jsonText = [&query](std::string_view column,
                                   std::string_view table,
                                   std::string_view key) {
        return query.binary(query.column(column, table),
                            ruvia::DbBinaryOperator::kJsonGetText,
                            query.cast(query.value(key), ruvia::DbDataType::kText));
    };
    query
        .select({
            query.cast(query.column("id", "d"), ruvia::DbDataType::kText),
            query.cast(query.column("link_id", "d"), ruvia::DbDataType::kText),
            jsonText("protocol_params", "d", "device_code"),
            query.column("protocol", "p"),
            query.coalesce({
                query.nullIf(jsonText("config", "p", "storagePolicy"),
                             query.value(std::string_view{})),
                query.value(std::string_view{"report"})}),
            query.coalesce({
                query.nullIf(jsonText("protocol_params", "d", "online_timeout"),
                             query.value(std::string_view{})),
                query.value(std::string_view{"300"})}),
        })
        .from("device", "d")
        .join(
            ruvia::DbJoinType::kInner, "link",
            query.binary(
                query.binary(query.column("id", "l"),
                             ruvia::DbBinaryOperator::kEqual,
                             query.column("link_id", "d")),
                ruvia::DbBinaryOperator::kAnd,
                query.binary(
                    query.binary(query.column("execution", "l"),
                                 ruvia::DbBinaryOperator::kEqual,
                                 query.value(std::string_view{"edge"})),
                    ruvia::DbBinaryOperator::kAnd,
                    query.unary(ruvia::DbUnaryOperator::kIsNull,
                                query.column("deleted_at", "l")))),
            "l")
        .join(
            ruvia::DbJoinType::kInner, "device_model",
            query.binary(
                query.binary(query.column("device_id", "p"),
                             ruvia::DbBinaryOperator::kEqual,
                             query.column("id", "d")),
                ruvia::DbBinaryOperator::kAnd,
                query.unary(ruvia::DbUnaryOperator::kIsNull,
                            query.column("deleted_at", "p"))),
            "p")
        .where(query.binary(query.column("edge_node_id", "l"),
                           ruvia::DbBinaryOperator::kEqual,
                           query.cast(query.value(nodeId),
                                      ruvia::DbDataType::kUuid)))
        .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull,
                              query.column("deleted_at", "d")))
        .orderBy(query.column("id", "d"));
    const auto rows = co_await context.db().query(query);
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
    ruvia::DbQuery query;
    const auto jsonText = [&query](std::string_view column,
                                   std::string_view table,
                                   std::string_view key) {
        return query.binary(query.column(column, table),
                            ruvia::DbBinaryOperator::kJsonGetText,
                            query.cast(query.value(key), ruvia::DbDataType::kText));
    };
    query
        .select({
            query.cast(query.column("id", "n"), ruvia::DbDataType::kText),
            query.cast(query.column("id", "d"), ruvia::DbDataType::kText),
            query.cast(query.column("link_id", "d"), ruvia::DbDataType::kText),
            jsonText("protocol_params", "d", "device_code"),
            query.column("protocol", "p"),
            query.coalesce({
                query.nullIf(jsonText("config", "p", "storagePolicy"),
                             query.value(std::string_view{})),
                query.value(std::string_view{"report"})}),
            query.coalesce({
                query.nullIf(jsonText("protocol_params", "d", "online_timeout"),
                             query.value(std::string_view{})),
                query.value(std::string_view{"300"})}),
        })
        .from("edge_node", "n")
        .join(
            ruvia::DbJoinType::kLeft, "link",
            query.binary(
                query.binary(query.column("edge_node_id", "l"),
                             ruvia::DbBinaryOperator::kEqual,
                             query.column("id", "n")),
                ruvia::DbBinaryOperator::kAnd,
                query.binary(
                    query.binary(query.column("execution", "l"),
                                 ruvia::DbBinaryOperator::kEqual,
                                 query.value(std::string_view{"edge"})),
                    ruvia::DbBinaryOperator::kAnd,
                    query.unary(ruvia::DbUnaryOperator::kIsNull,
                                query.column("deleted_at", "l")))),
            "l")
        .join(
            ruvia::DbJoinType::kLeft, "device",
            query.binary(
                query.binary(query.column("link_id", "d"),
                             ruvia::DbBinaryOperator::kEqual,
                             query.column("id", "l")),
                ruvia::DbBinaryOperator::kAnd,
                query.unary(ruvia::DbUnaryOperator::kIsNull,
                            query.column("deleted_at", "d"))),
            "d")
        .join(
            ruvia::DbJoinType::kLeft, "device_model",
            query.binary(
                query.binary(query.column("device_id", "p"),
                             ruvia::DbBinaryOperator::kEqual,
                             query.column("id", "d")),
                ruvia::DbBinaryOperator::kAnd,
                query.unary(ruvia::DbUnaryOperator::kIsNull,
                            query.column("deleted_at", "p"))),
            "p")
        .orderBy(query.column("id", "n"))
        .addOrderBy(query.column("id", "d"));
    const auto rows = co_await context.db().query(query);
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

inline ruvia::DbQuery::Expr jsonGet(ruvia::DbQuery& query,
                                    ruvia::DbQuery::Expr value,
                                    std::string_view key) {
    return query.binary(std::move(value), ruvia::DbBinaryOperator::kJsonGet,
                        query.cast(query.value(key), ruvia::DbDataType::kText));
}

inline ruvia::DbQuery::Expr jsonText(ruvia::DbQuery& query,
                                     ruvia::DbQuery::Expr value,
                                     std::string_view key) {
    return query.binary(std::move(value),
                        ruvia::DbBinaryOperator::kJsonGetText,
                        query.cast(query.value(key), ruvia::DbDataType::kText));
}

inline ruvia::DbQuery::Expr jsonText(ruvia::DbQuery& query,
                                     std::string_view column,
                                     std::string_view table,
                                     std::string_view key) {
    return jsonText(query, query.column(column, table), key);
}

inline ruvia::DbQuery::Expr jsonPath(ruvia::DbQuery& query,
                                     std::string_view path) {
    return query.cast(
        query.value(path),
        ruvia::DbTypeDefinition{.dataType = ruvia::DbDataType::kText,
                                .array = true});
}

inline ruvia::DbQuery::Expr toJsonb(ruvia::DbQuery& query,
                                   ruvia::DbQuery::Expr value) {
    return query.call("to_jsonb", {std::move(value)});
}

inline ruvia::DbQuery::Expr toJsonbText(ruvia::DbQuery& query,
                                       std::string_view value) {
    return toJsonb(query, query.cast(query.value(value), ruvia::DbDataType::kText));
}

inline ruvia::DbQuery::Expr nullableDefault(ruvia::DbQuery& query,
                                            ruvia::DbQuery::Expr value,
                                            std::string_view fallback) {
    return query.coalesce({std::move(value),
                           query.cast(query.value(fallback), ruvia::DbDataType::kText)});
}

inline ruvia::DbQuery::Expr textDefault(ruvia::DbQuery& query,
                                        ruvia::DbQuery::Expr value,
                                        std::string_view fallback) {
    return query.coalesce({
        query.nullIf(std::move(value), query.value(std::string_view{})),
        query.cast(query.value(fallback), ruvia::DbDataType::kText)});
}

inline ruvia::DbQuery::Expr booleanText(ruvia::DbQuery& query,
                                        ruvia::DbQuery::Expr value) {
    const auto lower = query.call(
        "lower",
        {query.coalesce({std::move(value),
                         query.cast(query.value(std::string_view{}),
                                    ruvia::DbDataType::kText)})});
    return query.caseWhen(
        {{query.binary(lower, ruvia::DbBinaryOperator::kEqual,
                       query.cast(query.value(std::string_view{"true"}),
                                  ruvia::DbDataType::kText)),
          query.cast(query.value(true), ruvia::DbDataType::kBoolean)},
         {query.binary(lower, ruvia::DbBinaryOperator::kEqual,
                       query.cast(query.value(std::string_view{"t"}),
                                  ruvia::DbDataType::kText)),
          query.cast(query.value(true), ruvia::DbDataType::kBoolean)},
         {query.binary(lower, ruvia::DbBinaryOperator::kEqual,
                       query.cast(query.value(std::string_view{"1"}),
                                  ruvia::DbDataType::kText)),
          query.cast(query.value(true), ruvia::DbDataType::kBoolean)}},
        query.cast(query.value(false), ruvia::DbDataType::kBoolean));
}

inline ruvia::DbQuery::Expr jsonKey(ruvia::DbQuery& query,
                                    std::string_view value) {
    return query.cast(query.value(value), ruvia::DbDataType::kText);
}

inline ruvia::DbQuery::Expr configVersion(ruvia::DbQuery& query,
                                          ruvia::DbQuery::Expr status,
                                          std::string_view key) {
    const auto text = jsonText(query, jsonGet(query, std::move(status), "config"), key);
    const auto valid = query.binary(
        text, ruvia::DbBinaryOperator::kRegex,
        query.cast(query.value(std::string_view{"^-?[0-9]{1,18}$"}),
                   ruvia::DbDataType::kText));
    return query.coalesce(
        {query.caseWhen({{valid, query.cast(text, ruvia::DbDataType::kBigInt)}}),
         query.value(std::int64_t{0})});
}

inline ruvia::DbQuery::Expr configVersion(ruvia::DbQuery& query,
                                          std::string_view table,
                                          std::string_view key) {
    return configVersion(query, query.column("status", table), key);
}

inline ruvia::DbQuery::Expr configState(ruvia::DbQuery& query,
                                        ruvia::DbQuery::Expr status) {
    return query.coalesce(
        {jsonText(query, jsonGet(query, std::move(status), "config"), "state"),
         query.cast(query.value(std::string_view{"idle"}),
                    ruvia::DbDataType::kText)});
}

inline ruvia::DbQuery::Expr capabilityEnabled(ruvia::DbQuery& query,
                                              std::string_view table) {
    return booleanText(query, jsonText(query, query.column("capability", table),
                                        "deviceConfig"));
}

inline ruvia::DbQuery queueSnapshotQuery(std::string_view nodeId) {
    ruvia::DbQuery next;
    const auto desired = configVersion(next, next.column("status"),
                                       "desiredVersion");
    const auto active = configVersion(next, next.column("status"),
                                      "activeVersion");
    const auto nowMilliseconds = next.cast(
        next.binary(next.extract(ruvia::DbDatePart::kEpoch,
                                 next.call("clock_timestamp")),
                    ruvia::DbBinaryOperator::kMultiply,
                    next.value(std::int64_t{1000})),
        ruvia::DbDataType::kBigInt);
    next.select({
            next.column("id"),
            next.alias(next.greatest({
                           nowMilliseconds,
                           next.binary(desired, ruvia::DbBinaryOperator::kAdd,
                                       next.value(std::int64_t{1})),
                           next.binary(active, ruvia::DbBinaryOperator::kAdd,
                                       next.value(std::int64_t{1}))}),
                       "revision"),
        })
        .from("edge_node")
        .where(next.binary(next.column("id"), ruvia::DbBinaryOperator::kEqual,
                           next.cast(next.value(nodeId), ruvia::DbDataType::kUuid)))
        .andWhere(next.binary(next.column("enrollment_status"),
                              ruvia::DbBinaryOperator::kEqual,
                              next.value(std::string_view{"approved"})))
        .andWhere(capabilityEnabled(next, {}));

    ruvia::DbQuery update;
    const auto status = update.column("status", "node");
    const auto statusWithVersion = update.call(
        "jsonb_set",
        {status, jsonPath(update, "{config,desiredVersion}"),
         toJsonb(update, update.column("revision", "next")),
         update.cast(update.value(true), ruvia::DbDataType::kBoolean)});
    const auto statusWithState = update.call(
        "jsonb_set",
        {statusWithVersion, jsonPath(update, "{config,state}"),
         toJsonbText(update, "pending"),
         update.cast(update.value(true), ruvia::DbDataType::kBoolean)});
    const auto statusWithMessage = update.call(
        "jsonb_set",
        {statusWithState, jsonPath(update, "{config,message}"),
         toJsonbText(update, ""),
         update.cast(update.value(true), ruvia::DbDataType::kBoolean)});
    update.with("next", next)
        .update("edge_node", "node")
        .set("status", statusWithMessage)
        .set("updated_at", update.call("now"))
        .updateFrom("next")
        .where(update.binary(update.column("id", "node"),
                            ruvia::DbBinaryOperator::kEqual,
                            update.column("id", "next")))
        .returning({update.column("revision", "next")});
    return update;
}

inline ruvia::DbQuery requeueDesiredQuery(std::string_view nodeId) {
    ruvia::DbQuery query;
    query.select({configVersion(query, query.column("status"), "desiredVersion"),
                  configState(query, query.column("status"))})
        .from("edge_node")
        .where(query.binary(query.column("id"), ruvia::DbBinaryOperator::kEqual,
                           query.cast(query.value(nodeId), ruvia::DbDataType::kUuid)))
        .andWhere(query.binary(query.column("enrollment_status"),
                              ruvia::DbBinaryOperator::kEqual,
                              query.value(std::string_view{"approved"})))
        .andWhere(capabilityEnabled(query, {}));
    return query;
}

inline ruvia::DbQuery requeuePendingQuery(std::string_view nodeId,
                                          std::uint64_t revision) {
    ruvia::DbQuery query;
    const auto status = query.column("status");
    const auto statusWithState = query.call(
        "jsonb_set",
        {status, jsonPath(query, "{config,state}"),
         toJsonbText(query, "pending"),
         query.cast(query.value(true), ruvia::DbDataType::kBoolean)});
    query.update("edge_node")
        .set("status", query.call(
                           "jsonb_set",
                           {statusWithState, jsonPath(query, "{config,message}"),
                            toJsonbText(query, ""),
                            query.cast(query.value(true), ruvia::DbDataType::kBoolean)}))
        .set("updated_at", query.call("now"))
        .where(query.binary(query.column("id"), ruvia::DbBinaryOperator::kEqual,
                           query.cast(query.value(nodeId), ruvia::DbDataType::kUuid)))
        .andWhere(query.binary(configVersion(query, query.column("status"),
                                             "desiredVersion"),
                              ruvia::DbBinaryOperator::kEqual,
                              query.cast(query.value(static_cast<std::int64_t>(revision)),
                                         ruvia::DbDataType::kBigInt)))
        .andWhere(query.binary(configState(query, query.column("status")),
                              ruvia::DbBinaryOperator::kNotEqual,
                              query.value(std::string_view{"rejected"})));
    return query;
}

inline ruvia::DbQuery rejectBuildQuery(std::string_view message,
                                       std::string_view nodeId,
                                       std::uint64_t revision) {
    ruvia::DbQuery query;
    const auto status = query.column("status");
    const auto statusWithState = query.call(
        "jsonb_set",
        {status, jsonPath(query, "{config,state}"),
         toJsonbText(query, "rejected"),
         query.cast(query.value(true), ruvia::DbDataType::kBoolean)});
    query.update("edge_node")
        .set("status", query.call(
                           "jsonb_set",
                           {statusWithState, jsonPath(query, "{config,message}"),
                            toJsonb(query, query.cast(query.value(message),
                                                      ruvia::DbDataType::kText)),
                            query.cast(query.value(true), ruvia::DbDataType::kBoolean)}))
        .set("updated_at", query.call("now"))
        .where(query.binary(query.column("id"), ruvia::DbBinaryOperator::kEqual,
                           query.cast(query.value(nodeId), ruvia::DbDataType::kUuid)))
        .andWhere(query.binary(configVersion(query, query.column("status"),
                                             "desiredVersion"),
                              ruvia::DbBinaryOperator::kEqual,
                              query.cast(query.value(static_cast<std::int64_t>(revision)),
                                         ruvia::DbDataType::kBigInt)));
    return query;
}

inline ruvia::DbQuery buildItemsQuery(std::string_view nodeId) {
    ruvia::DbQuery query;
    const auto protocolParams = query.column("protocol_params", "d");
    const auto modelConfig = query.column("config", "p");
    const auto endpoint = query.column("endpoint", "l");
    const auto packetConfig = jsonGet(query, modelConfig, "packet");
    const auto connectionConfig = jsonGet(query, modelConfig, "connection");
    const auto heartbeatConfig = jsonGet(query, protocolParams, "heartbeat");
    const auto deviceEnabled = query.binary(
        query.column("status", "d"), ruvia::DbBinaryOperator::kEqual,
        query.value(std::string_view{"enabled"}));
    const auto linkEnabled = query.binary(
        query.column("status", "l"), ruvia::DbBinaryOperator::kEqual,
        query.value(std::string_view{"enabled"}));

    query
        .select({
            query.cast(query.column("id", "d"), ruvia::DbDataType::kText),
            query.column("name", "d"),
            jsonText(query, protocolParams, "device_code"),
            query.column("protocol", "p"),
            textDefault(query, jsonText(query, protocolParams, "timezone"), "+08:00"),
            textDefault(query, jsonText(query, modelConfig, "readInterval"), "1"),
            textDefault(query, jsonText(query, protocolParams, "online_timeout"), "300"),
            textDefault(query, jsonText(query, protocolParams, "slave_id"), "1"),
            nullableDefault(query, jsonText(query, protocolParams, "modbus_mode"), "TCP"),
            jsonText(query, endpoint, "transport"),
            jsonText(query, endpoint, "interface"),
            nullableDefault(query, jsonText(query, endpoint, "mode"), ""),
            nullableDefault(query, jsonText(query, endpoint, "ip"), ""),
            textDefault(query, jsonText(query, endpoint, "port"), "0"),
            textDefault(query, jsonText(query, endpoint, "baud_rate"), "9600"),
            textDefault(query, jsonText(query, endpoint, "data_bits"), "8"),
            textDefault(query, jsonText(query, endpoint, "stop_bits"), "1"),
            nullableDefault(query, jsonText(query, endpoint, "parity"), "none"),
            booleanText(query, jsonText(query, endpoint, "rs485")),
            textDefault(query, jsonText(query, packetConfig, "mergeGap"), "0"),
            textDefault(query, jsonText(query, packetConfig, "maxQuantity"), "125"),
            nullableDefault(query, jsonText(query, connectionConfig, "mode"), "RACK_SLOT"),
            nullableDefault(query, jsonText(query, connectionConfig, "connectionType"), "PG"),
            textDefault(query, jsonText(query, connectionConfig, "rack"), "0"),
            textDefault(query, jsonText(query, connectionConfig, "slot"), "1"),
            nullableDefault(query, jsonText(query, connectionConfig, "localTSAP"), ""),
            nullableDefault(query, jsonText(query, connectionConfig, "remoteTSAP"), ""),
            nullableDefault(query, jsonText(query, heartbeatConfig, "mode"), "OFF"),
            nullableDefault(query, jsonText(query, heartbeatConfig, "content"), ""),
            query.binary(query.binary(deviceEnabled, ruvia::DbBinaryOperator::kAnd,
                                      query.column("enabled", "p")),
                         ruvia::DbBinaryOperator::kAnd, linkEnabled),
            query.cast(query.column("link_id", "d"), ruvia::DbDataType::kText),
            textDefault(query, jsonText(query, modelConfig, "commandFastReadDuration"), "60"),
            textDefault(query, jsonText(query, modelConfig, "commandFastReadInterval"), "1"),
            query.column("name", "l"),
            linkEnabled,
        })
        .from("device", "d")
        .join(
            ruvia::DbJoinType::kInner, "link",
            query.binary(
                query.binary(query.column("id", "l"),
                             ruvia::DbBinaryOperator::kEqual,
                             query.column("link_id", "d")),
                ruvia::DbBinaryOperator::kAnd,
                query.binary(
                    query.binary(query.column("execution", "l"),
                                 ruvia::DbBinaryOperator::kEqual,
                                 query.value(std::string_view{"edge"})),
                    ruvia::DbBinaryOperator::kAnd,
                    query.unary(ruvia::DbUnaryOperator::kIsNull,
                                query.column("deleted_at", "l")))),
            "l")
        .join(
            ruvia::DbJoinType::kInner, "device_model",
            query.binary(
                query.binary(query.column("device_id", "p"),
                             ruvia::DbBinaryOperator::kEqual,
                             query.column("id", "d")),
                ruvia::DbBinaryOperator::kAnd,
                query.unary(ruvia::DbUnaryOperator::kIsNull,
                            query.column("deleted_at", "p"))),
            "p")
        .where(query.binary(query.column("edge_node_id", "l"),
                           ruvia::DbBinaryOperator::kEqual,
                           query.cast(query.value(nodeId),
                                      ruvia::DbDataType::kUuid)))
        .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull,
                              query.column("deleted_at", "d")))
        .orderBy(query.column("id", "d"));
    return query;
}

inline ruvia::DbQuery appendModbusQuery(std::string_view nodeId) {
    ruvia::DbQuery query;
    const auto config = query.column("config", "p");
    const auto itemSource = query.call(
        "jsonb_array_elements",
        {query.coalesce({jsonGet(query, config, "registers"),
                         query.cast(query.value(std::string_view{"[]"}),
                                    ruvia::DbDataType::kJsonb)})});
    const auto item = query.column("item");
    query
        .select({
            query.cast(query.column("id", "d"), ruvia::DbDataType::kText),
            jsonText(query, item, "id"),
            jsonText(query, item, "name"),
            nullableDefault(query, jsonText(query, item, "unit"), ""),
            jsonText(query, item, "registerType"),
            jsonText(query, item, "dataType"),
            query.coalesce({jsonText(query, item, "byteOrder"),
                           jsonText(query, config, "byteOrder"),
                           query.value(std::string_view{"BIG_ENDIAN"})}),
            textDefault(query, jsonText(query, item, "address"), "0"),
            textDefault(query, jsonText(query, item, "quantity"), "1"),
            textDefault(query, jsonText(query, item, "scale"), "1"),
            textDefault(query, jsonText(query, item, "decimals"), "-1"),
            booleanText(query, jsonText(query, item, "writable")),
        })
        .from("device", "d")
        .join(
            ruvia::DbJoinType::kInner, "link",
            query.binary(
                query.binary(query.column("id", "l"),
                             ruvia::DbBinaryOperator::kEqual,
                             query.column("link_id", "d")),
                ruvia::DbBinaryOperator::kAnd,
                query.binary(
                    query.binary(query.column("execution", "l"),
                                 ruvia::DbBinaryOperator::kEqual,
                                 query.value(std::string_view{"edge"})),
                    ruvia::DbBinaryOperator::kAnd,
                    query.unary(ruvia::DbUnaryOperator::kIsNull,
                                query.column("deleted_at", "l")))),
            "l")
        .join(
            ruvia::DbJoinType::kInner, "device_model",
            query.binary(
                query.binary(query.column("device_id", "p"),
                             ruvia::DbBinaryOperator::kEqual,
                             query.column("id", "d")),
                ruvia::DbBinaryOperator::kAnd,
                query.binary(query.column("protocol", "p"),
                             ruvia::DbBinaryOperator::kEqual,
                             query.value(std::string_view{"Modbus"}))),
            "p")
        .joinFunction(ruvia::DbJoinType::kCross, itemSource, {}, "item",
                      {.lateral = true})
        .where(query.binary(query.column("edge_node_id", "l"),
                           ruvia::DbBinaryOperator::kEqual,
                           query.cast(query.value(nodeId),
                                      ruvia::DbDataType::kUuid)))
        .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull,
                              query.column("deleted_at", "d")))
        .orderBy(query.column("id", "d"))
        .addOrderBy(jsonText(query, item, "id"));
    return query;
}

inline ruvia::DbQuery appendS7Query(std::string_view nodeId) {
    ruvia::DbQuery query;
    const auto config = query.column("config", "p");
    const auto itemSource = query.call(
        "jsonb_array_elements",
        {query.coalesce({jsonGet(query, config, "areas"),
                         query.cast(query.value(std::string_view{"[]"}),
                                    ruvia::DbDataType::kJsonb)})});
    const auto item = query.column("item");
    query
        .select({
            query.cast(query.column("id", "d"), ruvia::DbDataType::kText),
            jsonText(query, item, "id"),
            jsonText(query, item, "name"),
            nullableDefault(query, jsonText(query, item, "unit"), ""),
            jsonText(query, item, "area"),
            textDefault(query, jsonText(query, item, "dbNumber"), "0"),
            textDefault(query, jsonText(query, item, "start"), "0"),
            textDefault(query, jsonText(query, item, "startBit"), "0"),
            textDefault(query, jsonText(query, item, "size"), "1"),
            nullableDefault(query, jsonText(query, item, "dataType"), "BOOL"),
            textDefault(query, jsonText(query, item, "decimals"), "-1"),
            booleanText(query, jsonText(query, item, "writable")),
        })
        .from("device", "d")
        .join(
            ruvia::DbJoinType::kInner, "link",
            query.binary(
                query.binary(query.column("id", "l"),
                             ruvia::DbBinaryOperator::kEqual,
                             query.column("link_id", "d")),
                ruvia::DbBinaryOperator::kAnd,
                query.binary(
                    query.binary(query.column("execution", "l"),
                                 ruvia::DbBinaryOperator::kEqual,
                                 query.value(std::string_view{"edge"})),
                    ruvia::DbBinaryOperator::kAnd,
                    query.unary(ruvia::DbUnaryOperator::kIsNull,
                                query.column("deleted_at", "l")))),
            "l")
        .join(
            ruvia::DbJoinType::kInner, "device_model",
            query.binary(
                query.binary(query.column("device_id", "p"),
                             ruvia::DbBinaryOperator::kEqual,
                             query.column("id", "d")),
                ruvia::DbBinaryOperator::kAnd,
                query.binary(query.column("protocol", "p"),
                             ruvia::DbBinaryOperator::kEqual,
                             query.value(std::string_view{"S7"}))),
            "p")
        .joinFunction(ruvia::DbJoinType::kCross, itemSource, {}, "item",
                      {.lateral = true})
        .where(query.binary(query.column("edge_node_id", "l"),
                           ruvia::DbBinaryOperator::kEqual,
                           query.cast(query.value(nodeId),
                                      ruvia::DbDataType::kUuid)))
        .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull,
                              query.column("deleted_at", "d")))
        .orderBy(query.column("id", "d"))
        .addOrderBy(jsonText(query, item, "id"));
    return query;
}

inline ruvia::DbQuery appendSl651FunctionsQuery(std::string_view nodeId) {
    ruvia::DbQuery query;
    const auto config = query.column("config", "p");
    const auto functionSource = query.call(
        "jsonb_array_elements",
        {query.coalesce({jsonGet(query, config, "funcs"),
                         query.cast(query.value(std::string_view{"[]"}),
                                    ruvia::DbDataType::kJsonb)})});
    const auto function = query.column("func");
    query
        .select({query.cast(query.column("id", "d"), ruvia::DbDataType::kText),
                 jsonText(query, function, "funcCode"),
                 jsonText(query, function, "name"),
                 jsonText(query, function, "dir")})
        .from("device", "d")
        .join(
            ruvia::DbJoinType::kInner, "link",
            query.binary(
                query.binary(query.column("id", "l"),
                             ruvia::DbBinaryOperator::kEqual,
                             query.column("link_id", "d")),
                ruvia::DbBinaryOperator::kAnd,
                query.binary(
                    query.binary(query.column("execution", "l"),
                                 ruvia::DbBinaryOperator::kEqual,
                                 query.value(std::string_view{"edge"})),
                    ruvia::DbBinaryOperator::kAnd,
                    query.unary(ruvia::DbUnaryOperator::kIsNull,
                                query.column("deleted_at", "l")))),
            "l")
        .join(
            ruvia::DbJoinType::kInner, "device_model",
            query.binary(
                query.binary(query.column("device_id", "p"),
                             ruvia::DbBinaryOperator::kEqual,
                             query.column("id", "d")),
                ruvia::DbBinaryOperator::kAnd,
                query.binary(query.column("protocol", "p"),
                             ruvia::DbBinaryOperator::kEqual,
                             query.value(std::string_view{"SL651"}))),
            "p")
        .joinFunction(ruvia::DbJoinType::kCross, functionSource, {}, "func",
                      {.lateral = true})
        .where(query.binary(query.column("edge_node_id", "l"),
                           ruvia::DbBinaryOperator::kEqual,
                           query.cast(query.value(nodeId),
                                      ruvia::DbDataType::kUuid)))
        .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull,
                              query.column("deleted_at", "d")))
        .orderBy(query.column("id", "d"))
        .addOrderBy(jsonText(query, function, "funcCode"));
    return query;
}

inline ruvia::DbQuery appendSl651ElementsQuery(std::string_view nodeId) {
    ruvia::DbQuery query;
    const auto config = query.column("config", "p");
    const auto functionSource = query.call(
        "jsonb_array_elements",
        {query.coalesce({jsonGet(query, config, "funcs"),
                         query.cast(query.value(std::string_view{"[]"}),
                                    ruvia::DbDataType::kJsonb)})});
    const auto function = query.column("func");
    ruvia::DbQuery elementRows;
    const auto elementSource = elementRows.call(
        "jsonb_array_elements",
        {elementRows.coalesce({
            jsonGet(elementRows, elementRows.column("func"), "elements"),
            elementRows.cast(elementRows.value(std::string_view{"[]"}),
                              ruvia::DbDataType::kJsonb)})});
    elementRows
        .select({elementRows.alias(elementRows.column("value", "element_value"),
                                   "element"),
                 elementRows.alias(elementRows.cast(elementRows.value(false),
                                                    ruvia::DbDataType::kBoolean),
                                   "response_element")})
        .fromFunction(elementSource, "element_value",
                      {.lateral = false, .columns = {{.name = "value"}}});

    ruvia::DbQuery responseRows;
    const auto responseElementSource = responseRows.call(
        "jsonb_array_elements",
        {responseRows.coalesce({
            jsonGet(responseRows, responseRows.column("func"),
                    "responseElements"),
            responseRows.cast(responseRows.value(std::string_view{"[]"}),
                               ruvia::DbDataType::kJsonb)})});
    responseRows
        .select({responseRows.alias(
                     responseRows.column("value", "response_value"),
                     "element"),
                 responseRows.alias(responseRows.cast(responseRows.value(true),
                                                       ruvia::DbDataType::kBoolean),
                                    "response_element")})
        .fromFunction(responseElementSource, "response_value",
                      {.lateral = false, .columns = {{.name = "value"}}});
    elementRows.combine(ruvia::DbSetOperation::kUnionAll, responseRows);
    const auto element = query.column("element", "values");
    const auto responseElement = query.column("response_element", "values");
    const auto functionCode = jsonText(query, function, "funcCode");

    query
        .select({
            query.cast(query.column("id", "d"), ruvia::DbDataType::kText),
            functionCode,
            jsonText(query, element, "id"),
            jsonText(query, element, "name"),
            nullableDefault(query, jsonText(query, element, "unit"), ""),
            jsonText(query, element, "encode"),
            textDefault(query, jsonText(query, element, "length"), "0"),
            textDefault(query, jsonText(query, element, "digits"), "0"),
            nullableDefault(query, jsonText(query, element, "guideHex"), ""),
            responseElement,
            query.binary(jsonText(query, function, "dir"),
                         ruvia::DbBinaryOperator::kEqual,
                         query.value(std::string_view{"DOWN"})),
        })
        .from("device", "d")
        .join(
            ruvia::DbJoinType::kInner, "link",
            query.binary(
                query.binary(query.column("id", "l"),
                             ruvia::DbBinaryOperator::kEqual,
                             query.column("link_id", "d")),
                ruvia::DbBinaryOperator::kAnd,
                query.binary(
                    query.binary(query.column("execution", "l"),
                                 ruvia::DbBinaryOperator::kEqual,
                                 query.value(std::string_view{"edge"})),
                    ruvia::DbBinaryOperator::kAnd,
                    query.unary(ruvia::DbUnaryOperator::kIsNull,
                                query.column("deleted_at", "l")))),
            "l")
        .join(
            ruvia::DbJoinType::kInner, "device_model",
            query.binary(
                query.binary(query.column("device_id", "p"),
                             ruvia::DbBinaryOperator::kEqual,
                             query.column("id", "d")),
                ruvia::DbBinaryOperator::kAnd,
                query.binary(query.column("protocol", "p"),
                             ruvia::DbBinaryOperator::kEqual,
                             query.value(std::string_view{"SL651"}))),
            "p")
        .joinFunction(ruvia::DbJoinType::kCross, functionSource, {}, "func",
                      {.lateral = true})
        .join(ruvia::DbJoinType::kCross, elementRows, {}, "values",
              {.lateral = true})
        .where(query.binary(query.column("edge_node_id", "l"),
                           ruvia::DbBinaryOperator::kEqual,
                           query.cast(query.value(nodeId),
                                      ruvia::DbDataType::kUuid)))
        .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull,
                              query.column("deleted_at", "d")))
        .orderBy(query.column("id", "d"))
        .addOrderBy(functionCode)
        .addOrderBy(responseElement)
        .addOrderBy(jsonText(query, element, "id"));
    return query;
}
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
        const auto version =
            co_await c.db().query(config::detail::queueSnapshotQuery(nodeId));
        if (version.empty())
            service::common::fail(17011, "边缘节点未批准或不支持设备配置", 409);
        const auto revision = unsignedInteger(version.front()[0].value().value_or(std::string_view{}));

        auto snapshot = co_await buildSnapshot(c, nodeId, revision);
        if (!snapshot) {
            service::common::fail(17012, "边缘节点配置条目超过 512 条", 409);
        }

        if (!service::common::isUuid(actorId))
            service::common::fail(10002, "invalid edge snapshot actor", 400);
        ruvia::DbQuery insert;
        insert
            .insertInto("edge_config_revision",
                        {"node_id", "revision", "sha256", "item_count",
                         "created_by"})
            .values({
                insert.cast(insert.value(nodeId), ruvia::DbDataType::kUuid),
                insert.value(static_cast<std::int64_t>(revision)),
                insert.value(snapshot->digest),
                insert.value(static_cast<std::int64_t>(snapshot->itemCount)),
                insert.cast(insert.value(actorId), ruvia::DbDataType::kUuid),
            });
        (void)co_await c.db().execute(insert);
        co_await replaceQueue(c, nodeId, revision, snapshot->wires);
        co_await metadata::publishNode(c, nodeId);
        co_return revision;
    }

    ruvia::Task<bool> requeueIfStale(ruvia::Context& c, std::string_view nodeId,
                                     std::uint64_t activeRevision) {
        const auto desired =
            co_await c.db().query(config::detail::requeueDesiredQuery(nodeId));
        if (desired.empty())
            co_return false;
        const auto revision = unsignedInteger(desired.front()[0].value().value_or(std::string_view{}));
        if (revision == 0 || revision == activeRevision ||
            desired.front()[1].value().value_or(std::string_view{}) == "rejected")
            co_return false;

        auto snapshot = co_await buildSnapshot(c, nodeId, revision);
        if (!snapshot)
            co_return false;
        ruvia::DbQuery insert;
        insert
            .insertInto("edge_config_revision",
                        {"node_id", "revision", "sha256", "item_count",
                         "created_by"})
            .values({
                insert.cast(insert.value(nodeId), ruvia::DbDataType::kUuid),
                insert.value(static_cast<std::int64_t>(revision)),
                insert.value(snapshot->digest),
                insert.value(static_cast<std::int64_t>(snapshot->itemCount)),
                insert.nullValue(),
            });
        ruvia::DbConflictOptions conflict;
        conflict.columns = {"node_id", "revision"};
        conflict.update = {
            {"sha256", insert.excluded("sha256")},
            {"item_count", insert.excluded("item_count")},
            {"status", insert.value(std::string_view{"pending"})},
            {"message", insert.value(std::string_view{})},
            {"completed_at", insert.nullValue()},
        };
        insert.onConflict(conflict);
        (void)co_await c.db().execute(insert);
        (void)co_await c.db().execute(
            config::detail::requeuePendingQuery(nodeId, revision));
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
        (void)co_await c.db().execute(
            config::detail::rejectBuildQuery(message, nodeId, revision));
    }

    template <typename Context>
    static ruvia::Task<std::vector<pb::ConfigItem>>
    buildItems(Context& c, std::string_view nodeId) {
        std::vector<pb::ConfigItem> items;
        std::set<std::string> endpoints;
        const auto devices =
            co_await c.db().query(config::detail::buildItemsQuery(nodeId));
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
        const auto rows =
            co_await c.db().query(config::detail::appendModbusQuery(nodeId));
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
        const auto rows =
            co_await c.db().query(config::detail::appendS7Query(nodeId));
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
        const auto functions = co_await c.db().query(
            config::detail::appendSl651FunctionsQuery(nodeId));
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

        const auto elements = co_await c.db().query(
            config::detail::appendSl651ElementsQuery(nodeId));
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
        ruvia::DbQuery query;
        query.select({query.column("imei"),
                      query.cast(query.column("id"), ruvia::DbDataType::kText),
                      query.column("enrollment_status")})
            .from("edge_node");
        const auto rows = co_await context.db().query(query);
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
        ruvia::DbQuery query;
        const auto uuid = [&query](std::string_view value) {
            return query.cast(query.value(value), ruvia::DbDataType::kUuid);
        };
        const auto text = [&query](const auto& value) {
            return query.cast(query.value(std::string_view(value)),
                              ruvia::DbDataType::kText);
        };
        const auto boolean = [&query](bool value) {
            return query.cast(query.value(value), ruvia::DbDataType::kBoolean);
        };
        const auto integer = [&query](auto value) {
            return query.cast(query.value(static_cast<std::int64_t>(value)),
                              ruvia::DbDataType::kBigInt);
        };
        const auto jsonKey = [&query](std::string_view value) {
            return config::detail::jsonKey(query, value);
        };
        const auto vpn = query.call(
            "jsonb_build_object",
            {jsonKey("supportsVpn"), boolean(false),
             jsonKey("wireguardVersion"), text(std::string_view{}),
             jsonKey("agentVersion"), text(std::string_view{}),
             jsonKey("publicKey"), text(std::string_view{})});
        const auto capability = query.call(
            "jsonb_build_object",
            {jsonKey("networkConfig"), boolean(hello.supports_network_config()),
             jsonKey("firmwareUpdate"), boolean(hello.supports_firmware_update()),
             jsonKey("firmwareStream"), boolean(hello.supports_firmware_stream()),
             jsonKey("platformConfig"), boolean(hello.supports_platform_config()),
             jsonKey("deviceConfig"), boolean(hello.supports_device_config()),
             jsonKey("networkConfigVersion"), integer(hello.network_config_version()),
             jsonKey("modemControl"), boolean(hello.supports_modem_control()),
             jsonKey("logs"), boolean(hello.supports_logs()),
             jsonKey("terminal"), boolean(false), jsonKey("vpn"), vpn});
        const auto signal = query.call(
            "jsonb_build_object", {jsonKey("csq"), integer(hello.signal_csq()),
                                    jsonKey("rssiDbm"), integer(hello.signal_rssi_dbm()),
                                    jsonKey("percent"), integer(hello.signal_percent())});
        const auto mobile = query.call(
            "jsonb_build_object",
            {jsonKey("available"), boolean(hello.modem_available()),
             jsonKey("simState"), text(simState(hello.sim_state())),
             jsonKey("iccid"), text(hello.iccid()), jsonKey("signal"), signal,
             jsonKey("registered"), boolean(hello.mobile_registered()),
             jsonKey("registrationStatus"), integer(hello.mobile_registration_status()),
             jsonKey("apn"), text(hello.apn()), jsonKey("operator"), text(hello.mobile_operator()),
             jsonKey("connected"), boolean(hello.mobile_connected()),
             jsonKey("ipv4"), text(hello.mobile_ipv4())});
        const auto config = query.call(
            "jsonb_build_object", {jsonKey("activeVersion"), integer(0),
                                    jsonKey("desiredVersion"), integer(0),
                                    jsonKey("state"), text("idle"),
                                    jsonKey("message"), text(std::string_view{})});
        const auto outbox = query.call(
            "jsonb_build_object", {jsonKey("records"), integer(0),
                                    jsonKey("bytes"), integer(0)});
        const auto log = query.call(
            "jsonb_build_object", {jsonKey("level"),
                                    config::detail::textDefault(
                                        query, text(hello.log_level()), "info")});
        const auto status = query.call(
            "jsonb_build_object", {jsonKey("config"), config,
                                    jsonKey("outbox"), outbox, jsonKey("log"), log});
        query.insertInto("edge_node",
                         {"id", "platform_id", "imei", "model", "software_version",
                          "hostname", "architecture", "openwrt_release", "capability",
                          "mobile", "status", "last_seen_at", "updated_at"})
            .values({uuid(std::string_view(candidate)), uuid(protocol::platformId()),
                     query.value(std::string_view(hello.imei())),
                     query.value(std::string_view(hello.model())),
                     query.value(std::string_view(hello.software_version())),
                     query.value(std::string_view(hello.hostname())),
                     query.value(std::string_view(hello.architecture())),
                     query.value(std::string_view(hello.openwrt_release())), capability, mobile,
                     status,
                     query.call("now"), query.call("now")});

        const auto existingCapability = query.column("capability", "edge_node");
        const auto existingMobile = query.column("mobile", "edge_node");
        const auto existingStatus = query.column("status", "edge_node");
        const auto excludedMobile = query.excluded("mobile");
        const auto apn = query.coalesce({
            query.nullIf(config::detail::jsonText(query, excludedMobile, "apn"),
                         query.value(std::string_view{})),
            config::detail::jsonText(query, existingMobile, "apn"),
            query.value(std::string_view{})});
        const auto operatorName = query.coalesce({
            query.nullIf(config::detail::jsonText(query, excludedMobile, "operator"),
                         query.value(std::string_view{})),
            config::detail::jsonText(query, existingMobile, "operator"),
            query.value(std::string_view{})});
        const auto mobileWithApn = query.call(
            "jsonb_set", {excludedMobile, config::detail::jsonPath(query, "{apn}"),
                           config::detail::toJsonb(query,
                               query.cast(apn, ruvia::DbDataType::kText)),
                           query.cast(query.value(true), ruvia::DbDataType::kBoolean)});
        const auto mobileUpdate = query.call(
            "jsonb_set", {mobileWithApn, config::detail::jsonPath(query, "{operator}"),
                           config::detail::toJsonb(query,
                               query.cast(operatorName, ruvia::DbDataType::kText)),
                           query.cast(query.value(true), ruvia::DbDataType::kBoolean)});
        const auto capabilityUpdate = query.binary(
            query.excluded("capability"), ruvia::DbBinaryOperator::kJsonConcat,
            query.call("jsonb_build_object",
                       {jsonKey("terminal"),
                        config::detail::booleanText(
                            query, config::detail::jsonText(query, existingCapability, "terminal")),
                        jsonKey("vpn"),
                        query.coalesce({config::detail::jsonGet(query, existingCapability, "vpn"),
                                        config::detail::jsonGet(query, query.excluded("capability"),
                                                                 "vpn")})}));
        const auto statusWithLog = query.call(
            "jsonb_set", {existingStatus, config::detail::jsonPath(query, "{log}"),
                           query.coalesce({config::detail::jsonGet(query, existingStatus, "log"),
                                           query.cast(query.value(std::string_view{"{}"}),
                                                      ruvia::DbDataType::kJsonb)}),
                           query.cast(query.value(true), ruvia::DbDataType::kBoolean)});
        const auto statusUpdate = query.call(
            "jsonb_set", {statusWithLog, config::detail::jsonPath(query, "{log,level}"),
                           config::detail::toJsonb(
                               query, config::detail::textDefault(
                                         query, text(hello.log_level()), "info")),
                           query.cast(query.value(true), ruvia::DbDataType::kBoolean)});
        ruvia::DbConflictOptions conflict;
        conflict.columns = {"platform_id", "imei"};
        conflict.update = {
            {"model", query.excluded("model")},
            {"software_version", query.excluded("software_version")},
            {"hostname", query.excluded("hostname")},
            {"architecture", query.excluded("architecture")},
            {"openwrt_release", query.excluded("openwrt_release")},
            {"capability", capabilityUpdate},
            {"mobile", mobileUpdate},
            {"status", statusUpdate},
            {"last_seen_at", query.call("now")},
            {"updated_at", query.call("now")},
        };
        query.onConflict(conflict)
            .returning({query.cast(query.column("id"), ruvia::DbDataType::kText),
                        query.column("enrollment_status")});
        const auto rows = co_await context.db().query(query);
        const auto key = protocol::authKey(hello.imei());
        const auto nodeId = std::string(rows.front()[0].value().value_or(std::string_view{}));
        const auto enrollmentStatus = std::string(rows.front()[1].value().value_or(std::string_view{}));
        const auto value = nodeId + "|" + enrollmentStatus;
        co_await context.redis().set(key, value);
        if (enrollmentStatus == "approved") {
            ruvia::DbQuery target;
            target
                .select({target.alias(target.column("id", "task"), "task_id"),
                         target.alias(target.column("id", "firmware"), "firmware_id")})
                .from("edge_task", "task")
                .join(ruvia::DbJoinType::kInner, "edge_firmware",
                      target.binary(
                          target.cast(target.column("id", "firmware"),
                                      ruvia::DbDataType::kText),
                          ruvia::DbBinaryOperator::kEqual,
                          config::detail::jsonText(
                              target, target.column("request", "task"), "firmware_id")),
                      "firmware")
                .where(target.binary(
                    target.column("node_id", "task"), ruvia::DbBinaryOperator::kEqual,
                    target.cast(target.value(nodeId), ruvia::DbDataType::kUuid)))
                .andWhere(target.binary(target.column("task_type", "task"),
                                        ruvia::DbBinaryOperator::kEqual,
                                        target.value(std::string_view{"firmware"})))
                .andWhere(target.binary(target.column("status", "task"),
                                        ruvia::DbBinaryOperator::kEqual,
                                        target.value(std::string_view{"running"})))
                .andWhere(target.binary(
                    config::detail::jsonText(target, target.column("result", "task"), "state"),
                    ruvia::DbBinaryOperator::kEqual,
                    target.value(std::string_view{"flashing"})))
                .orderBy(target.column("created_at", "task"), ruvia::DbOrderDirection::kDesc)
                .limit(1);

            ruvia::DbQuery completed;
            const auto rebootedResult = completed.binary(
                completed.column("result", "task"), ruvia::DbBinaryOperator::kJsonConcat,
                completed.call(
                    "jsonb_build_object",
                    {config::detail::jsonKey(completed, "state"),
                     completed.cast(completed.value(std::string_view{"rebooted"}),
                                    ruvia::DbDataType::kText),
                     config::detail::jsonKey(completed, "message"),
                     completed.cast(completed.value(std::string_view{"firmware reboot confirmed"}),
                                    ruvia::DbDataType::kText),
                     config::detail::jsonKey(completed, "softwareVersion"),
                     completed.cast(completed.value(std::string_view(hello.software_version())),
                                    ruvia::DbDataType::kText)}));
            completed.update("edge_task", "task")
                .set("status", completed.value(std::string_view{"succeeded"}))
                .set("result", rebootedResult)
                .set("updated_at", completed.call("now"))
                .set("completed_at", completed.call("now"))
                .updateFrom("target")
                .where(completed.binary(completed.column("id", "task"),
                                        ruvia::DbBinaryOperator::kEqual,
                                        completed.column("task_id", "target")))
                .returning({completed.column("firmware_id", "target")});

            ruvia::DbQuery recovery;
            recovery.with("target", target)
                .with("completed", completed)
                .update("edge_firmware", "firmware")
                .set("version", recovery.cast(
                                                recovery.value(std::string_view(
                                                    hello.software_version())),
                                                ruvia::DbDataType::kText))
                .updateFrom("completed")
                .where(recovery.binary(recovery.column("id", "firmware"),
                                       ruvia::DbBinaryOperator::kEqual,
                                       recovery.column("firmware_id", "completed")));
            (void)co_await context.db().execute(recovery);
        }
    }

    static ruvia::Task<void> saveHeartbeat(ruvia::WebWorkerContext& context,
                                           std::string_view nodeId,
                                           const pb::Heartbeat& heartbeat) {
        ruvia::DbQuery query;
        const auto text = [&query](std::string_view value) {
            return query.cast(query.value(value), ruvia::DbDataType::kText);
        };
        const auto integer = [&query](auto value) {
            return query.cast(query.value(static_cast<std::int64_t>(value)),
                              ruvia::DbDataType::kBigInt);
        };
        const auto boolean = [&query](bool value) {
            return query.cast(query.value(value), ruvia::DbDataType::kBoolean);
        };
        const auto status = query.column("status");
        const auto desiredVersion = config::detail::configVersion(
            query, status, "desiredVersion");
        const auto receivedVersion = integer(heartbeat.active_config_version());
        const auto applied = query.binary(
            query.binary(desiredVersion, ruvia::DbBinaryOperator::kEqual,
                         receivedVersion),
            ruvia::DbBinaryOperator::kAnd,
            query.binary(receivedVersion, ruvia::DbBinaryOperator::kGreater,
                         integer(0)));
        const auto config = query.call(
            "jsonb_build_object",
            {config::detail::jsonKey(query, "activeVersion"),
             query.greatest({config::detail::configVersion(query, status, "activeVersion"),
                             receivedVersion}),
             config::detail::jsonKey(query, "desiredVersion"), desiredVersion,
             config::detail::jsonKey(query, "state"),
             query.caseWhen({{applied, text("applied")}},
                            config::detail::configState(query, status)),
             config::detail::jsonKey(query, "message"),
             query.caseWhen(
                 {{applied, text(std::string_view{})}},
                 query.coalesce({config::detail::jsonText(
                                     query, config::detail::jsonGet(query, status, "config"),
                                     "message"),
                                 query.value(std::string_view{})}))});
        const auto signal = query.call(
            "jsonb_build_object", {config::detail::jsonKey(query, "csq"),
                                    integer(heartbeat.signal_csq()),
                                    config::detail::jsonKey(query, "rssiDbm"),
                                    integer(heartbeat.signal_rssi_dbm()),
                                    config::detail::jsonKey(query, "percent"),
                                    integer(heartbeat.signal_percent())});
        const auto existingMobile = query.column("mobile");
        const auto apn = query.coalesce({
            query.nullIf(text(heartbeat.apn()), query.value(std::string_view{})),
            config::detail::jsonText(query, existingMobile, "apn"),
            query.value(std::string_view{})});
        const auto operatorName = query.coalesce({
            query.nullIf(text(heartbeat.mobile_operator()), query.value(std::string_view{})),
            config::detail::jsonText(query, existingMobile, "operator"),
            query.value(std::string_view{})});
        const auto mobile = query.call(
            "jsonb_build_object",
            {config::detail::jsonKey(query, "available"), boolean(heartbeat.modem_available()),
             config::detail::jsonKey(query, "simState"), text(simState(heartbeat.sim_state())),
             config::detail::jsonKey(query, "iccid"), text(heartbeat.iccid()),
             config::detail::jsonKey(query, "signal"), signal,
             config::detail::jsonKey(query, "registered"), boolean(heartbeat.mobile_registered()),
             config::detail::jsonKey(query, "registrationStatus"),
             integer(heartbeat.mobile_registration_status()),
             config::detail::jsonKey(query, "apn"), apn,
             config::detail::jsonKey(query, "operator"), operatorName,
             config::detail::jsonKey(query, "connected"), boolean(heartbeat.mobile_connected()),
             config::detail::jsonKey(query, "ipv4"), text(heartbeat.mobile_ipv4())});
        query.update("edge_node")
            .set("status", query.call(
                               "jsonb_build_object",
                               {config::detail::jsonKey(query, "config"), config,
                                config::detail::jsonKey(query, "outbox"),
                                query.call("jsonb_build_object",
                                           {config::detail::jsonKey(query, "records"),
                                            integer(heartbeat.outbox_records()),
                                            config::detail::jsonKey(query, "bytes"),
                                            integer(heartbeat.outbox_bytes())}),
                                config::detail::jsonKey(query, "log"),
                                query.call("jsonb_build_object",
                                           {config::detail::jsonKey(query, "level"),
                                            config::detail::textDefault(
                                                query, text(heartbeat.log_level()), "info")})}))
            .set("mobile", mobile)
            .set("capability", query.call(
                                  "jsonb_set",
                                  {query.column("capability"),
                                   config::detail::jsonPath(query, "{modemControl}"),
                                   config::detail::toJsonb(
                                       query, boolean(heartbeat.supports_modem_control())),
                                   query.cast(query.value(true), ruvia::DbDataType::kBoolean)}))
            .set("last_seen_at", query.call("now"))
            .set("updated_at", query.call("now"))
            .where(query.binary(query.column("id"), ruvia::DbBinaryOperator::kEqual,
                                query.cast(query.value(nodeId), ruvia::DbDataType::kUuid)));
        (void)co_await context.db().execute(query);
        if (heartbeat.active_config_version() != 0) {
            ruvia::DbQuery revision;
            revision.update("edge_config_revision", "revision")
                .set("status", revision.value(std::string_view{"applied"}))
                .set("message", revision.value(std::string_view{}))
                .set("completed_at", revision.coalesce({
                    revision.column("completed_at", "revision"), revision.call("now")}))
                .updateFrom("edge_node", "node")
                .where(revision.binary(
                    revision.column("node_id", "revision"), ruvia::DbBinaryOperator::kEqual,
                    revision.column("id", "node")))
                .andWhere(revision.binary(
                    revision.column("id", "node"), ruvia::DbBinaryOperator::kEqual,
                    revision.cast(revision.value(nodeId), ruvia::DbDataType::kUuid)))
                .andWhere(revision.binary(
                    revision.column("revision", "revision"),
                    ruvia::DbBinaryOperator::kEqual,
                    revision.cast(revision.value(static_cast<std::int64_t>(
                                                  heartbeat.active_config_version())),
                                                ruvia::DbDataType::kBigInt)))
                .andWhere(revision.binary(
                    config::detail::configVersion(
                        revision, revision.column("status", "node"), "desiredVersion"),
                    ruvia::DbBinaryOperator::kEqual,
                    revision.cast(revision.value(static_cast<std::int64_t>(
                                                  heartbeat.active_config_version())),
                                  ruvia::DbDataType::kBigInt)));
            (void)co_await context.db().execute(revision);
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
        ruvia::DbQuery deleteInterfaces;
        deleteInterfaces.deleteFrom("edge_node_interface")
            .where(deleteInterfaces.binary(
                deleteInterfaces.column("node_id"), ruvia::DbBinaryOperator::kEqual,
                deleteInterfaces.cast(deleteInterfaces.value(nodeId), ruvia::DbDataType::kUuid)));
        (void)co_await context.db().execute(deleteInterfaces);
        for (const auto& item : report.interfaces()) {
            const auto macAddress = mac(item);
            const auto ports = jsonArray(item.bridge_ports());
            ruvia::DbQuery insert;
            insert.insertInto("edge_node_interface",
                              {"node_id", "name", "display_name", "mac", "is_up",
                               "is_bridge", "ipv4", "prefix_length", "gateway",
                               "bridge_ports"})
                .values({
                    insert.cast(insert.value(nodeId), ruvia::DbDataType::kUuid),
                    insert.value(std::string_view(item.name())),
                    insert.value(std::string_view(item.display_name())),
                    insert.nullIf(insert.value(std::string_view(macAddress)),
                                  insert.value(std::string_view{})),
                    insert.cast(insert.value(item.up()), ruvia::DbDataType::kBoolean),
                    insert.cast(insert.value(item.bridge()), ruvia::DbDataType::kBoolean),
                    insert.nullIf(insert.value(std::string_view(item.ipv4())),
                                  insert.value(std::string_view{})),
                    insert.value(item.prefix_length()),
                    insert.nullIf(insert.value(std::string_view(item.gateway())),
                                  insert.value(std::string_view{})),
                    insert.cast(insert.value(std::string_view(ports)),
                                 ruvia::DbDataType::kJsonb),
                });
            (void)co_await context.db().execute(insert);
        }
        ruvia::DbQuery deleteNetworks;
        deleteNetworks.deleteFrom("edge_node_network")
            .where(deleteNetworks.binary(
                deleteNetworks.column("node_id"), ruvia::DbBinaryOperator::kEqual,
                deleteNetworks.cast(deleteNetworks.value(nodeId), ruvia::DbDataType::kUuid)));
        (void)co_await context.db().execute(deleteNetworks);
        for (const auto& item : report.networks()) {
            const auto ports = jsonArray(item.bridge_ports());
            const auto mode = addressMode(item.mode());
            ruvia::DbQuery insert;
            insert.insertInto("edge_node_network",
                              {"node_id", "name", "address_mode", "device", "is_up",
                               "is_bridge", "ipv4", "prefix_length", "gateway",
                               "bridge_ports"})
                .values({
                    insert.cast(insert.value(nodeId), ruvia::DbDataType::kUuid),
                    insert.value(std::string_view(item.name())),
                    insert.value(std::string_view(mode)),
                    insert.value(std::string_view(item.device())),
                    insert.cast(insert.value(item.up()), ruvia::DbDataType::kBoolean),
                    insert.cast(insert.value(item.bridge()), ruvia::DbDataType::kBoolean),
                    insert.nullIf(insert.value(std::string_view(item.ipv4())),
                                  insert.value(std::string_view{})),
                    insert.value(item.prefix_length()),
                    insert.nullIf(insert.value(std::string_view(item.gateway())),
                                  insert.value(std::string_view{})),
                    insert.cast(insert.value(std::string_view(ports)),
                                 ruvia::DbDataType::kJsonb),
                });
            (void)co_await context.db().execute(insert);
        }
        ruvia::DbQuery deleteSerial;
        deleteSerial.deleteFrom("edge_node_serial")
            .where(deleteSerial.binary(
                deleteSerial.column("node_id"), ruvia::DbBinaryOperator::kEqual,
                deleteSerial.cast(deleteSerial.value(nodeId), ruvia::DbDataType::kUuid)));
        (void)co_await context.db().execute(deleteSerial);
        for (const auto& item : report.serial_ports()) {
            ruvia::DbQuery insert;
            insert.insertInto("edge_node_serial",
                              {"node_id", "path", "display_name", "available", "rs485"})
                .values({
                    insert.cast(insert.value(nodeId), ruvia::DbDataType::kUuid),
                    insert.value(std::string_view(item.path())),
                    insert.value(std::string_view(item.display_name())),
                    insert.cast(insert.value(item.available()), ruvia::DbDataType::kBoolean),
                    insert.cast(insert.value(item.rs485()), ruvia::DbDataType::kBoolean),
                });
            (void)co_await context.db().execute(insert);
        }
        ruvia::DbQuery terminal;
        terminal.update("edge_node")
            .set("capability", terminal.call(
                                   "jsonb_set",
                                   {terminal.column("capability"),
                                    config::detail::jsonPath(terminal, "{terminal}"),
                                    config::detail::toJsonb(
                                        terminal, terminal.cast(terminal.value(
                                                                           report.ttyd_available()),
                                                                       ruvia::DbDataType::kBoolean)),
                                    terminal.cast(terminal.value(true),
                                                  ruvia::DbDataType::kBoolean)}))
            .set("updated_at", terminal.call("now"))
            .where(terminal.binary(terminal.column("id"), ruvia::DbBinaryOperator::kEqual,
                                   terminal.cast(terminal.value(nodeId),
                                                 ruvia::DbDataType::kUuid)));
        (void)co_await context.db().execute(terminal);
        if (report.has_vpn()) {
            const auto vpnPublicKey = validVpnPublicKey(report.vpn().public_key())
                                          ? std::string(report.vpn().public_key())
                                          : std::string{};
            ruvia::DbQuery vpn;
            vpn.update("edge_node")
                .set("capability", vpn.call(
                                       "jsonb_set",
                                       {vpn.column("capability"),
                                        config::detail::jsonPath(vpn, "{vpn}"),
                                        vpn.call(
                                            "jsonb_build_object",
                                            {config::detail::jsonKey(vpn, "supportsVpn"),
                                             vpn.cast(vpn.value(report.vpn().supports_vpn()),
                                                      ruvia::DbDataType::kBoolean),
                                             config::detail::jsonKey(vpn, "wireguardVersion"),
                                             vpn.cast(vpn.value(std::string_view(
                                                                     report.vpn().wireguard_version())),
                                                      ruvia::DbDataType::kText),
                                             config::detail::jsonKey(vpn, "agentVersion"),
                                             vpn.cast(vpn.value(std::string_view(
                                                                     report.vpn().agent_version())),
                                                      ruvia::DbDataType::kText),
                                             config::detail::jsonKey(vpn, "publicKey"),
                                             vpn.cast(vpn.value(std::string_view(vpnPublicKey)),
                                                      ruvia::DbDataType::kText)}),
                                        vpn.cast(vpn.value(true), ruvia::DbDataType::kBoolean)}))
                .set("updated_at", vpn.call("now"))
                .where(vpn.binary(vpn.column("id"), ruvia::DbBinaryOperator::kEqual,
                                  vpn.cast(vpn.value(nodeId), ruvia::DbDataType::kUuid)));
            (void)co_await context.db().execute(vpn);
        }
        if (report.has_vpn() && report.vpn().supports_vpn()) {
            const auto& publicKey = report.vpn().public_key();
            if (validVpnPublicKey(publicKey)) {
                ruvia::DbQuery activatedQuery;
                activatedQuery.update("vpn_peer", "p")
                    .set("public_key", activatedQuery.value(std::string_view(publicKey)))
                    .set("status", activatedQuery.value(std::string_view{"active"}))
                    .set("updated_at", activatedQuery.call("now"))
                    .updateFrom("vpn_network", "n")
                    .where(activatedQuery.binary(
                        activatedQuery.column("peer_type", "p"),
                        ruvia::DbBinaryOperator::kEqual,
                        activatedQuery.value(std::string_view{"edge"})))
                    .andWhere(activatedQuery.binary(
                        activatedQuery.column("edge_node_id", "p"),
                        ruvia::DbBinaryOperator::kEqual,
                        activatedQuery.cast(activatedQuery.value(nodeId),
                                            ruvia::DbDataType::kUuid)))
                    .andWhere(activatedQuery.binary(
                        activatedQuery.column("status", "p"),
                        ruvia::DbBinaryOperator::kNotEqual,
                        activatedQuery.value(std::string_view{"revoked"})))
                    .andWhere(activatedQuery.binary(
                        activatedQuery.column("id", "n"),
                        ruvia::DbBinaryOperator::kEqual,
                        activatedQuery.column("network_id", "p")))
                    .returning({activatedQuery.cast(activatedQuery.column("id", "p"),
                                                    ruvia::DbDataType::kText),
                                activatedQuery.cast(activatedQuery.column("network_id", "p"),
                                                    ruvia::DbDataType::kText),
                                activatedQuery.cast(activatedQuery.column("created_by", "n"),
                                                    ruvia::DbDataType::kText)});
                const auto activated = co_await context.db().query(activatedQuery);
                for (const auto& row : activated) {
                    auto transaction = co_await context.db().beginTransaction();
                    ruvia::DbQuery lock;
                    lock.select(lock.call(
                        "pg_advisory_xact_lock",
                        {lock.cast(lock.value(std::int64_t{5282804697543808068LL}),
                                   ruvia::DbDataType::kBigInt)}));
                    (void)co_await transaction.query(lock);
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
        ruvia::DbQuery query;
        query.update("edge_task")
            .set("status", query.value(std::string_view(status)))
            .set("result", query.cast(query.value(std::string_view(json)),
                                       ruvia::DbDataType::kJsonb))
            .set("updated_at", query.call("now"))
            .set("completed_at", query.call("now"))
            .where(query.binary(query.column("id"), ruvia::DbBinaryOperator::kEqual,
                                query.cast(query.value(id), ruvia::DbDataType::kUuid)))
            .andWhere(query.binary(query.column("node_id"), ruvia::DbBinaryOperator::kEqual,
                                   query.cast(query.value(nodeId), ruvia::DbDataType::kUuid)))
            .andWhere(query.binary(query.column("task_type"), ruvia::DbBinaryOperator::kEqual,
                                   query.value(std::string_view{"network"})))
            .andWhere(query.binary(
                query.column("status"), ruvia::DbBinaryOperator::kNotIn,
                query.list({query.value(std::string_view{"succeeded"}),
                            query.value(std::string_view{"failed"})})));
        (void)co_await context.db().execute(query);
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
        ruvia::DbQuery transitioned;
        transitioned.update("edge_task")
            .set("status", transitioned.value(std::string_view(status)))
            .set("result", transitioned.cast(
                               transitioned.value(std::string_view(json)),
                               ruvia::DbDataType::kJsonb))
            .set("updated_at", transitioned.call("now"))
            .set("completed_at", transitioned.call("now"))
            .where(transitioned.binary(
                transitioned.column("id"), ruvia::DbBinaryOperator::kEqual,
                transitioned.cast(transitioned.value(id), ruvia::DbDataType::kUuid)))
            .andWhere(transitioned.binary(
                transitioned.column("node_id"), ruvia::DbBinaryOperator::kEqual,
                transitioned.cast(transitioned.value(nodeId), ruvia::DbDataType::kUuid)))
            .andWhere(transitioned.binary(
                transitioned.column("task_type"), ruvia::DbBinaryOperator::kEqual,
                transitioned.value(std::string_view{"vpn"})))
            .andWhere(transitioned.binary(
                transitioned.column("status"), ruvia::DbBinaryOperator::kNotIn,
                transitioned.list({transitioned.value(std::string_view{"succeeded"}),
                                   transitioned.value(std::string_view{"failed"})})))
            .returning({transitioned.alias(
                            config::detail::jsonText(
                                transitioned, transitioned.column("request"), "peerId"),
                            "peer_id"),
                        transitioned.alias(
                            config::detail::jsonText(
                                transitioned, transitioned.column("request"), "enabled"),
                            "enabled"),
                        transitioned.alias(
                            config::detail::jsonText(transitioned,
                                                     transitioned.column("request"),
                                                     "configVersion"),
                            "config_version")});

        ruvia::DbQuery update;
        const auto appliedValue = update.cast(update.value(applied), ruvia::DbDataType::kBoolean);
        const auto enabled = update.coalesce({
            update.cast(update.column("enabled", "task"), ruvia::DbDataType::kBoolean),
            update.cast(update.value(true), ruvia::DbDataType::kBoolean)});
        const auto activeRoute = update.binary(
            enabled, ruvia::DbBinaryOperator::kAnd,
            update.column("enabled", "route"));
        const auto routeStatus = update.caseWhen(
            {{activeRoute,
              update.cast(update.value(std::string_view{"active"}),
                          ruvia::DbDataType::kText)}},
            update.cast(update.value(std::string_view{"disabled"}),
                        ruvia::DbDataType::kText));
        const auto statusValue = update.caseWhen(
            {{appliedValue, routeStatus}},
            update.cast(update.value(std::string_view{"error"}),
                        ruvia::DbDataType::kText));
        const auto lastError = update.caseWhen(
            {{appliedValue, update.cast(update.value(std::string_view{}),
                                        ruvia::DbDataType::kText)}},
            update.cast(update.value(std::string_view(result.error_message())),
                        ruvia::DbDataType::kText));

        ruvia::DbQuery peer;
        const auto taskPeerId = peer.importExpression(
            update.column("peer_id", "task"), "task", "task");
        peer.select({peer.cast(peer.column("config_revision", "peer"),
                               ruvia::DbDataType::kText)})
            .from("vpn_peer", "peer")
            .where(peer.binary(peer.column("id", "peer"),
                               ruvia::DbBinaryOperator::kEqual,
                               peer.cast(taskPeerId, ruvia::DbDataType::kUuid)));
        update.with("transitioned", transitioned)
            .update("vpn_route", "route")
            .set("status", statusValue)
            .set("last_error", lastError)
            .set("updated_at", update.call("now"))
            .updateFrom("transitioned", "task")
            .where(update.binary(
                update.column("edge_peer_id", "route"), ruvia::DbBinaryOperator::kEqual,
                update.cast(update.column("peer_id", "task"), ruvia::DbDataType::kUuid)))
            .andWhere(update.binary(update.subquery(peer),
                                    ruvia::DbBinaryOperator::kEqual,
                                    update.column("config_version", "task")));
        (void)co_await context.db().execute(update);
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
        ruvia::DbQuery query;
        query.update("edge_task")
            .set("status", query.value(std::string_view(status)))
            .set("result", query.cast(query.value(std::string_view(json)),
                                       ruvia::DbDataType::kJsonb))
            .set("updated_at", query.call("now"))
            .set("completed_at", query.caseWhen(
                                     {{query.cast(query.value(completed),
                                                  ruvia::DbDataType::kBoolean),
                                       query.call("now")} },
                                     query.nullValue()))
            .where(query.binary(query.column("id"), ruvia::DbBinaryOperator::kEqual,
                                query.cast(query.value(id), ruvia::DbDataType::kUuid)))
            .andWhere(query.binary(query.column("node_id"), ruvia::DbBinaryOperator::kEqual,
                                   query.cast(query.value(nodeId), ruvia::DbDataType::kUuid)))
            .andWhere(query.binary(query.column("task_type"), ruvia::DbBinaryOperator::kEqual,
                                   query.value(std::string_view{"firmware"})))
            .andWhere(query.binary(
                query.column("status"), ruvia::DbBinaryOperator::kNotIn,
                query.list({query.value(std::string_view{"succeeded"}),
                            query.value(std::string_view{"failed"})})));
        (void)co_await context.db().execute(query);
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
        ruvia::DbQuery query;
        query.update("edge_task")
            .set("status", query.value(std::string_view(status)))
            .set("result", query.cast(query.value(std::string_view(json)),
                                       ruvia::DbDataType::kJsonb))
            .set("updated_at", query.call("now"))
            .set("completed_at", query.caseWhen(
                                     {{query.cast(query.value(completed),
                                                  ruvia::DbDataType::kBoolean),
                                       query.call("now")} },
                                     query.nullValue()))
            .where(query.binary(query.column("id"), ruvia::DbBinaryOperator::kEqual,
                                query.cast(query.value(id), ruvia::DbDataType::kUuid)))
            .andWhere(query.binary(query.column("node_id"), ruvia::DbBinaryOperator::kEqual,
                                   query.cast(query.value(nodeId), ruvia::DbDataType::kUuid)))
            .andWhere(query.binary(query.column("task_type"), ruvia::DbBinaryOperator::kEqual,
                                   query.value(std::string_view{"modem"})))
            .andWhere(query.binary(
                query.column("status"), ruvia::DbBinaryOperator::kNotIn,
                query.list({query.value(std::string_view{"succeeded"}),
                            query.value(std::string_view{"failed"})})));
        (void)co_await context.db().execute(query);
    }

    static ruvia::Task<void> savePlatformResult(
        ruvia::WebWorkerContext& context, std::string_view nodeId,
        const pb::PlatformConfigResult& result) {
        if (result.request_id().size() != 16)
            co_return;
        const auto id = protocol::uuidText(result.request_id());
        const std::string status = result.success() ? "succeeded" : "failed";
        const std::string json = "{\"message\":\"" + jsonEscape(result.message()) + "\"}";
        ruvia::DbQuery transitioned;
        transitioned.update("edge_task")
            .set("status", transitioned.value(std::string_view(status)))
            .set("result", transitioned.cast(
                               transitioned.value(std::string_view(json)),
                               ruvia::DbDataType::kJsonb))
            .set("updated_at", transitioned.call("now"))
            .set("completed_at", transitioned.call("now"))
            .where(transitioned.binary(
                transitioned.column("id"), ruvia::DbBinaryOperator::kEqual,
                transitioned.cast(transitioned.value(id), ruvia::DbDataType::kUuid)))
            .andWhere(transitioned.binary(
                transitioned.column("node_id"), ruvia::DbBinaryOperator::kEqual,
                transitioned.cast(transitioned.value(nodeId), ruvia::DbDataType::kUuid)))
            .andWhere(transitioned.binary(
                transitioned.column("task_type"), ruvia::DbBinaryOperator::kIn,
                transitioned.list({transitioned.value(std::string_view{"platform_upsert"}),
                                   transitioned.value(std::string_view{"platform_delete"})})))
            .andWhere(transitioned.binary(
                transitioned.column("status"), ruvia::DbBinaryOperator::kNotIn,
                transitioned.list({transitioned.value(std::string_view{"succeeded"}),
                                   transitioned.value(std::string_view{"failed"})})))
            .returning({transitioned.alias(
                            transitioned.cast(
                                config::detail::jsonText(
                                    transitioned, transitioned.column("request"), "platform_id"),
                                ruvia::DbDataType::kUuid),
                            "platform_id"),
                        transitioned.column("task_type")});

        ruvia::DbQuery updated;
        const auto success = updated.cast(updated.value(result.success()),
                                          ruvia::DbDataType::kBoolean);
        const auto deleteTask = updated.binary(
            updated.column("task_type", "task"), ruvia::DbBinaryOperator::kEqual,
            updated.value(std::string_view{"platform_delete"}));
        updated.update("edge_node_platform", "target")
            .set("status", updated.call(
                               "jsonb_build_object",
                               {config::detail::jsonKey(updated, "state"),
                                updated.cast(updated.value(std::string_view(
                                                               result.success() ? "applied"
                                                                                : "failed")),
                                             ruvia::DbDataType::kText),
                                config::detail::jsonKey(updated, "message"),
                                updated.cast(updated.value(std::string_view(result.message())),
                                             ruvia::DbDataType::kText)}))
            .set("updated_at", updated.call("now"))
            .updateFrom("transitioned", "task")
            .where(updated.binary(
                updated.column("node_id", "target"), ruvia::DbBinaryOperator::kEqual,
                updated.cast(updated.value(nodeId), ruvia::DbDataType::kUuid)))
            .andWhere(updated.binary(updated.column("platform_id", "target"),
                                     ruvia::DbBinaryOperator::kEqual,
                                     updated.column("platform_id", "task")))
            .andWhere(updated.unary(
                ruvia::DbUnaryOperator::kNot,
                updated.binary(success, ruvia::DbBinaryOperator::kAnd, deleteTask)))
            .returning({updated.column("platform_id", "target")});

        ruvia::DbQuery cleanup;
        const auto cleanupSuccess = cleanup.importExpression(success);
        const auto cleanupDeleteTask = cleanup.importExpression(deleteTask);
        cleanup.with("transitioned", transitioned)
            .with("updated", updated)
            .deleteFrom("edge_node_platform", "target")
            .deleteUsing("transitioned", "task")
            .where(cleanup.binary(cleanupSuccess, ruvia::DbBinaryOperator::kAnd,
                                  cleanupDeleteTask))
            .andWhere(cleanup.binary(
                cleanup.column("node_id", "target"), ruvia::DbBinaryOperator::kEqual,
                cleanup.cast(cleanup.value(nodeId), ruvia::DbDataType::kUuid)))
            .andWhere(cleanup.binary(cleanup.column("platform_id", "target"),
                                     ruvia::DbBinaryOperator::kEqual,
                                     cleanup.column("platform_id", "task")));
        (void)co_await context.db().execute(cleanup);
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
        ruvia::DbQuery revision;
        revision.update("edge_config_revision")
            .set("status", revision.value(std::string_view{"applied"}))
            .set("message", revision.value(std::string_view{}))
            .set("completed_at", revision.call("now"))
            .where(revision.binary(
                revision.column("node_id"), ruvia::DbBinaryOperator::kEqual,
                revision.cast(revision.value(nodeId), ruvia::DbDataType::kUuid)))
            .andWhere(revision.binary(
                revision.column("revision"), ruvia::DbBinaryOperator::kEqual,
                revision.cast(revision.value(static_cast<std::int64_t>(result.revision())),
                              ruvia::DbDataType::kBigInt)))
            .andWhere(revision.binary(revision.column("sha256"),
                                      ruvia::DbBinaryOperator::kEqual,
                                      revision.value(std::string_view(digest))));
        (void)co_await context.db().execute(revision);

        ruvia::DbQuery node;
        const auto status = node.column("status");
        const auto revisionValue = node.cast(
            node.value(static_cast<std::int64_t>(result.revision())),
            ruvia::DbDataType::kBigInt);
        const auto desired = config::detail::configVersion(node, status, "desiredVersion");
        const auto matches = node.binary(desired, ruvia::DbBinaryOperator::kEqual,
                                         revisionValue);
        const auto configWithActive = node.call(
            "jsonb_set",
            {status, config::detail::jsonPath(node, "{config,activeVersion}"),
             config::detail::toJsonb(
                 node, node.greatest({config::detail::configVersion(
                                           node, status, "activeVersion"),
                                      revisionValue})),
             node.cast(node.value(true), ruvia::DbDataType::kBoolean)});
        const auto configWithState = node.call(
            "jsonb_set",
            {configWithActive, config::detail::jsonPath(node, "{config,state}"),
             config::detail::toJsonb(
                 node, node.caseWhen(
                           {{matches, node.cast(node.value(std::string_view{"applied"}),
                                                ruvia::DbDataType::kText)}},
                           config::detail::configState(node, status))),
             node.cast(node.value(true), ruvia::DbDataType::kBoolean)});
        const auto configWithMessage = node.call(
            "jsonb_set",
            {configWithState, config::detail::jsonPath(node, "{config,message}"),
             config::detail::toJsonb(
                 node, node.caseWhen(
                           {{matches, node.cast(node.value(std::string_view{}),
                                                ruvia::DbDataType::kText)}},
                           node.coalesce({config::detail::jsonText(
                                              node,
                                              config::detail::jsonGet(node, status, "config"),
                                              "message"),
                                          node.value(std::string_view{})}))),
             node.cast(node.value(true), ruvia::DbDataType::kBoolean)});
        node.update("edge_node")
            .set("status", configWithMessage)
            .set("updated_at", node.call("now"))
            .where(node.binary(node.column("id"), ruvia::DbBinaryOperator::kEqual,
                               node.cast(node.value(nodeId), ruvia::DbDataType::kUuid)));
        (void)co_await context.db().execute(node);
    }

    static ruvia::Task<void> saveConfigRejected(ruvia::WebWorkerContext& context,
                                                 std::string_view nodeId,
                                                 const pb::ConfigRejected& result) {
        if (result.revision() == 0)
            co_return;
        const std::string message = result.code() + ": " + result.message();
        ruvia::DbQuery revision;
        revision.update("edge_config_revision")
            .set("status", revision.value(std::string_view{"rejected"}))
            .set("message", revision.value(std::string_view(message)))
            .set("completed_at", revision.call("now"))
            .where(revision.binary(
                revision.column("node_id"), ruvia::DbBinaryOperator::kEqual,
                revision.cast(revision.value(nodeId), ruvia::DbDataType::kUuid)))
            .andWhere(revision.binary(
                revision.column("revision"), ruvia::DbBinaryOperator::kEqual,
                revision.cast(revision.value(static_cast<std::int64_t>(result.revision())),
                              ruvia::DbDataType::kBigInt)));
        (void)co_await context.db().execute(revision);

        ruvia::DbQuery node;
        const auto status = node.column("status");
        const auto revisionValue = node.cast(
            node.value(static_cast<std::int64_t>(result.revision())),
            ruvia::DbDataType::kBigInt);
        const auto matches = node.binary(
            config::detail::configVersion(node, status, "desiredVersion"),
            ruvia::DbBinaryOperator::kEqual, revisionValue);
        const auto state = node.caseWhen(
            {{matches, node.cast(node.value(std::string_view{"rejected"}),
                                 ruvia::DbDataType::kText)}},
            config::detail::configState(node, status));
        const auto messageValue = node.caseWhen(
            {{matches, node.cast(node.value(std::string_view(message)),
                                 ruvia::DbDataType::kText)}},
            node.coalesce({config::detail::jsonText(
                               node, config::detail::jsonGet(node, status, "config"),
                               "message"),
                           node.value(std::string_view{})}));
        const auto statusWithState = node.call(
            "jsonb_set", {status, config::detail::jsonPath(node, "{config,state}"),
                           config::detail::toJsonb(node, state),
                           node.cast(node.value(true), ruvia::DbDataType::kBoolean)});
        node.update("edge_node")
            .set("status", node.call(
                               "jsonb_set",
                               {statusWithState,
                                config::detail::jsonPath(node, "{config,message}"),
                                config::detail::toJsonb(node, messageValue),
                                node.cast(node.value(true),
                                          ruvia::DbDataType::kBoolean)}))
            .set("updated_at", node.call("now"))
            .where(node.binary(node.column("id"), ruvia::DbBinaryOperator::kEqual,
                               node.cast(node.value(nodeId), ruvia::DbDataType::kUuid)));
        (void)co_await context.db().execute(node);
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
