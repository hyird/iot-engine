#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/core/Timer.h>
#include <ruvia/web/WebWorker.h>

#include "service/common/message/contract.h"
#include "service/common/message/shard.h"
#include "service/common/http.h"
#include "service/common/uuid.h"
#include "service/features/edge/metadata.h"
#include "service/features/edge/protocol.h"
#include "service/features/edge/projector-stream.h"
#include "service/features/collector/stream.h"
#include "service/features/event/stream-multiplexer.h"
#include "service/features/telemetry/persistence.h"

namespace service::edge {

inline constexpr std::string_view kEdgeIngressGroup{"iot-engine:edge-projector"};

class Projector final {
  public:
    Projector() = default;
    Projector(const Projector&) = delete;
    Projector& operator=(const Projector&) = delete;
    ~Projector() { stop(); }

    void start(std::vector<ruvia::WebWorkerHandle> workers) {
        if (running_.exchange(true))
            return;
        workers_ = std::move(workers);
        if (workers_.empty()) {
            running_.store(false);
            throw std::runtime_error("edge projector requires Service Workers");
        }
        std::vector<std::future<void>> readiness;
        readiness.reserve(workers_.size());
        stopped_.reserve(workers_.size());
        for (std::size_t index = 0; index < workers_.size(); ++index) {
            auto ready = std::make_shared<std::promise<void>>();
            auto stopped = std::make_shared<std::promise<void>>();
            readiness.push_back(ready->get_future());
            stopped_.push_back(stopped->get_future().share());
            const auto posted = workers_[index].post(
                [this, index, ready, stopped](ruvia::WebWorkerContext& context) {
                    return run(context, index, ready, stopped);
                });
            if (!posted.accepted()) {
                running_.store(false);
                throw std::runtime_error("service worker rejected edge projector");
            }
        }
        for (auto& ready : readiness)
            ready.get();
    }

    void stop() noexcept {
        if (!running_.exchange(false))
            return;
        service::message::workerStreamMultiplexer().signal(
            service::message::WorkerStreamTask::EdgeProjector);
        for (const auto& stopped : stopped_)
            if (stopped.valid())
                (void)stopped.wait_for(std::chrono::seconds(3));
        stopped_.clear();
        workers_.clear();
    }

  private:
    static constexpr std::size_t kBatchSize = 512;

    ruvia::Task<void> run(ruvia::WebWorkerContext& context, std::size_t index,
                           std::shared_ptr<std::promise<void>> ready,
                           std::shared_ptr<std::promise<void>> stopped) {
        try {
            const auto redis = context.redis();
            std::vector<std::string> streams;
            for (auto streamIndex = index;
                 streamIndex < service::message::shard::kCount;
                 streamIndex += workers_.size())
                streams.push_back(projector_stream::stream(streamIndex));
            for (const auto& stream : streams)
                co_await service::message::redis::ensureGroup(redis, stream,
                                                                kEdgeIngressGroup);
            co_await hydrateAuth(context);
            auto catalog = co_await metadata::hydrate(context);
            ready->set_value();
            bool recovering = true;
            const auto consumer = "service-" + std::to_string(index);
            while (running_.load() && !context.stopToken().stopRequested()) {
                std::vector<service::message::redis::StreamBatch> batches;
                bool readFailed = false;
                try {
                    batches = recovering
                        ? co_await service::message::redis::claimGroupMany(
                              redis, streams, kEdgeIngressGroup, consumer,
                              kBatchSize)
                        : co_await service::message::redis::readGroupMany(
                              redis, streams, kEdgeIngressGroup, consumer, ">",
                              kBatchSize);
                } catch (const std::exception& error) {
                    if (context.stopToken().stopRequested())
                        break;
                    std::cerr << "edge stream read failed: " << error.what() << '\n';
                    recovering = true;
                    readFailed = true;
                }
                if (readFailed) {
                    (void)co_await ruvia::sleepFor(context.worker(),
                                                   std::chrono::milliseconds(250));
                    continue;
                }
                if (recovering && batches.empty())
                    recovering = false;
                if (batches.empty()) {
                    co_await service::message::workerStreamMultiplexer().wait(
                        index, service::message::WorkerStreamTask::EdgeProjector,
                        context.stopToken());
                    continue;
                }
                bool projectionFailed = false;
                try {
                    for (const auto& batch : batches) {
                        std::vector<service::message::StreamMessage> telemetry;
                        telemetry.reserve(batch.messages.size());
                        for (const auto& message : batch.messages) {
                            const bool metadataEvent =
                                message.get("kind") == projector_stream::kMetadataKind;
                            if (metadataEvent) {
                                const auto nodeId = message.get("node_id");
                                if (!nodeId.empty()) {
                                    auto snapshot = co_await metadata::loadNodeFromDatabase(
                                        context, nodeId);
                                    co_await metadata::storeNode(
                                        redis, nodeId, snapshot, false);
                                    catalog[std::string(nodeId)] = std::move(snapshot);
                                }
                                continue;
                            }
                            const bool ingressEvent =
                                message.get("kind") == projector_stream::kIngressKind;
                            if (ingressEvent)
                                co_await project(context, catalog, message.get("wire"),
                                                 message.get("received_at_ms"), telemetry);
                        }
                        co_await service::telemetry::PersistenceRuntime::ingest(context,
                                                                                 telemetry);
                        co_await service::message::redis::acknowledgeAndDeleteMany(
                            redis, batch.stream, kEdgeIngressGroup, batch.messages);
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
        NOW(), NOW())
ON CONFLICT (platform_id, imei) DO UPDATE
SET model = EXCLUDED.model, software_version = EXCLUDED.software_version,
    hostname = EXCLUDED.hostname, architecture = EXCLUDED.architecture,
	    openwrt_release = EXCLUDED.openwrt_release,
	    capability = EXCLUDED.capability || jsonb_build_object(
	        'terminal', CASE lower(COALESCE(edge_node.capability->>'terminal', ''))
	                        WHEN 'true' THEN true WHEN 't' THEN true WHEN '1' THEN true
	                        ELSE false END),
    mobile = EXCLUDED.mobile,
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
                                                          hello.log_level()));
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
            context.redis(), message::commandResultStream(0), fields,
            service::message::workerForPartition(0),
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
                context.redis(), {"PEXPIRE", key, "300000"});
            updated = true;
        }
        if (updated)
            co_await service::telemetry::latest::bumpRealtimeRevision(context.redis());
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

    std::vector<ruvia::WebWorkerHandle> workers_;
    std::vector<std::shared_future<void>> stopped_;
    std::atomic_bool running_{false};
};

} // namespace service::edge
