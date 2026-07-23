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
        iot_edge_v1_Envelope envelope = iot_edge_v1_Envelope_init_zero;
        if (!protocol::decode(wire, envelope))
            co_return;
        if (envelope.which_payload == iot_edge_v1_Envelope_hello_tag) {
            co_await saveHello(context, envelope.payload.hello);
            co_return;
        }
        if (envelope.node_id.size != 16)
            co_return;
        const auto nodeId = protocol::uuidText(envelope.node_id.bytes);
        switch (envelope.which_payload) {
        case iot_edge_v1_Envelope_heartbeat_tag:
            co_await saveHeartbeat(context, nodeId, envelope.payload.heartbeat);
            break;
        case iot_edge_v1_Envelope_capability_report_tag:
            co_await saveCapabilities(context, nodeId, envelope.payload.capability_report);
            break;
        case iot_edge_v1_Envelope_network_config_result_tag:
            co_await saveNetworkResult(context, envelope.payload.network_config_result);
            break;
        case iot_edge_v1_Envelope_firmware_update_result_tag:
            co_await saveFirmwareResult(context, envelope.payload.firmware_update_result);
            break;
        case iot_edge_v1_Envelope_platform_config_result_tag:
            co_await savePlatformResult(context, nodeId,
                                        envelope.payload.platform_config_result);
            break;
        case iot_edge_v1_Envelope_config_applied_tag:
            co_await saveConfigApplied(context, nodeId, envelope.payload.config_applied);
            break;
        case iot_edge_v1_Envelope_config_rejected_tag:
            co_await saveConfigRejected(context, nodeId, envelope.payload.config_rejected);
            break;
        case iot_edge_v1_Envelope_telemetry_batch_tag:
            co_await saveTelemetry(context, nodeId, envelope.payload.telemetry_batch);
            break;
        case iot_edge_v1_Envelope_command_result_tag:
            co_await saveCommandResult(context, nodeId, envelope.payload.command_result);
            break;
        default:
            break;
        }
    }

    static ruvia::Task<void> saveHello(ruvia::WebWorkerContext& context,
                                       const iot_edge_v1_Hello& hello) {
        if (!protocol::validImei(hello.imei))
            co_return;
        const auto candidate = service::common::nextUuidV7();
        const auto rows = co_await context.db().query(R"sql(
INSERT INTO edge_node(id, platform_id, imei, model, software_version, hostname, architecture,
                      openwrt_release, supports_network_config, supports_firmware_update,
                      supports_platform_config, supports_device_config, updated_at)
VALUES ($1::uuid, $2::uuid, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, NOW())
ON CONFLICT (platform_id, imei) DO UPDATE
SET model = EXCLUDED.model, software_version = EXCLUDED.software_version,
    hostname = EXCLUDED.hostname, architecture = EXCLUDED.architecture,
    openwrt_release = EXCLUDED.openwrt_release,
    supports_network_config = EXCLUDED.supports_network_config,
    supports_firmware_update = EXCLUDED.supports_firmware_update,
    supports_platform_config = EXCLUDED.supports_platform_config,
    supports_device_config = EXCLUDED.supports_device_config,
    updated_at = NOW()
RETURNING id::text, enrollment_status)sql",
                                                     service::common::dbParams(
                                                         candidate, protocol::kBootstrapPlatformId,
                                                         hello.imei, hello.model,
                                                         hello.software_version, hello.hostname,
                                                         hello.architecture, hello.openwrt_release,
                                                         hello.supports_network_config,
                                                         hello.supports_firmware_update,
                                                         hello.supports_platform_config,
                                                         hello.supports_device_config));
        const auto key = protocol::authKey(hello.imei);
        const auto value = std::string(rows.rows().front()[0].text()) + "|" +
                           std::string(rows.rows().front()[1].text());
        co_await context.redis().set(key, value);
    }

    static ruvia::Task<void> saveHeartbeat(ruvia::WebWorkerContext& context,
                                           std::string_view nodeId,
                                           const iot_edge_v1_Heartbeat& heartbeat) {
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
                                                heartbeat.active_config_version,
                                                heartbeat.outbox_records, heartbeat.outbox_bytes,
                                                nodeId));
        if (heartbeat.active_config_version != 0) {
            (void)co_await context.db().execute(R"sql(
UPDATE edge_config_revision revision
SET status = 'applied', message = '', completed_at = COALESCE(completed_at, NOW())
FROM edge_node node
WHERE revision.node_id = node.id AND node.id = $1::uuid
  AND revision.revision = $2 AND node.desired_config_version = $2)sql",
                                                service::common::dbParams(
                                                    nodeId,
                                                    heartbeat.active_config_version));
        }
    }

    static std::string mac(const iot_edge_v1_InterfaceCapability& item) {
        if (item.mac.size != 6)
            return {};
        constexpr char digits[] = "0123456789abcdef";
        std::string output;
        output.reserve(17);
        for (std::size_t index = 0; index < 6; ++index) {
            if (index != 0)
                output.push_back(':');
            output.push_back(digits[item.mac.bytes[index] >> 4U]);
            output.push_back(digits[item.mac.bytes[index] & 0x0fU]);
        }
        return output;
    }

    static std::string jsonArray(const iot_edge_v1_InterfaceCapability& item) {
        std::string output{"["};
        for (pb_size_t index = 0; index < item.bridge_ports_count; ++index) {
            if (index != 0)
                output.push_back(',');
            output.push_back('"');
            output += jsonEscape(item.bridge_ports[index]);
            output.push_back('"');
        }
        output.push_back(']');
        return output;
    }

    static ruvia::Task<void> saveCapabilities(
        ruvia::WebWorkerContext& context, std::string_view nodeId,
        const iot_edge_v1_CapabilityReport& report) {
        (void)co_await context.db().execute(
            "DELETE FROM edge_node_interface WHERE node_id = $1::uuid",
            service::common::dbParams(nodeId));
        for (pb_size_t index = 0; index < report.interfaces_count; ++index) {
            const auto& item = report.interfaces[index];
            const auto macAddress = mac(item);
            const auto ports = jsonArray(item);
            (void)co_await context.db().execute(R"sql(
INSERT INTO edge_node_interface(node_id, name, display_name, mac, is_up, is_bridge, ipv4,
                                prefix_length, gateway, bridge_ports)
VALUES ($1::uuid, $2, $3, NULLIF($4, ''), $5, $6, NULLIF($7, ''), $8,
        NULLIF($9, ''), $10::jsonb))sql",
                                                service::common::dbParams(
                                                    nodeId, item.name, item.display_name,
                                                    macAddress, item.up, item.bridge, item.ipv4,
                                                    item.prefix_length, item.gateway, ports));
        }
        (void)co_await context.db().execute(
            "DELETE FROM edge_node_serial WHERE node_id = $1::uuid",
            service::common::dbParams(nodeId));
        for (pb_size_t index = 0; index < report.serial_ports_count; ++index) {
            const auto& item = report.serial_ports[index];
            (void)co_await context.db().execute(R"sql(
INSERT INTO edge_node_serial(node_id, path, display_name, available, rs485)
VALUES ($1::uuid, $2, $3, $4, $5))sql",
                                                service::common::dbParams(
                                                    nodeId, item.path, item.display_name,
                                                    item.available, item.rs485));
        }
        (void)co_await context.db().execute(
            "UPDATE edge_node SET ttyd_available = $1, updated_at = NOW() WHERE id = $2::uuid",
            service::common::dbParams(report.ttyd_available, nodeId));
    }

    static ruvia::Task<void> saveNetworkResult(
        ruvia::WebWorkerContext& context, const iot_edge_v1_NetworkConfigResult& result) {
        if (result.request_id.size != 16)
            co_return;
        const auto id = protocol::uuidText(result.request_id.bytes);
        const std::string status = result.success ? "succeeded" : "failed";
        const std::string json = "{\"message\":\"" + jsonEscape(result.message) +
                                 "\",\"rolled_back\":" +
                                 (result.rolled_back ? "true" : "false") + "}";
        (void)co_await context.db().execute(R"sql(
UPDATE edge_task SET status = $1, result = $2::jsonb, updated_at = NOW(), completed_at = NOW()
WHERE id = $3::uuid AND task_type = 'network')sql",
                                            service::common::dbParams(status, json, id));
    }

    static ruvia::Task<void> saveFirmwareResult(
        ruvia::WebWorkerContext& context, const iot_edge_v1_FirmwareUpdateResult& result) {
        if (result.request_id.size != 16)
            co_return;
        const auto id = protocol::uuidText(result.request_id.bytes);
        std::string status = "running";
        bool completed = false;
        if (result.state == iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_ACCEPTED)
            status = "accepted";
        else if (result.state == iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_FLASHING) {
            status = "succeeded";
            completed = true;
        } else if (result.state == iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_FAILED) {
            status = "failed";
            completed = true;
        }
        const std::string json = "{\"message\":\"" + jsonEscape(result.message) + "\"}";
        (void)co_await context.db().execute(R"sql(
UPDATE edge_task SET status = $1, result = $2::jsonb, updated_at = NOW(),
    completed_at = CASE WHEN $3 THEN NOW() ELSE NULL END
WHERE id = $4::uuid AND task_type = 'firmware')sql",
                                            service::common::dbParams(status, json, completed, id));
    }

    static ruvia::Task<void> savePlatformResult(
        ruvia::WebWorkerContext& context, std::string_view nodeId,
        const iot_edge_v1_PlatformConfigResult& result) {
        if (result.request_id.size != 16)
            co_return;
        const auto id = protocol::uuidText(result.request_id.bytes);
        const std::string status = result.success ? "succeeded" : "failed";
        const std::string json = "{\"message\":\"" + jsonEscape(result.message) + "\"}";
        (void)co_await context.db().execute(R"sql(
UPDATE edge_task SET status = $1, result = $2::jsonb, updated_at = NOW(), completed_at = NOW()
WHERE id = $3::uuid AND task_type IN ('platform_upsert', 'platform_delete'))sql",
                                            service::common::dbParams(status, json, id));
        (void)co_await context.db().execute(R"sql(
UPDATE edge_node_platform SET apply_status = $1, last_message = $2, updated_at = NOW()
WHERE node_id = $3::uuid
  AND platform_id = (SELECT (request->>'platform_id')::uuid FROM edge_task WHERE id = $4::uuid))sql",
                                            service::common::dbParams(
                                                result.success ? "applied" : "failed",
                                                result.message, nodeId, id));
        if (result.success) {
            (void)co_await context.db().execute(R"sql(
DELETE FROM edge_node_platform
WHERE node_id = $1::uuid
  AND platform_id = (SELECT (request->>'platform_id')::uuid FROM edge_task
                     WHERE id = $2::uuid AND task_type = 'platform_delete'))sql",
                                                service::common::dbParams(nodeId, id));
        }
    }

    static std::string hex(const std::uint8_t* value, std::size_t size) {
        constexpr char digits[] = "0123456789abcdef";
        std::string output;
        output.reserve(size * 2);
        for (std::size_t index = 0; index < size; ++index) {
            output.push_back(digits[value[index] >> 4U]);
            output.push_back(digits[value[index] & 0x0fU]);
        }
        return output;
    }

    static ruvia::Task<void> saveConfigApplied(ruvia::WebWorkerContext& context,
                                                std::string_view nodeId,
                                                const iot_edge_v1_ConfigApplied& result) {
        if (result.revision == 0 || result.sha256.size != 32)
            co_return;
        const auto digest = hex(result.sha256.bytes, 32);
        (void)co_await context.db().execute(R"sql(
UPDATE edge_config_revision
SET status = 'applied', message = '', completed_at = NOW()
WHERE node_id = $1::uuid AND revision = $2 AND sha256 = $3)sql",
                                            service::common::dbParams(
                                                nodeId, static_cast<std::int64_t>(result.revision),
                                                digest));
        (void)co_await context.db().execute(R"sql(
UPDATE edge_node
SET active_config_version = GREATEST(active_config_version, $1),
    config_status = CASE WHEN desired_config_version = $1 THEN 'applied' ELSE config_status END,
    config_message = CASE WHEN desired_config_version = $1 THEN '' ELSE config_message END,
    updated_at = NOW()
WHERE id = $2::uuid)sql",
                                            service::common::dbParams(
                                                static_cast<std::int64_t>(result.revision),
                                                nodeId));
    }

    static ruvia::Task<void> saveConfigRejected(ruvia::WebWorkerContext& context,
                                                 std::string_view nodeId,
                                                 const iot_edge_v1_ConfigRejected& result) {
        if (result.revision == 0)
            co_return;
        const std::string message = std::string(result.code) + ": " + result.message;
        (void)co_await context.db().execute(R"sql(
UPDATE edge_config_revision
SET status = 'rejected', message = $1, completed_at = NOW()
WHERE node_id = $2::uuid AND revision = $3)sql",
                                            service::common::dbParams(
                                                message, nodeId,
                                                static_cast<std::int64_t>(result.revision)));
        (void)co_await context.db().execute(R"sql(
UPDATE edge_node
SET config_status = CASE WHEN desired_config_version = $1 THEN 'rejected' ELSE config_status END,
    config_message = CASE WHEN desired_config_version = $1 THEN $2 ELSE config_message END,
    updated_at = NOW()
WHERE id = $3::uuid)sql",
                                            service::common::dbParams(
                                                static_cast<std::int64_t>(result.revision), message,
                                                nodeId));
    }

    static std::string protocolName(iot_edge_v1_Protocol value) {
        if (value == iot_edge_v1_Protocol_PROTOCOL_MODBUS)
            return "Modbus";
        if (value == iot_edge_v1_Protocol_PROTOCOL_S7)
            return "S7";
        if (value == iot_edge_v1_Protocol_PROTOCOL_SL651)
            return "SL651";
        return {};
    }

    static std::string scalarJson(const iot_edge_v1_ScalarValue& value) {
        switch (value.which_value) {
        case iot_edge_v1_ScalarValue_bool_value_tag:
            return value.value.bool_value ? "true" : "false";
        case iot_edge_v1_ScalarValue_signed_value_tag:
            return std::to_string(value.value.signed_value);
        case iot_edge_v1_ScalarValue_unsigned_value_tag:
            return std::to_string(value.value.unsigned_value);
        case iot_edge_v1_ScalarValue_double_value_tag: {
            std::ostringstream output;
            output.precision(15);
            output << value.value.double_value;
            return output.str();
        }
        case iot_edge_v1_ScalarValue_string_value_tag:
            return "\"" + jsonEscape(value.value.string_value) + "\"";
        case iot_edge_v1_ScalarValue_bytes_value_tag:
            return "\"" + hex(value.value.bytes_value.bytes, value.value.bytes_value.size) +
                   "\"";
        default:
            return "null";
        }
    }

    static std::string telemetryJson(const iot_edge_v1_TelemetryRecord& record) {
        std::string output = "{\"function_code\":\"" +
                             jsonEscape(record.function_code) + "\",\"function_name\":\"" +
                             jsonEscape(record.function_name) + "\",\"direction\":\"" +
                             jsonEscape(record.direction) + "\",\"values\":{";
        for (pb_size_t index = 0; index < record.values_count; ++index) {
            if (index != 0)
                output.push_back(',');
            const auto& item = record.values[index];
            output += "\"" + jsonEscape(item.element_id) + "\":{\"name\":\"" +
                      jsonEscape(item.name) + "\",\"value\":" +
                      (item.has_value ? scalarJson(item.value) : "null") +
                      ",\"unit\":\"" + jsonEscape(item.unit) + "\"}";
        }
        output += "}}";
        return output;
    }

    static ruvia::Task<void> saveTelemetry(ruvia::WebWorkerContext& context,
                                            std::string_view nodeId,
                                            const iot_edge_v1_TelemetryBatch& batch) {
        for (pb_size_t index = 0; index < batch.records_count; ++index) {
            const auto& record = batch.records[index];
            if (record.record_id.size != 16 || record.device_id.size != 16)
                continue;
            const auto deviceId = protocol::uuidText(record.device_id.bytes);
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
            parsed.messageId = protocol::uuidText(record.record_id.bytes);
            parsed.causationId = parsed.messageId;
            parsed.linkId = "";
            parsed.deviceId = deviceId;
            parsed.deviceCode = std::string(row[0].text());
            parsed.protocol = protocolName(record.protocol);
            if (parsed.protocol.empty())
                parsed.protocol = std::string(row[3].text());
            parsed.connectionId = std::string(nodeId);
            parsed.occurredAtMs = record.observed_at_ms;
            parsed.observedAtMs = record.observed_at_ms;
            parsed.storageInterval = std::stoll(std::string(row[1].text()));
            parsed.onlineWindowMs = std::stoll(std::string(row[2].text())) * 1000;
            parsed.source = "edge";
            parsed.valuesJson = telemetryJson(record);
            if (record.raw_payload.size != 0)
                parsed.rawPayloads.emplace_back(record.raw_payload.bytes,
                                                record.raw_payload.bytes +
                                                    record.raw_payload.size);
            (void)co_await bridge::redis_async::publish(context.redis(), bridge::parsedStream(0),
                                                        bridge::parsedFields(parsed), 100000);
        }
    }

    static ruvia::Task<void> saveCommandResult(ruvia::WebWorkerContext& context,
                                                std::string_view nodeId,
                                                const iot_edge_v1_CommandResult& result) {
        if (result.command_id.size != 16 || result.device_id.size != 16)
            co_return;
        const auto commandId = protocol::uuidText(result.command_id.bytes);
        const auto deviceId = protocol::uuidText(result.device_id.bytes);
        const auto device = co_await context.db().query(R"sql(
SELECT d.protocol_params->>'device_code', p.protocol
FROM device d JOIN protocol_config p ON p.id = d.protocol_config_id
WHERE d.id = $1::uuid AND d.edge_node_id = $2::uuid AND d.deleted_at IS NULL LIMIT 1)sql",
                                                        service::common::dbParams(deviceId,
                                                                                  nodeId));
        if (device.rows().empty())
            co_return;
        const bool success =
            result.state == iot_edge_v1_CommandState_COMMAND_STATE_SUCCEEDED;
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
             {"reason", std::string(result.message)},
             {"worker_id", "0"},
             {"created_at_ms", std::to_string(bridge::utcNowMilliseconds())},
             {"completed_at_ms", std::to_string(result.completed_at_ms)}},
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
