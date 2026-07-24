#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

#include <openssl/rand.h>
#include <ruvia/core/Timer.h>
#include <ruvia/web/Context.h>

#include "service/common/http.h"
#include "service/common/uuid.h"
#include "service/middleware/auth.h"
#include "service/features/edge/protocol.h"
#include "service/domains/edge/edge.types.h"

namespace service::edge {

class EdgeService {
  public:
    ruvia::Task<EdgePageDto> list(ruvia::Context& c, std::int64_t page, std::int64_t pageSize,
                                  std::optional<std::string> keyword,
                                  std::optional<std::string> status) {
        page = std::max<std::int64_t>(1, page);
        pageSize = std::clamp<std::int64_t>(pageSize, 1, 100);
        std::string where = " WHERE 1=1";
        std::vector<ruvia::DbValue> params;
        std::optional<std::string> pattern;
        if (keyword && !keyword->empty()) {
            pattern = "%" + *keyword + "%";
            params.emplace_back(*pattern);
            where += " AND (imei ILIKE $" + std::to_string(params.size()) +
                     " OR COALESCE(name, '') ILIKE $" + std::to_string(params.size()) +
                     " OR model ILIKE $" + std::to_string(params.size()) + ")";
        }
        if (status && !status->empty()) {
            params.emplace_back(*status);
            where += " AND enrollment_status = $" + std::to_string(params.size());
        }
        const auto count = co_await c.db().query("SELECT COUNT(*) FROM edge_node" + where, params);
        const auto total = integer(count.rows().front()[0].text());
        auto listParams = params;
        listParams.emplace_back(pageSize);
        const auto limit = listParams.size();
        listParams.emplace_back((page - 1) * pageSize);
        const auto offset = listParams.size();
        const auto rows = co_await c.db().query(
            nodeSelect() + where + " ORDER BY created_at DESC LIMIT $" +
                std::to_string(limit) + " OFFSET $" + std::to_string(offset),
            listParams);
        ruvia::List<EdgeNodeDto> nodes(c.resource());
        for (const auto& row : rows.rows()) {
            auto& node = nodes.emplace(c);
            fillNode(c, node, row);
        }
        EdgePageDto result(c);
        result.list(std::move(nodes))
            .total(total)
            .page(page)
            .pageSize(pageSize)
            .totalPages(total == 0 ? 0 : (total + pageSize - 1) / pageSize);
        co_return result;
    }

    ruvia::Task<EdgeNodeDto> detail(ruvia::Context& c, std::string_view id) {
        const auto rows = co_await c.db().query(nodeSelect() + " WHERE id = $1::uuid LIMIT 1",
                                                service::common::dbParams(id));
        if (rows.rows().empty())
            service::common::fail(17001, "边缘节点不存在", 404);
        EdgeNodeDto node(c);
        fillNode(c, node, rows.rows().front());
        node.interfaces(co_await interfaces(c, id));
        node.networks(co_await networks(c, id));
        node.serialPorts(co_await serialPorts(c, id));
        node.platforms(co_await platforms(c, id));
        node.tasks(co_await tasks(c, id));
        co_return node;
    }

    ruvia::Task<void> setEnrollment(ruvia::Context& c, std::string_view id,
                                    const EnrollmentBody& body) {
        const auto principal = service::middleware::requireAuth(c);
        const std::string status(body.status()->view());
        const std::string name = body.name() ? std::string(body.name()->view()) : std::string{};
        std::string_view stage = "database";
        try {
            const auto updated = co_await c.db().query(R"sql(
UPDATE edge_node
SET enrollment_status = $1::text, name = NULLIF($2::text, ''), approved_by = $3::uuid,
    approved_at = CASE WHEN $1::text = 'approved' THEN NOW() ELSE NULL END, updated_at = NOW()
WHERE id = $4::uuid
RETURNING imei)sql",
                                                         service::common::dbParams(
                                                             status, name, principal.userId, id));
            if (updated.rows().empty())
                service::common::fail(17001, "边缘节点不存在", 404);
            const auto key = protocol::authKey(updated.rows().front()[0].text());
            const auto value = std::string(id) + "|" + status;
            stage = "redis";
            co_await c.redis().set(key, value);
        } catch (const std::exception& error) {
            std::cerr << "edge enrollment update failed: stage=" << stage << " node_id=" << id
                      << " status=" << status << " error=" << error.what() << '\n';
            throw;
        }
    }

    ruvia::Task<void> renameNode(ruvia::Context& c, std::string_view id,
                                 const NodeNameBody& body) {
        const std::string name(body.name()->view());
        const auto updated = co_await c.db().query(R"sql(
UPDATE edge_node SET name = $1::text, updated_at = NOW()
WHERE id = $2::uuid
RETURNING id)sql",
                                                   service::common::dbParams(name, id));
        if (updated.rows().empty())
            service::common::fail(17001, "边缘节点不存在", 404);
    }

    ruvia::Task<void> queueNetwork(ruvia::Context& c, std::string_view nodeId,
                                   const NetworkBody& body) {
        const auto networkConfigVersion = co_await requireNetworkManagement(c, nodeId);
        const auto& configs = *body.interfaces();
        const auto available = co_await manageableInterfaces(c, nodeId);
        std::unordered_set<std::string> names;
        std::unordered_set<std::string> previousNames;
        std::unordered_set<std::string> devices;
        const auto taskId = service::common::nextUuidV7();
        auto envelope = protocol::outbound(nodeId);
        auto* request = envelope.mutable_network_config_request();
        std::uint8_t requestId[16]{};
        protocol::uuidBytes(taskId, requestId);
        request->set_request_id(protocol::bytes(requestId, 16));
        for (const auto& config : configs) {
            const std::string operation(config.operation()->view());
            const std::string name(config.name()->view());
            const std::string previousName =
                config.previousName() ? std::string(config.previousName()->view())
                                      : std::string{};
            if (!names.emplace(name).second)
                service::common::fail(17003, "同一请求不能重复配置逻辑接口 " + name, 400);
            if (name == "loopback")
                service::common::fail(17003, "loopback 接口不允许远程修改", 400);
            if (!previousName.empty()) {
                if (operation != "upsert" || previousName == name ||
                    previousName == "loopback")
                    service::common::fail(17003, "原逻辑接口名称无效", 400);
                if (networkConfigVersion < 3)
                    service::common::fail(
                        17004, "节点代理版本过旧，请先升级后再修改逻辑接口名称", 409);
                if (!previousNames.emplace(previousName).second)
                    service::common::fail(
                        17003, "同一请求不能重复修改原逻辑接口 " + previousName, 400);
            }

            auto* item = request->add_interfaces();
            item->set_logical_name(name);
            if (operation == "delete") {
                item->set_operation(pb::NETWORK_CONFIG_DELETE);
            } else {
                item->set_operation(pb::NETWORK_CONFIG_UPSERT);
                item->set_previous_logical_name(previousName);
                const std::string mode =
                    config.mode() ? std::string(config.mode()->view()) : std::string{};
                const std::string device =
                    config.device() ? std::string(config.device()->view()) : std::string{};
                const std::string ip =
                    config.ip() ? std::string(config.ip()->view()) : std::string{};
                const std::string gateway =
                    config.gateway() ? std::string(config.gateway()->view()) : std::string{};
                const bool bridge = config.bridge() && *config.bridge();
                const auto prefix =
                    config.prefixLength() ? static_cast<std::uint32_t>(*config.prefixLength()) : 0U;
                const auto& ports = config.bridgePorts();
                validateNetworkConfig(name, mode, device, bridge, ports, ip, prefix, gateway,
                                      available, devices);
                item->set_mode(mode == "static" ? pb::NETWORK_ADDRESS_STATIC
                                                 : pb::NETWORK_ADDRESS_DHCP);
                item->set_bridge(bridge);
                item->set_device(device);
                item->set_name(bridge ? "br-" + name : device);
                if (ports) {
                    for (const auto& port : *ports)
                        item->add_bridge_ports(port.view());
                }
                item->set_ip(ip);
                item->set_prefix_length(prefix);
                item->set_gateway(gateway);
            }
        }
        request->set_rollback_timeout_sec(
            static_cast<std::uint32_t>(*body.rollbackTimeoutSec()));
        co_await createNetworkTaskAndQueue(c, nodeId, taskId, configs.size(), envelope);
    }

    ruvia::Task<void> queueModem(ruvia::Context& c, std::string_view nodeId,
                                 const ModemControlBody& body) {
        co_await requireNodeCapability(c, nodeId, "modemControl", "4G 控制");
        const std::string action(body.action()->view());
        const std::string apn = body.apn() ? std::string(body.apn()->view()) : std::string{};
        if (action == "set_apn" && apn.empty())
            service::common::fail(17012, "设置 APN 时不能为空", 400);

        const auto taskId = service::common::nextUuidV7();
        auto envelope = protocol::outbound(nodeId);
        auto* request = envelope.mutable_modem_control_request();
        std::uint8_t requestId[16]{};
        protocol::uuidBytes(taskId, requestId);
        request->set_request_id(protocol::bytes(requestId, sizeof(requestId)));
        request->set_action(action == "set_apn" ? pb::MODEM_CONTROL_SET_APN
                                                  : pb::MODEM_CONTROL_REDIAL);
        request->set_apn(apn);
        const std::string json = "{\"action\":\"" + action + "\",\"apn\":\"" +
                                 jsonEscape(apn) + "\"}";
        co_await createTaskAndQueue(c, nodeId, taskId, "modem", json, envelope);
    }

    ruvia::Task<std::string> queuePlatform(ruvia::Context& c, std::string_view nodeId,
                                           const PlatformBody& body) {
        co_await requireNodeCapability(c, nodeId, "platformConfig", "多平台配置");
        const std::string platformId = body.platformId()
                                           ? std::string(body.platformId()->view())
                                           : service::common::nextUuidV7();
        if (platformId == protocol::kBootstrapPlatformId)
            service::common::fail(17007, "固化平台不能被修改", 400);
        const std::string name(body.name()->view());
        const std::string baseUrl(body.baseUrl()->view());
        validatePlatformUrl(baseUrl);
        const auto taskId = service::common::nextUuidV7();
        auto envelope = protocol::outbound(nodeId);
        auto* request = envelope.mutable_platform_config_request();
        std::uint8_t bytes[16]{};
        protocol::uuidBytes(taskId, bytes);
        request->set_request_id(protocol::bytes(bytes, 16));
        if (!protocol::uuidBytes(platformId, bytes))
            service::common::fail(17008, "平台 ID 无效", 400);
        request->set_target_platform_id(protocol::bytes(bytes, 16));
        request->set_operation(pb::PLATFORM_CONFIG_UPSERT);
        request->set_name(name);
        request->set_url(baseUrl);
        if (body.enrollmentToken())
            request->set_enrollment_token(body.enrollmentToken()->view());
        request->set_enabled(*body.enabled());
        request->set_priority(static_cast<std::uint32_t>(*body.priority()));
        request->set_reconnect_interval_sec(
            static_cast<std::uint32_t>(*body.reconnectIntervalSec()));
        request->set_outbox_max_bytes(static_cast<std::uint32_t>(*body.outboxMaxBytes()));
        const std::string json = "{\"platform_id\":\"" + platformId +
                                 "\",\"name\":\"" + jsonEscape(name) +
                                 "\",\"base_url\":\"" + jsonEscape(baseUrl) + "\"}";
        const auto principal = service::middleware::requireAuth(c);
        (void)co_await c.db().execute(R"sql(
INSERT INTO edge_node_platform(node_id, platform_id, name, base_url, enabled, priority,
                               reconnect_interval_sec, outbox_max_bytes, status)
VALUES ($1::uuid, $2::uuid, $3, $4, $5, $6, $7, $8,
        jsonb_build_object('state', 'pending', 'message', ''))
ON CONFLICT (node_id, platform_id) DO UPDATE
SET name = EXCLUDED.name, base_url = EXCLUDED.base_url, enabled = EXCLUDED.enabled,
    priority = EXCLUDED.priority, reconnect_interval_sec = EXCLUDED.reconnect_interval_sec,
    outbox_max_bytes = EXCLUDED.outbox_max_bytes,
    status = jsonb_build_object('state', 'pending', 'message', ''), updated_at = NOW())sql",
                                      service::common::dbParams(
                                          nodeId, platformId, name, baseUrl, *body.enabled(),
                                          *body.priority(), *body.reconnectIntervalSec(),
                                          *body.outboxMaxBytes()));
        co_await insertTask(c, nodeId, taskId, "platform_upsert", json, principal.userId);
        co_await push(c, nodeId, envelope);
        co_return platformId;
    }

    ruvia::Task<void> deletePlatform(ruvia::Context& c, std::string_view nodeId,
                                     std::string_view platformId) {
        co_await requireNodeCapability(c, nodeId, "platformConfig", "多平台配置");
        if (platformId == protocol::kBootstrapPlatformId)
            service::common::fail(17007, "固化平台不能被删除", 400);
        const auto taskId = service::common::nextUuidV7();
        auto envelope = protocol::outbound(nodeId);
        auto* request = envelope.mutable_platform_config_request();
        std::uint8_t bytes[16]{};
        protocol::uuidBytes(taskId, bytes);
        request->set_request_id(protocol::bytes(bytes, 16));
        protocol::uuidBytes(platformId, bytes);
        request->set_target_platform_id(protocol::bytes(bytes, 16));
        request->set_operation(pb::PLATFORM_CONFIG_DELETE);
        const auto principal = service::middleware::requireAuth(c);
        const std::string json = "{\"platform_id\":\"" + std::string(platformId) + "\"}";
        co_await insertTask(c, nodeId, taskId, "platform_delete", json, principal.userId);
        co_await push(c, nodeId, envelope);
    }

    ruvia::Task<void> validateFirmwareTarget(ruvia::Context& c, std::string_view nodeId) {
        co_await requireNodeCapability(c, nodeId, "firmwareUpdate", "远程刷写");
    }

    ruvia::Task<void> queueFirmware(ruvia::Context& c, std::string_view nodeId,
                                    std::string_view firmwareId, bool keepSettings) {
        co_await validateFirmwareTarget(c, nodeId);
        const std::string firmwareIdText(firmwareId);
        const auto rows = co_await c.db().query(R"sql(
SELECT version, sha256, size_bytes, download_token
FROM edge_firmware WHERE id = $1::uuid LIMIT 1)sql",
                                                service::common::dbParams(firmwareIdText));
        if (rows.rows().empty())
            service::common::fail(17009, "固件不存在", 404);
        const auto& row = rows.rows().front();
        const auto taskId = service::common::nextUuidV7();
        auto envelope = protocol::outbound(nodeId);
        auto* request = envelope.mutable_firmware_update_request();
        std::uint8_t bytes[32]{};
        protocol::uuidBytes(taskId, bytes);
        request->set_request_id(protocol::bytes(bytes, 16));
        const auto download = std::string(protocol::kPublicPlatformUrl) +
                              "/edge/v1/firmware/" + firmwareIdText + "/download?token=" +
                              std::string(row[3].text());
        request->set_download_url(download);
        if (!hex(row[1].text(), bytes, 32))
            service::common::fail(17010, "固件摘要无效", 500);
        request->set_sha256(protocol::bytes(bytes, 32));
        request->set_size_bytes(static_cast<std::uint64_t>(integer(row[2].text())));
        request->set_version(row[0].text());
        request->set_keep_settings(keepSettings);
        const std::string json = "{\"firmware_id\":\"" + firmwareIdText +
                                 "\",\"version\":\"" + jsonEscape(row[0].text()) + "\"}";
        co_await createTaskAndQueue(c, nodeId, taskId, "firmware", json, envelope);
    }

    ruvia::Task<ruvia::List<FirmwareDto>> firmwares(ruvia::Context& c) {
        const auto rows = co_await c.db().query(R"sql(
SELECT id::text, version, file_name, sha256, size_bytes, created_at::text
FROM edge_firmware ORDER BY created_at DESC LIMIT 100)sql");
        ruvia::List<FirmwareDto> result(c.resource());
        for (const auto& row : rows.rows()) {
            auto& item = result.emplace(c);
            item.id(row[0].text())
                .version(row[1].text())
                .fileName(row[2].text())
                .sha256(row[3].text())
                .sizeBytes(integer(row[4].text()))
                .createdAt(row[5].text());
        }
        co_return result;
    }

    ruvia::Task<void> registerFirmware(ruvia::Context& c, std::string_view id,
                                       std::string version, std::string fileName,
                                       const std::filesystem::path& path,
                                       std::string sha256, std::int64_t size) {
        const auto principal = service::middleware::requireAuth(c);
        const auto token = randomToken();
        const auto storagePath = path.string();
        (void)co_await c.db().execute(R"sql(
INSERT INTO edge_firmware(id, version, file_name, storage_path, sha256, size_bytes,
                          download_token, created_by)
VALUES ($1::uuid, $2, $3, $4, $5, $6, $7, $8::uuid))sql",
                                      service::common::dbParams(
                                          id, version, fileName, storagePath, sha256, size,
                                          token, principal.userId));
    }

    ruvia::Task<std::pair<std::filesystem::path, std::string>> firmwareDownload(
        ruvia::Context& c, std::string_view id, std::string_view token) {
        const auto rows = co_await c.db().query(R"sql(
SELECT storage_path, file_name FROM edge_firmware
WHERE id = $1::uuid AND download_token = $2 LIMIT 1)sql",
                                                service::common::dbParams(id, token));
        if (rows.rows().empty())
            service::common::fail(17009, "固件不存在或下载凭据无效", 404);
        co_return std::pair<std::filesystem::path, std::string>{
            std::filesystem::path(std::string(rows.rows().front()[0].text())),
            std::string(rows.rows().front()[1].text())};
    }

    ruvia::Task<TerminalTicketDto> terminalTicket(ruvia::Context& c,
                                                  std::string_view nodeId) {
        const auto rows = co_await c.db().query(R"sql(
SELECT enrollment_status, COALESCE((capability->>'terminal')::boolean, false),
       (last_seen_at IS NOT NULL AND last_seen_at > NOW() - INTERVAL '90 seconds')
FROM edge_node WHERE id = $1::uuid LIMIT 1)sql",
                                                service::common::dbParams(nodeId));
        if (rows.rows().empty())
            service::common::fail(17001, "边缘节点不存在", 404);
        if (rows.rows().front()[0].text() != "approved" ||
            rows.rows().front()[1].text() != "t")
            service::common::fail(17018, "节点未检测到 ttyd", 409);
        if (rows.rows().front()[2].text() != "t")
            service::common::fail(17019, "节点当前离线", 409);
        const auto sessionKey = "iot:edge:session:" + std::string(nodeId);
        const auto session = co_await c.redis().get(sessionKey);
        if (!session)
            service::common::fail(17019, "节点当前离线", 409);
        const auto ticket = service::common::nextUuidV7();
        const auto key = "iot:edge:terminal:ticket:" + ticket;
        co_await c.redis().setEx(key, std::chrono::seconds(60), nodeId);
        TerminalTicketDto result(c);
        result.ticket(ticket);
        co_return result;
    }

    ruvia::Task<LogsDto> logs(ruvia::Context& c, std::string_view nodeId,
                              const LogsQuery& query) {
        co_await requireNodeCapability(c, nodeId, "logs", "节点日志");
        const auto session = co_await c.redis().get(sessionKey(nodeId));
        if (!session)
            service::common::fail(17019, "节点当前离线", 409);

        const auto requestId = service::common::nextUuidV7();
        std::uint8_t bytes[16]{};
        protocol::uuidBytes(requestId, bytes);
        auto envelope = protocol::outbound(nodeId);
        auto* request = envelope.mutable_log_request();
        request->set_request_id(protocol::bytes(bytes, sizeof(bytes)));
        request->set_limit(static_cast<std::uint32_t>(
            std::clamp<std::int64_t>(*query.limit(), 1, 48)));
        if (query.level())
            request->set_level(query.level()->view());
        if (query.source())
            request->set_source(query.source()->view());
        co_await push(c, nodeId, envelope);

        const auto key = logResultKey(requestId);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            if (auto payload = co_await c.redis().get(key)) {
                (void)co_await c.redis().del(key);
                pb::LogResult result;
                if (!result.ParseFromArray(payload->data(), static_cast<int>(payload->size())))
                    service::common::fail(17020, "节点日志回包解析失败", 502);
                if (!result.success())
                    service::common::fail(17020, result.message(), 502);
                LogsDto output(c);
                ruvia::List<LogLineDto> lines(c.resource());
                for (const auto& line : result.lines()) {
                    auto& item = lines.emplace(c);
                    item.time(line.time_ms())
                        .level(line.level())
                        .source(line.source())
                        .message(line.message())
                        .detail(line.detail());
                }
                output.lines(std::move(lines));
                co_return output;
            }
            (void)co_await ruvia::sleepFor(c.worker(), std::chrono::milliseconds(50));
        }
        service::common::fail(17020, "节点日志请求超时", 504);
    }

    ruvia::Task<void> setLogLevel(ruvia::Context& c, std::string_view nodeId,
                                  const LogLevelBody& body) {
        co_await requireNodeCapability(c, nodeId, "logs", "节点日志");
        const auto session = co_await c.redis().get(sessionKey(nodeId));
        if (!session)
            service::common::fail(17019, "节点当前离线", 409);

        const auto level = std::string(body.level()->view());
        const auto requestId = service::common::nextUuidV7();
        std::uint8_t bytes[16]{};
        protocol::uuidBytes(requestId, bytes);
        auto envelope = protocol::outbound(nodeId);
        auto* request = envelope.mutable_log_level_request();
        request->set_request_id(protocol::bytes(bytes, sizeof(bytes)));
        request->set_level(level);
        co_await push(c, nodeId, envelope);

        const auto key = logLevelResultKey(requestId);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            if (auto payload = co_await c.redis().get(key)) {
                (void)co_await c.redis().del(key);
                pb::LogLevelResult result;
                if (!result.ParseFromArray(payload->data(), static_cast<int>(payload->size())))
                    service::common::fail(17020, "节点日志等级回包解析失败", 502);
                if (!result.success())
                    service::common::fail(17020, result.message(), 502);
                const auto currentLevel = result.level().empty() ? level : result.level();
                (void)co_await c.db().execute(R"sql(
UPDATE edge_node
SET status = jsonb_set(
        jsonb_set(status, '{log}', COALESCE(status->'log', '{}'::jsonb), true),
        '{log,level}', to_jsonb($1::text), true),
    updated_at = NOW()
WHERE id = $2::uuid)sql",
                                               service::common::dbParams(currentLevel, nodeId));
                co_return;
            }
            (void)co_await ruvia::sleepFor(c.worker(), std::chrono::milliseconds(50));
        }
        service::common::fail(17020, "节点日志等级请求超时", 504);
    }

  private:
    static std::string nodeSelect() {
        return R"sql(SELECT id::text, imei, COALESCE(name, ''), model, software_version,
       hostname, architecture, openwrt_release, enrollment_status,
       (last_seen_at IS NOT NULL AND last_seen_at > NOW() - INTERVAL '90 seconds'),
       COALESCE(last_seen_at::text, ''), created_at::text,
       COALESCE((status->'config'->>'activeVersion')::bigint, 0),
       COALESCE((status->'config'->>'desiredVersion')::bigint, 0),
       COALESCE(status->'config'->>'state', 'idle'),
       COALESCE(status->'config'->>'message', ''),
       COALESCE((status->'outbox'->>'records')::bigint, 0),
       COALESCE((status->'outbox'->>'bytes')::bigint, 0),
       COALESCE(status->'log'->>'level', 'info'),
       COALESCE((capability->>'networkConfig')::boolean, false),
       COALESCE((capability->>'networkConfigVersion')::bigint, 0),
       COALESCE((capability->>'firmwareUpdate')::boolean, false),
       COALESCE((capability->>'platformConfig')::boolean, false),
       COALESCE((capability->>'deviceConfig')::boolean, false),
       COALESCE((capability->>'modemControl')::boolean, false),
       COALESCE((capability->>'terminal')::boolean, false),
       COALESCE((capability->>'logs')::boolean, false),
       COALESCE((mobile->>'available')::boolean, false),
       COALESCE(mobile->>'simState', 'unknown'),
       COALESCE(mobile->>'iccid', ''),
       COALESCE((mobile->'signal'->>'csq')::bigint, 99),
       COALESCE((mobile->'signal'->>'rssiDbm')::bigint, -1),
       COALESCE((mobile->'signal'->>'percent')::bigint, 0),
       COALESCE((mobile->>'registered')::boolean, false),
       COALESCE((mobile->>'registrationStatus')::bigint, -1),
       COALESCE(mobile->>'apn', ''),
       COALESCE(mobile->>'operator', ''),
       COALESCE((mobile->>'connected')::boolean, false),
       COALESCE(mobile->>'ipv4', ''),
       COALESCE((SELECT task.status FROM edge_task task
                 WHERE task.node_id = edge_node.id AND task.task_type = 'firmware'
                 ORDER BY task.created_at DESC LIMIT 1), ''),
       COALESCE((SELECT (task.result->>'progressPercent')::bigint FROM edge_task task
                 WHERE task.node_id = edge_node.id AND task.task_type = 'firmware'
                 ORDER BY task.created_at DESC LIMIT 1), 0),
       COALESCE((SELECT (task.result->>'downloadedBytes')::bigint FROM edge_task task
                 WHERE task.node_id = edge_node.id AND task.task_type = 'firmware'
                 ORDER BY task.created_at DESC LIMIT 1), 0),
       COALESCE((SELECT (task.result->>'totalBytes')::bigint FROM edge_task task
                 WHERE task.node_id = edge_node.id AND task.task_type = 'firmware'
                 ORDER BY task.created_at DESC LIMIT 1), 0),
       COALESCE((SELECT task.result->>'message' FROM edge_task task
                 WHERE task.node_id = edge_node.id AND task.task_type = 'firmware'
                 ORDER BY task.created_at DESC LIMIT 1), '')
FROM edge_node)sql";
    }

    static std::int64_t integer(std::string_view value) {
        std::int64_t result{};
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
        return error == std::errc{} && end == value.data() + value.size() ? result : 0;
    }

    template <typename Row> static void fillNode(ruvia::Context& c, EdgeNodeDto& node, const Row& row) {
        NodeStatusDto status(c);
        ConfigStatusDto config(c);
        config.activeVersion(integer(row[12].text()))
            .desiredVersion(integer(row[13].text()))
            .state(row[14].text())
            .message(row[15].text());
        OutboxStatusDto outbox(c);
        outbox.records(integer(row[16].text())).bytes(integer(row[17].text()));
        LogStatusDto log(c);
        log.level(row[18].text());
        status.online(row[9].text() == "t")
            .lastSeenAt(row[10].text())
            .config(std::move(config))
            .outbox(std::move(outbox))
            .log(std::move(log));

        CapabilityDto capability(c);
        capability.networkConfig(row[19].text() == "t")
            .networkConfigVersion(integer(row[20].text()))
            .firmwareUpdate(row[21].text() == "t")
            .platformConfig(row[22].text() == "t")
            .deviceConfig(row[23].text() == "t")
            .modemControl(row[24].text() == "t")
            .terminal(row[25].text() == "t")
            .logs(row[26].text() == "t");

        SignalDto signal(c);
        signal.csq(integer(row[30].text()))
            .rssiDbm(integer(row[31].text()))
            .percent(integer(row[32].text()));
        MobileDto mobile(c);
        mobile.available(row[27].text() == "t")
            .simState(row[28].text())
            .iccid(row[29].text())
            .signal(std::move(signal))
            .registered(row[33].text() == "t")
            .registrationStatus(integer(row[34].text()))
            .apn(row[35].text())
            .operatorName(row[36].text())
            .connected(row[37].text() == "t")
            .ipv4(row[38].text());

        FirmwareStatusDto firmware(c);
        firmware.state(row[39].text())
            .progressPercent(integer(row[40].text()))
            .downloadedBytes(integer(row[41].text()))
            .totalBytes(integer(row[42].text()))
            .message(row[43].text());

        node.id(row[0].text())
            .imei(row[1].text())
            .name(row[2].text())
            .model(row[3].text())
            .softwareVersion(row[4].text())
            .hostname(row[5].text())
            .architecture(row[6].text())
            .openwrtRelease(row[7].text())
            .enrollmentStatus(row[8].text())
            .status(std::move(status))
            .capability(std::move(capability))
            .mobile(std::move(mobile))
            .firmware(std::move(firmware))
            .createdAt(row[11].text());
    }

    static std::vector<std::string> split(std::string_view value) {
        std::vector<std::string> result;
        while (!value.empty()) {
            const auto comma = value.find(',');
            result.emplace_back(value.substr(0, comma));
            if (comma == std::string_view::npos)
                break;
            value.remove_prefix(comma + 1);
        }
        return result;
    }

    static ruvia::Task<ruvia::List<InterfaceDto>> interfaces(ruvia::Context& c,
                                                             std::string_view id) {
        const auto rows = co_await c.db().query(R"sql(
SELECT name, display_name, COALESCE(mac, ''), is_up, is_bridge, COALESCE(ipv4, ''),
       COALESCE(prefix_length, 0), COALESCE(gateway, ''),
       COALESCE((SELECT string_agg(value, ',' ORDER BY value)
                 FROM jsonb_array_elements_text(bridge_ports) AS values(value)), '')
FROM edge_node_interface WHERE node_id = $1::uuid ORDER BY name)sql",
                                                service::common::dbParams(id));
        ruvia::List<InterfaceDto> result(c.resource());
        for (const auto& row : rows.rows()) {
            auto& item = result.emplace(c);
            ruvia::List<ruvia::String> ports(c.resource());
            for (const auto& port : split(row[8].text()))
                if (!port.empty())
                    ports.emplace(port, c.resource());
            item.name(row[0].text())
                .displayName(row[1].text())
                .mac(row[2].text())
                .up(row[3].text() == "t")
                .bridge(row[4].text() == "t")
                .ipv4(row[5].text())
                .prefixLength(integer(row[6].text()))
                .gateway(row[7].text())
                .bridgePorts(std::move(ports));
        }
        co_return result;
    }

    static ruvia::Task<ruvia::List<NetworkDto>> networks(ruvia::Context& c,
                                                         std::string_view id) {
        const auto rows = co_await c.db().query(R"sql(
SELECT name, address_mode, device, is_up, is_bridge, COALESCE(ipv4, ''),
       COALESCE(prefix_length, 0), COALESCE(gateway, ''),
       COALESCE((SELECT string_agg(value, ',' ORDER BY value)
                 FROM jsonb_array_elements_text(bridge_ports) AS values(value)), '')
FROM edge_node_network WHERE node_id = $1::uuid ORDER BY name)sql",
                                                service::common::dbParams(id));
        ruvia::List<NetworkDto> result(c.resource());
        for (const auto& row : rows.rows()) {
            auto& item = result.emplace(c);
            ruvia::List<ruvia::String> ports(c.resource());
            for (const auto& port : split(row[8].text()))
                if (!port.empty())
                    ports.emplace(port, c.resource());
            item.name(row[0].text())
                .mode(row[1].text())
                .device(row[2].text())
                .up(row[3].text() == "t")
                .bridge(row[4].text() == "t")
                .ipv4(row[5].text())
                .prefixLength(integer(row[6].text()))
                .gateway(row[7].text())
                .bridgePorts(std::move(ports));
        }
        co_return result;
    }

    static ruvia::Task<ruvia::List<SerialDto>> serialPorts(ruvia::Context& c,
                                                           std::string_view id) {
        const auto rows = co_await c.db().query(R"sql(
SELECT path, display_name, available, rs485 FROM edge_node_serial
WHERE node_id = $1::uuid ORDER BY path)sql",
                                                service::common::dbParams(id));
        ruvia::List<SerialDto> result(c.resource());
        for (const auto& row : rows.rows()) {
            auto& item = result.emplace(c);
            item.path(row[0].text())
                .displayName(row[1].text())
                .available(row[2].text() == "t")
                .rs485(row[3].text() == "t");
        }
        co_return result;
    }

    static ruvia::Task<ruvia::List<PlatformDto>> platforms(ruvia::Context& c,
                                                           std::string_view id) {
        const auto rows = co_await c.db().query(R"sql(
SELECT platform_id::text, name, base_url, enabled, priority, reconnect_interval_sec,
       outbox_max_bytes, COALESCE(status->>'state', 'pending'), COALESCE(status->>'message', '')
FROM edge_node_platform WHERE node_id = $1::uuid ORDER BY priority, name)sql",
                                                service::common::dbParams(id));
        ruvia::List<PlatformDto> result(c.resource());
        for (const auto& row : rows.rows()) {
            auto& item = result.emplace(c);
            PlatformStatusDto status(c);
            status.state(row[7].text()).message(row[8].text());
            item.platformId(row[0].text())
                .name(row[1].text())
                .baseUrl(row[2].text())
                .enabled(row[3].text() == "t")
                .priority(integer(row[4].text()))
                .reconnectIntervalSec(integer(row[5].text()))
                .outboxMaxBytes(integer(row[6].text()))
                .status(std::move(status));
        }
        co_return result;
    }

    static ruvia::Task<ruvia::List<TaskDto>> tasks(ruvia::Context& c, std::string_view id) {
        const auto rows = co_await c.db().query(R"sql(
SELECT id::text, task_type, status, COALESCE(result->>'message', ''),
       COALESCE((result->>'progressPercent')::bigint, 0),
       COALESCE((result->>'downloadedBytes')::bigint, 0),
       COALESCE((result->>'totalBytes')::bigint, 0),
       created_at::text, updated_at::text
FROM edge_task WHERE node_id = $1::uuid ORDER BY created_at DESC LIMIT 50)sql",
                                                service::common::dbParams(id));
        ruvia::List<TaskDto> result(c.resource());
        for (const auto& row : rows.rows()) {
            auto& item = result.emplace(c);
            item.id(row[0].text())
                .taskType(row[1].text())
                .status(row[2].text())
                .message(row[3].text())
                .progressPercent(integer(row[4].text()))
                .downloadedBytes(integer(row[5].text()))
                .totalBytes(integer(row[6].text()))
                .createdAt(row[7].text())
                .updatedAt(row[8].text());
        }
        co_return result;
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

    static bool ipv4(std::string_view input, std::uint32_t& output) {
        output = 0;
        for (int part = 0; part < 4; ++part) {
            const auto dot = input.find('.');
            const auto token = input.substr(0, dot);
            unsigned value{};
            const auto [end, error] =
                std::from_chars(token.data(), token.data() + token.size(), value);
            if (token.empty() || error != std::errc{} || end != token.data() + token.size() ||
                value > 255 || (token.size() > 1 && token.front() == '0'))
                return false;
            output = (output << 8U) | value;
            if (part == 3)
                return dot == std::string_view::npos;
            if (dot == std::string_view::npos)
                return false;
            input.remove_prefix(dot + 1);
        }
        return false;
    }

    static void validateStaticNetwork(std::string_view ipText, std::uint32_t prefix,
                                      std::string_view gatewayText) {
        std::uint32_t address{};
        if (!ipv4(ipText, address) || prefix == 0 || prefix > 30)
            service::common::fail(17003, "静态 IPv4 地址或前缀长度无效", 400);
        const auto mask = 0xffffffffU << (32U - prefix);
        const auto host = address & ~mask;
        if (host == 0 || host == ~mask)
            service::common::fail(17003, "静态 IPv4 不能是网络地址或广播地址", 400);
        if (!gatewayText.empty()) {
            std::uint32_t gateway{};
            if (!ipv4(gatewayText, gateway) || (gateway & mask) != (address & mask) ||
                gateway == address || (gateway & ~mask) == 0 || (gateway & ~mask) == ~mask)
                service::common::fail(17003, "网关必须是同网段内不同的合法主机地址", 400);
        }
    }

    static ruvia::Task<std::unordered_set<std::string>>
    manageableInterfaces(ruvia::Context& c, std::string_view nodeId) {
        const auto rows = co_await c.db().query(
            "SELECT name FROM edge_node_interface "
            "WHERE node_id = $1::uuid AND name <> 'lo' AND is_bridge = FALSE",
            service::common::dbParams(nodeId));
        std::unordered_set<std::string> result;
        for (const auto& row : rows.rows())
            result.emplace(row[0].text());
        co_return result;
    }

    template <typename Ports>
    static void validateNetworkConfig(std::string_view name, std::string_view mode,
                                      std::string_view device, bool bridge, const Ports& ports,
                                      std::string_view ip, std::uint32_t prefix,
                                      std::string_view gateway,
                                      const std::unordered_set<std::string>& available,
                                      std::unordered_set<std::string>& selected) {
        if (name.size() > 15 || (bridge && name.size() > 12))
            service::common::fail(
                17003, bridge ? "网桥逻辑名称不能超过 12 个字符"
                              : "逻辑接口名称不能超过 15 个字符",
                400);
        if (mode != "dhcp" && mode != "static")
            service::common::fail(17003, "地址模式只支持 DHCP 或静态 IPv4", 400);

        const auto useDevice = [&](std::string_view value) {
            const std::string text(value);
            if (!available.contains(text))
                service::common::fail(
                    17003, "网卡 " + text + " 不可管理、未上报或属于受保护的 4G 上联", 400);
            if (!selected.emplace(text).second)
                service::common::fail(17003, "网卡 " + text + " 在同一请求中被重复占用", 400);
        };

        if (bridge) {
            if (!device.empty())
                service::common::fail(17003, "网桥不能同时指定单一设备", 400);
            if (!ports || ports->empty())
                service::common::fail(17003, "网桥至少需要一个成员网卡", 400);
            for (const auto& port : *ports)
                useDevice(port.view());
        } else {
            if (device.empty())
                service::common::fail(17003, "非网桥接口必须选择一个网卡", 400);
            if (ports && !ports->empty())
                service::common::fail(17003, "非网桥接口不能配置网桥成员", 400);
            useDevice(device);
        }

        if (mode == "static") {
            validateStaticNetwork(ip, prefix, gateway);
        } else if (!ip.empty() || prefix != 0 || !gateway.empty()) {
            service::common::fail(17003, "DHCP 接口不能携带静态 IPv4 配置", 400);
        }
    }

    static ruvia::Task<std::int64_t> requireNetworkManagement(
        ruvia::Context& c, std::string_view nodeId) {
        const auto rows = co_await c.db().query(R"sql(
SELECT enrollment_status, COALESCE((capability->>'networkConfig')::boolean, false),
       COALESCE((capability->>'networkConfigVersion')::bigint, 0)
FROM edge_node WHERE id = $1::uuid LIMIT 1)sql",
                                                service::common::dbParams(nodeId));
        if (rows.rows().empty())
            service::common::fail(17001, "边缘节点不存在", 404);
        if (rows.rows().front()[0].text() != "approved")
            service::common::fail(17002, "边缘节点尚未批准注册", 409);
        if (rows.rows().front()[1].text() != "t")
            service::common::fail(17004, "网络配置不可用", 409);
        const auto version = integer(rows.rows().front()[2].text());
        if (version < 2)
            service::common::fail(17004, "节点代理版本过旧，请先升级后再管理网络", 409);
        co_return version;
    }

    static void validatePlatformUrl(std::string_view value) {
        const std::string_view prefix = value.starts_with("https://") ? "https://"
                                        : value.starts_with("http://") ? "http://"
                                                                       : "";
        if (prefix.empty() || value.size() <= prefix.size())
            service::common::fail(17006, "平台地址必须使用 http:// 或 https://", 400);
        for (const unsigned char ch : value)
            if (ch < 0x20U || ch == 0x7fU)
                service::common::fail(17006, "平台地址包含非法字符", 400);
    }

    static bool hex(std::string_view value, std::uint8_t* output, std::size_t size) {
        if (value.size() != size * 2)
            return false;
        for (std::size_t index = 0; index < size; ++index) {
            const int high = protocol::hexDigit(value[index * 2]);
            const int low = protocol::hexDigit(value[index * 2 + 1]);
            if (high < 0 || low < 0)
                return false;
            output[index] = static_cast<std::uint8_t>((high << 4U) | low);
        }
        return true;
    }

    static std::string randomToken() {
        std::array<unsigned char, 32> bytes{};
        if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
            throw std::runtime_error("cannot generate firmware token");
        constexpr char digits[] = "0123456789abcdef";
        std::string output;
        output.reserve(64);
        for (const auto byte : bytes) {
            output.push_back(digits[byte >> 4U]);
            output.push_back(digits[byte & 0x0fU]);
        }
        return output;
    }

    static ruvia::Task<void> requireNodeCapability(ruvia::Context& c, std::string_view nodeId,
                                                   std::string_view key,
                                                   std::string_view feature) {
        const auto rows = co_await c.db().query(
            "SELECT enrollment_status, COALESCE((capability->>$1)::boolean, false) "
            "FROM edge_node WHERE id = $2::uuid LIMIT 1",
            service::common::dbParams(key, nodeId));
        if (rows.rows().empty())
            service::common::fail(17001, "边缘节点不存在", 404);
        if (rows.rows().front()[0].text() != "approved")
            service::common::fail(17002, "边缘节点尚未批准注册", 409);
        if (rows.rows().front()[1].text() != "t")
            service::common::fail(17004, std::string(feature) + "不可用", 409);
    }

    static ruvia::Task<void> insertTask(ruvia::Context& c, std::string_view nodeId,
                                        std::string_view taskId, std::string_view type,
                                        std::string_view json, std::string_view userId) {
        (void)co_await c.db().execute(R"sql(
INSERT INTO edge_task(id, node_id, task_type, request, created_by)
VALUES ($1::uuid, $2::uuid, $3, $4::jsonb, $5::uuid))sql",
                                      service::common::dbParams(taskId, nodeId, type, json, userId));
    }

    static ruvia::Task<void> push(ruvia::Context& c, std::string_view nodeId,
                                  const pb::Envelope& envelope) {
        const auto wire = protocol::encode(envelope);
        if (wire.empty())
            service::common::fail(17005, "边缘命令编码失败", 500);
        const auto key = "iot:edge:egress:" + std::string(nodeId);
        (void)co_await c.redis().rpush(key, wire);
        co_await c.redis().ltrim(key, -100, -1);
    }

    static std::string sessionKey(std::string_view nodeId) {
        return "iot:edge:session:" + std::string(nodeId);
    }

    static std::string logResultKey(std::string_view requestId) {
        return "iot:edge:logs:" + std::string(requestId);
    }

    static std::string logLevelResultKey(std::string_view requestId) {
        return "iot:edge:logs:level:" + std::string(requestId);
    }

    static ruvia::Task<void> createTaskAndQueue(ruvia::Context& c, std::string_view nodeId,
                                                std::string_view taskId,
                                                std::string_view type,
                                                std::string_view json,
                                                const pb::Envelope& envelope) {
        const auto principal = service::middleware::requireAuth(c);
        co_await insertTask(c, nodeId, taskId, type, json, principal.userId);
        co_await push(c, nodeId, envelope);
    }

    static ruvia::Task<void>
    createNetworkTaskAndQueue(ruvia::Context& c, std::string_view nodeId,
                              std::string_view taskId, std::size_t interfaceCount,
                              const pb::Envelope& envelope) {
        const auto principal = service::middleware::requireAuth(c);
        (void)co_await c.db().execute(R"sql(
INSERT INTO edge_task(id, node_id, task_type, request, created_by)
VALUES ($1::uuid, $2::uuid, 'network',
        jsonb_build_object('interfaceCount', $3::bigint), $4::uuid))sql",
                                      service::common::dbParams(
                                          taskId, nodeId,
                                          static_cast<std::int64_t>(interfaceCount),
                                          principal.userId));
        co_await push(c, nodeId, envelope);
    }
};

inline EdgeService& edgeService() {
    static EdgeService instance;
    return instance;
}

} // namespace service::edge
