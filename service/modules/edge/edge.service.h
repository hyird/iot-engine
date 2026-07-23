#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <stdexcept>
#include <utility>
#include <vector>

#include <openssl/rand.h>
#include <ruvia/web/Context.h>

#include "service/common/http.h"
#include "service/common/uuid.h"
#include "service/middleware/auth.h"
#include "service/modules/edge/edge.protocol.h"
#include "service/modules/edge/edge.types.h"

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
            fillNode(node, row);
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
        fillNode(node, rows.rows().front());
        node.interfaces(co_await interfaces(c, id));
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
        co_await requireNodeCapability(c, nodeId, "supports_network_config", "网络配置");
        const std::string ip(body.ip()->view());
        const std::string netmask(body.netmask()->view());
        const std::string gateway = body.gateway() ? std::string(body.gateway()->view()) : "";
        const auto prefix = validateNetwork(ip, netmask, gateway);
        const auto taskId = service::common::nextUuidV7();
        auto envelope = protocol::outbound(nodeId);
        envelope.which_payload = iot_edge_v1_Envelope_network_config_request_tag;
        auto& request = envelope.payload.network_config_request;
        std::uint8_t requestId[16]{};
        protocol::uuidBytes(taskId, requestId);
        protocol::setBytes(&request.request_id, sizeof(request.request_id.bytes), requestId, 16);
        request.interfaces_count = 1;
        auto& item = request.interfaces[0];
        copy(item.name, "br-lan");
        item.mode = iot_edge_v1_NetworkAddressMode_NETWORK_ADDRESS_STATIC;
        copy(item.ip, ip);
        item.prefix_length = prefix;
        copy(item.gateway, gateway);
        item.bridge = true;
        request.rollback_timeout_sec = static_cast<std::uint32_t>(*body.rollbackTimeoutSec());
        const std::string json = "{\"interface\":\"br-lan\",\"ip\":\"" + jsonEscape(ip) +
                                 "\",\"netmask\":\"" + jsonEscape(netmask) +
                                 "\",\"gateway\":\"" + jsonEscape(gateway) + "\"}";
        co_await createTaskAndQueue(c, nodeId, taskId, "network", json, envelope);
    }

    ruvia::Task<std::string> queuePlatform(ruvia::Context& c, std::string_view nodeId,
                                           const PlatformBody& body) {
        co_await requireNodeCapability(c, nodeId, "supports_platform_config", "多平台配置");
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
        envelope.which_payload = iot_edge_v1_Envelope_platform_config_request_tag;
        auto& request = envelope.payload.platform_config_request;
        std::uint8_t bytes[16]{};
        protocol::uuidBytes(taskId, bytes);
        protocol::setBytes(&request.request_id, sizeof(request.request_id.bytes), bytes, 16);
        if (!protocol::uuidBytes(platformId, bytes))
            service::common::fail(17008, "平台 ID 无效", 400);
        protocol::setBytes(&request.target_platform_id,
                           sizeof(request.target_platform_id.bytes), bytes, 16);
        request.operation = iot_edge_v1_PlatformConfigOperation_PLATFORM_CONFIG_UPSERT;
        copy(request.name, name);
        copy(request.url, baseUrl);
        if (body.enrollmentToken())
            copy(request.enrollment_token, body.enrollmentToken()->view());
        request.enabled = *body.enabled();
        request.priority = static_cast<std::uint32_t>(*body.priority());
        request.reconnect_interval_sec =
            static_cast<std::uint32_t>(*body.reconnectIntervalSec());
        request.outbox_max_bytes = static_cast<std::uint32_t>(*body.outboxMaxBytes());
        const std::string json = "{\"platform_id\":\"" + platformId +
                                 "\",\"name\":\"" + jsonEscape(name) +
                                 "\",\"base_url\":\"" + jsonEscape(baseUrl) + "\"}";
        const auto principal = service::middleware::requireAuth(c);
        (void)co_await c.db().execute(R"sql(
INSERT INTO edge_node_platform(node_id, platform_id, name, base_url, enabled, priority,
                               reconnect_interval_sec, outbox_max_bytes, apply_status)
VALUES ($1::uuid, $2::uuid, $3, $4, $5, $6, $7, $8, 'pending')
ON CONFLICT (node_id, platform_id) DO UPDATE
SET name = EXCLUDED.name, base_url = EXCLUDED.base_url, enabled = EXCLUDED.enabled,
    priority = EXCLUDED.priority, reconnect_interval_sec = EXCLUDED.reconnect_interval_sec,
    outbox_max_bytes = EXCLUDED.outbox_max_bytes, apply_status = 'pending',
    last_message = NULL, updated_at = NOW())sql",
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
        co_await requireNodeCapability(c, nodeId, "supports_platform_config", "多平台配置");
        if (platformId == protocol::kBootstrapPlatformId)
            service::common::fail(17007, "固化平台不能被删除", 400);
        const auto taskId = service::common::nextUuidV7();
        auto envelope = protocol::outbound(nodeId);
        envelope.which_payload = iot_edge_v1_Envelope_platform_config_request_tag;
        auto& request = envelope.payload.platform_config_request;
        std::uint8_t bytes[16]{};
        protocol::uuidBytes(taskId, bytes);
        protocol::setBytes(&request.request_id, sizeof(request.request_id.bytes), bytes, 16);
        protocol::uuidBytes(platformId, bytes);
        protocol::setBytes(&request.target_platform_id,
                           sizeof(request.target_platform_id.bytes), bytes, 16);
        request.operation = iot_edge_v1_PlatformConfigOperation_PLATFORM_CONFIG_DELETE;
        const auto principal = service::middleware::requireAuth(c);
        const std::string json = "{\"platform_id\":\"" + std::string(platformId) + "\"}";
        co_await insertTask(c, nodeId, taskId, "platform_delete", json, principal.userId);
        co_await push(c, nodeId, envelope);
    }

    ruvia::Task<void> validateFirmwareTarget(ruvia::Context& c, std::string_view nodeId) {
        co_await requireNodeCapability(c, nodeId, "supports_firmware_update", "远程刷写");
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
        envelope.which_payload = iot_edge_v1_Envelope_firmware_update_request_tag;
        auto& request = envelope.payload.firmware_update_request;
        std::uint8_t bytes[32]{};
        protocol::uuidBytes(taskId, bytes);
        protocol::setBytes(&request.request_id, sizeof(request.request_id.bytes), bytes, 16);
        const auto download = std::string(protocol::kPublicPlatformUrl) +
                              "/edge/v1/firmware/" + firmwareIdText + "/download?token=" +
                              std::string(row[3].text());
        copy(request.download_url, download);
        if (!hex(row[1].text(), bytes, 32))
            service::common::fail(17010, "固件摘要无效", 500);
        protocol::setBytes(&request.sha256, sizeof(request.sha256.bytes), bytes, 32);
        request.size_bytes = static_cast<std::uint64_t>(integer(row[2].text()));
        copy(request.version, row[0].text());
        request.keep_settings = keepSettings;
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
SELECT enrollment_status, ttyd_available,
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

  private:
    static std::string nodeSelect() {
        return R"sql(SELECT id::text, imei, COALESCE(name, ''), model, software_version,
       hostname, architecture, openwrt_release, enrollment_status,
       (last_seen_at IS NOT NULL AND last_seen_at > NOW() - INTERVAL '90 seconds'),
       supports_network_config, supports_firmware_update, supports_platform_config,
       supports_device_config, ttyd_available, active_config_version, desired_config_version,
       config_status, config_message, outbox_records, outbox_bytes,
       COALESCE(last_seen_at::text, ''), created_at::text
FROM edge_node)sql";
    }

    template <std::size_t Size> static void copy(char (&output)[Size], std::string_view input) {
        const auto size = std::min(input.size(), Size - 1);
        std::memcpy(output, input.data(), size);
        output[size] = '\0';
    }

    static std::int64_t integer(std::string_view value) {
        std::int64_t result{};
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
        return error == std::errc{} && end == value.data() + value.size() ? result : 0;
    }

    template <typename Row> static void fillNode(EdgeNodeDto& node, const Row& row) {
        node.id(row[0].text())
            .imei(row[1].text())
            .name(row[2].text())
            .model(row[3].text())
            .softwareVersion(row[4].text())
            .hostname(row[5].text())
            .architecture(row[6].text())
            .openwrtRelease(row[7].text())
            .enrollmentStatus(row[8].text())
            .online(row[9].text() == "t")
            .supportsNetworkConfig(row[10].text() == "t")
            .supportsFirmwareUpdate(row[11].text() == "t")
            .supportsPlatformConfig(row[12].text() == "t")
            .supportsDeviceConfig(row[13].text() == "t")
            .ttydAvailable(row[14].text() == "t")
            .activeConfigVersion(integer(row[15].text()))
            .desiredConfigVersion(integer(row[16].text()))
            .configStatus(row[17].text())
            .configMessage(row[18].text())
            .outboxRecords(integer(row[19].text()))
            .outboxBytes(integer(row[20].text()))
            .lastSeenAt(row[21].text())
            .createdAt(row[22].text());
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
       outbox_max_bytes, apply_status, COALESCE(last_message, '')
FROM edge_node_platform WHERE node_id = $1::uuid ORDER BY priority, name)sql",
                                                service::common::dbParams(id));
        ruvia::List<PlatformDto> result(c.resource());
        for (const auto& row : rows.rows()) {
            auto& item = result.emplace(c);
            item.platformId(row[0].text())
                .name(row[1].text())
                .baseUrl(row[2].text())
                .enabled(row[3].text() == "t")
                .priority(integer(row[4].text()))
                .reconnectIntervalSec(integer(row[5].text()))
                .outboxMaxBytes(integer(row[6].text()))
                .applyStatus(row[7].text())
                .lastMessage(row[8].text());
        }
        co_return result;
    }

    static ruvia::Task<ruvia::List<TaskDto>> tasks(ruvia::Context& c, std::string_view id) {
        const auto rows = co_await c.db().query(R"sql(
SELECT id::text, task_type, status, COALESCE(result->>'message', ''),
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
                .createdAt(row[4].text())
                .updatedAt(row[5].text());
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

    static std::uint32_t validateNetwork(std::string_view ipText, std::string_view maskText,
                                         std::string_view gatewayText) {
        std::uint32_t address{}, mask{};
        if (!ipv4(ipText, address) || !ipv4(maskText, mask))
            service::common::fail(17003, "br-lan IP 或掩码格式无效", 400);
        bool zeroSeen = false;
        std::uint32_t prefix = 0;
        for (int bit = 31; bit >= 0; --bit) {
            const bool one = (mask & (1U << static_cast<unsigned>(bit))) != 0;
            if (zeroSeen && one)
                service::common::fail(17003, "br-lan 掩码必须连续", 400);
            if (one)
                ++prefix;
            else
                zeroSeen = true;
        }
        if (prefix == 0 || prefix > 30)
            service::common::fail(17003, "br-lan 掩码必须为 /1 到 /30", 400);
        const auto host = address & ~mask;
        if (host == 0 || host == ~mask)
            service::common::fail(17003, "br-lan IP 不能是网络地址或广播地址", 400);
        if (!gatewayText.empty()) {
            std::uint32_t gateway{};
            if (!ipv4(gatewayText, gateway) || (gateway & mask) != (address & mask) ||
                gateway == address || (gateway & ~mask) == 0 || (gateway & ~mask) == ~mask)
                service::common::fail(17003, "网关必须是同网段内不同的合法主机地址", 400);
        }
        return prefix;
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
                                                   std::string_view column,
                                                   std::string_view feature) {
        const auto rows = co_await c.db().query(
            "SELECT enrollment_status, " + std::string(column) +
                " FROM edge_node WHERE id = $1::uuid LIMIT 1",
            service::common::dbParams(nodeId));
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
                                  const iot_edge_v1_Envelope& envelope) {
        const auto wire = protocol::encode(envelope);
        if (wire.empty())
            service::common::fail(17005, "边缘命令编码失败", 500);
        const auto key = "iot:edge:egress:" + std::string(nodeId);
        (void)co_await c.redis().rpush(key, wire);
        co_await c.redis().ltrim(key, -100, -1);
    }

    static ruvia::Task<void> createTaskAndQueue(ruvia::Context& c, std::string_view nodeId,
                                                std::string_view taskId,
                                                std::string_view type,
                                                std::string_view json,
                                                const iot_edge_v1_Envelope& envelope) {
        const auto principal = service::middleware::requireAuth(c);
        co_await insertTask(c, nodeId, taskId, type, json, principal.userId);
        co_await push(c, nodeId, envelope);
    }
};

inline EdgeService& edgeService() {
    static EdgeService instance;
    return instance;
}

} // namespace service::edge
