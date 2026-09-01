#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/evp.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/features/edge/dispatch.h"
#include "service/features/edge/metadata.h"
#include "service/features/edge/protocol.h"
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
       CASE WHEN p.protocol = 'S7'
            THEN COALESCE(NULLIF(p.config->>'pollInterval', ''),
                          NULLIF(p.config->>'readInterval', ''), '1')
            ELSE COALESCE(NULLIF(p.config->>'readInterval', ''),
                          NULLIF(p.config->>'pollInterval', ''), '1')
       END,
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
       d.link_id::text
FROM device d
JOIN link l ON l.id = d.link_id AND l.execution = 'edge' AND l.deleted_at IS NULL
JOIN protocol_config p ON p.id = d.protocol_config_id AND p.deleted_at IS NULL
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
JOIN protocol_config p ON p.id = d.protocol_config_id AND p.protocol = 'Modbus'
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
JOIN protocol_config p ON p.id = d.protocol_config_id AND p.protocol = 'S7'
CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'areas', '[]')) item
WHERE l.edge_node_id = $1::uuid AND d.deleted_at IS NULL
ORDER BY d.id, item->>'id')sql";

inline constexpr std::string_view kAppendSl651FunctionsSql = R"sql(
SELECT d.id::text, func->>'funcCode', func->>'name', func->>'dir'
FROM device d
JOIN link l ON l.id = d.link_id AND l.execution = 'edge' AND l.deleted_at IS NULL
JOIN protocol_config p ON p.id = d.protocol_config_id AND p.protocol = 'SL651'
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
JOIN protocol_config p ON p.id = d.protocol_config_id AND p.protocol = 'SL651'
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

    ruvia::Task<std::uint64_t> queueSnapshot(ruvia::Context& c, std::string_view nodeId) {
        const auto version = co_await c.db().query(config::detail::kQueueSnapshotSql,
                                                   service::common::dbParams(nodeId));
        if (version.empty())
            service::common::fail(17011, "边缘节点未批准或不支持设备配置", 409);
        const auto revision = unsignedInteger(version.front()[0].value().value_or(std::string_view{}));

        auto snapshot = co_await buildSnapshot(c, nodeId, revision);
        if (!snapshot) {
            service::common::fail(17012, "边缘节点配置条目超过 512 条", 409);
        }

        const auto principal = service::middleware::requireAuth(c);
        (void)co_await c.db().execute(R"sql(
INSERT INTO edge_config_revision(node_id, revision, sha256, item_count, created_by)
VALUES ($1::uuid, $2, $3, $4, $5::uuid))sql",
                                      service::common::dbParams(
                                          nodeId, static_cast<std::int64_t>(revision),
                                          snapshot->digest,
                                          static_cast<std::int64_t>(snapshot->itemCount),
                                          principal.userId));
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

    static ruvia::Task<std::optional<Snapshot>> buildSnapshot(ruvia::Context& c,
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

    static ruvia::Task<void> replaceQueue(ruvia::Context& c, std::string_view nodeId,
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

    static ruvia::Task<void> rejectBuild(ruvia::Context& c, std::string_view nodeId,
                                         std::uint64_t revision, std::string_view message) {
        (void)co_await c.db().execute(config::detail::kRejectBuildSql,
                                      service::common::dbParams(
                                          message, nodeId, static_cast<std::int64_t>(revision)));
    }

    static ruvia::Task<std::vector<pb::ConfigItem>>
    buildItems(ruvia::Context& c, std::string_view nodeId) {
        std::vector<pb::ConfigItem> items;
        const auto devices = co_await c.db().query(config::detail::kBuildItemsSql,
                                                     service::common::dbParams(nodeId));
        for (const auto& row : devices) {
            const auto protocol = protocolValue(row[3].value().value_or(std::string_view{}));
            pb::ConfigItem endpoint;
            endpoint.set_kind(pb::CONFIG_ITEM_ENDPOINT);
            auto* endpointValue = endpoint.mutable_endpoint();
            if (!setUuid(endpointValue->mutable_endpoint_id(), row[30].value().value_or(std::string_view{})))
                throw std::runtime_error("invalid edge link UUID");
            endpointValue->set_name(row[1].value().value_or(std::string_view{}));
            endpointValue->set_interface_name(row[10].value().value_or(std::string_view{}));
            endpointValue->set_protocol(protocol);
            endpointValue->set_enabled(row[29].value().value_or(std::string_view{}) == "t");
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
            // read interval controls edge-to-platform reporting; storageInterval remains
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
            deviceValue->set_command_fast_read_duration_sec(10);
            deviceValue->set_command_fast_read_interval_sec(1);
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

    static ruvia::Task<void> appendModbus(ruvia::Context& c, std::string_view nodeId,
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

    static ruvia::Task<void> appendS7(ruvia::Context& c, std::string_view nodeId,
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

    static ruvia::Task<void> appendSl651(ruvia::Context& c, std::string_view nodeId,
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
