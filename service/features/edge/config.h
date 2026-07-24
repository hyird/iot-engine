#pragma once

#include <algorithm>
#include <array>
#include <charconv>
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
#include "service/features/edge/protocol.h"
#include "service/middleware/auth.h"

namespace service::edge {

class ConfigService final {
  public:
    static ConfigService& instance() {
        static ConfigService value;
        return value;
    }

    ruvia::Task<std::uint64_t> queueSnapshot(ruvia::Context& c, std::string_view nodeId) {
        const auto version = co_await c.db().query(R"sql(
WITH next AS (
    SELECT id,
           GREATEST(
               (EXTRACT(EPOCH FROM clock_timestamp()) * 1000)::bigint,
               COALESCE((status->'config'->>'desiredVersion')::bigint, 0) + 1,
               COALESCE((status->'config'->>'activeVersion')::bigint, 0) + 1) AS revision
    FROM edge_node
    WHERE id = $1::uuid AND enrollment_status = 'approved'
      AND COALESCE((capability->>'deviceConfig')::boolean, false)
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
RETURNING next.revision)sql",
                                                   service::common::dbParams(nodeId));
        if (version.rows().empty())
            service::common::fail(17011, "边缘节点未批准或不支持设备配置", 409);
        const auto revision = unsignedInteger(version.rows().front()[0].text());

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
        co_return revision;
    }

    ruvia::Task<bool> requeueIfStale(ruvia::Context& c, std::string_view nodeId,
                                     std::uint64_t activeRevision) {
        const auto desired = co_await c.db().query(R"sql(
SELECT COALESCE((status->'config'->>'desiredVersion')::bigint, 0),
       COALESCE(status->'config'->>'state', 'idle')
FROM edge_node
WHERE id = $1::uuid AND enrollment_status = 'approved'
  AND COALESCE((capability->>'deviceConfig')::boolean, false))sql",
                                                   service::common::dbParams(nodeId));
        if (desired.rows().empty())
            co_return false;
        const auto revision = unsignedInteger(desired.rows().front()[0].text());
        if (revision == 0 || revision == activeRevision ||
            desired.rows().front()[1].text() == "rejected")
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
        (void)co_await c.db().execute(R"sql(
UPDATE edge_node
SET status = jsonb_set(
        jsonb_set(status, '{config,state}', to_jsonb('pending'::text), true),
        '{config,message}', to_jsonb(''::text), true),
    updated_at = NOW()
WHERE id = $1::uuid
  AND COALESCE((status->'config'->>'desiredVersion')::bigint, 0) = $2
  AND COALESCE(status->'config'->>'state', 'idle') <> 'rejected')sql",
                                      service::common::dbParams(
                                          nodeId, static_cast<std::int64_t>(revision)));
        co_await replaceQueue(c, nodeId, revision, snapshot->wires);
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

    static double number(std::string_view value, double fallback = 0.0) {
        if (value.empty())
            return fallback;
        try {
            return std::stod(std::string(value));
        } catch (...) {
            return fallback;
        }
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

    static std::vector<std::uint8_t> bytes(std::string_view mode, std::string_view content) {
        std::vector<std::uint8_t> output;
        if (mode == "OFF" || content.empty())
            return output;
        if (mode == "ASCII") {
            output.assign(content.begin(), content.end());
            return output;
        }
        int high = -1;
        for (const char character : content) {
            if (character == ' ' || character == '\t' || character == '\r' || character == '\n')
                continue;
            const int digit = protocol::hexDigit(character);
            if (digit < 0)
                return {};
            if (high < 0)
                high = digit;
            else {
                output.push_back(static_cast<std::uint8_t>((high << 4U) | digit));
                high = -1;
            }
        }
        return high < 0 ? output : std::vector<std::uint8_t>{};
    }

    static void packet(std::string* output, std::string_view mode, std::string_view content) {
        const auto value = bytes(mode, content);
        output->assign(protocol::bytes(value.data(), value.size()));
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
        static constexpr std::string_view script = R"lua(
local incoming = tonumber(ARGV[1])
local current = tonumber(redis.call('GET', KEYS[2]) or '0')
if current > incoming then return 0 end
redis.call('DEL', KEYS[1])
for index = 2, #ARGV do redis.call('RPUSH', KEYS[1], ARGV[index]) end
redis.call('EXPIRE', KEYS[1], 604800)
redis.call('SETEX', KEYS[2], 604800, ARGV[1])
return #ARGV - 1
)lua";
        const std::string key = "iot:edge:config:" + std::string(nodeId);
        const std::string revisionKey = "iot:edge:config-revision:" + std::string(nodeId);
        const std::array<std::string_view, 2> keys{key, revisionKey};
        const auto revisionText = std::to_string(revision);
        std::vector<std::string_view> values;
        values.reserve(wires.size() + 1);
        values.push_back(revisionText);
        for (const auto& wire : wires)
            values.push_back(wire);
        (void)co_await c.redis().eval(script, std::span<const std::string_view>(keys),
                                     std::span<const std::string_view>(values));
    }

    static ruvia::Task<void> rejectBuild(ruvia::Context& c, std::string_view nodeId,
                                         std::uint64_t revision, std::string_view message) {
        (void)co_await c.db().execute(R"sql(
UPDATE edge_node
SET status = jsonb_set(
        jsonb_set(status, '{config,state}', to_jsonb('rejected'::text), true),
        '{config,message}', to_jsonb($1::text), true),
    updated_at = NOW()
WHERE id = $2::uuid
  AND COALESCE((status->'config'->>'desiredVersion')::bigint, 0) = $3)sql",
                                      service::common::dbParams(
                                          message, nodeId, static_cast<std::int64_t>(revision)));
    }

    static ruvia::Task<std::vector<pb::ConfigItem>>
    buildItems(ruvia::Context& c, std::string_view nodeId) {
        std::vector<pb::ConfigItem> items;
        const auto devices = co_await c.db().query(R"sql(
SELECT d.id::text, d.name, d.protocol_params->>'device_code', p.protocol,
       COALESCE(NULLIF(d.protocol_params->>'timezone', ''), '+08:00'),
       GREATEST(1, CEIL(COALESCE((p.config->>'storageInterval')::numeric, 1)))::bigint,
       COALESCE((d.protocol_params->>'online_timeout')::integer, 300),
       COALESCE((d.protocol_params->>'slave_id')::integer, 1),
       COALESCE(d.protocol_params->>'modbus_mode', 'TCP'),
       d.edge_endpoint->>'transport', d.edge_endpoint->>'interface',
       COALESCE(d.edge_endpoint->>'mode', ''), COALESCE(d.edge_endpoint->>'ip', ''),
       COALESCE((d.edge_endpoint->>'port')::integer, 0),
       COALESCE((d.edge_endpoint->>'baud_rate')::integer, 9600),
       COALESCE((d.edge_endpoint->>'data_bits')::integer, 8),
       COALESCE((d.edge_endpoint->>'stop_bits')::integer, 1),
       COALESCE(d.edge_endpoint->>'parity', 'none'),
       COALESCE((d.edge_endpoint->>'rs485')::boolean, false),
       COALESCE((p.config->'packet'->>'mergeGap')::integer, 0),
       COALESCE((p.config->'packet'->>'maxQuantity')::integer, 125),
       COALESCE(p.config->'connection'->>'mode', 'RACK_SLOT'),
       COALESCE(p.config->'connection'->>'connectionType', 'PG'),
       COALESCE((p.config->'connection'->>'rack')::integer, 0),
       COALESCE((p.config->'connection'->>'slot')::integer, 1),
       COALESCE(p.config->'connection'->>'localTSAP', ''),
       COALESCE(p.config->'connection'->>'remoteTSAP', ''),
       COALESCE(d.protocol_params->'heartbeat'->>'mode', 'OFF'),
       COALESCE(d.protocol_params->'heartbeat'->>'content', ''),
       COALESCE(d.protocol_params->'registration'->>'mode', 'OFF'),
       COALESCE(d.protocol_params->'registration'->>'content', ''),
       d.status = 'enabled' AND p.enabled
FROM device d
JOIN protocol_config p ON p.id = d.protocol_config_id AND p.deleted_at IS NULL
WHERE d.edge_node_id = $1::uuid AND d.deleted_at IS NULL
ORDER BY d.id)sql",
                                                     service::common::dbParams(nodeId));
        for (const auto& row : devices.rows()) {
            const auto protocol = protocolValue(row[3].text());
            pb::ConfigItem endpoint;
            endpoint.set_kind(pb::CONFIG_ITEM_ENDPOINT);
            auto* endpointValue = endpoint.mutable_endpoint();
            if (!setUuid(endpointValue->mutable_endpoint_id(), row[0].text()))
                throw std::runtime_error("invalid edge device UUID");
            endpointValue->set_name(row[1].text());
            endpointValue->set_interface_name(row[10].text());
            endpointValue->set_protocol(protocol);
            endpointValue->set_enabled(row[31].text() == "t");
            if (row[9].text() == "serial") {
                endpointValue->set_transport(pb::TRANSPORT_SERIAL);
                endpointValue->set_mode(pb::LINK_MODE_SERIAL);
                auto* serial = endpointValue->mutable_serial();
                serial->set_channel(row[10].text());
                serial->set_baud_rate(
                    static_cast<std::uint32_t>(integer(row[14].text(), 9600)));
                serial->set_data_bits(
                    static_cast<std::uint32_t>(integer(row[15].text(), 8)));
                serial->set_stop_bits(
                    static_cast<std::uint32_t>(integer(row[16].text(), 1)));
                serial->set_parity(row[17].text());
                serial->set_rs485(row[18].text() == "t");
            } else {
                endpointValue->set_transport(pb::TRANSPORT_ETHERNET);
                endpointValue->set_mode(row[11].text() == "TCP Server"
                                            ? pb::LINK_MODE_TCP_SERVER
                                            : pb::LINK_MODE_TCP_CLIENT);
                endpointValue->set_ip(row[12].text());
                endpointValue->set_port(
                    static_cast<std::uint32_t>(integer(row[13].text())));
            }
            items.push_back(std::move(endpoint));

            pb::ConfigItem device;
            device.set_kind(pb::CONFIG_ITEM_DEVICE);
            auto* deviceValue = device.mutable_device();
            setUuid(deviceValue->mutable_device_id(), row[0].text());
            setUuid(deviceValue->mutable_endpoint_id(), row[0].text());
            deviceValue->set_device_code(row[2].text());
            deviceValue->set_name(row[1].text());
            deviceValue->set_protocol(protocol);
            deviceValue->set_timezone(row[4].text());
            deviceValue->set_io_interval_ms(1000);
            deviceValue->set_report_interval_sec(
                static_cast<std::uint32_t>(integer(row[5].text(), 1)));
            deviceValue->set_online_timeout_sec(
                static_cast<std::uint32_t>(integer(row[6].text(), 300)));
            deviceValue->set_modbus_slave_id(
                static_cast<std::uint32_t>(integer(row[7].text(), 1)));
            deviceValue->set_modbus_mode(row[8].text());
            deviceValue->set_modbus_merge_gap(
                static_cast<std::uint32_t>(integer(row[19].text())));
            deviceValue->set_modbus_max_quantity(
                static_cast<std::uint32_t>(integer(row[20].text(), 125)));
            deviceValue->set_s7_connection_mode(row[21].text());
            deviceValue->set_s7_connection_type(row[22].text());
            deviceValue->set_s7_rack(
                static_cast<std::uint32_t>(integer(row[23].text())));
            deviceValue->set_s7_slot(
                static_cast<std::uint32_t>(integer(row[24].text(), 1)));
            deviceValue->set_s7_local_tsap(row[25].text());
            deviceValue->set_s7_remote_tsap(row[26].text());
            deviceValue->set_command_fast_read_duration_sec(10);
            deviceValue->set_command_fast_read_interval_sec(1);
            packet(deviceValue->mutable_heartbeat_payload(), row[27].text(), row[28].text());
            packet(deviceValue->mutable_registration_payload(), row[29].text(),
                   row[30].text());
            deviceValue->set_enabled(row[31].text() == "t");
            items.push_back(std::move(device));
        }

        co_await appendModbus(c, nodeId, items);
        co_await appendS7(c, nodeId, items);
        co_await appendSl651(c, nodeId, items);
        co_return items;
    }

    static ruvia::Task<void> appendModbus(ruvia::Context& c, std::string_view nodeId,
                                          std::vector<pb::ConfigItem>& items) {
        const auto rows = co_await c.db().query(R"sql(
SELECT d.id::text, item->>'id', item->>'name', COALESCE(item->>'unit', ''),
       item->>'registerType', item->>'dataType',
       COALESCE(item->>'byteOrder', p.config->>'byteOrder', 'BIG_ENDIAN'),
       (item->>'address')::integer, (item->>'quantity')::integer,
       COALESCE((item->>'scale')::numeric, 1)::text,
       COALESCE((item->>'decimals')::integer, -1),
       COALESCE((item->>'writable')::boolean, false)
FROM device d
JOIN protocol_config p ON p.id = d.protocol_config_id AND p.protocol = 'Modbus'
CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'registers', '[]')) item
WHERE d.edge_node_id = $1::uuid AND d.deleted_at IS NULL
ORDER BY d.id, item->>'id')sql",
                                                 service::common::dbParams(nodeId));
        for (const auto& row : rows.rows()) {
            pb::ConfigItem item;
            item.set_kind(pb::CONFIG_ITEM_MODBUS_REGISTER);
            auto* value = item.mutable_modbus_register();
            setUuid(value->mutable_device_id(), row[0].text());
            value->set_element_id(row[1].text());
            value->set_name(row[2].text());
            value->set_unit(row[3].text());
            value->set_register_type(row[4].text());
            value->set_data_type(row[5].text());
            value->set_byte_order(row[6].text());
            value->set_address(static_cast<std::uint32_t>(integer(row[7].text())));
            value->set_quantity(
                static_cast<std::uint32_t>(integer(row[8].text(), 1)));
            value->set_scale(number(row[9].text(), 1.0));
            value->set_decimals(
                static_cast<std::int32_t>(integer(row[10].text(), -1)));
            value->set_writable(row[11].text() == "t");
            items.push_back(std::move(item));
        }
    }

    static ruvia::Task<void> appendS7(ruvia::Context& c, std::string_view nodeId,
                                      std::vector<pb::ConfigItem>& items) {
        const auto rows = co_await c.db().query(R"sql(
SELECT d.id::text, item->>'id', item->>'name', COALESCE(item->>'unit', ''),
       item->>'area', COALESCE((item->>'dbNumber')::integer, 0),
       (item->>'start')::integer, COALESCE((item->>'startBit')::integer, 0),
       (item->>'size')::integer, COALESCE(item->>'dataType', 'BOOL'),
       COALESCE((item->>'decimals')::integer, -1),
       COALESCE((item->>'writable')::boolean, false)
FROM device d
JOIN protocol_config p ON p.id = d.protocol_config_id AND p.protocol = 'S7'
CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'areas', '[]')) item
WHERE d.edge_node_id = $1::uuid AND d.deleted_at IS NULL
ORDER BY d.id, item->>'id')sql",
                                                 service::common::dbParams(nodeId));
        for (const auto& row : rows.rows()) {
            pb::ConfigItem item;
            item.set_kind(pb::CONFIG_ITEM_S7_AREA);
            auto* value = item.mutable_s7_area();
            setUuid(value->mutable_device_id(), row[0].text());
            value->set_element_id(row[1].text());
            value->set_name(row[2].text());
            value->set_unit(row[3].text());
            value->set_area(row[4].text());
            value->set_db_number(
                static_cast<std::uint32_t>(integer(row[5].text())));
            value->set_start(static_cast<std::uint32_t>(integer(row[6].text())));
            value->set_start_bit(
                static_cast<std::uint32_t>(integer(row[7].text())));
            value->set_size(
                static_cast<std::uint32_t>(integer(row[8].text(), 1)));
            value->set_data_type(row[9].text());
            value->set_scale(1.0);
            value->set_decimals(
                static_cast<std::int32_t>(integer(row[10].text(), -1)));
            value->set_writable(row[11].text() == "t");
            items.push_back(std::move(item));
        }
    }

    static ruvia::Task<void> appendSl651(ruvia::Context& c, std::string_view nodeId,
                                         std::vector<pb::ConfigItem>& items) {
        const auto functions = co_await c.db().query(R"sql(
SELECT d.id::text, func->>'funcCode', func->>'name', func->>'dir'
FROM device d
JOIN protocol_config p ON p.id = d.protocol_config_id AND p.protocol = 'SL651'
CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'funcs', '[]')) func
WHERE d.edge_node_id = $1::uuid AND d.deleted_at IS NULL
ORDER BY d.id, func->>'funcCode')sql",
                                                      service::common::dbParams(nodeId));
        for (const auto& row : functions.rows()) {
            pb::ConfigItem item;
            item.set_kind(pb::CONFIG_ITEM_SL651_FUNCTION);
            auto* value = item.mutable_sl651_function();
            setUuid(value->mutable_device_id(), row[0].text());
            value->set_function_code(row[1].text());
            value->set_name(row[2].text());
            value->set_direction(row[3].text());
            items.push_back(std::move(item));
        }

        const auto elements = co_await c.db().query(R"sql(
SELECT d.id::text, func->>'funcCode', element->>'id', element->>'name',
       COALESCE(element->>'unit', ''), element->>'encode',
       (element->>'length')::integer, (element->>'digits')::integer,
       COALESCE(element->>'guideHex', ''), response_element,
       func->>'dir' = 'DOWN'
FROM device d
JOIN protocol_config p ON p.id = d.protocol_config_id AND p.protocol = 'SL651'
CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'funcs', '[]')) func
CROSS JOIN LATERAL (
  SELECT value AS element, false AS response_element
  FROM jsonb_array_elements(COALESCE(func->'elements', '[]'))
  UNION ALL
  SELECT value AS element, true AS response_element
  FROM jsonb_array_elements(COALESCE(func->'responseElements', '[]'))
) values
WHERE d.edge_node_id = $1::uuid AND d.deleted_at IS NULL
ORDER BY d.id, func->>'funcCode', response_element, element->>'id')sql",
                                                     service::common::dbParams(nodeId));
        for (const auto& row : elements.rows()) {
            pb::ConfigItem item;
            item.set_kind(pb::CONFIG_ITEM_SL651_ELEMENT);
            auto* value = item.mutable_sl651_element();
            setUuid(value->mutable_device_id(), row[0].text());
            value->set_function_code(row[1].text());
            value->set_element_id(row[2].text());
            value->set_name(row[3].text());
            value->set_unit(row[4].text());
            value->set_encoding(row[5].text());
            value->set_length(
                static_cast<std::uint32_t>(integer(row[6].text())));
            value->set_digits(
                static_cast<std::uint32_t>(integer(row[7].text())));
            const auto guide = bytes("HEX", row[8].text());
            value->set_guide(protocol::bytes(guide.data(), guide.size()));
            value->set_response_element(row[9].text() == "t");
            value->set_writable(row[10].text() == "t" && !value->response_element());
            items.push_back(std::move(item));
        }
    }
};

inline ConfigService& configService() { return ConfigService::instance(); }

} // namespace service::edge
