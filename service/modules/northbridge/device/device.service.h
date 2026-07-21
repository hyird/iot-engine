#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/web/ModelObject.h>
#include <ruvia/web/db/Db.h>

#include "service/modules/northbridge/queue/config_event.publisher.h"
#include "service/common/http.h"
#include "service/common/uuid.h"
#include "service/middleware/auth.h"
#include "service/modules/northbridge/device/device.types.h"

namespace service::device {

class DeviceService {
  public:
    static DeviceService& instance() {
        static DeviceService service;
        return service;
    }

    ruvia::Task<DevicePageDataDto> list(ruvia::Context& c) {
        const auto rows = co_await c.db().query(
            "SELECT " + itemColumns() +
            " FROM iot_device d JOIN iot_link l ON l.id = d.link_id "
            "JOIN iot_protocol_config p ON p.id = d.protocol_config_id "
            "WHERE d.deleted_at IS NULL ORDER BY d.group_id NULLS LAST, d.created_at, d.id");
        ruvia::List<DeviceItemDto> items(c.resource());
        for (const auto& row : rows.rows())
            fillItem(c, items.emplace(c), row);
        DevicePageDataDto result(c);
        result.list(std::move(items)).total(static_cast<std::int64_t>(rows.rows().size()));
        co_return result;
    }

    ruvia::Task<DeviceRealtimePageDto> realtime(ruvia::Context& c) {
        const auto rows = co_await c.db().query(
            "SELECT id::text FROM iot_device WHERE deleted_at IS NULL ORDER BY id");
        ruvia::List<DeviceRealtimeDto> items(c.resource());
        for (const auto& row : rows.rows()) {
            auto& item = items.emplace(c);
            item.id(row[0].text())
                .connected(false)
                .connectionState("offline")
                .elements(ruvia::List<ruvia::String>(c.resource()))
                .canEdit(true)
                .canDelete(true)
                .canShare(false)
                .canCommand(false)
                .accessLevel("owner");
        }
        DeviceRealtimePageDto result(c);
        result.list(std::move(items)).total(static_cast<std::int64_t>(rows.rows().size()));
        co_return result;
    }

    ruvia::Task<DeviceItemDto> detail(ruvia::Context& c, std::string_view id) {
        const auto rows = co_await c.db().query(
            "SELECT " + itemColumns() +
                " FROM iot_device d JOIN iot_link l ON l.id = d.link_id "
                "JOIN iot_protocol_config p ON p.id = d.protocol_config_id "
                "WHERE d.id = $1 AND d.deleted_at IS NULL LIMIT 1",
            service::common::dbParams(id));
        if (rows.rows().empty())
            service::common::fail(18001, "设备不存在", 404);
        DeviceItemDto item(c);
        fillItem(c, item, rows.rows().front());
        co_return item;
    }

    ruvia::Task<ruvia::List<DeviceOptionDto>> options(ruvia::Context& c) {
        const auto rows = co_await c.db().query(
            "SELECT id::text, name, device_code FROM iot_device "
            "WHERE deleted_at IS NULL AND status = 'enabled' ORDER BY name");
        ruvia::List<DeviceOptionDto> result(c.resource());
        for (const auto& row : rows.rows()) {
            auto& item = result.emplace(c);
            item.id(row[0].text())
                .name(row[1].text())
                .deviceCode(row[2].text())
                .canEdit(true)
                .canDelete(true)
                .canShare(false)
                .canCommand(false)
                .accessLevel("owner");
        }
        co_return result;
    }

    ruvia::Task<void> create(ruvia::Context& c, const SaveDeviceBody& body) {
        co_await validate(c, body, true);
        co_await ensureUnique(c, body, std::nullopt);
        co_await validateRuntimeIdentity(c, body, std::nullopt);
        const auto principal = service::middleware::requireAuth(c);
        const auto id = service::common::nextUuidV7();
        const std::string name(body.name()->view());
        const std::string deviceCode(body.deviceCode()->view());
        const std::string linkId(body.linkId()->view());
        const std::string targetId = str(body.targetId());
        const std::string protocolConfigId(body.protocolConfigId()->view());
        const std::string groupId = str(body.groupId());
        const std::string status = body.status() ? std::string(body.status()->view()) : "enabled";
        const std::int64_t onlineTimeout =
            body.onlineTimeout() ? static_cast<std::int64_t>(*body.onlineTimeout()) : 300;
        const std::string remoteControl =
            (!body.remoteControl() || *body.remoteControl()) ? "true" : "false";
        const std::string modbusMode = str(body.modbusMode());
        const std::string slaveId =
            body.slaveId() ? std::to_string(static_cast<std::int64_t>(*body.slaveId())) : "";
        const std::string timezone =
            (body.timezone() && !body.timezone()->view().empty()) ? std::string(body.timezone()->view())
                                                                  : "+08:00";
        const std::string heartbeat = packetJson(body.heartbeat());
        const std::string registration = packetJson(body.registration());
        const std::string remark = str(body.remark());
        (void)co_await c.db().execute(
            R"sql(
INSERT INTO iot_device(
  id, name, device_code, link_id, target_id, protocol_config_id, group_id, status,
  online_timeout, remote_control, modbus_mode, slave_id, timezone,
  heartbeat, registration, remark, created_by)
VALUES ($1::uuid, $2, $3, $4::uuid, NULLIF($5, ''), $6::uuid, NULLIF($7, '')::uuid, $8,
  $9, $10::boolean, NULLIF($11, ''), NULLIF($12, '')::integer, $13,
  COALESCE(NULLIF($14, '')::jsonb, '{"mode":"OFF"}'::jsonb),
  COALESCE(NULLIF($15, '')::jsonb, '{"mode":"OFF"}'::jsonb), NULLIF($16, ''), $17))sql",
            service::common::dbParams(id, name, deviceCode, linkId, targetId, protocolConfigId,
                                      groupId, status, onlineTimeout, remoteControl, modbusMode,
                                      slaveId, timezone, heartbeat, registration, remark,
                                      principal.userId));
        co_await service::bridge::publishConfigEvent(c, "device", "created", id);
    }

    ruvia::Task<void> update(ruvia::Context& c, std::string_view id, const SaveDeviceBody& body) {
        const auto rows =
            co_await c.db().query("SELECT created_by, link_id, protocol_config_id FROM iot_device "
                                  "WHERE id = $1 AND deleted_at IS NULL",
                                  service::common::dbParams(id));
        if (rows.rows().empty())
            service::common::fail(18001, "设备不存在", 404);
        co_await requireOwner(c, rows.rows().front()[0].text());
        if (body.linkId() && body.linkId()->view() != rows.rows().front()[1].text())
            service::common::fail(18003, "设备所属链路不可修改", 409);
        if (body.protocolConfigId() &&
            body.protocolConfigId()->view() != rows.rows().front()[2].text())
            service::common::fail(18003, "设备类型不可修改", 409);
        co_await validate(c, body, false);
        co_await ensureUnique(c, body, std::string(id));
        co_await validateRuntimeIdentity(c, body, std::string(id));

        std::string set;
        std::vector<ruvia::DbValue> params;
        auto raw = [&](std::string_view assign, ruvia::DbValue value) {
            if (!set.empty())
                set += ", ";
            params.emplace_back(std::move(value));
            set += std::string(assign) + std::to_string(params.size());
        };
        if (body.name())
            raw("name = $", ruvia::DbValue{body.name()->view()});
        if (body.deviceCode())
            raw("device_code = $", ruvia::DbValue{body.deviceCode()->view()});
        if (body.linkId())
            raw("link_id = $", ruvia::DbValue{body.linkId()->view()}), set += "::uuid";
        if (body.targetId())
            raw("target_id = NULLIF($", ruvia::DbValue{body.targetId()->view()}), set += ", '')";
        if (body.protocolConfigId())
            raw("protocol_config_id = $", ruvia::DbValue{body.protocolConfigId()->view()}),
                set += "::uuid";
        if (body.groupId())
            raw("group_id = NULLIF($", ruvia::DbValue{body.groupId()->view()}), set += ", '')::uuid";
        if (body.status())
            raw("status = $", ruvia::DbValue{body.status()->view()});
        if (body.onlineTimeout())
            raw("online_timeout = $",
                ruvia::DbValue{static_cast<std::int64_t>(*body.onlineTimeout())});
        if (body.remoteControl())
            raw("remote_control = $",
                ruvia::DbValue{std::string_view{*body.remoteControl() ? "true" : "false"}}),
                set += "::boolean";
        if (body.modbusMode())
            raw("modbus_mode = NULLIF($", ruvia::DbValue{body.modbusMode()->view()}), set += ", '')";
        if (body.slaveId())
            raw("slave_id = $", ruvia::DbValue{static_cast<std::int64_t>(*body.slaveId())});
        if (body.timezone())
            raw("timezone = $", ruvia::DbValue{body.timezone()->view()});
        std::string heartbeat;
        if (body.heartbeat()) {
            heartbeat = packetJson(body.heartbeat());
            raw("heartbeat = $", ruvia::DbValue{std::string_view{heartbeat}}), set += "::jsonb";
        }
        std::string registration;
        if (body.registration()) {
            registration = packetJson(body.registration());
            raw("registration = $", ruvia::DbValue{std::string_view{registration}}),
                set += "::jsonb";
        }
        if (body.remark())
            raw("remark = NULLIF($", ruvia::DbValue{body.remark()->view()}), set += ", '')";

        if (!set.empty()) {
            params.emplace_back(id);
            (void)co_await c.db().execute("UPDATE iot_device SET " + set +
                                              ", updated_at = NOW() WHERE id = $" +
                                              std::to_string(params.size()),
                                          params);
        }
        co_await service::bridge::publishConfigEvent(c, "device", "updated", id);
    }

    ruvia::Task<void> remove(ruvia::Context& c, std::string_view id) {
        const auto rows = co_await c.db().query(
            "SELECT created_by FROM iot_device WHERE id = $1 AND deleted_at IS NULL",
            service::common::dbParams(id));
        if (rows.rows().empty())
            service::common::fail(18001, "设备不存在", 404);
        co_await requireOwner(c, rows.rows().front()[0].text());
        (void)co_await c.db().execute(
            "UPDATE iot_device SET deleted_at = NOW(), updated_at = NOW() WHERE id = $1",
            service::common::dbParams(id));
        co_await service::bridge::publishConfigEvent(c, "device", "deleted", id);
    }

    // ===== 设备分组（合并入同一 DeviceService 类）=====

    ruvia::Task<ruvia::List<DeviceGroupItemDto>> listGroups(ruvia::Context& c, bool withCount) {
        const std::string countExpr =
            withCount ? "(SELECT COUNT(*) FROM iot_device d WHERE d.group_id = g.id AND d.deleted_at "
                        "IS NULL)"
                      : "0";
        const std::string sql =
            "SELECT g.id::text, g.name, COALESCE(g.parent_id::text, ''), g.status, g.sort_order, "
            "COALESCE(g.remark, ''), " +
            countExpr +
            ", g.created_at::text, g.updated_at::text FROM iot_device_group g "
            "WHERE g.deleted_at IS NULL ORDER BY g.sort_order, g.id";
        const auto rows = co_await c.db().query(sql);
        ruvia::List<DeviceGroupItemDto> result(c.resource());
        for (const auto& row : rows.rows())
            fillGroup(result.emplace(c), row);
        co_return result;
    }

    ruvia::Task<DeviceGroupItemDto> groupDetail(ruvia::Context& c, std::string_view id) {
        const auto rows = co_await c.db().query(
            "SELECT id::text, name, COALESCE(parent_id::text, ''), status, sort_order, "
            "COALESCE(remark, ''), 0, created_at::text, updated_at::text "
            "FROM iot_device_group WHERE id = $1 AND deleted_at IS NULL LIMIT 1",
            service::common::dbParams(id));
        if (rows.rows().empty())
            service::common::fail(17001, "设备分组不存在", 404);
        DeviceGroupItemDto item(c);
        fillGroup(item, rows.rows().front());
        co_return item;
    }

    ruvia::Task<void> createGroup(ruvia::Context& c, const SaveDeviceGroupBody& body) {
        co_await validateParent(c, body, std::nullopt);
        const auto principal = service::middleware::requireAuth(c);
        const auto id = service::common::nextUuidV7();
        const std::string name(body.name()->view());
        const std::string parentId = body.parentId() ? std::string(body.parentId()->view()) : "";
        const std::string status = body.status() ? std::string(body.status()->view()) : "enabled";
        const std::int64_t sortOrder =
            body.sortOrder() ? static_cast<std::int64_t>(*body.sortOrder()) : 0;
        const std::string remark = body.remark() ? std::string(body.remark()->view()) : "";
        (void)co_await c.db().execute(
            R"sql(
INSERT INTO iot_device_group(id, name, parent_id, status, sort_order, remark, created_by)
VALUES ($1::uuid, $2, NULLIF($3, '')::uuid, $4, $5, NULLIF($6, ''), $7))sql",
            service::common::dbParams(id, name, parentId, status, sortOrder, remark,
                                      principal.userId));
    }

    ruvia::Task<void> updateGroup(ruvia::Context& c, std::string_view id,
                                  const SaveDeviceGroupBody& body) {
        const auto rows = co_await c.db().query(
            "SELECT created_by FROM iot_device_group WHERE id = $1 AND deleted_at IS NULL",
            service::common::dbParams(id));
        if (rows.rows().empty())
            service::common::fail(17001, "设备分组不存在", 404);
        co_await requireGroupOwner(c, rows.rows().front()[0].text());
        co_await validateParent(c, body, std::string(id));

        std::string set;
        std::vector<ruvia::DbValue> params;
        auto assign = [&](std::string_view column, ruvia::DbValue value) {
            if (!set.empty())
                set += ", ";
            params.emplace_back(std::move(value));
            set += std::string(column) + " = $" + std::to_string(params.size());
        };
        if (body.name())
            assign("name", ruvia::DbValue{body.name()->view()});
        if (body.parentId()) {
            if (!set.empty())
                set += ", ";
            params.emplace_back(ruvia::DbValue{body.parentId()->view()});
            set += "parent_id = NULLIF($" + std::to_string(params.size()) + ", '')::uuid";
        }
        if (body.status())
            assign("status", ruvia::DbValue{body.status()->view()});
        if (body.sortOrder())
            assign("sort_order", ruvia::DbValue{static_cast<std::int64_t>(*body.sortOrder())});
        if (body.remark()) {
            if (!set.empty())
                set += ", ";
            params.emplace_back(ruvia::DbValue{body.remark()->view()});
            set += "remark = NULLIF($" + std::to_string(params.size()) + ", '')";
        }
        if (set.empty())
            co_return;
        params.emplace_back(id);
        (void)co_await c.db().execute("UPDATE iot_device_group SET " + set +
                                          ", updated_at = NOW() WHERE id = $" +
                                          std::to_string(params.size()),
                                      params);
    }

    ruvia::Task<void> removeGroup(ruvia::Context& c, std::string_view id) {
        const auto rows = co_await c.db().query(
            "SELECT created_by FROM iot_device_group WHERE id = $1 AND deleted_at IS NULL",
            service::common::dbParams(id));
        if (rows.rows().empty())
            service::common::fail(17001, "设备分组不存在", 404);
        co_await requireGroupOwner(c, rows.rows().front()[0].text());
        const auto used = co_await c.db().query(R"sql(
SELECT EXISTS (SELECT 1 FROM iot_device_group WHERE parent_id = $1 AND deleted_at IS NULL)
    OR EXISTS (SELECT 1 FROM iot_device WHERE group_id = $1 AND deleted_at IS NULL))sql",
                                                service::common::dbParams(id));
        if (used.rows().front()[0].text() == "t")
            service::common::fail(17004, "请先移除子分组和设备", 409);
        (void)co_await c.db().execute(
            "UPDATE iot_device_group SET deleted_at = NOW(), updated_at = NOW() WHERE id = $1",
            service::common::dbParams(id));
    }

  private:
    static std::int64_t toInt(std::string_view value) { return std::stoll(std::string(value)); }

    // 列顺序必须与 fillItem 的 row 下标严格对应。
    static std::string itemColumns() {
        return R"sql(d.id::text, d.name, d.device_code, d.link_id::text, d.target_id,
  d.protocol_config_id::text, d.group_id::text, d.status, d.online_timeout, d.remote_control,
  d.modbus_mode, d.slave_id, d.timezone,
  d.heartbeat->>'mode', d.heartbeat->>'content',
  d.registration->>'mode', d.registration->>'content',
  COALESCE(d.remark, ''), d.created_by::text, d.created_at::text, d.updated_at::text,
  l.name, l.mode, l.protocol, p.name, p.protocol,
  COALESCE((p.config->>'readInterval')::numeric, (p.config->>'pollInterval')::numeric)::text,
  (p.config->>'storageInterval')::numeric::text,
  CASE p.protocol
      WHEN 'Modbus' THEN jsonb_array_length(COALESCE(p.config->'registers', '[]'::jsonb))
      WHEN 'S7' THEN jsonb_array_length(COALESCE(p.config->'areas', '[]'::jsonb))
      ELSE jsonb_array_length(COALESCE(p.config->'funcs', '[]'::jsonb)) END)sql";
    }

    template <typename Row> static void fillItem(ruvia::Context& c, DeviceItemDto& item, Row&& row) {
        item.id(row[0].text()).name(row[1].text()).deviceCode(row[2].text()).linkId(row[3].text());
        if (!row[4].isNull())
            item.targetId(row[4].text());
        item.protocolConfigId(row[5].text());
        if (!row[6].isNull())
            item.groupId(row[6].text());
        item.status(row[7].text())
            .onlineTimeout(toInt(row[8].text()))
            .remoteControl(row[9].text() == "t");
        if (!row[10].isNull())
            item.modbusMode(row[10].text());
        if (!row[11].isNull())
            item.slaveId(toInt(row[11].text()));
        item.timezone(row[12].text());
        {
            DevicePacketDto heartbeat(c);
            if (!row[13].isNull())
                heartbeat.mode(row[13].text());
            if (!row[14].isNull())
                heartbeat.content(row[14].text());
            item.heartbeat(std::move(heartbeat));
        }
        {
            DevicePacketDto registration(c);
            if (!row[15].isNull())
                registration.mode(row[15].text());
            if (!row[16].isNull())
                registration.content(row[16].text());
            item.registration(std::move(registration));
        }
        item.remark(row[17].text())
            .createdBy(row[18].text())
            .createdAt(row[19].text())
            .updatedAt(row[20].text())
            .linkName(row[21].text())
            .linkMode(row[22].text())
            .linkProtocol(row[23].text())
            .protocolName(row[24].text())
            .protocolType(row[25].text());
        if (!row[26].isNull())
            item.readInterval(std::stod(std::string(row[26].text())));
        if (!row[27].isNull())
            item.storageInterval(std::stod(std::string(row[27].text())));
        item.elementCount(toInt(row[28].text()))
            .connected(false)
            .connectionState("offline")
            .elements(ruvia::List<ruvia::String>(c.resource()))
            .canEdit(true)
            .canDelete(true)
            .canShare(false)
            .canCommand(false)
            .accessLevel("owner");
    }

    static std::string str(const std::optional<ruvia::String>& value) {
        return value ? std::string(value->view()) : std::string{};
    }

    static void appendJsonString(std::string& out, std::string_view value) {
        out.push_back('"');
        for (const char ch : value) {
            switch (ch) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                                  static_cast<unsigned char>(ch));
                    out += buffer;
                } else {
                    out.push_back(ch);
                }
            }
        }
        out.push_back('"');
    }

    // 未提供返回空串（SQL 里以 NULLIF 回退到默认或当前值）；提供则序列化为 {"mode":..,"content":..}
    static std::string packetJson(const std::optional<DevicePacketBody>& packet) {
        if (!packet)
            return "";
        std::string out = "{\"mode\":";
        appendJsonString(out, packet->mode() ? packet->mode()->view() : std::string_view("OFF"));
        if (packet->content()) {
            out += ",\"content\":";
            appendJsonString(out, packet->content()->view());
        }
        out.push_back('}');
        return out;
    }

    // 心跳/注册包内容校验（对应旧 SQL shape-check 的第 8、9 条，语义一致）
    static void validatePacket(const std::optional<DevicePacketBody>& packet) {
        if (!packet)
            return;
        const std::string_view mode =
            packet->mode() ? packet->mode()->view() : std::string_view("OFF");
        if (mode != "OFF" && mode != "HEX" && mode != "ASCII")
            service::common::fail(18002, "设备参数无效", 400);
        if (mode == "OFF")
            return;
        const std::string_view content =
            packet->content() ? packet->content()->view() : std::string_view{};
        if (content.empty())
            service::common::fail(18002, "设备参数无效", 400);
        if (mode == "HEX") {
            std::string stripped;
            for (const char ch : content)
                if (!std::isspace(static_cast<unsigned char>(ch)))
                    stripped.push_back(ch);
            if (stripped.empty() || stripped.size() % 2 != 0)
                service::common::fail(18002, "设备参数无效", 400);
            for (const char ch : stripped)
                if (!std::isxdigit(static_cast<unsigned char>(ch)))
                    service::common::fail(18002, "设备参数无效", 400);
        }
    }

    // 扁平字段（必填/长度/枚举/范围/UUID/timezone）由声明式校验器保证；
    // 此处只做跨字段、依赖 DB 与协议相关的校验（保留 18002/18003 域码）。
    ruvia::Task<void> validate(ruvia::Context& c, const SaveDeviceBody& body, bool required) {
        (void)required;
        validatePacket(body.heartbeat());
        validatePacket(body.registration());
        const auto& linkId = body.linkId();
        const auto& configId = body.protocolConfigId();
        if (!linkId || linkId->view().empty() || !configId || configId->view().empty())
            co_return;
        const auto relation = co_await c.db().query(R"sql(
SELECT l.protocol, l.mode, p.protocol
FROM iot_link l CROSS JOIN iot_protocol_config p
WHERE l.id = $1 AND l.deleted_at IS NULL AND p.id = $2 AND p.deleted_at IS NULL LIMIT 1)sql",
                                                    service::common::dbParams(linkId->view(),
                                                                              configId->view()));
        if (relation.rows().empty())
            service::common::fail(18003, "链路或设备类型不存在", 400);
        const std::string linkProtocol(relation.rows().front()[0].text());
        const std::string configProtocol(relation.rows().front()[2].text());
        if (linkProtocol != configProtocol)
            service::common::fail(18003, "链路协议与设备类型不一致", 409);
        const auto& code = body.deviceCode();
        if (!code || code->view().empty() || code->view().size() > 100)
            service::common::fail(18002, "设备编码长度必须在 1 - 100 之间", 400);
        for (const auto character : code->view())
            if (!std::isalnum(static_cast<unsigned char>(character)))
                service::common::fail(18002, "设备编码只能包含字母和数字", 400);
        if (configProtocol == "SL651") {
            if (code->view().size() > 10)
                service::common::fail(18002, "SL651 遥测站地址最多 10 位数字", 400);
            for (const auto character : code->view())
                if (!std::isdigit(static_cast<unsigned char>(character)))
                    service::common::fail(18002, "SL651 设备编码必须是数字遥测站地址", 400);
        }
        if (body.groupId() && !body.groupId()->view().empty()) {
            const auto group = co_await c.db().query(
                "SELECT 1 FROM iot_device_group WHERE id = $1 AND deleted_at IS NULL",
                service::common::dbParams(body.groupId()->view()));
            if (group.rows().empty())
                service::common::fail(18003, "设备分组不存在", 400);
        }
    }

    ruvia::Task<void> validateRuntimeIdentity(ruvia::Context& c, const SaveDeviceBody& body,
                                              std::optional<std::string> excludedId) {
        const std::string excluded = excludedId.value_or(std::string(kNilUuid));
        const std::string inLinkId = str(body.linkId());
        const std::string inTargetId = str(body.targetId());
        const std::string inConfigId = str(body.protocolConfigId());
        const std::string inSlaveId =
            body.slaveId() ? std::to_string(static_cast<std::int64_t>(*body.slaveId())) : "";
        const std::string inRegistration = packetJson(body.registration());
        const std::string inHeartbeat = packetJson(body.heartbeat());
        const auto candidate =
            co_await c.db().query(R"sql(
WITH current_device AS (
  SELECT link_id, target_id, protocol_config_id, slave_id, registration, heartbeat
  FROM iot_device WHERE id = $2 AND deleted_at IS NULL
),
candidate AS (
  SELECT
    COALESCE(NULLIF($1, '')::uuid, current_device.link_id) AS link_id,
    COALESCE(NULLIF($3, ''), current_device.target_id, '') AS target_id,
    COALESCE(NULLIF($4, '')::uuid, current_device.protocol_config_id) AS protocol_config_id,
    COALESCE(NULLIF($5, '')::integer, current_device.slave_id, 1) AS slave_id,
    COALESCE(NULLIF($6, '')::jsonb, current_device.registration, '{"mode":"OFF"}'::jsonb) AS registration,
    COALESCE(NULLIF($7, '')::jsonb, current_device.heartbeat, '{"mode":"OFF"}'::jsonb) AS heartbeat
  FROM (SELECT 1) b LEFT JOIN current_device ON TRUE
)
SELECT candidate.link_id, link.mode, protocol.protocol, candidate.target_id,
       candidate.slave_id,
       upper(COALESCE(candidate.registration->>'mode', 'OFF')),
       CASE upper(COALESCE(candidate.registration->>'mode', 'OFF'))
         WHEN 'OFF' THEN 'OFF:'
         WHEN 'HEX' THEN 'HEX:' || upper(regexp_replace(
           COALESCE(candidate.registration->>'content', ''), '\\s', '', 'g'))
         ELSE 'ASCII:' || COALESCE(candidate.registration->>'content', '')
       END,
       upper(COALESCE(candidate.heartbeat->>'mode', 'OFF'))
FROM candidate
JOIN iot_link link ON link.id = candidate.link_id AND link.deleted_at IS NULL
JOIN iot_protocol_config protocol
  ON protocol.id = candidate.protocol_config_id AND protocol.deleted_at IS NULL
LIMIT 1)sql",
                                  service::common::dbParams(inLinkId, excluded, inTargetId,
                                                            inConfigId, inSlaveId, inRegistration,
                                                            inHeartbeat));
        if (candidate.rows().empty())
            co_return;

        const auto& current = candidate.rows().front();
        const std::string linkId(current[0].text());
        const std::string linkMode(current[1].text());
        const std::string protocol(current[2].text());
        const std::string targetId(current[3].text());
        const auto slaveId = toInt(current[4].text());
        const std::string registrationMode(current[5].text());
        const std::string registrationKey(current[6].text());
        const std::string heartbeatMode(current[7].text());
        if (linkMode != "TCP Server" || protocol == "SL651") {
            if (registrationMode != "OFF" || heartbeatMode != "OFF")
                service::common::fail(18002,
                                      protocol == "SL651"
                                          ? "SL651 设备不支持配置注册包或心跳包"
                                          : "仅 TCP Server 设备支持配置注册包或心跳包",
                                      400);
        }
        if (protocol != "Modbus" && protocol != "S7")
            co_return;

        const auto siblings =
            co_await c.db().query(R"sql(
SELECT device.name, COALESCE(device.slave_id, 1), COALESCE(device.target_id, ''),
       upper(COALESCE(device.registration->>'mode', 'OFF')),
       CASE upper(COALESCE(device.registration->>'mode', 'OFF'))
         WHEN 'OFF' THEN 'OFF:'
         WHEN 'HEX' THEN 'HEX:' || upper(regexp_replace(
           COALESCE(device.registration->>'content', ''), '\\s', '', 'g'))
         ELSE 'ASCII:' || COALESCE(device.registration->>'content', '')
       END
FROM iot_device device
JOIN iot_protocol_config config
  ON config.id = device.protocol_config_id AND config.deleted_at IS NULL
WHERE device.link_id = $1 AND device.id <> $2 AND device.deleted_at IS NULL
  AND config.protocol = $3
ORDER BY device.id)sql",
                                  service::common::dbParams(linkId, excluded, protocol));

        if (linkMode == "TCP Client") {
            if (targetId.empty())
                co_return;
            for (const auto& sibling : siblings.rows()) {
                if (sibling[2].text() != targetId)
                    continue;
                const std::string name(sibling[0].text());
                if (protocol == "S7")
                    service::common::fail(
                        18006, "S7 同一目标地址只能关联一个设备，冲突设备: " + name, 409);
                if (toInt(sibling[1].text()) == slaveId)
                    service::common::fail(
                        18006, "Modbus 同一目标地址下 Slave ID 重复，冲突设备: " + name, 409);
            }
            co_return;
        }
        if (linkMode != "TCP Server")
            co_return;

        if (protocol == "Modbus" && !siblings.rows().empty()) {
            if (registrationMode == "OFF")
                service::common::fail(18006, "Modbus TCP Server 链路存在多个设备时必须配置注册包",
                                      409);
            for (const auto& sibling : siblings.rows()) {
                const std::string name(sibling[0].text());
                if (sibling[3].text() == "OFF")
                    service::common::fail(
                        18006, "Modbus TCP Server 链路存在未配置注册包的设备: " + name, 409);
                if (sibling[4].text() == registrationKey && toInt(sibling[1].text()) == slaveId)
                    service::common::fail(
                        18006, "Modbus 同一链路和注册码下 Slave ID 重复，冲突设备: " + name, 409);
            }
            co_return;
        }

        if (protocol == "S7") {
            for (const auto& sibling : siblings.rows())
                if (sibling[4].text() == registrationKey)
                    service::common::fail(18006,
                                          "S7 TCP Server 同一链路下注册码重复，冲突设备: " +
                                              std::string(sibling[0].text()),
                                          409);
        }
    }

    ruvia::Task<void> ensureUnique(ruvia::Context& c, const SaveDeviceBody& body,
                                   std::optional<std::string> excludedId) {
        const auto& name = body.name();
        const auto& code = body.deviceCode();
        if (!name && !code)
            co_return;
        const std::string nameValue = str(name);
        const std::string codeValue = str(code);
        const std::string excluded = excludedId.value_or(std::string(kNilUuid));
        const auto rows = co_await c.db().query(
            R"sql(
SELECT 1 FROM iot_device
WHERE deleted_at IS NULL AND id <> $1::uuid
  AND (($2 <> '' AND name = $2) OR ($3 <> '' AND device_code = $3)) LIMIT 1)sql",
            service::common::dbParams(excluded, std::string_view(nameValue),
                                      std::string_view(codeValue)));
        if (!rows.rows().empty())
            service::common::fail(18004, "设备名称或编码已存在", 409);
    }

    ruvia::Task<void> requireOwner(ruvia::Context& c, std::string_view ownerId) {
        const auto principal = service::middleware::requireAuth(c);
        if (principal.userId == ownerId)
            co_return;
        const auto rows = co_await c.db().query(R"sql(
SELECT EXISTS (SELECT 1 FROM sys_user_role ur JOIN sys_role r ON r.id = ur.role_id
WHERE ur.user_id = $1 AND r.code = 'superadmin' AND r.status = 'enabled'
AND r.deleted_at IS NULL))sql",
                                                service::common::dbParams(principal.userId));
        if (rows.rows().front()[0].text() != "t")
            service::common::fail(18005, "只能管理自己创建的设备", 403);
    }

    // ----- 设备分组私有工具 -----

    template <typename Row> static void fillGroup(DeviceGroupItemDto& item, const Row& row) {
        item.id(row[0].text())
            .name(row[1].text())
            .parentId(row[2].text())
            .status(row[3].text())
            .sortOrder(toInt(row[4].text()))
            .remark(row[5].text())
            .deviceCount(toInt(row[6].text()))
            .createdAt(row[7].text())
            .updatedAt(row[8].text());
    }

    ruvia::Task<void> validateParent(ruvia::Context& c, const SaveDeviceGroupBody& body,
                                     std::optional<std::string> currentId) {
        const auto& parent = body.parentId();
        if (!parent || parent->view().empty())
            co_return;
        if (!service::common::isUuid(parent->view()))
            service::common::fail(17002, "上级分组必须是 UUID", 400);
        if (currentId && parent->view() == *currentId)
            service::common::fail(17003, "上级分组不能是自身", 409);
        const auto exists = co_await c.db().query(
            "SELECT 1 FROM iot_device_group WHERE id = $1 AND deleted_at IS NULL",
            service::common::dbParams(parent->view()));
        if (exists.rows().empty())
            service::common::fail(17003, "上级分组不存在", 400);
    }

    ruvia::Task<void> requireGroupOwner(ruvia::Context& c, std::string_view ownerId) {
        const auto principal = service::middleware::requireAuth(c);
        if (principal.userId == ownerId)
            co_return;
        const auto rows = co_await c.db().query(R"sql(
SELECT EXISTS (SELECT 1 FROM sys_user_role ur JOIN sys_role r ON r.id = ur.role_id
WHERE ur.user_id = $1 AND r.code = 'superadmin' AND r.status = 'enabled'
AND r.deleted_at IS NULL))sql",
                                                service::common::dbParams(principal.userId));
        if (rows.rows().front()[0].text() != "t")
            service::common::fail(17005, "只能管理自己创建的设备分组", 403);
    }

    static constexpr std::string_view kNilUuid = "00000000-0000-0000-0000-000000000000";
};

inline DeviceService& deviceService() { return DeviceService::instance(); }

} // namespace service::device
