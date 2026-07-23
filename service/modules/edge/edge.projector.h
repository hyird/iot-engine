#pragma once

#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/core/Timer.h>
#include <ruvia/web/WebWorker.h>

#include "service/common/bridge/message.contract.h"
#include "service/common/http.h"
#include "service/common/uuid.h"
#include "service/modules/edge/edge.protocol.h"
#include "service/modules/southbridge/queue/redis_stream_async.h"

namespace service::edge {

inline constexpr std::string_view kEdgeIngressStream{"iot:edge:ingress"};
inline constexpr std::string_view kEdgeIngressGroup{"iot-engine:edge-projector"};

class EdgeProjector final {
  public:
    EdgeProjector() = default;
    EdgeProjector(const EdgeProjector&) = delete;
    EdgeProjector& operator=(const EdgeProjector&) = delete;
    ~EdgeProjector() { stop(); }

    void start(ruvia::WebWorkerHandle worker) {
        if (running_.exchange(true))
            return;
        worker_ = std::move(worker);
        auto ready = std::make_shared<std::promise<void>>();
        auto stopped = std::make_shared<std::promise<void>>();
        auto readyFuture = ready->get_future();
        stopped_ = stopped->get_future().share();
        const auto posted = worker_.post([this, ready, stopped](ruvia::WebWorkerContext& context) {
            return run(context, ready, stopped);
        });
        if (!posted.accepted()) {
            running_.store(false);
            throw std::runtime_error("north worker rejected edge projector");
        }
        readyFuture.get();
    }

    void stop() noexcept {
        if (!running_.exchange(false))
            return;
        if (stopped_.valid())
            (void)stopped_.wait_for(std::chrono::seconds(3));
        stopped_ = {};
        worker_ = {};
    }

  private:
    ruvia::Task<void> run(ruvia::WebWorkerContext& context,
                          std::shared_ptr<std::promise<void>> ready,
                          std::shared_ptr<std::promise<void>> stopped) {
        try {
            const auto redis = context.redis();
            co_await service::bridge::redis_async::ensureGroup(
                redis, kEdgeIngressStream, kEdgeIngressGroup);
            co_await hydrateAuth(context);
            ready->set_value();
            bool recovering = true;
            while (running_.load() && !context.stopToken().stopRequested()) {
                const auto messages = co_await service::bridge::redis_async::readGroup(
                    redis, kEdgeIngressStream, kEdgeIngressGroup, "edge-projector",
                    recovering ? "0" : ">", std::chrono::milliseconds(0), 100);
                if (recovering && messages.empty())
                    recovering = false;
                if (messages.empty()) {
                    (void)co_await ruvia::sleepFor(context.worker(),
                                                   std::chrono::milliseconds(20));
                    continue;
                }
                bool projectionFailed = false;
                try {
                    for (const auto& message : messages) {
                        co_await project(context, message.get("wire"));
                        co_await service::bridge::redis_async::acknowledgeAndDelete(
                            redis, kEdgeIngressStream, kEdgeIngressGroup, message.id);
                    }
                } catch (const std::exception& error) {
                    recovering = true;
                    projectionFailed = true;
                    std::cerr << "edge projection failed: " << error.what() << '\n';
                }
                if (projectionFailed) {
                    (void)co_await ruvia::sleepFor(context.worker(),
                                                   std::chrono::milliseconds(250));
                }
            }
        } catch (...) {
            try {
                ready->set_exception(std::current_exception());
            } catch (...) {
            }
        }
        try {
            stopped->set_value();
        } catch (...) {
        }
    }

    static ruvia::Task<void> hydrateAuth(ruvia::WebWorkerContext& context) {
        const auto rows = co_await context.db().query(
            "SELECT imei, id::text, enrollment_status FROM edge_node");
        for (const auto& row : rows.rows()) {
            const auto key = protocol::authKey(row[0].text());
            const auto value = std::string(row[1].text()) + "|" + std::string(row[2].text());
            co_await context.redis().set(key, value);
        }
    }

    static ruvia::Task<void> project(ruvia::WebWorkerContext& context,
                                     std::string_view wire) {
        pb::Envelope envelope;
        if (!protocol::decode(wire, envelope))
            co_return;
        if (envelope.payload_case() == pb::Envelope::kHello) {
            co_await saveHello(context, envelope.hello());
            co_return;
        }
        if (envelope.node_id().size() != 16)
            co_return;
        const auto nodeId = protocol::uuidText(envelope.node_id());
        switch (envelope.payload_case()) {
        case pb::Envelope::kHeartbeat:
            co_await saveHeartbeat(context, nodeId, envelope.heartbeat());
            break;
        case pb::Envelope::kCapabilityReport:
            co_await saveCapabilities(context, nodeId, envelope.capability_report());
            break;
        case pb::Envelope::kNetworkConfigResult:
            co_await saveNetworkResult(context, envelope.network_config_result());
            break;
        case pb::Envelope::kFirmwareUpdateResult:
            co_await saveFirmwareResult(context, envelope.firmware_update_result());
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
            co_await saveTelemetry(context, nodeId, envelope.telemetry_batch());
            break;
        case pb::Envelope::kCommandResult:
            co_await saveCommandResult(context, nodeId, envelope.command_result());
            break;
        default:
            break;
        }
    }

    static ruvia::Task<void> saveHello(ruvia::WebWorkerContext& context,
                                       const pb::Hello& hello) {
        if (!protocol::validImei(hello.imei()))
            co_return;
        const auto candidate = service::common::nextUuidV7();
        const auto rows = co_await context.db().query(R"sql(
INSERT INTO edge_node(id, platform_id, imei, model, software_version, hostname, architecture,
                      openwrt_release, supports_network_config, supports_firmware_update,
                      supports_platform_config, supports_device_config, network_config_version,
                      updated_at)
VALUES ($1::uuid, $2::uuid, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, NOW())
ON CONFLICT (platform_id, imei) DO UPDATE
SET model = EXCLUDED.model, software_version = EXCLUDED.software_version,
    hostname = EXCLUDED.hostname, architecture = EXCLUDED.architecture,
    openwrt_release = EXCLUDED.openwrt_release,
    supports_network_config = EXCLUDED.supports_network_config,
    supports_firmware_update = EXCLUDED.supports_firmware_update,
    supports_platform_config = EXCLUDED.supports_platform_config,
    supports_device_config = EXCLUDED.supports_device_config,
    network_config_version = EXCLUDED.network_config_version,
    updated_at = NOW()
RETURNING id::text, enrollment_status)sql",
                                                     service::common::dbParams(
                                                         candidate, protocol::kBootstrapPlatformId,
                                                         hello.imei(), hello.model(),
                                                         hello.software_version(), hello.hostname(),
                                                         hello.architecture(),
                                                         hello.openwrt_release(),
                                                         hello.supports_network_config(),
                                                         hello.supports_firmware_update(),
                                                         hello.supports_platform_config(),
                                                         hello.supports_device_config(),
                                                         hello.network_config_version()));
        const auto key = protocol::authKey(hello.imei());
        const auto value = std::string(rows.rows().front()[0].text()) + "|" +
                           std::string(rows.rows().front()[1].text());
        co_await context.redis().set(key, value);
    }

    static ruvia::Task<void> saveHeartbeat(ruvia::WebWorkerContext& context,
                                           std::string_view nodeId,
                                           const pb::Heartbeat& heartbeat) {
        (void)co_await context.db().execute(R"sql(
UPDATE edge_node SET active_config_version = $1, outbox_records = $2, outbox_bytes = $3,
                     config_status = CASE
                         WHEN desired_config_version = $1 AND $1 > 0 THEN 'applied'
                         ELSE config_status END,
                     config_message = CASE
                         WHEN desired_config_version = $1 AND $1 > 0 THEN ''
                         ELSE config_message END,
                     last_seen_at = NOW(), updated_at = NOW()
WHERE id = $4::uuid)sql",
                                            service::common::dbParams(
                                                heartbeat.active_config_version(),
                                                heartbeat.outbox_records(),
                                                heartbeat.outbox_bytes(), nodeId));
        if (heartbeat.active_config_version() != 0) {
            (void)co_await context.db().execute(R"sql(
UPDATE edge_config_revision revision
SET status = 'applied', message = '', completed_at = COALESCE(completed_at, NOW())
FROM edge_node node
WHERE revision.node_id = node.id AND node.id = $1::uuid
  AND revision.revision = $2 AND node.desired_config_version = $2)sql",
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
            "UPDATE edge_node SET ttyd_available = $1, updated_at = NOW() WHERE id = $2::uuid",
            service::common::dbParams(report.ttyd_available(), nodeId));
    }

    static ruvia::Task<void> saveNetworkResult(
        ruvia::WebWorkerContext& context, const pb::NetworkConfigResult& result) {
        if (result.request_id().size() != 16)
            co_return;
        const auto id = protocol::uuidText(result.request_id());
        const std::string status = result.success() ? "succeeded" : "failed";
        const std::string json = "{\"message\":\"" + jsonEscape(result.message()) +
                                 "\",\"rolled_back\":" +
                                 (result.rolled_back() ? "true" : "false") + "}";
        (void)co_await context.db().execute(R"sql(
UPDATE edge_task SET status = $1, result = $2::jsonb, updated_at = NOW(), completed_at = NOW()
WHERE id = $3::uuid AND task_type = 'network')sql",
                                            service::common::dbParams(status, json, id));
    }

    static ruvia::Task<void> saveFirmwareResult(
        ruvia::WebWorkerContext& context, const pb::FirmwareUpdateResult& result) {
        if (result.request_id().size() != 16)
            co_return;
        const auto id = protocol::uuidText(result.request_id());
        std::string status = "running";
        bool completed = false;
        if (result.state() == pb::FIRMWARE_UPDATE_ACCEPTED)
            status = "accepted";
        else if (result.state() == pb::FIRMWARE_UPDATE_FLASHING) {
            status = "succeeded";
            completed = true;
        } else if (result.state() == pb::FIRMWARE_UPDATE_FAILED) {
            status = "failed";
            completed = true;
        }
        const std::string json = "{\"message\":\"" + jsonEscape(result.message()) + "\"}";
        (void)co_await context.db().execute(R"sql(
UPDATE edge_task SET status = $1, result = $2::jsonb, updated_at = NOW(),
    completed_at = CASE WHEN $3 THEN NOW() ELSE NULL END
WHERE id = $4::uuid AND task_type = 'firmware')sql",
                                            service::common::dbParams(status, json, completed, id));
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
UPDATE edge_task SET status = $1, result = $2::jsonb, updated_at = NOW(), completed_at = NOW()
WHERE id = $3::uuid AND task_type IN ('platform_upsert', 'platform_delete'))sql",
                                            service::common::dbParams(status, json, id));
        (void)co_await context.db().execute(R"sql(
UPDATE edge_node_platform SET apply_status = $1, last_message = $2, updated_at = NOW()
WHERE node_id = $3::uuid
  AND platform_id = (SELECT (request->>'platform_id')::uuid FROM edge_task WHERE id = $4::uuid))sql",
                                            service::common::dbParams(
                                                result.success() ? "applied" : "failed",
                                                result.message(), nodeId, id));
        if (result.success()) {
            (void)co_await context.db().execute(R"sql(
DELETE FROM edge_node_platform
WHERE node_id = $1::uuid
  AND platform_id = (SELECT (request->>'platform_id')::uuid FROM edge_task
                     WHERE id = $2::uuid AND task_type = 'platform_delete'))sql",
                                                service::common::dbParams(nodeId, id));
        }
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
SET active_config_version = GREATEST(active_config_version, $1),
    config_status = CASE WHEN desired_config_version = $1 THEN 'applied' ELSE config_status END,
    config_message = CASE WHEN desired_config_version = $1 THEN '' ELSE config_message END,
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
SET config_status = CASE WHEN desired_config_version = $1 THEN 'rejected' ELSE config_status END,
    config_message = CASE WHEN desired_config_version = $1 THEN $2 ELSE config_message END,
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
            return value.bool_value() ? "true" : "false";
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
                      ",\"unit\":\"" + jsonEscape(item.unit()) + "\"}";
            first = false;
        }
        output += "}}";
        return output;
    }

    static ruvia::Task<void> saveTelemetry(ruvia::WebWorkerContext& context,
                                            std::string_view nodeId,
                                            const pb::TelemetryBatch& batch) {
        for (const auto& record : batch.records()) {
            if (record.record_id().size() != 16 || record.device_id().size() != 16)
                continue;
            const auto deviceId = protocol::uuidText(record.device_id());
            const auto metadata = co_await context.db().query(R"sql(
SELECT d.protocol_params->>'device_code',
       GREATEST(1, CEIL(COALESCE((p.config->>'storageInterval')::numeric, 1)))::bigint,
       COALESCE((d.protocol_params->>'online_timeout')::integer, 300), p.protocol
FROM device d JOIN protocol_config p ON p.id = d.protocol_config_id
WHERE d.id = $1::uuid AND d.edge_node_id = $2::uuid AND d.deleted_at IS NULL LIMIT 1)sql",
                                                              service::common::dbParams(deviceId,
                                                                                        nodeId));
            if (metadata.rows().empty())
                continue;
            const auto& row = metadata.rows().front();
            bridge::ParsedDeviceMessage parsed;
            parsed.messageId = protocol::uuidText(record.record_id());
            parsed.causationId = parsed.messageId;
            parsed.linkId = "";
            parsed.deviceId = deviceId;
            parsed.deviceCode = std::string(row[0].text());
            parsed.protocol = protocolName(record.protocol());
            if (parsed.protocol.empty())
                parsed.protocol = std::string(row[3].text());
            parsed.connectionId = std::string(nodeId);
            parsed.occurredAtMs = record.observed_at_ms();
            parsed.observedAtMs = record.observed_at_ms();
            parsed.storageInterval = std::stoll(std::string(row[1].text()));
            parsed.onlineWindowMs = std::stoll(std::string(row[2].text())) * 1000;
            parsed.source = "edge";
            parsed.valuesJson = telemetryJson(record);
            if (!record.raw_payload().empty())
                parsed.rawPayloads.emplace_back(record.raw_payload().begin(),
                                                record.raw_payload().end());
            (void)co_await bridge::redis_async::publish(context.redis(), bridge::parsedStream(0),
                                                        bridge::parsedFields(parsed), 100000);
        }
    }

    static ruvia::Task<void> saveCommandResult(ruvia::WebWorkerContext& context,
                                                std::string_view nodeId,
                                                const pb::CommandResult& result) {
        if (result.command_id().size() != 16 || result.device_id().size() != 16)
            co_return;
        const auto commandId = protocol::uuidText(result.command_id());
        const auto deviceId = protocol::uuidText(result.device_id());
        const auto device = co_await context.db().query(R"sql(
SELECT d.protocol_params->>'device_code', p.protocol
FROM device d JOIN protocol_config p ON p.id = d.protocol_config_id
WHERE d.id = $1::uuid AND d.edge_node_id = $2::uuid AND d.deleted_at IS NULL LIMIT 1)sql",
                                                        service::common::dbParams(deviceId,
                                                                                  nodeId));
        if (device.rows().empty())
            co_return;
        const bool success = result.state() == pb::COMMAND_STATE_SUCCEEDED;
        (void)co_await bridge::redis_async::publish(
            context.redis(), bridge::commandResultStream(0),
            {{"message_id", bridge::nextMessageId()},
             {"causation_id", commandId},
             {"command_id", commandId},
             {"device_id", deviceId},
             {"device_code", std::string(device.rows().front()[0].text())},
             {"protocol", std::string(device.rows().front()[1].text())},
             {"attempt", "1"},
             {"success", success ? "1" : "0"},
             {"reason", result.message()},
             {"worker_id", "0"},
             {"created_at_ms", std::to_string(bridge::utcNowMilliseconds())},
             {"completed_at_ms", std::to_string(result.completed_at_ms())}},
            10000);
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

    ruvia::WebWorkerHandle worker_;
    std::shared_future<void> stopped_;
    std::atomic_bool running_{false};
};

} // namespace service::edge
