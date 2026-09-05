#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
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
#include "service/common/timestamp.h"
#include "service/common/uuid.h"
#include "service/middleware/auth.h"
#include "service/features/edge/dispatch.h"
#include "service/features/edge/config.h"
#include "service/features/edge/firmware.h"
#include "service/features/edge/protocol.h"
#include "service/domains/edge/edge.types.h"

namespace service::edge {

class EdgeService {
  public:
    ruvia::Task<EdgePageDto> list(ruvia::Context& c, std::int64_t page, std::int64_t pageSize,
                                  std::optional<std::string> keyword,
                                  std::optional<std::string> status,
                                  std::optional<std::string> groupId) {
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
                     " OR COALESCE((SELECT name FROM edge_node_group WHERE id = edge_node.group_id), '') ILIKE $" +
                     std::to_string(params.size()) +
                     " OR model ILIKE $" + std::to_string(params.size()) + ")";
        }
        if (status && !status->empty()) {
            params.emplace_back(*status);
            where += " AND enrollment_status = $" + std::to_string(params.size());
        }
        if (groupId && !groupId->empty()) {
            if (*groupId == "ungrouped") {
                where += " AND group_id IS NULL";
            } else {
                params.emplace_back(*groupId);
                const auto parameter = std::to_string(params.size());
                where += " AND group_id IN (WITH RECURSIVE selected_group AS ("
                         "SELECT id FROM edge_node_group WHERE id = $" + parameter +
                         "::uuid AND deleted_at IS NULL UNION ALL "
                         "SELECT child.id FROM edge_node_group child JOIN selected_group parent "
                         "ON child.parent_id = parent.id WHERE child.deleted_at IS NULL) "
                         "SELECT id FROM selected_group)";
            }
        }
        const auto count = co_await c.db().query("SELECT COUNT(*) FROM edge_node" + where, params);
        const auto total = integer(count.front()[0].value().value_or(std::string_view{}));
        auto listParams = params;
        listParams.emplace_back(pageSize);
        const auto limit = listParams.size();
        listParams.emplace_back((page - 1) * pageSize);
        const auto offset = listParams.size();
        const auto rows = co_await c.db().query(
            nodeSelect() + where + " ORDER BY created_at DESC LIMIT $" +
                std::to_string(limit) + " OFFSET $" + std::to_string(offset),
            listParams);
        ruvia::BoxedArray<EdgeNodeDto> nodes(ruvia::ModelOptions{.resource = c.resource()});
        for (const auto& row : rows) {
            auto& node = nodes.emplace(c);
            co_await fillNode(c, node, row);
        }
        EdgePageDto result(c);
        result.set<"list">(std::move(nodes))
            .set<"total">(total)
            .set<"page">(page)
            .set<"pageSize">(pageSize)
            .set<"totalPages">(total == 0 ? 0 : (total + pageSize - 1) / pageSize);
        co_return result;
    }

    ruvia::Task<ruvia::BoxedArray<EdgeGroupDto>> groups(ruvia::Context& c) {
        const auto rows = co_await c.db().query(R"sql(
SELECT group_item.id::text, group_item.name, COALESCE(group_item.parent_id::text, ''),
       group_item.status, group_item.sort_order, COALESCE(group_item.remark, ''),
       (SELECT COUNT(*) FROM edge_node node WHERE node.group_id = group_item.id)
FROM edge_node_group group_item
WHERE group_item.deleted_at IS NULL
ORDER BY group_item.sort_order, group_item.id)sql");
        ruvia::BoxedArray<EdgeGroupDto> result(
            ruvia::ModelOptions{.resource = c.resource()});
        for (const auto& row : rows) {
            auto& item = result.emplace(c);
            item.set<"id">(row[0].value().value_or(std::string_view{}));
            item.set<"name">(row[1].value().value_or(std::string_view{}));
            item.set<"parentId">(row[2].value().value_or(std::string_view{}));
            item.set<"status">(row[3].value().value_or(std::string_view{}));
            item.set<"sortOrder">(integer(row[4].value().value_or(std::string_view{})));
            item.set<"remark">(row[5].value().value_or(std::string_view{}));
            item.set<"nodeCount">(integer(row[6].value().value_or(std::string_view{})));
        }
        co_return result;
    }

    ruvia::Task<void> createGroup(ruvia::Context& c, const EdgeGroupBody& body) {
        const auto principal = service::middleware::requireAuth(c);
        const auto name = std::string(body.get<"name">()->view());
        const auto parentId = body.get<"parentId">()
                                  ? std::string(body.get<"parentId">()->view())
                                  : std::string{};
        co_await validateGroupParent(c, parentId, {});
        const auto duplicate = co_await c.db().query(
            "SELECT 1 FROM edge_node_group WHERE name = $1 AND deleted_at IS NULL",
            service::common::dbParams(name));
        if (!duplicate.empty())
            service::common::fail(17002, "边缘节点分组名称已存在", 409);
        const auto status = body.get<"status">()
                                ? std::string(body.get<"status">()->view())
                                : std::string{"enabled"};
        const auto sortOrder = body.get<"sortOrder">()
                                   ? static_cast<std::int64_t>(*body.get<"sortOrder">())
                                   : 0;
        const auto remark = body.get<"remark">()
                                ? std::string(body.get<"remark">()->view())
                                : std::string{};
        const auto groupId = service::common::nextUuidV7();
        (void)co_await c.db().execute(R"sql(
INSERT INTO edge_node_group(id, name, parent_id, status, sort_order, remark, created_by)
VALUES ($1::uuid, $2, NULLIF($3, '')::uuid, $4::status_enum, $5, NULLIF($6, ''), $7::uuid))sql",
                                      service::common::dbParams(
                                          groupId, name, parentId,
                                          status, sortOrder, remark, principal.userId));
    }

    ruvia::Task<void> updateGroup(ruvia::Context& c, std::string_view id,
                                  const EdgeGroupBody& body) {
        const auto current = co_await c.db().query(
            "SELECT 1 FROM edge_node_group WHERE id = $1::uuid AND deleted_at IS NULL",
            service::common::dbParams(id));
        if (current.empty())
            service::common::fail(17001, "边缘节点分组不存在", 404);
        const auto name = std::string(body.get<"name">()->view());
        const auto parentId = body.get<"parentId">()
                                  ? std::string(body.get<"parentId">()->view())
                                  : std::string{};
        co_await validateGroupParent(c, parentId, id);
        const auto duplicate = co_await c.db().query(
            "SELECT 1 FROM edge_node_group WHERE name = $1 AND id <> $2::uuid "
            "AND deleted_at IS NULL",
            service::common::dbParams(name, id));
        if (!duplicate.empty())
            service::common::fail(17002, "边缘节点分组名称已存在", 409);
        const auto status = body.get<"status">()
                                ? std::string(body.get<"status">()->view())
                                : std::string{"enabled"};
        const auto sortOrder = body.get<"sortOrder">()
                                   ? static_cast<std::int64_t>(*body.get<"sortOrder">())
                                   : 0;
        const auto remark = body.get<"remark">()
                                ? std::string(body.get<"remark">()->view())
                                : std::string{};
        (void)co_await c.db().execute(R"sql(
UPDATE edge_node_group
SET name = $2, parent_id = NULLIF($3, '')::uuid, status = $4::status_enum,
    sort_order = $5, remark = NULLIF($6, ''), updated_at = NOW()
WHERE id = $1::uuid AND deleted_at IS NULL)sql",
                                      service::common::dbParams(
                                          id, name, parentId, status, sortOrder, remark));
    }

    ruvia::Task<void> removeGroup(ruvia::Context& c, std::string_view id) {
        const auto current = co_await c.db().query(
            "SELECT 1 FROM edge_node_group WHERE id = $1::uuid AND deleted_at IS NULL",
            service::common::dbParams(id));
        if (current.empty())
            service::common::fail(17001, "边缘节点分组不存在", 404);
        const auto removed = co_await c.db().query(R"sql(
UPDATE edge_node_group target
SET deleted_at = NOW(), updated_at = NOW()
WHERE target.id = $1::uuid AND target.deleted_at IS NULL
  AND NOT EXISTS (
      SELECT 1 FROM edge_node_group child
      WHERE child.parent_id = target.id AND child.deleted_at IS NULL)
  AND NOT EXISTS (
      SELECT 1 FROM edge_node node WHERE node.group_id = target.id)
RETURNING target.id)sql",
                                                 service::common::dbParams(id));
        if (removed.empty())
            service::common::fail(17004, "请先移除子分组和边缘节点", 409);
    }

    ruvia::Task<EdgeNodeDto> detail(ruvia::Context& c, std::string_view id) {
        const auto rows = co_await c.db().query(nodeSelect() + " WHERE id = $1::uuid LIMIT 1",
                                                service::common::dbParams(id));
        if (rows.empty())
            service::common::fail(17001, "边缘节点不存在", 404);
        EdgeNodeDto node(c);
        co_await fillNode(c, node, rows.front());
        node.set<"interfaces">(co_await interfaces(c, id));
        node.set<"networks">(co_await networks(c, id));
        node.set<"serialPorts">(co_await serialPorts(c, id));
        node.set<"tasks">(co_await tasks(c, id));
        co_return node;
    }

    ruvia::Task<void> setEnrollment(ruvia::Context& c, std::string_view id,
                                    const EnrollmentBody& body) {
        const auto principal = service::middleware::requireAuth(c);
        const auto& maybeStatus = body.get<"status">();
        if (!maybeStatus || maybeStatus->view().empty())
            service::common::fail(17003, "注册状态不能为空", 400);
        const std::string status(maybeStatus->view());
        if (status != "approved")
            service::common::fail(17003, "注册状态无效", 400);
        const std::string name = body.get<"name">() ? std::string(body.get<"name">()->view()) : std::string{};
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
            if (updated.empty())
                service::common::fail(17001, "边缘节点不存在", 404);
            const auto key = protocol::authKey(updated.front()[0].value().value_or(std::string_view{}));
            const auto value = std::string(id) + "|" + status;
            stage = "redis";
            co_await c.redis().set(key, value);
            if (status == "approved") {
                try {
                    // Approval must be useful without a second manual click. Queue the
                    // current device snapshot immediately; the gateway will deliver it
                    // as soon as the pending WebSocket becomes an enrolled session.
                    (void)co_await configService().queueSnapshot(c, id);
                } catch (const std::exception& error) {
                    // A node may be approved before its first capability projection or
                    // before it has any configurable devices. Approval itself must not
                    // fail in those compatible/empty cases; the existing heartbeat retry
                    // and manual sync endpoint remain available.
                    std::cerr << "edge enrollment initial config sync skipped: node_id=" << id
                              << " error=" << error.what() << '\n';
                } catch (...) {
                    std::cerr << "edge enrollment initial config sync skipped: node_id=" << id
                              << " error=unknown exception\n";
                }
            }
        } catch (const std::exception& error) {
            std::cerr << "edge enrollment update failed: stage=" << stage << " node_id=" << id
                      << " status=" << status << " error=" << error.what() << '\n';
            throw;
        }
    }

    ruvia::Task<void> removeEnrollment(ruvia::Context& c, std::string_view id) {
        const auto rows = co_await c.db().query(R"sql(
SELECT imei, enrollment_status,
       EXISTS(SELECT 1 FROM link WHERE edge_node_id = edge_node.id)
FROM edge_node
WHERE id = $1::uuid
LIMIT 1)sql",
                                                service::common::dbParams(id));
        if (rows.empty())
            service::common::fail(17001, "边缘节点不存在", 404);
        const auto imei = std::string(rows.front()[0].value().value_or(std::string_view{}));
        const auto status = rows.front()[1].value().value_or(std::string_view{});
        if (status != "pending")
            service::common::fail(17021, "只能删除待处理的注册申请", 409);
        if (rows.front()[2].value().value_or(std::string_view{}) == "t")
            service::common::fail(17021, "注册申请仍被采集链路引用，无法删除", 409);

        const std::array<std::string, 5> keys{
            "iot:edge:session:" + std::string(id),
            "iot:edge:metadata:" + std::string(id),
            "iot:edge:egress:" + std::string(id),
            "iot:edge:config:" + std::string(id),
            "iot:edge:config-revision:" + std::string(id),
        };
        co_await c.redis().set(protocol::authKey(imei),
                               std::string(id) + "|pending");
        for (const auto& key : keys)
            (void)co_await c.redis().del(key);
        const auto removed = co_await c.db().query(R"sql(
DELETE FROM edge_node
WHERE id = $1::uuid AND enrollment_status = 'pending'
RETURNING id)sql",
                                                   service::common::dbParams(id));
        if (removed.empty())
            service::common::fail(17021, "注册状态已变化，请刷新后重试", 409);
        (void)co_await c.redis().del(protocol::authKey(imei));
    }

    ruvia::Task<void> renameNode(ruvia::Context& c, std::string_view id,
                                 const NodeNameBody& body) {
        const auto& maybeName = body.get<"name">();
        if (!maybeName || maybeName->view().empty())
            service::common::fail(17003, "节点名称不能为空", 400);
        const std::string name(maybeName->view());
        const auto updated = co_await c.db().query(R"sql(
UPDATE edge_node SET name = $1::text, updated_at = NOW()
WHERE id = $2::uuid
RETURNING id)sql",
                                                   service::common::dbParams(name, id));
        if (updated.empty())
            service::common::fail(17001, "边缘节点不存在", 404);
    }

    ruvia::Task<void> setNodeGroup(ruvia::Context& c, std::string_view id,
                                   const NodeGroupBody& body) {
        const auto& maybeGroup = body.get<"groupId">();
        if (!maybeGroup)
            service::common::fail(17003, "节点分组参数不能为空", 400);
        const std::string groupId(maybeGroup->view());
        if (!groupId.empty()) {
            const auto group = co_await c.db().query(
                "SELECT 1 FROM edge_node_group WHERE id = $1::uuid "
                "AND status = 'enabled' AND deleted_at IS NULL",
                service::common::dbParams(groupId));
            if (group.empty())
                service::common::fail(17001, "边缘节点分组不存在或已停用", 404);
        }
        const auto updated = co_await c.db().query(
            "UPDATE edge_node SET group_id = NULLIF($1, '')::uuid, updated_at = NOW() "
            "WHERE id = $2::uuid RETURNING id",
            service::common::dbParams(groupId, id));
        if (updated.empty())
            service::common::fail(17001, "边缘节点不存在", 404);
    }

    ruvia::Task<void> queueNetwork(ruvia::Context& c, std::string_view nodeId,
                                   const NetworkBody& body) {
        const auto networkConfigVersion = co_await requireNetworkManagement(c, nodeId);
        const auto& maybeConfigs = body.get<"interfaces">();
        if (!maybeConfigs || maybeConfigs->empty())
            service::common::fail(17003, "至少配置一个网络接口", 400);
        const auto& configs = *maybeConfigs;
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
            if (!config.get<"operation">() || config.get<"operation">()->view().empty())
                service::common::fail(17003, "网络接口操作不能为空", 400);
            if (!config.get<"name">() || config.get<"name">()->view().empty())
                service::common::fail(17003, "逻辑接口名称不能为空", 400);
            const std::string operation(config.get<"operation">()->view());
            const std::string name(config.get<"name">()->view());
            if (operation != "upsert" && operation != "delete")
                service::common::fail(17003, "网络接口操作只支持 upsert 或 delete", 400);
            const std::string previousName =
                config.get<"previousName">() ? std::string(config.get<"previousName">()->view())
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
                    config.get<"mode">() ? std::string(config.get<"mode">()->view()) : std::string{};
                const std::string device = config.get<"device">()
                                               ? std::string(config.get<"device">()->view())
                                               : std::string{};
                const std::string ip =
                    config.get<"ip">() ? std::string(config.get<"ip">()->view()) : std::string{};
                const std::string gateway =
                    config.get<"gateway">() ? std::string(config.get<"gateway">()->view()) : std::string{};
                const bool bridge = config.get<"bridge">() && *config.get<"bridge">();
                const auto prefix =
                    config.get<"prefixLength">() ? static_cast<std::uint32_t>(*config.get<"prefixLength">()) : 0U;
                const auto& ports = config.get<"bridgePorts">();
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
        const auto rollbackTimeoutSec = body.get<"rollbackTimeoutSec">().value_or(60);
        if (rollbackTimeoutSec < 30 || rollbackTimeoutSec > 300)
            service::common::fail(17003, "回滚等待时间必须在 30 - 300 秒之间", 400);
        request->set_rollback_timeout_sec(static_cast<std::uint32_t>(rollbackTimeoutSec));
        co_await createNetworkTaskAndQueue(c, nodeId, taskId, configs.size(), envelope);
    }

    ruvia::Task<void> validateFirmwareTarget(ruvia::Context& c, std::string_view nodeId) {
        co_await requireNodeCapability(c, nodeId, "firmwareUpdate", "远程刷写");
    }

    ruvia::Task<void> queueFirmware(ruvia::Context& c, std::string_view nodeId,
                                    std::string_view firmwareId, bool keepSettings) {
        co_await validateFirmwareTarget(c, nodeId);
        const std::string firmwareIdText(firmwareId);
        const auto rows = co_await c.db().query(R"sql(
SELECT firmware.sha256, firmware.size_bytes, firmware.download_token,
       CASE lower(COALESCE(node.capability->>'firmwareStream', ''))
            WHEN 'true' THEN true WHEN 't' THEN true WHEN '1' THEN true
            ELSE false END
FROM edge_firmware firmware
JOIN edge_node node ON node.id = $2::uuid
WHERE firmware.id = $1::uuid
LIMIT 1)sql",
                                                service::common::dbParams(firmwareIdText, nodeId));
        if (rows.empty())
            service::common::fail(17009, "固件不存在", 404);
        const auto& row = rows.front();
        const auto taskId = service::common::nextUuidV7();
        auto envelope = protocol::outbound(nodeId);
        auto* request = envelope.mutable_firmware_update_request();
        std::uint8_t bytes[32]{};
        protocol::uuidBytes(taskId, bytes);
        const auto requestId = protocol::bytes(bytes, 16);
        std::string download;
        if (row[3].value().value_or(std::string_view{}) != "t") {
            download = std::string(protocol::publicBaseUrl()) +
                       "/edge/v1/firmware/" + firmwareIdText + "/download?token=" +
                       std::string(row[2].value().value_or(std::string_view{}));
        }
        if (!hex(row[0].value().value_or(std::string_view{}), bytes, 32))
            service::common::fail(17010, "固件摘要无效", 500);
        if (!firmware::populateUpdateRequest(
                *request, requestId, download, protocol::bytes(bytes, 32),
                static_cast<std::uint64_t>(integer(row[1].value().value_or(std::string_view{}))), keepSettings))
            service::common::fail(17010, "固件请求元数据无效", 500);
        const std::string json = "{\"firmware_id\":\"" + firmwareIdText + "\"}";
        co_await createTaskAndQueue(c, nodeId, taskId, "firmware", json, envelope);
    }

    ruvia::Task<ruvia::BoxedArray<FirmwareDto>> firmwares(ruvia::Context& c) {
        const auto rows = co_await c.db().query(R"sql(
SELECT id::text, version, file_name, sha256, size_bytes, iot_utc_timestamp(created_at)
FROM edge_firmware ORDER BY created_at DESC LIMIT 100)sql");
        ruvia::BoxedArray<FirmwareDto> result(ruvia::ModelOptions{.resource = c.resource()});
        for (const auto& row : rows) {
            auto& item = result.emplace(c);
            item.set<"id">(row[0].value().value_or(std::string_view{}))
                .set<"version">(row[1].value().value_or(std::string_view{}))
                .set<"fileName">(row[2].value().value_or(std::string_view{}))
                .set<"sha256">(row[3].value().value_or(std::string_view{}))
                .set<"sizeBytes">(integer(row[4].value().value_or(std::string_view{})))
                .set<"createdAt">(row[5].value().value_or(std::string_view{}));
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
        if (rows.empty())
            service::common::fail(17009, "固件不存在或下载凭据无效", 404);
        co_return std::pair<std::filesystem::path, std::string>{
            std::filesystem::path(std::string(rows.front()[0].value().value_or(std::string_view{}))),
            std::string(rows.front()[1].value().value_or(std::string_view{}))};
    }

    ruvia::Task<TerminalTicketDto> terminalTicket(ruvia::Context& c,
                                                  std::string_view nodeId) {
        const auto rows = co_await c.db().query(R"sql(
SELECT enrollment_status,
       CASE lower(COALESCE(capability->>'terminal', ''))
            WHEN 'true' THEN true WHEN 't' THEN true WHEN '1' THEN true
            ELSE false END
FROM edge_node WHERE id = $1::uuid LIMIT 1)sql",
                                                service::common::dbParams(nodeId));
        if (rows.empty())
            service::common::fail(17001, "边缘节点不存在", 404);
        if (rows.front()[0].value().value_or(std::string_view{}) != "approved" ||
            rows.front()[1].value().value_or(std::string_view{}) != "t")
            service::common::fail(17018, "节点未检测到 ttyd", 409);
        const auto sessionKey = "iot:edge:session:" + std::string(nodeId);
        const auto session = co_await c.redis().get(sessionKey);
        if (!session)
            service::common::fail(17019, "节点当前离线", 409);
        const auto ticket = service::common::nextUuidV7();
        const auto key = "iot:edge:terminal:ticket:" + ticket;
        co_await c.redis().set(
            key, nodeId,
            {.expiration = ruvia::RedisSetExpiration::expiresAfter(std::chrono::seconds(60))});
        TerminalTicketDto result(c);
        result.set<"ticket">(ticket);
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
        const auto limit = query.get<"limit">().value_or(48);
        if (limit < 1 || limit > 48)
            service::common::fail(17020, "日志条数必须在 1 - 48 之间", 400);
        request->set_limit(static_cast<std::uint32_t>(limit));
        if (query.get<"level">()) {
            const auto level = std::string(query.get<"level">()->view());
            if (level != "debug" && level != "info" && level != "warn" && level != "error")
                service::common::fail(17020, "日志级别无效", 400);
            request->set_level(level);
        }
        if (query.get<"source">()) {
            const auto sourceValue = std::string(query.get<"source">()->view());
            if (sourceValue.size() > 16)
                service::common::fail(17020, "日志来源不能超过 16 个字符", 400);
            request->set_source(sourceValue);
        }
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
                ruvia::BoxedArray<LogLineDto> lines(
                    ruvia::ModelOptions{.resource = c.resource()});
                for (const auto& line : result.lines()) {
                    auto& item = lines.emplace(c);
                    item.set<"time">(service::common::utcTimestampFromMilliseconds(line.time_ms()))
                        .set<"level">(line.level())
                        .set<"source">(line.source())
                        .set<"message">(line.message())
                        .set<"detail">(line.detail());
                }
                output.set<"lines">(std::move(lines));
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

        const auto& maybeLevel = body.get<"level">();
        if (!maybeLevel || maybeLevel->view().empty())
            service::common::fail(17020, "日志级别不能为空", 400);
        const auto level = std::string(maybeLevel->view());
        if (level != "debug" && level != "info" && level != "warn" && level != "error")
            service::common::fail(17020, "日志级别无效", 400);
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

#ifdef IOT_ENGINE_TESTING
    static std::string nodeSelectForTest() { return nodeSelect(); }
    static std::string tasksQueryForTest() { return taskSelect(); }
#endif

  private:
    static ruvia::Task<void> validateGroupParent(ruvia::Context& c,
                                                  std::string_view parentId,
                                                  std::string_view groupId) {
        if (parentId.empty())
            co_return;
        if (parentId == groupId)
            service::common::fail(17003, "上级分组不能是自身", 409);
        const auto parent = co_await c.db().query(
            "SELECT 1 FROM edge_node_group WHERE id = $1::uuid AND deleted_at IS NULL",
            service::common::dbParams(parentId));
        if (parent.empty())
            service::common::fail(17001, "上级分组不存在", 404);
        if (!groupId.empty()) {
            const auto cycle = co_await c.db().query(R"sql(
WITH RECURSIVE descendants AS (
    SELECT id FROM edge_node_group WHERE parent_id = $1::uuid AND deleted_at IS NULL
    UNION ALL
    SELECT child.id FROM edge_node_group child
    JOIN descendants parent ON child.parent_id = parent.id
    WHERE child.deleted_at IS NULL
)
SELECT 1 FROM descendants WHERE id = $2::uuid LIMIT 1)sql",
                                                    service::common::dbParams(groupId, parentId));
            if (!cycle.empty())
                service::common::fail(17003, "不能把分组移动到自己的子分组", 409);
        }
    }

    static std::string nodeSelect() {
        return R"sql(SELECT id::text, imei, COALESCE(name, ''), model, software_version,
       hostname, architecture, openwrt_release, enrollment_status,
       false,
       COALESCE(iot_utc_timestamp(last_seen_at), ''), iot_utc_timestamp(created_at),
       COALESCE(CASE WHEN status->'config'->>'activeVersion' ~ '^-?[0-9]{1,18}$'
                     THEN (status->'config'->>'activeVersion')::bigint END, 0),
       COALESCE(CASE WHEN status->'config'->>'desiredVersion' ~ '^-?[0-9]{1,18}$'
                     THEN (status->'config'->>'desiredVersion')::bigint END, 0),
       COALESCE(status->'config'->>'state', 'idle'),
       COALESCE(status->'config'->>'message', ''),
       COALESCE(CASE WHEN status->'outbox'->>'records' ~ '^-?[0-9]{1,18}$'
                     THEN (status->'outbox'->>'records')::bigint END, 0),
       COALESCE(CASE WHEN status->'outbox'->>'bytes' ~ '^-?[0-9]{1,18}$'
                     THEN (status->'outbox'->>'bytes')::bigint END, 0),
       COALESCE(status->'log'->>'level', 'info'),
       CASE lower(COALESCE(capability->>'networkConfig', ''))
            WHEN 'true' THEN true WHEN 't' THEN true WHEN '1' THEN true
            ELSE false END,
       COALESCE(CASE WHEN capability->>'networkConfigVersion' ~ '^-?[0-9]{1,18}$'
                     THEN (capability->>'networkConfigVersion')::bigint END, 0),
       CASE lower(COALESCE(capability->>'firmwareUpdate', ''))
            WHEN 'true' THEN true WHEN 't' THEN true WHEN '1' THEN true
            ELSE false END,
       CASE lower(COALESCE(capability->>'deviceConfig', ''))
            WHEN 'true' THEN true WHEN 't' THEN true WHEN '1' THEN true
            ELSE false END,
       CASE lower(COALESCE(capability->>'modemControl', ''))
            WHEN 'true' THEN true WHEN 't' THEN true WHEN '1' THEN true
            ELSE false END,
       CASE lower(COALESCE(capability->>'terminal', ''))
            WHEN 'true' THEN true WHEN 't' THEN true WHEN '1' THEN true
            ELSE false END,
       CASE lower(COALESCE(capability->>'logs', ''))
            WHEN 'true' THEN true WHEN 't' THEN true WHEN '1' THEN true
            ELSE false END,
       CASE lower(COALESCE(mobile->>'available', ''))
            WHEN 'true' THEN true WHEN 't' THEN true WHEN '1' THEN true
            ELSE false END,
       COALESCE(mobile->>'simState', 'unknown'),
       COALESCE(mobile->>'iccid', ''),
       COALESCE(CASE WHEN mobile->'signal'->>'csq' ~ '^-?[0-9]{1,18}$'
                     THEN (mobile->'signal'->>'csq')::bigint END, 99),
       COALESCE(CASE WHEN mobile->'signal'->>'rssiDbm' ~ '^-?[0-9]{1,18}$'
                     THEN (mobile->'signal'->>'rssiDbm')::bigint END, -1),
       COALESCE(CASE WHEN mobile->'signal'->>'percent' ~ '^-?[0-9]{1,18}$'
                     THEN (mobile->'signal'->>'percent')::bigint END, 0),
       CASE lower(COALESCE(mobile->>'registered', ''))
            WHEN 'true' THEN true WHEN 't' THEN true WHEN '1' THEN true
            ELSE false END,
       COALESCE(CASE WHEN mobile->>'registrationStatus' ~ '^-?[0-9]{1,18}$'
                     THEN (mobile->>'registrationStatus')::bigint END, -1),
       COALESCE(mobile->>'apn', ''),
       COALESCE(mobile->>'operator', ''),
       CASE lower(COALESCE(mobile->>'connected', ''))
            WHEN 'true' THEN true WHEN 't' THEN true WHEN '1' THEN true
            ELSE false END,
       COALESCE(mobile->>'ipv4', ''),
       COALESCE((SELECT task.status FROM edge_task task
                 WHERE task.node_id = edge_node.id AND task.task_type = 'firmware'
                 ORDER BY task.created_at DESC LIMIT 1), ''),
       COALESCE((SELECT CASE WHEN task.result->>'progressPercent' ~ '^-?[0-9]{1,18}$'
                             THEN (task.result->>'progressPercent')::bigint END FROM edge_task task
                 WHERE task.node_id = edge_node.id AND task.task_type = 'firmware'
                 ORDER BY task.created_at DESC LIMIT 1), 0),
       COALESCE((SELECT CASE WHEN task.result->>'downloadedBytes' ~ '^-?[0-9]{1,18}$'
                             THEN (task.result->>'downloadedBytes')::bigint END FROM edge_task task
                 WHERE task.node_id = edge_node.id AND task.task_type = 'firmware'
                 ORDER BY task.created_at DESC LIMIT 1), 0),
       COALESCE((SELECT CASE WHEN task.result->>'totalBytes' ~ '^-?[0-9]{1,18}$'
                             THEN (task.result->>'totalBytes')::bigint END FROM edge_task task
                 WHERE task.node_id = edge_node.id AND task.task_type = 'firmware'
                 ORDER BY task.created_at DESC LIMIT 1), 0),
       COALESCE((SELECT task.result->>'message' FROM edge_task task
                 WHERE task.node_id = edge_node.id AND task.task_type = 'firmware'
                 ORDER BY task.created_at DESC LIMIT 1), ''),
       CASE lower(COALESCE(capability->'vpn'->>'supportsVpn', ''))
            WHEN 'true' THEN true WHEN 't' THEN true WHEN '1' THEN true
            ELSE false END,
       COALESCE(capability->'vpn'->>'wireguardVersion', ''),
       COALESCE(capability->'vpn'->>'agentVersion', ''),
       COALESCE(capability->'vpn'->>'publicKey', ''),
       COALESCE(group_id::text, ''),
       COALESCE((SELECT name FROM edge_node_group WHERE id = edge_node.group_id), ''),
       COALESCE((SELECT string_agg(route.virtual_cidr, ',' ORDER BY route.virtual_cidr)
                 FROM vpn_route route
                 JOIN vpn_peer peer ON peer.id = route.edge_peer_id
                 WHERE peer.edge_node_id = edge_node.id AND peer.status = 'active'
                   AND route.enabled AND route.status = 'active'), '')
FROM edge_node)sql";
    }

    static std::int64_t integer(std::string_view value) {
        std::int64_t result{};
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
        return error == std::errc{} && end == value.data() + value.size() ? result : 0;
    }

    template <typename Row> static ruvia::Task<void> fillNode(ruvia::Context& c, EdgeNodeDto& node, const Row& row) {
        NodeStatusDto status(c);
        ConfigStatusDto config(c);
        config.set<"activeVersion">(integer(row[12].value().value_or(std::string_view{})));
        config.set<"desiredVersion">(integer(row[13].value().value_or(std::string_view{})));
        config.set<"state">(row[14].value().value_or(std::string_view{}));
        config.set<"message">(row[15].value().value_or(std::string_view{}));
        OutboxStatusDto outbox(c);
        outbox.set<"records">(integer(row[16].value().value_or(std::string_view{})));
        outbox.set<"bytes">(integer(row[17].value().value_or(std::string_view{})));
        LogStatusDto log(c);
        log.set<"level">(row[18].value().value_or(std::string_view{}));
        const auto session = co_await c.redis().get(
            "iot:edge:session:" + std::string(row[0].value().value_or(std::string_view{})));
        status.set<"online">(row[8].value().value_or(std::string_view{}) == "approved" &&
                               session.has_value());
        status.set<"lastSeenAt">(row[10].value().value_or(std::string_view{}));
        status.set<"config">(std::move(config));
        status.set<"outbox">(std::move(outbox));
        status.set<"log">(std::move(log));

        CapabilityDto capability(c);
        capability.set<"networkConfig">(row[19].value().value_or(std::string_view{}) == "t");
        capability.set<"networkConfigVersion">(
            integer(row[20].value().value_or(std::string_view{})));
        capability.set<"firmwareUpdate">(row[21].value().value_or(std::string_view{}) == "t");
        capability.set<"deviceConfig">(row[22].value().value_or(std::string_view{}) == "t");
        capability.set<"modemControl">(row[23].value().value_or(std::string_view{}) == "t");
        capability.set<"terminal">(row[24].value().value_or(std::string_view{}) == "t");
        capability.set<"logs">(row[25].value().value_or(std::string_view{}) == "t");
        VpnCapabilityDto vpn(c);
        vpn.set<"supportsVpn">(row[43].value().value_or(std::string_view{}) == "t");
        vpn.set<"wireguardVersion">(row[44].value().value_or(std::string_view{}));
        vpn.set<"agentVersion">(row[45].value().value_or(std::string_view{}));
        vpn.set<"publicKey">(row[46].value().value_or(std::string_view{}));
        capability.set<"vpn">(std::move(vpn));

        SignalDto signal(c);
        signal.set<"csq">(integer(row[29].value().value_or(std::string_view{})));
        signal.set<"rssiDbm">(integer(row[30].value().value_or(std::string_view{})));
        signal.set<"percent">(integer(row[31].value().value_or(std::string_view{})));
        MobileDto mobile(c);
        mobile.set<"available">(row[26].value().value_or(std::string_view{}) == "t");
        mobile.set<"simState">(row[27].value().value_or(std::string_view{}));
        mobile.set<"iccid">(row[28].value().value_or(std::string_view{}));
        mobile.set<"signal">(std::move(signal));
        mobile.set<"registered">(row[32].value().value_or(std::string_view{}) == "t");
        mobile.set<"registrationStatus">(
            integer(row[33].value().value_or(std::string_view{})));
        mobile.set<"apn">(row[34].value().value_or(std::string_view{}));
        mobile.set<"operatorName">(row[35].value().value_or(std::string_view{}));
        mobile.set<"connected">(row[36].value().value_or(std::string_view{}) == "t");
        mobile.set<"ipv4">(row[37].value().value_or(std::string_view{}));

        FirmwareStatusDto firmware(c);
        firmware.set<"state">(row[38].value().value_or(std::string_view{}));
        firmware.set<"progressPercent">(
            integer(row[39].value().value_or(std::string_view{})));
        firmware.set<"downloadedBytes">(
            integer(row[40].value().value_or(std::string_view{})));
        firmware.set<"totalBytes">(integer(row[41].value().value_or(std::string_view{})));
        firmware.set<"message">(row[42].value().value_or(std::string_view{}));

        node.set<"id">(row[0].value().value_or(std::string_view{}));
        node.set<"imei">(row[1].value().value_or(std::string_view{}));
        node.set<"name">(row[2].value().value_or(std::string_view{}));
        node.set<"groupId">(row[47].value().value_or(std::string_view{}));
        node.set<"groupName">(row[48].value().value_or(std::string_view{}));
        node.set<"model">(row[3].value().value_or(std::string_view{}));
        node.set<"softwareVersion">(row[4].value().value_or(std::string_view{}));
        node.set<"hostname">(row[5].value().value_or(std::string_view{}));
        node.set<"architecture">(row[6].value().value_or(std::string_view{}));
        node.set<"openwrtRelease">(row[7].value().value_or(std::string_view{}));
        node.set<"enrollmentStatus">(row[8].value().value_or(std::string_view{}));
        node.set<"status">(std::move(status));
        node.set<"capability">(std::move(capability));
        node.set<"mobile">(std::move(mobile));
        node.set<"firmware">(std::move(firmware));
        ruvia::BoxedArray<ruvia::String> virtualCidrs(
            ruvia::ModelOptions{.resource = c.resource()});
        for (const auto& cidr : split(row[49].value().value_or(std::string_view{})))
            if (!cidr.empty())
                virtualCidrs.emplace(cidr,
                                     ruvia::ModelOptions{.resource = c.resource()});
        node.set<"vpnVirtualCidrs">(std::move(virtualCidrs));
        node.set<"createdAt">(row[11].value().value_or(std::string_view{}));
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

    static std::string taskSelect() {
        return R"sql(
SELECT id::text, task_type, status, COALESCE(result->>'message', ''),
       COALESCE(CASE WHEN result->>'progressPercent' ~ '^-?[0-9]{1,18}$'
                     THEN (result->>'progressPercent')::bigint END, 0),
       COALESCE(CASE WHEN result->>'downloadedBytes' ~ '^-?[0-9]{1,18}$'
                     THEN (result->>'downloadedBytes')::bigint END, 0),
       COALESCE(CASE WHEN result->>'totalBytes' ~ '^-?[0-9]{1,18}$'
                     THEN (result->>'totalBytes')::bigint END, 0),
       iot_utc_timestamp(created_at), iot_utc_timestamp(updated_at)
FROM edge_task WHERE node_id = $1::uuid ORDER BY created_at DESC LIMIT 50)sql";
    }

    static ruvia::Task<ruvia::BoxedArray<InterfaceDto>> interfaces(ruvia::Context& c,
                                                             std::string_view id) {
        const auto rows = co_await c.db().query(R"sql(
SELECT name, display_name, COALESCE(mac, ''), is_up, is_bridge, COALESCE(ipv4, ''),
       COALESCE(prefix_length, 0), COALESCE(gateway, ''),
       COALESCE((SELECT string_agg(value, ',' ORDER BY value)
                 FROM jsonb_array_elements_text(bridge_ports) AS values(value)), '')
FROM edge_node_interface WHERE node_id = $1::uuid ORDER BY name)sql",
                                                service::common::dbParams(id));
        ruvia::BoxedArray<InterfaceDto> result(
            ruvia::ModelOptions{.resource = c.resource()});
        for (const auto& row : rows) {
            auto& item = result.emplace(c);
            ruvia::BoxedArray<ruvia::String> ports(
                ruvia::ModelOptions{.resource = c.resource()});
            for (const auto& port : split(row[8].value().value_or(std::string_view{})))
                if (!port.empty())
                    ports.emplace(port, ruvia::ModelOptions{.resource = c.resource()});
            item.set<"name">(row[0].value().value_or(std::string_view{}))
                .set<"displayName">(row[1].value().value_or(std::string_view{}))
                .set<"mac">(row[2].value().value_or(std::string_view{}))
                .set<"up">(row[3].value().value_or(std::string_view{}) == "t")
                .set<"bridge">(row[4].value().value_or(std::string_view{}) == "t")
                .set<"ipv4">(row[5].value().value_or(std::string_view{}))
                .set<"prefixLength">(integer(row[6].value().value_or(std::string_view{})))
                .set<"gateway">(row[7].value().value_or(std::string_view{}))
                .set<"bridgePorts">(std::move(ports));
        }
        co_return result;
    }

    static ruvia::Task<ruvia::BoxedArray<NetworkDto>> networks(ruvia::Context& c,
                                                         std::string_view id) {
        const auto rows = co_await c.db().query(R"sql(
SELECT name, address_mode, device, is_up, is_bridge, COALESCE(ipv4, ''),
       COALESCE(prefix_length, 0), COALESCE(gateway, ''),
       COALESCE((SELECT string_agg(value, ',' ORDER BY value)
                 FROM jsonb_array_elements_text(bridge_ports) AS values(value)), '')
FROM edge_node_network WHERE node_id = $1::uuid ORDER BY name)sql",
                                                service::common::dbParams(id));
        ruvia::BoxedArray<NetworkDto> result(ruvia::ModelOptions{.resource = c.resource()});
        for (const auto& row : rows) {
            auto& item = result.emplace(c);
            ruvia::BoxedArray<ruvia::String> ports(
                ruvia::ModelOptions{.resource = c.resource()});
            for (const auto& port : split(row[8].value().value_or(std::string_view{})))
                if (!port.empty())
                    ports.emplace(port, ruvia::ModelOptions{.resource = c.resource()});
            item.set<"name">(row[0].value().value_or(std::string_view{}))
                .set<"mode">(row[1].value().value_or(std::string_view{}))
                .set<"device">(row[2].value().value_or(std::string_view{}))
                .set<"up">(row[3].value().value_or(std::string_view{}) == "t")
                .set<"bridge">(row[4].value().value_or(std::string_view{}) == "t")
                .set<"ipv4">(row[5].value().value_or(std::string_view{}))
                .set<"prefixLength">(integer(row[6].value().value_or(std::string_view{})))
                .set<"gateway">(row[7].value().value_or(std::string_view{}))
                .set<"bridgePorts">(std::move(ports));
        }
        co_return result;
    }

    static ruvia::Task<ruvia::BoxedArray<SerialDto>> serialPorts(ruvia::Context& c,
                                                           std::string_view id) {
        const auto rows = co_await c.db().query(R"sql(
SELECT path, display_name, available, rs485 FROM edge_node_serial
WHERE node_id = $1::uuid ORDER BY path)sql",
                                                service::common::dbParams(id));
        ruvia::BoxedArray<SerialDto> result(ruvia::ModelOptions{.resource = c.resource()});
        for (const auto& row : rows) {
            auto& item = result.emplace(c);
            item.set<"path">(row[0].value().value_or(std::string_view{}))
                .set<"displayName">(row[1].value().value_or(std::string_view{}))
                .set<"available">(row[2].value().value_or(std::string_view{}) == "t")
                .set<"rs485">(row[3].value().value_or(std::string_view{}) == "t");
        }
        co_return result;
    }

    static ruvia::Task<ruvia::BoxedArray<TaskDto>> tasks(ruvia::Context& c, std::string_view id) {
        const auto rows = co_await c.db().query(taskSelect(), service::common::dbParams(id));
        ruvia::BoxedArray<TaskDto> result(ruvia::ModelOptions{.resource = c.resource()});
        for (const auto& row : rows) {
            auto& item = result.emplace(c);
            item.set<"id">(row[0].value().value_or(std::string_view{}))
                .set<"taskType">(row[1].value().value_or(std::string_view{}))
                .set<"status">(row[2].value().value_or(std::string_view{}))
                .set<"message">(row[3].value().value_or(std::string_view{}))
                .set<"progressPercent">(integer(row[4].value().value_or(std::string_view{})))
                .set<"downloadedBytes">(integer(row[5].value().value_or(std::string_view{})))
                .set<"totalBytes">(integer(row[6].value().value_or(std::string_view{})))
                .set<"createdAt">(row[7].value().value_or(std::string_view{}))
                .set<"updatedAt">(row[8].value().value_or(std::string_view{}));
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
        for (const auto& row : rows)
            result.emplace(row[0].value().value_or(std::string_view{}));
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
SELECT enrollment_status,
       CASE lower(COALESCE(capability->>'networkConfig', ''))
            WHEN 'true' THEN true WHEN 't' THEN true WHEN '1' THEN true
            ELSE false END,
       COALESCE(CASE WHEN capability->>'networkConfigVersion' ~ '^-?[0-9]{1,18}$'
                     THEN (capability->>'networkConfigVersion')::bigint END, 0)
FROM edge_node WHERE id = $1::uuid LIMIT 1)sql",
                                                service::common::dbParams(nodeId));
        if (rows.empty())
            service::common::fail(17001, "边缘节点不存在", 404);
        if (rows.front()[0].value().value_or(std::string_view{}) != "approved")
            service::common::fail(17002, "边缘节点尚未批准注册", 409);
        if (rows.front()[1].value().value_or(std::string_view{}) != "t")
            service::common::fail(17004, "网络配置不可用", 409);
        const auto version = integer(rows.front()[2].value().value_or(std::string_view{}));
        if (version < 2)
            service::common::fail(17004, "节点代理版本过旧，请先升级后再管理网络", 409);
        co_return version;
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
            "SELECT enrollment_status, CASE lower(COALESCE(capability->>$1, '')) "
            "WHEN 'true' THEN true WHEN 't' THEN true WHEN '1' THEN true "
            "ELSE false END "
            "FROM edge_node WHERE id = $2::uuid LIMIT 1",
            service::common::dbParams(key, nodeId));
        if (rows.empty())
            service::common::fail(17001, "边缘节点不存在", 404);
        if (rows.front()[0].value().value_or(std::string_view{}) != "approved")
            service::common::fail(17002, "边缘节点尚未批准注册", 409);
        if (rows.front()[1].value().value_or(std::string_view{}) != "t")
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
        co_await dispatch::notifyNode(c.redis(), nodeId);
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
