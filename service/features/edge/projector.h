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

#include "service/common/message/contract.h"
#include "service/common/http.h"
#include "service/common/uuid.h"
#include "service/features/edge/protocol.h"
#include "service/features/collector/stream.h"

namespace service::edge {

inline constexpr std::string_view kEdgeIngressStream{"iot:edge:ingress"};
inline constexpr std::string_view kEdgeIngressGroup{"iot-engine:edge-projector"};

class Projector final {
  public:
    Projector() = default;
    Projector(const Projector&) = delete;
    Projector& operator=(const Projector&) = delete;
    ~Projector() { stop(); }

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
            throw std::runtime_error("service worker rejected edge projector");
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
            co_await service::message::redis::ensureGroup(
                redis, kEdgeIngressStream, kEdgeIngressGroup);
            co_await hydrateAuth(context);
            ready->set_value();
            bool recovering = true;
            while (running_.load() && !context.stopToken().stopRequested()) {
                const auto messages = co_await service::message::redis::readGroup(
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
                        co_await service::message::redis::acknowledgeAndDelete(
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
        case pb::Envelope::kModemControlResult:
            co_await saveModemResult(context, envelope.modem_control_result());
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
        case pb::Envelope::kEndpointStatusReport:
            co_await saveEndpointStatus(context, nodeId, envelope.endpoint_status_report());
            break;
        default:
            break;
        }
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
                      openwrt_release, capability, mobile, status, updated_at)
VALUES ($1::uuid, $2::uuid, $3, $4, $5, $6, $7, $8,
        jsonb_build_object(
            'networkConfig', $9::boolean,
            'firmwareUpdate', $10::boolean,
            'platformConfig', $11::boolean,
            'deviceConfig', $12::boolean,
            'networkConfigVersion', $13::bigint,
            'modemControl', $14::boolean,
            'logs', $15::boolean,
            'terminal', false),
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
        NOW())
ON CONFLICT (platform_id, imei) DO UPDATE
SET model = EXCLUDED.model, software_version = EXCLUDED.software_version,
    hostname = EXCLUDED.hostname, architecture = EXCLUDED.architecture,
    openwrt_release = EXCLUDED.openwrt_release,
    capability = EXCLUDED.capability || jsonb_build_object(
        'terminal', COALESCE((edge_node.capability->>'terminal')::boolean, false)),
    mobile = EXCLUDED.mobile,
    status = jsonb_set(
        jsonb_set(edge_node.status, '{log}',
                  COALESCE(edge_node.status->'log', '{}'::jsonb), true),
        '{log,level}', to_jsonb(COALESCE(NULLIF($28::text, ''), 'info')::text), true),
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
                                                          hello.log_level()));
        const auto key = protocol::authKey(hello.imei());
        const auto nodeId = std::string(rows.rows().front()[0].text());
        const auto enrollmentStatus = std::string(rows.rows().front()[1].text());
        const auto value = nodeId + "|" + enrollmentStatus;
        co_await context.redis().set(key, value);
        if (enrollmentStatus == "approved") {
            (void)co_await context.db().execute(R"sql(
WITH target AS (
    SELECT task.id,
           COALESCE(firmware.version, task.request->>'version', '') AS target_version
    FROM edge_task task
    LEFT JOIN edge_firmware firmware ON firmware.id::text = task.request->>'firmware_id'
    WHERE task.node_id = $2::uuid
      AND task.task_type = 'firmware'
      AND task.status = 'running'
      AND task.result->>'state' = 'flashing'
    ORDER BY created_at DESC
    LIMIT 1
)
UPDATE edge_task task
SET status = CASE
        WHEN target.target_version = $1::text THEN 'succeeded'
        ELSE 'failed'
    END,
    result = task.result || jsonb_build_object(
        'state', CASE
            WHEN target.target_version = $1::text THEN 'rebooted'
            ELSE 'versionMismatch'
        END,
        'message', CASE
            WHEN target.target_version = $1::text THEN 'firmware reboot confirmed'
            ELSE 'firmware rebooted but version mismatch'
        END,
        'targetVersion', target.target_version,
        'softwareVersion', $1::text),
    updated_at = NOW(),
    completed_at = NOW()
FROM target
WHERE task.id = target.id)sql",
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
                COALESCE((status->'config'->>'activeVersion')::bigint, 0),
                $1::bigint),
            'desiredVersion', COALESCE((status->'config'->>'desiredVersion')::bigint, 0),
            'state', CASE
                WHEN COALESCE((status->'config'->>'desiredVersion')::bigint, 0) = $1
                     AND $1 > 0 THEN 'applied'
                ELSE COALESCE(status->'config'->>'state', 'idle') END,
            'message', CASE
                WHEN COALESCE((status->'config'->>'desiredVersion')::bigint, 0) = $1
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
        'apn', $12::text,
        'operator', $13::text,
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
  AND COALESCE((node.status->'config'->>'desiredVersion')::bigint, 0) = $2)sql",
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
WHERE id = $4::uuid AND task_type = 'firmware')sql",
                                             service::common::dbParams(status, json, completed, id));
    }

    static ruvia::Task<void> saveModemResult(
        ruvia::WebWorkerContext& context, const pb::ModemControlResult& result) {
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
WHERE id = $4::uuid AND task_type = 'modem')sql",
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
UPDATE edge_node_platform
SET status = jsonb_build_object('state', $1::text, 'message', $2::text),
    updated_at = NOW()
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
SET status = jsonb_set(
        jsonb_set(
            jsonb_set(status, '{config,activeVersion}', to_jsonb(GREATEST(
                COALESCE((status->'config'->>'activeVersion')::bigint, 0), $1::bigint)), true),
            '{config,state}', to_jsonb(CASE
                WHEN COALESCE((status->'config'->>'desiredVersion')::bigint, 0) = $1
                THEN 'applied'
                ELSE COALESCE(status->'config'->>'state', 'idle') END::text), true),
        '{config,message}', to_jsonb(CASE
            WHEN COALESCE((status->'config'->>'desiredVersion')::bigint, 0) = $1
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
            WHEN COALESCE((status->'config'->>'desiredVersion')::bigint, 0) = $1
            THEN 'rejected'
            ELSE COALESCE(status->'config'->>'state', 'idle') END::text), true),
        '{config,message}', to_jsonb(CASE
            WHEN COALESCE((status->'config'->>'desiredVersion')::bigint, 0) = $1
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
            message::ParsedDeviceMessage parsed;
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
            (void)co_await message::redis::publish(context.redis(), message::parsedStream(0),
                                                        message::parsedFields(parsed), 100000);
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
        (void)co_await message::redis::publish(
            context.redis(), message::commandResultStream(0),
            {{"message_id", message::nextMessageId()},
             {"causation_id", commandId},
             {"command_id", commandId},
             {"device_id", deviceId},
             {"device_code", std::string(device.rows().front()[0].text())},
             {"protocol", std::string(device.rows().front()[1].text())},
             {"attempt", "1"},
             {"success", success ? "1" : "0"},
             {"reason", result.message()},
             {"worker_id", "0"},
             {"created_at_ms", std::to_string(message::utcNowMilliseconds())},
             {"completed_at_ms", std::to_string(result.completed_at_ms())}},
            10000);
    }

    static std::string endpointStatusKey(std::string_view nodeId, std::string_view endpointId) {
        return "iot:runtime:edge:" + std::string(nodeId) + ":endpoint:" +
               std::string(endpointId);
    }

    static ruvia::Task<void> saveEndpointStatus(ruvia::WebWorkerContext& context,
                                                 std::string_view nodeId,
                                                 const pb::EndpointStatusReport& report) {
        for (const auto& status : report.endpoints()) {
            if (status.endpoint_id().size() != 16)
                continue;
            const auto endpointId = protocol::uuidText(status.endpoint_id());
            std::string clients;
            for (const auto& client : status.clients()) {
                if (!clients.empty())
                    clients.push_back(',');
                clients += client;
            }
            const auto key = endpointStatusKey(nodeId, endpointId);
            co_await service::message::redis::eraseHash(context.redis(), key);
            co_await service::message::redis::setHash(
                context.redis(), key,
                {{"node_id", std::string(nodeId)},
                 {"endpoint_id", endpointId},
                 {"state", status.state()},
                 {"reason", status.reason()},
                 {"client_count", std::to_string(status.client_count())},
                 {"clients", clients},
                 {"last_activity_at_ms", std::to_string(status.last_activity_at_ms())},
                 {"updated_at_ms", std::to_string(protocol::nowMs())}});
            (void)co_await service::message::redis::command(
                context.redis(), {"PEXPIRE", key, "300000"});
        }
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
