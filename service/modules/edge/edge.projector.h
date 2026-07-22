#pragma once

#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
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
                      supports_platform_config, updated_at)
VALUES ($1::uuid, $2::uuid, $3, $4, $5, $6, $7, $8, $9, $10, $11, NOW())
ON CONFLICT (platform_id, imei) DO UPDATE
SET model = EXCLUDED.model, software_version = EXCLUDED.software_version,
    hostname = EXCLUDED.hostname, architecture = EXCLUDED.architecture,
    openwrt_release = EXCLUDED.openwrt_release,
    supports_network_config = EXCLUDED.supports_network_config,
    supports_firmware_update = EXCLUDED.supports_firmware_update,
    supports_platform_config = EXCLUDED.supports_platform_config,
    updated_at = NOW()
RETURNING id::text, enrollment_status)sql",
                                                     service::common::dbParams(
                                                         candidate, protocol::kBootstrapPlatformId,
                                                         hello.imei, hello.model,
                                                         hello.software_version, hello.hostname,
                                                         hello.architecture, hello.openwrt_release,
                                                         hello.supports_network_config,
                                                         hello.supports_firmware_update,
                                                         hello.supports_platform_config));
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
                     last_seen_at = NOW(), updated_at = NOW()
WHERE id = $4::uuid)sql",
                                            service::common::dbParams(
                                                heartbeat.active_config_version,
                                                heartbeat.outbox_records, heartbeat.outbox_bytes,
                                                nodeId));
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
