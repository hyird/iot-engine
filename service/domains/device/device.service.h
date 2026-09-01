#pragma once

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <map>
#include <memory_resource>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/web/ModelObject.h>
#include <ruvia/web/db/Db.h>
#include <ruvia/web/redis/Redis.h>

#include "service/features/event/config.h"
#include "service/common/http.h"
#include "service/common/timestamp.h"
#include "service/common/uuid.h"
#include "service/middleware/auth.h"
#include "service/features/edge/config.h"
#include "service/domains/device/device.types.h"
#include "service/features/telemetry/latest.h"
#include "service/utils/number.h"

namespace service::device {

enum class DeviceAccessLevel : std::int64_t {
    none = 0,
    view = 1,
    operate = 2,
    owner = 4,
};

struct DeviceActor final {
    std::string userId;
    std::string departmentId;
    bool superadmin{};
    bool canEdit{};
    bool canDelete{};
    bool canShare{};
    bool canCommand{};
    bool canGroupShare{};
};

struct DeviceAccessDecision final {
    DeviceActor actor;
    DeviceAccessLevel level{DeviceAccessLevel::none};
};

struct DeviceCapabilities final {
    bool canEdit{};
    bool canDelete{};
    bool canShare{};
    bool canCommand{};
    std::string_view accessLevel{"none"};
};

class DeviceAccessService {
  public:
    static DeviceAccessService& instance() {
        static DeviceAccessService service;
        return service;
    }

    ruvia::Task<DeviceActor> actor(ruvia::Context& c) const {
        const auto principal = service::middleware::requireAuth(c);
        const auto rows = co_await c.db().query(R"sql(
SELECT COALESCE(department.id::text, ''),
       COALESCE(BOOL_OR(role.code = 'superadmin'), FALSE),
       COALESCE(BOOL_OR(role.code = 'superadmin' OR role.permissions ? '*'
                        OR role.permissions ? 'iot:device:edit'), FALSE),
       COALESCE(BOOL_OR(role.code = 'superadmin' OR role.permissions ? '*'
                        OR role.permissions ? 'iot:device:delete'), FALSE),
       COALESCE(BOOL_OR(role.code = 'superadmin' OR role.permissions ? '*'
                        OR role.permissions ? 'iot:device:share'), FALSE),
       COALESCE(BOOL_OR(role.code = 'superadmin' OR role.permissions ? '*'
                        OR role.permissions ? 'iot:device:command'), FALSE),
       COALESCE(BOOL_OR(role.code = 'superadmin' OR role.permissions ? '*'
                        OR role.permissions ? 'iot:device-group:share'), FALSE)
FROM sys_user actor
LEFT JOIN sys_department department
       ON department.id = actor.department_id
      AND department.status = 'enabled'
      AND department.deleted_at IS NULL
LEFT JOIN sys_user_role user_role ON user_role.user_id = actor.id
LEFT JOIN sys_role role
       ON role.id = user_role.role_id
      AND role.status = 'enabled'
      AND role.deleted_at IS NULL
WHERE actor.id = $1 AND actor.status = 'enabled' AND actor.deleted_at IS NULL
GROUP BY actor.id, department.id)sql",
                                                service::common::dbParams(principal.userId));
        if (rows.empty())
            service::common::fail(service::common::kTokenInvalidErrorCode, "用户状态无效", 401);
        const auto& row = rows.front();
        DeviceActor result;
        result.userId = principal.userId;
        result.departmentId = std::string(row[0].value().value_or(std::string_view{}));
        result.superadmin = isTrue(row[1].value().value_or(std::string_view{}));
        result.canEdit = isTrue(row[2].value().value_or(std::string_view{}));
        result.canDelete = isTrue(row[3].value().value_or(std::string_view{}));
        result.canShare = isTrue(row[4].value().value_or(std::string_view{}));
        result.canCommand = isTrue(row[5].value().value_or(std::string_view{}));
        result.canGroupShare = isTrue(row[6].value().value_or(std::string_view{}));
        co_return result;
    }

    ruvia::Task<DeviceActor> requireGroupOwner(ruvia::Context& c,
                                                std::string_view groupId) const {
        auto currentActor = co_await actor(c);
        const auto rows = co_await c.db().query(
            "SELECT created_by::text FROM device_group WHERE id = $1 "
            "AND deleted_at IS NULL LIMIT 1",
            service::common::dbParams(groupId));
        if (rows.empty())
            service::common::fail(17001, "设备分组不存在", 404);
        if (!currentActor.superadmin && rows.front()[0].value().value_or(std::string_view{}) != currentActor.userId)
            service::common::fail(17005, "只能分享自己创建的设备分组", 403);
        co_return currentActor;
    }

    ruvia::Task<DeviceAccessDecision> require(ruvia::Context& c, std::string_view deviceId,
                                               DeviceAccessLevel minimum) const {
        auto currentActor = co_await actor(c);
        const auto rows = co_await c.db().query(
            "SELECT " + effectiveRankSql("device") +
                " FROM device WHERE device.id = $4 AND device.deleted_at IS NULL LIMIT 1",
            service::common::dbParams(currentActor.userId, currentActor.departmentId,
                                      currentActor.superadmin ? "true" : "false", deviceId));
        if (rows.empty())
            service::common::fail(18001, "设备不存在", 404);
        const auto level = rank(rows.front()[0].value().value_or(std::string_view{}));
        if (level == DeviceAccessLevel::none)
            service::common::fail(18001, "设备不存在", 404);
        if (level < minimum)
            service::common::fail(18005, "设备权限不足", 403);
        co_return DeviceAccessDecision{std::move(currentActor), level};
    }

    static std::string scopedDevicesCte() {
        // Calculate the actor's direct and group access once for the whole list. The previous
        // correlated effectiveRankSql("source") expression ran a recursive group walk for every
        // device, which made the list latency grow quickly with the number of devices and the
        // depth of the group tree.
        return R"sql(WITH RECURSIVE shared_group_access(group_id, access_rank) AS (
    SELECT access_grant.group_id,
           CASE access_grant.access_level WHEN 'operate' THEN 2 WHEN 'view' THEN 1 ELSE 0 END
      FROM device_group_access_grant access_grant
      JOIN device_group granted_group
        ON granted_group.id = access_grant.group_id
       AND granted_group.deleted_at IS NULL
     WHERE (access_grant.user_id = $1::uuid
        OR access_grant.department_id = NULLIF($2, '')::uuid)
       AND NOT $3::boolean
    UNION
    SELECT child.id, shared.access_rank
      FROM device_group child
      JOIN shared_group_access shared ON shared.group_id = child.parent_id
     WHERE child.deleted_at IS NULL
), group_access AS (
    SELECT group_id, MAX(access_rank) AS access_rank
      FROM shared_group_access
     GROUP BY group_id
), device_access AS (
    SELECT access_grant.device_id,
           MAX(CASE access_grant.access_level WHEN 'operate' THEN 2 WHEN 'view' THEN 1 ELSE 0 END)
             AS access_rank
      FROM device_access_grant access_grant
     WHERE (access_grant.user_id = $1::uuid
        OR access_grant.department_id = NULLIF($2, '')::uuid)
       AND NOT $3::boolean
     GROUP BY access_grant.device_id
), scoped_device AS (
    SELECT source.*,
           CASE WHEN $3::boolean OR source.created_by = $1::uuid THEN 4
                ELSE GREATEST(COALESCE(device_access.access_rank, 0),
                              COALESCE(group_access.access_rank, 0))
           END AS access_rank
      FROM device source
      LEFT JOIN device_access ON device_access.device_id = source.id
      LEFT JOIN group_access ON group_access.group_id = source.group_id
     WHERE source.deleted_at IS NULL
))sql";
    }

    static std::string visibleGroupsCte() {
        return scopedDevicesCte() + R"sql(, shared_group_tree(id) AS (
    SELECT access_grant.group_id
      FROM device_group_access_grant access_grant
     WHERE access_grant.user_id = $1::uuid
        OR access_grant.department_id = NULLIF($2, '')::uuid
    UNION
    SELECT child.id
      FROM device_group child
      JOIN shared_group_tree parent ON parent.id = child.parent_id
     WHERE child.deleted_at IS NULL
), visible_group(id) AS (
    (SELECT scoped.group_id
       FROM scoped_device scoped
      WHERE scoped.access_rank > 0 AND scoped.group_id IS NOT NULL
     UNION
     SELECT owned.id
       FROM device_group owned
      WHERE owned.deleted_at IS NULL
        AND ($3::boolean OR owned.created_by = $1::uuid)
     UNION
     SELECT shared.id FROM shared_group_tree shared)
    UNION
    SELECT parent.parent_id
      FROM device_group parent
      JOIN visible_group visible ON visible.id = parent.id
     WHERE parent.parent_id IS NOT NULL AND parent.deleted_at IS NULL
))sql";
    }

    static DeviceCapabilities capabilities(const DeviceActor& actor, DeviceAccessLevel level,
                                            bool remoteControl) {
        const auto rankValue = static_cast<std::int64_t>(level);
        DeviceCapabilities result;
        result.canEdit = actor.canEdit && level == DeviceAccessLevel::owner;
        result.canDelete = actor.canDelete && level == DeviceAccessLevel::owner;
        result.canShare = actor.canShare && level == DeviceAccessLevel::owner;
        result.canCommand = actor.canCommand && remoteControl &&
                            rankValue >= rankValueOf(DeviceAccessLevel::operate);
        result.accessLevel = name(level);
        return result;
    }

    static DeviceAccessLevel rank(std::string_view value) {
        const auto parsed =
            service::common::parseInt64(std::optional<std::string_view>{value}).value_or(0);
        if (parsed >= rankValueOf(DeviceAccessLevel::owner))
            return DeviceAccessLevel::owner;
        if (parsed == rankValueOf(DeviceAccessLevel::operate))
            return DeviceAccessLevel::operate;
        if (parsed == rankValueOf(DeviceAccessLevel::view))
            return DeviceAccessLevel::view;
        return DeviceAccessLevel::none;
    }

  private:
    static bool isTrue(std::string_view value) { return value == "t" || value == "true"; }

    static constexpr std::int64_t rankValueOf(DeviceAccessLevel level) {
        return static_cast<std::int64_t>(level);
    }

    static constexpr std::string_view name(DeviceAccessLevel level) {
        switch (level) {
        case DeviceAccessLevel::owner:
            return "owner";
        case DeviceAccessLevel::operate:
            return "operate";
        case DeviceAccessLevel::view:
            return "view";
        case DeviceAccessLevel::none:
            return "none";
        }
        return "none";
    }

    static std::string effectiveRankSql(std::string_view alias) {
        const std::string device(alias);
        return "CASE WHEN $3::boolean OR " + device +
               ".created_by = $1::uuid THEN 4 ELSE COALESCE((WITH RECURSIVE "
               "ancestor_group(id, parent_id) AS (SELECT group_entry.id, group_entry.parent_id "
               "FROM device_group group_entry WHERE group_entry.id = " +
               device +
               ".group_id AND group_entry.deleted_at IS NULL UNION ALL SELECT parent.id, "
               "parent.parent_id FROM device_group parent JOIN ancestor_group child ON "
               "child.parent_id = parent.id WHERE parent.deleted_at IS NULL), "
               "effective_access(access_rank) AS (SELECT CASE access_grant.access_level WHEN "
               "'operate' THEN 2 WHEN 'view' THEN 1 ELSE 0 END FROM device_access_grant "
               "access_grant WHERE access_grant.device_id = " +
               device +
               ".id AND (access_grant.user_id = $1::uuid OR access_grant.department_id = "
               "NULLIF($2, '')::uuid) UNION ALL SELECT CASE group_access.access_level WHEN "
               "'operate' THEN 2 WHEN 'view' THEN 1 ELSE 0 END FROM "
               "device_group_access_grant group_access JOIN ancestor_group ancestor ON "
               "ancestor.id = group_access.group_id WHERE group_access.user_id = $1::uuid OR "
               "group_access.department_id = NULLIF($2, '')::uuid) SELECT MAX(access_rank) FROM "
               "effective_access), 0) END";
    }
};

inline DeviceAccessService& deviceAccessService() { return DeviceAccessService::instance(); }

class DeviceService {
  public:
    static DeviceService& instance() {
        static DeviceService service;
        return service;
    }

    ruvia::Task<DevicePageDataDto> list(ruvia::Context& c) {
        const auto actor = co_await deviceAccessService().actor(c);
        const auto rows = co_await c.db().query(
            DeviceAccessService::scopedDevicesCte() + "SELECT " + itemColumns() +
                " FROM scoped_device d LEFT JOIN link l ON l.id = d.link_id "
                "LEFT JOIN edge_node en ON en.id = l.edge_node_id "
                "JOIN protocol_config p ON p.id = d.protocol_config_id "
                "WHERE d.access_rank > 0 ORDER BY d.group_id NULLS LAST, d.created_at, d.id",
            service::common::dbParams(actor.userId, actor.departmentId,
                                      actor.superadmin ? "true" : "false"));
        ruvia::BoxedArray<DeviceItemDto> items(ruvia::ModelOptions{.resource = c.resource()});
        std::map<std::string, DeviceItemDto*, std::less<>> itemsById;
        for (const auto& row : rows) {
            auto& item = items.emplace(c);
            fillItem(c, item, row, actor);
            itemsById.emplace(std::string(row[0].value().value_or(std::string_view{})), &item);
        }
        co_await fillLatest(c, itemsById);
        DevicePageDataDto result(c);
        result.set<"list">(std::move(items)).set<"total">(static_cast<std::int64_t>(rows.size()));
        co_return result;
    }

    ruvia::Task<DeviceRealtimePageDto> realtime(ruvia::Context& c) {
        const auto actor = co_await deviceAccessService().actor(c);
        const auto rows = co_await c.db().query(
            DeviceAccessService::scopedDevicesCte() +
                R"sql(SELECT d.id::text, d.protocol_params->>'device_code',
                       CASE WHEN d.protocol_params ? 'remote_control' THEN
                         CASE lower(COALESCE(d.protocol_params->>'remote_control', ''))
                           WHEN 'true' THEN TRUE WHEN 't' THEN TRUE WHEN '1' THEN TRUE
                           WHEN 'yes' THEN TRUE WHEN 'y' THEN TRUE WHEN 'on' THEN TRUE
                           ELSE FALSE END
                       ELSE TRUE END,
                       d.access_rank,
                       CASE WHEN l.execution = 'edge' THEN l.edge_node_id::text END,
                       CASE WHEN l.execution = 'edge' THEN l.endpoint->>'transport' END
                FROM scoped_device d
                LEFT JOIN link l ON l.id = d.link_id
                WHERE d.access_rank > 0 ORDER BY d.id)sql",
            service::common::dbParams(actor.userId, actor.departmentId,
                                      actor.superadmin ? "true" : "false"));
        ruvia::BoxedArray<DeviceRealtimeDto> items(
            ruvia::ModelOptions{.resource = c.resource()});
        std::map<std::string, DeviceRealtimeDto*, std::less<>> itemsById;
        for (const auto& row : rows) {
            const auto capabilities = DeviceAccessService::capabilities(
                actor, DeviceAccessService::rank(row[3].value().value_or(std::string_view{})), row[2].value().value_or(std::string_view{}) == "t");
            auto& item = items.emplace(c);
            item.set<"id">(row[0].value().value_or(std::string_view{}))
                .set<"deviceCode">(row[1].value().value_or(std::string_view{}))
                .set<"connected">(false)
                .set<"connectionState">("disconnected")
                .set<"elements">(ruvia::BoxedArray<DeviceElementDto>(
                    ruvia::ModelOptions{.resource = c.resource()}))
                .set<"canEdit">(capabilities.canEdit)
                .set<"canDelete">(capabilities.canDelete)
                .set<"canShare">(capabilities.canShare)
                .set<"canCommand">(capabilities.canCommand)
                .set<"accessLevel">(capabilities.accessLevel);
            if (row[4].value().has_value())
                item.set<"edgeNodeId">(row[4].value().value_or(std::string_view{}));
            if (row[5].value().has_value())
                item.set<"edgeTransport">(row[5].value().value_or(std::string_view{}));
            itemsById.emplace(std::string(row[0].value().value_or(std::string_view{})), &item);
        }
        co_await fillLatest(c, itemsById);
        DeviceRealtimePageDto result(c);
        result.set<"list">(std::move(items)).set<"total">(static_cast<std::int64_t>(rows.size()));
        co_return result;
    }

    ruvia::Task<DeviceItemDto> detail(ruvia::Context& c, std::string_view id) {
        const auto actor = co_await deviceAccessService().actor(c);
        const auto rows = co_await c.db().query(
            DeviceAccessService::scopedDevicesCte() + "SELECT " + itemColumns() +
                " FROM scoped_device d LEFT JOIN link l ON l.id = d.link_id "
                "LEFT JOIN edge_node en ON en.id = l.edge_node_id "
                "JOIN protocol_config p ON p.id = d.protocol_config_id "
                "WHERE d.id = $4 AND d.access_rank > 0 LIMIT 1",
            service::common::dbParams(actor.userId, actor.departmentId,
                                      actor.superadmin ? "true" : "false", id));
        if (rows.empty())
            service::common::fail(18001, "设备不存在", 404);
        DeviceItemDto item(c);
        fillItem(c, item, rows.front(), actor);
        std::map<std::string, DeviceItemDto*, std::less<>> itemById{{std::string(id), &item}};
        co_await fillLatest(c, itemById);
        co_await fillCommandOperations(c, itemById, id);
        co_return item;
    }

    ruvia::Task<std::string> history(ruvia::Context& c, std::string_view id) {
        (void)co_await deviceAccessService().require(c, id, DeviceAccessLevel::view);

        const auto start = c.req().query("startTime").value_or("");
        const auto end = c.req().query("endTime").value_or("");
        if (start.empty() || end.empty())
            service::common::fail(18002, "startTime 和 endTime 不能为空", 400);

        const auto requestedPage = service::common::parseInt64(c.req().query("page")).value_or(1);
        const auto requestedPageSize =
            service::common::parseInt64(c.req().query("pageSize")).value_or(20);
        const auto page = requestedPage > 0 ? requestedPage : std::int64_t{1};
        const auto pageSize =
            requestedPageSize < 1 ? std::int64_t{20}
                                  : std::min<std::int64_t>(requestedPageSize, 100);
        const auto offset = (page - 1) * pageSize;

        try {
            const auto rows = co_await c.db().query(
                R"sql(
WITH counted AS (
  SELECT COUNT(*) AS total
  FROM device_data record
  WHERE record.device_id = $1::uuid
    AND record.report_time >= $2::timestamptz
    AND record.report_time <= $3::timestamptz
    AND jsonb_typeof(record.data->'values') = 'object'
), filtered AS (
  SELECT record.id, record.protocol, record.report_time, record.source, record.data
  FROM device_data record
  WHERE record.device_id = $1::uuid
    AND record.report_time >= $2::timestamptz
    AND record.report_time <= $3::timestamptz
    AND jsonb_typeof(record.data->'values') = 'object'
  ORDER BY record.report_time DESC, record.id DESC
  LIMIT $4::bigint OFFSET $5::bigint
)
SELECT jsonb_build_object(
  'list', COALESCE(jsonb_agg(jsonb_build_object(
    'id', id,
    'protocol', protocol,
    'reportTime', iot_utc_timestamp(report_time),
    'source', source,
    'functionCode', data->>'function_code',
    'values', COALESCE(data->'values', '{}'::jsonb)
  ) ORDER BY report_time DESC, id DESC), '[]'::jsonb),
  'total', COALESCE((SELECT total FROM counted), 0),
  'page', $6::bigint,
  'pageSize', $4::bigint,
  'totalPages',
    CEIL(COALESCE((SELECT total FROM counted), 0)::numeric / $4::numeric)::bigint
)::text
FROM filtered)sql",
                service::common::dbParams(id, start, end, pageSize, offset, page));
            co_return rows.empty() ? std::string{"{\"list\":[],\"total\":0}"}
                                         : std::string{rows.front()[0].value().value_or(std::string_view{})};
        } catch (const std::exception&) {
            service::common::fail(18002, "时间范围格式错误", 400);
        }
    }

    ruvia::Task<ruvia::BoxedArray<DeviceOptionDto>> options(ruvia::Context& c) {
        const auto actor = co_await deviceAccessService().actor(c);
        const auto rows = co_await c.db().query(
            DeviceAccessService::scopedDevicesCte() +
                "SELECT id::text, name, protocol_params->>'device_code', "
                R"sql(CASE WHEN protocol_params ? 'remote_control' THEN
                         CASE lower(COALESCE(protocol_params->>'remote_control', ''))
                           WHEN 'true' THEN TRUE WHEN 't' THEN TRUE WHEN '1' THEN TRUE
                           WHEN 'yes' THEN TRUE WHEN 'y' THEN TRUE WHEN 'on' THEN TRUE
                           ELSE FALSE END
                       ELSE TRUE END, access_rank )sql"
                "FROM scoped_device WHERE access_rank > 0 AND status = 'enabled' ORDER BY name",
            service::common::dbParams(actor.userId, actor.departmentId,
                                      actor.superadmin ? "true" : "false"));
        ruvia::BoxedArray<DeviceOptionDto> result(
            ruvia::ModelOptions{.resource = c.resource()});
        for (const auto& row : rows) {
            const auto capabilities = DeviceAccessService::capabilities(
                actor, DeviceAccessService::rank(row[4].value().value_or(std::string_view{})), row[3].value().value_or(std::string_view{}) == "t");
            auto& item = result.emplace(c);
            item.set<"id">(row[0].value().value_or(std::string_view{}))
                .set<"name">(row[1].value().value_or(std::string_view{}))
                .set<"deviceCode">(row[2].value().value_or(std::string_view{}))
                .set<"canEdit">(capabilities.canEdit)
                .set<"canDelete">(capabilities.canDelete)
                .set<"canShare">(capabilities.canShare)
                .set<"canCommand">(capabilities.canCommand)
                .set<"accessLevel">(capabilities.accessLevel);
        }
        co_return result;
    }

    ruvia::Task<void> create(ruvia::Context& c, const SaveDeviceBody& body) {
        co_await validate(c, body, true);
        co_await ensureUnique(c, body, std::nullopt);
        co_await validateRuntimeIdentity(c, body, std::nullopt);
        const auto principal = service::middleware::requireAuth(c);
        const auto id = service::common::nextUuidV7();
        const std::string name(body.get<"name">()->view());
        const std::string deviceCode(body.get<"deviceCode">()->view());
        const std::string edgeNodeId = str(body.get<"edgeNodeId">());
        const std::string linkId =
            edgeNodeId.empty() ? str(body.get<"linkId">()) : service::common::nextUuidV7();
        const std::string edgeEndpoint = edgeEndpointJson(body);
        const std::string targetId = str(body.get<"targetId">());
        const std::string protocolConfigId(body.get<"protocolConfigId">()->view());
        const std::string groupId = str(body.get<"groupId">());
        const std::string status = body.get<"status">() ? std::string(body.get<"status">()->view()) : "enabled";
        const std::int64_t onlineTimeout =
            body.get<"onlineTimeout">() ? static_cast<std::int64_t>(*body.get<"onlineTimeout">()) : 300;
        const std::string remoteControl =
            (!body.get<"remoteControl">() || *body.get<"remoteControl">()) ? "true" : "false";
        const std::string modbusMode = str(body.get<"modbusMode">());
        const std::string slaveId =
            body.get<"slaveId">() ? std::to_string(static_cast<std::int64_t>(*body.get<"slaveId">())) : "";
        const std::string timezone = (body.get<"timezone">() && !body.get<"timezone">()->view().empty())
                                         ? std::string(body.get<"timezone">()->view())
                                         : "+08:00";
        const std::string heartbeat = packetJson(body.get<"heartbeat">());
        const std::string registration =
            edgeNodeId.empty() ? packetJson(body.get<"registration">()) : R"({"mode":"OFF"})";
        const std::string remark = str(body.get<"remark">());
        // Keep the optional edge-node parameter typed as text through NULLIF. If PostgreSQL
        // infers it as uuid first, the empty-string sentinel is cast to uuid before NULLIF.
        auto transaction = co_await c.db().beginTransaction();
        (void)co_await transaction.execute(
            R"sql(
WITH inserted_edge_link AS (
  INSERT INTO link(
    id, name, protocol, endpoint, status, created_by, execution, edge_node_id)
  SELECT $4::uuid, 'edge:' || $1::text, protocol, NULLIF($6::text, '')::jsonb,
         $10, $19::uuid,
         'edge', NULLIF($5::text, '')::uuid
  FROM protocol_config
  WHERE id = $8::uuid AND deleted_at IS NULL AND NULLIF($5::text, '') IS NOT NULL
  RETURNING id
)
INSERT INTO device(
  id, name, link_id, protocol_config_id, group_id, status,
  protocol_params, remark, created_by)
VALUES ($1::uuid, $2, $4::uuid, $8::uuid, NULLIF($9, '')::uuid, $10,
  jsonb_strip_nulls(jsonb_build_object(
    'device_code', $3::text,
    'target_id', NULLIF($7::text, ''),
    'online_timeout', $11::integer,
    'remote_control', $12::boolean,
    'modbus_mode', NULLIF($13::text, ''),
    'slave_id', NULLIF($14::text, '')::integer,
    'timezone', $15::text,
    'heartbeat', COALESCE(NULLIF($16::text, '')::jsonb, '{"mode":"OFF"}'::jsonb),
    'registration', COALESCE(NULLIF($17::text, '')::jsonb, '{"mode":"OFF"}'::jsonb)
  )), NULLIF($18::text, ''), $19::uuid))sql",
            service::common::dbParams(id, name, deviceCode, linkId, edgeNodeId, edgeEndpoint,
                                      targetId, protocolConfigId, groupId, status, onlineTimeout,
                                      remoteControl, modbusMode, slaveId, timezone, heartbeat,
                                      registration, remark, principal.userId));
        co_await service::message::enqueueConfigEvent(transaction, "device", "created", id);
        co_await transaction.commit();
        try {
            co_await service::telemetry::latest::initializeDevice(c.redis(), id,
                                                                               deviceCode);
            co_await service::telemetry::latest::projectDevice(c, id);
        } catch (...) {
            // PostgreSQL is authoritative; startup hydration or the first report repairs Redis.
        }
        if (!edgeNodeId.empty())
            (void)co_await service::edge::configService().queueSnapshot(c, edgeNodeId);
    }

    ruvia::Task<void> update(ruvia::Context& c, std::string_view id, const SaveDeviceBody& body) {
        (void)co_await deviceAccessService().require(c, id, DeviceAccessLevel::owner);
        const auto rows = co_await c.db().query(R"sql(
SELECT d.link_id::text, COALESCE(l.edge_node_id::text, ''),
       d.protocol_config_id::text, d.protocol_params->>'device_code'
FROM device d JOIN link l ON l.id = d.link_id
WHERE d.id = $1::uuid AND d.deleted_at IS NULL)sql",
                                                service::common::dbParams(id));
        if (rows.empty())
            service::common::fail(18001, "设备不存在", 404);
        if (body.get<"linkId">() && body.get<"linkId">()->view() != rows.front()[0].value().value_or(std::string_view{}))
            service::common::fail(18003, "设备所属链路不可修改", 409);
        if (body.get<"edgeNodeId">() && body.get<"edgeNodeId">()->view() != rows.front()[1].value().value_or(std::string_view{}))
            service::common::fail(18003, "设备所属边缘节点不可修改", 409);
        if (body.get<"protocolConfigId">() &&
            body.get<"protocolConfigId">()->view() != rows.front()[2].value().value_or(std::string_view{}))
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
        if (body.get<"name">())
            raw("name = $", ruvia::DbValue{body.get<"name">()->view()});
        if (body.get<"linkId">())
            raw("link_id = NULLIF($", ruvia::DbValue{body.get<"linkId">()->view()}),
                set += ", '')::uuid";
        if (body.get<"protocolConfigId">())
            raw("protocol_config_id = $", ruvia::DbValue{body.get<"protocolConfigId">()->view()}),
                set += "::uuid";
        if (body.get<"groupId">())
            raw("group_id = NULLIF($", ruvia::DbValue{body.get<"groupId">()->view()}),
                set += ", '')::uuid";
        if (body.get<"status">())
            raw("status = $", ruvia::DbValue{body.get<"status">()->view()});
        std::string edgeEndpointUpdate;
        if (body.get<"edgeTransport">() || body.get<"edgeInterface">() || body.get<"edgeMode">() || body.get<"edgeIp">() ||
            body.get<"edgePort">() || body.get<"serialBaudRate">() || body.get<"serialDataBits">() ||
            body.get<"serialStopBits">() || body.get<"serialParity">() || body.get<"serialRs485">()) {
            edgeEndpointUpdate = edgeEndpointJson(body);
        }
        std::string protocolParams = "protocol_params";
        const auto jsonValue = [&](std::string_view key, ruvia::DbValue value,
                                   std::string_view cast = {}) {
            params.emplace_back(std::move(value));
            protocolParams = "jsonb_set(" + protocolParams + ", '{" + std::string(key) +
                             "}', to_jsonb($" + std::to_string(params.size()) + std::string(cast) +
                             "), true)";
        };
        const auto jsonDocument = [&](std::string_view key, std::string_view value) {
            params.emplace_back(value);
            protocolParams = "jsonb_set(" + protocolParams + ", '{" + std::string(key) + "}', $" +
                             std::to_string(params.size()) + "::jsonb, true)";
        };
        if (body.get<"deviceCode">())
            jsonValue("device_code", ruvia::DbValue{body.get<"deviceCode">()->view()}, "::text");
        if (body.get<"targetId">())
            jsonValue("target_id", ruvia::DbValue{body.get<"targetId">()->view()}, "::text");
        if (body.get<"onlineTimeout">())
            jsonValue("online_timeout",
                      ruvia::DbValue{static_cast<std::int64_t>(*body.get<"onlineTimeout">())}, "::bigint");
        if (body.get<"remoteControl">())
            jsonValue("remote_control",
                      ruvia::DbValue{std::string_view{*body.get<"remoteControl">() ? "true" : "false"}},
                      "::boolean");
        if (body.get<"modbusMode">())
            jsonValue("modbus_mode", ruvia::DbValue{body.get<"modbusMode">()->view()}, "::text");
        if (body.get<"slaveId">())
            jsonValue("slave_id", ruvia::DbValue{static_cast<std::int64_t>(*body.get<"slaveId">())},
                      "::bigint");
        if (body.get<"timezone">())
            jsonValue("timezone", ruvia::DbValue{body.get<"timezone">()->view()}, "::text");
        std::string heartbeat;
        if (body.get<"heartbeat">()) {
            heartbeat = packetJson(body.get<"heartbeat">());
            jsonDocument("heartbeat", heartbeat);
        }
        std::string registration;
        if (body.get<"registration">()) {
            registration = rows.front()[1].value().value_or(std::string_view{}).empty()
                               ? packetJson(body.get<"registration">())
                               : R"({"mode":"OFF"})";
            jsonDocument("registration", registration);
        }
        if (protocolParams != "protocol_params") {
            if (!set.empty())
                set += ", ";
            set += "protocol_params = " + protocolParams;
        }
        if (body.get<"remark">())
            raw("remark = NULLIF($", ruvia::DbValue{body.get<"remark">()->view()}), set += ", '')";

        const bool updateEdgeLink =
            !rows.front()[1].value().value_or(std::string_view{}).empty() &&
            (!edgeEndpointUpdate.empty() || body.get<"status">());
        {
            auto transaction = co_await c.db().beginTransaction();
            if (!set.empty() || updateEdgeLink) {
                if (!set.empty()) {
                    params.emplace_back(id);
                    (void)co_await transaction.execute(
                        "UPDATE device SET " + set + ", updated_at = NOW() WHERE id = $" +
                            std::to_string(params.size()),
                        params);
                }
                if (updateEdgeLink) {
                    const auto edgeStatus = body.get<"status">()
                                                ? std::string(body.get<"status">()->view())
                                                : std::string{};
                    (void)co_await transaction.execute(R"sql(
UPDATE link
SET endpoint = CASE WHEN NULLIF($1::text, '') IS NULL THEN endpoint
                    ELSE NULLIF($1::text, '')::jsonb END,
    status = COALESCE(NULLIF($2, '')::status_enum, status),
    updated_at = NOW()
WHERE id = $3::uuid AND execution = 'edge'
  AND (
    (NULLIF($1::text, '') IS NOT NULL
     AND endpoint IS DISTINCT FROM NULLIF($1::text, '')::jsonb)
    OR (NULLIF($2, '') IS NOT NULL AND status IS DISTINCT FROM $2::status_enum)
  ))sql",
                        service::common::dbParams(
                            edgeEndpointUpdate, edgeStatus,
                            rows.front()[0].value().value_or(std::string_view{})));
                }
            }
            co_await service::message::enqueueConfigEvent(transaction, "device", "updated", id);
            co_await transaction.commit();
        }
        try {
            if (body.get<"deviceCode">() && body.get<"deviceCode">()->view() != rows.front()[3].value().value_or(std::string_view{}))
                co_await service::telemetry::latest::eraseDevice(c.redis(),
                                                                 rows.front()[3].value().value_or(std::string_view{}));
            co_await service::telemetry::latest::projectDevice(c, id);
        } catch (...) {
            // PostgreSQL remains authoritative; startup hydration repairs Redis read models.
        }
        if (!rows.front()[1].value().value_or(std::string_view{}).empty())
            (void)co_await service::edge::configService().queueSnapshot(
                c, rows.front()[1].value().value_or(std::string_view{}));
    }

    ruvia::Task<void> remove(ruvia::Context& c, std::string_view id) {
        (void)co_await deviceAccessService().require(c, id, DeviceAccessLevel::owner);
        const auto rows = co_await c.db().query(R"sql(
SELECT d.protocol_params->>'device_code', COALESCE(l.edge_node_id::text, ''),
       d.link_id::text, l.execution
FROM device d JOIN link l ON l.id = d.link_id
WHERE d.id = $1::uuid AND d.deleted_at IS NULL)sql",
                                                service::common::dbParams(id));
        if (rows.empty())
            service::common::fail(18001, "设备不存在", 404);
        auto transaction = co_await c.db().beginTransaction();
        (void)co_await transaction.execute(
            "UPDATE device SET deleted_at = NOW(), updated_at = NOW() WHERE id = $1",
            service::common::dbParams(id));
        if (rows.front()[3].value().value_or(std::string_view{}) == "edge") {
            (void)co_await transaction.execute(
                "UPDATE link SET deleted_at = NOW(), updated_at = NOW() "
                "WHERE id = $1::uuid AND execution = 'edge'",
                service::common::dbParams(rows.front()[2].value().value_or(std::string_view{})));
        }
        co_await service::message::enqueueConfigEvent(transaction, "device", "deleted", id);
        co_await transaction.commit();
        try {
            co_await service::telemetry::latest::eraseDevice(
                c.redis(), rows.front()[0].value().value_or(std::string_view{}));
        } catch (...) {
            // The next startup hydration removes stale Redis state for deleted devices.
        }
        if (!rows.front()[1].value().value_or(std::string_view{}).empty())
            (void)co_await service::edge::configService().queueSnapshot(
                c, rows.front()[1].value().value_or(std::string_view{}));
    }

    // ===== 设备分组（合并入同一 DeviceService 类）=====

    ruvia::Task<ruvia::BoxedArray<DeviceGroupItemDto>> listGroups(ruvia::Context& c, bool withCount) {
        const auto actor = co_await deviceAccessService().actor(c);
        const std::string countExpr =
            withCount ? "(SELECT COUNT(*) FROM scoped_device d WHERE d.group_id = g.id "
                        "AND d.access_rank > 0)"
                      : "0";
        const std::string sql =
            DeviceAccessService::visibleGroupsCte() +
            " SELECT g.id::text, g.name, COALESCE(g.parent_id::text, ''), g.status, g.sort_order, "
            "COALESCE(g.remark, ''), " +
            countExpr +
            ", iot_utc_timestamp(g.created_at), iot_utc_timestamp(g.updated_at), "
            "g.created_by::text FROM device_group g "
            "WHERE g.deleted_at IS NULL AND g.id IN (SELECT id FROM visible_group) "
            "ORDER BY g.sort_order, g.id";
        const auto rows = co_await c.db().query(
            sql, service::common::dbParams(actor.userId, actor.departmentId,
                                           actor.superadmin ? "true" : "false"));
        ruvia::BoxedArray<DeviceGroupItemDto> result(
            ruvia::ModelOptions{.resource = c.resource()});
        for (const auto& row : rows)
            fillGroup(result.emplace(c), row, actor);
        co_return result;
    }

    ruvia::Task<DeviceGroupItemDto> groupDetail(ruvia::Context& c, std::string_view id) {
        const auto actor = co_await deviceAccessService().actor(c);
        const auto rows = co_await c.db().query(
            DeviceAccessService::visibleGroupsCte() +
                " SELECT device_group.id::text, device_group.name, "
                "COALESCE(device_group.parent_id::text, ''), device_group.status, "
                "device_group.sort_order, COALESCE(device_group.remark, ''), "
                "(SELECT COUNT(*) FROM scoped_device scoped WHERE scoped.group_id = "
                "device_group.id AND scoped.access_rank > 0), "
                "iot_utc_timestamp(device_group.created_at), "
                "iot_utc_timestamp(device_group.updated_at), "
                "device_group.created_by::text FROM device_group "
                "WHERE device_group.id = $4 "
                "AND device_group.deleted_at IS NULL "
                "AND device_group.id IN (SELECT id FROM visible_group) LIMIT 1",
            service::common::dbParams(actor.userId, actor.departmentId,
                                      actor.superadmin ? "true" : "false", id));
        if (rows.empty())
            service::common::fail(17001, "设备分组不存在", 404);
        DeviceGroupItemDto item(c);
        fillGroup(item, rows.front(), actor);
        co_return item;
    }

    ruvia::Task<void> createGroup(ruvia::Context& c, const SaveDeviceGroupBody& body) {
        co_await validateParent(c, body, std::nullopt);
        const auto principal = service::middleware::requireAuth(c);
        const auto id = service::common::nextUuidV7();
        const std::string name(body.get<"name">()->view());
        const std::string parentId = body.get<"parentId">() ? std::string(body.get<"parentId">()->view()) : "";
        const std::string status = body.get<"status">() ? std::string(body.get<"status">()->view()) : "enabled";
        const std::int64_t sortOrder =
            body.get<"sortOrder">() ? static_cast<std::int64_t>(*body.get<"sortOrder">()) : 0;
        const std::string remark = body.get<"remark">() ? std::string(body.get<"remark">()->view()) : "";
        (void)co_await c.db().execute(
            R"sql(
INSERT INTO device_group(id, name, parent_id, status, sort_order, remark, created_by)
VALUES ($1::uuid, $2, NULLIF($3, '')::uuid, $4, $5, NULLIF($6, ''), $7))sql",
            service::common::dbParams(id, name, parentId, status, sortOrder, remark,
                                      principal.userId));
    }

    ruvia::Task<void> updateGroup(ruvia::Context& c, std::string_view id,
                                  const SaveDeviceGroupBody& body) {
        const auto rows = co_await c.db().query(
            "SELECT created_by FROM device_group WHERE id = $1 AND deleted_at IS NULL",
            service::common::dbParams(id));
        if (rows.empty())
            service::common::fail(17001, "设备分组不存在", 404);
        co_await requireGroupOwner(c, rows.front()[0].value().value_or(std::string_view{}));
        co_await validateParent(c, body, std::string(id));

        std::string set;
        std::vector<ruvia::DbValue> params;
        auto assign = [&](std::string_view column, ruvia::DbValue value) {
            if (!set.empty())
                set += ", ";
            params.emplace_back(std::move(value));
            set += std::string(column) + " = $" + std::to_string(params.size());
        };
        if (body.get<"name">())
            assign("name", ruvia::DbValue{body.get<"name">()->view()});
        if (body.get<"parentId">()) {
            if (!set.empty())
                set += ", ";
            params.emplace_back(ruvia::DbValue{body.get<"parentId">()->view()});
            set += "parent_id = NULLIF($" + std::to_string(params.size()) + ", '')::uuid";
        }
        if (body.get<"status">())
            assign("status", ruvia::DbValue{body.get<"status">()->view()});
        if (body.get<"sortOrder">())
            assign("sort_order", ruvia::DbValue{static_cast<std::int64_t>(*body.get<"sortOrder">())});
        if (body.get<"remark">()) {
            if (!set.empty())
                set += ", ";
            params.emplace_back(ruvia::DbValue{body.get<"remark">()->view()});
            set += "remark = NULLIF($" + std::to_string(params.size()) + ", '')";
        }
        if (set.empty())
            co_return;
        params.emplace_back(id);
        (void)co_await c.db().execute("UPDATE device_group SET " + set +
                                          ", updated_at = NOW() WHERE id = $" +
                                          std::to_string(params.size()),
                                      params);
    }

    ruvia::Task<void> removeGroup(ruvia::Context& c, std::string_view id) {
        const auto rows = co_await c.db().query(
            "SELECT created_by FROM device_group WHERE id = $1 AND deleted_at IS NULL",
            service::common::dbParams(id));
        if (rows.empty())
            service::common::fail(17001, "设备分组不存在", 404);
        co_await requireGroupOwner(c, rows.front()[0].value().value_or(std::string_view{}));
        const auto used = co_await c.db().query(R"sql(
SELECT EXISTS (SELECT 1 FROM device_group WHERE parent_id = $1 AND deleted_at IS NULL)
    OR EXISTS (SELECT 1 FROM device WHERE group_id = $1 AND deleted_at IS NULL))sql",
                                                service::common::dbParams(id));
        if (used.front()[0].value().value_or(std::string_view{}) == "t")
            service::common::fail(17004, "请先移除子分组和设备", 409);
        (void)co_await c.db().execute(
            "UPDATE device_group SET deleted_at = NOW(), updated_at = NOW() WHERE id = $1",
            service::common::dbParams(id));
    }

  private:
    static std::int64_t toInt(std::string_view value, std::int64_t fallback = 0) {
        return service::common::parseInt64(std::optional<std::string_view>{value})
            .value_or(fallback);
    }

    static std::optional<double> parseDouble(std::string_view value) {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
            value.remove_prefix(1);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
            value.remove_suffix(1);
        if (value.empty())
            return std::nullopt;
        return service::utils::decimal(value);
    }

    static double toDouble(std::string_view value, double fallback = 0) {
        return parseDouble(value).value_or(fallback);
    }

    static std::string edgeDeviceStatusKey(std::string_view nodeId, std::string_view deviceId) {
        return "iot:runtime:edge:" + std::string(nodeId) + ":device:" +
               std::string(deviceId);
    }

    // 列顺序必须与 fillItem 的 row 下标严格对应。
    static std::string itemColumns() {
        return R"sql(d.id::text, d.name, d.protocol_params->>'device_code', d.link_id::text,
  NULLIF(d.protocol_params->>'target_id', ''),
  d.protocol_config_id::text, d.group_id::text, d.status,
  COALESCE(
    CASE WHEN COALESCE(d.protocol_params->>'online_timeout', '') ~ '^-?[0-9]{1,18}$'
         THEN (d.protocol_params->>'online_timeout')::integer END, 300),
  CASE WHEN d.protocol_params ? 'remote_control' THEN
    CASE lower(COALESCE(d.protocol_params->>'remote_control', ''))
      WHEN 'true' THEN TRUE WHEN 't' THEN TRUE WHEN '1' THEN TRUE
      WHEN 'yes' THEN TRUE WHEN 'y' THEN TRUE WHEN 'on' THEN TRUE
      ELSE FALSE END
    ELSE TRUE END,
  NULLIF(d.protocol_params->>'modbus_mode', ''),
  NULLIF(d.protocol_params->>'slave_id', ''),
  COALESCE(NULLIF(d.protocol_params->>'timezone', ''), '+08:00'),
  d.protocol_params->'heartbeat'->>'mode', d.protocol_params->'heartbeat'->>'content',
  d.protocol_params->'registration'->>'mode', d.protocol_params->'registration'->>'content',
  COALESCE(d.remark, ''), d.created_by::text,
  iot_utc_timestamp(d.created_at), iot_utc_timestamp(d.updated_at),
  COALESCE(l.name, ''), COALESCE(l.endpoint->>'mode', ''), COALESCE(l.protocol, ''), p.name, p.protocol,
  COALESCE(NULLIF(p.config->>'readInterval', ''), NULLIF(p.config->>'pollInterval', '')),
  NULLIF(p.config->>'storageInterval', ''),
  CASE p.protocol
      WHEN 'Modbus' THEN jsonb_array_length(COALESCE(p.config->'registers', '[]'::jsonb))
      WHEN 'S7' THEN jsonb_array_length(COALESCE(p.config->'areas', '[]'::jsonb))
      ELSE COALESCE((SELECT SUM(
          jsonb_array_length(COALESCE(func->'elements', '[]'::jsonb)) +
          jsonb_array_length(COALESCE(func->'responseElements', '[]'::jsonb)))
        FROM jsonb_array_elements(COALESCE(p.config->'funcs', '[]'::jsonb)) func), 0) END,
  d.access_rank,
  CASE WHEN l.execution = 'edge' THEN l.edge_node_id::text END,
  COALESCE(en.name, ''), COALESCE(en.imei, ''),
  CASE WHEN l.execution = 'edge' THEN NULLIF(l.endpoint->>'transport', '') END,
  CASE WHEN l.execution = 'edge' THEN NULLIF(l.endpoint->>'interface', '') END,
  CASE WHEN l.execution = 'edge' THEN NULLIF(l.endpoint->>'mode', '') END,
  CASE WHEN l.execution = 'edge' THEN NULLIF(l.endpoint->>'ip', '') END,
  CASE WHEN l.execution = 'edge' THEN NULLIF(l.endpoint->>'port', '') END,
  CASE WHEN l.execution = 'edge' THEN NULLIF(l.endpoint->>'baud_rate', '') END,
  CASE WHEN l.execution = 'edge' THEN NULLIF(l.endpoint->>'data_bits', '') END,
  CASE WHEN l.execution = 'edge' THEN NULLIF(l.endpoint->>'stop_bits', '') END,
  CASE WHEN l.execution = 'edge' THEN NULLIF(l.endpoint->>'parity', '') END,
  CASE WHEN l.execution = 'edge' THEN
    CASE lower(COALESCE(l.endpoint->>'rs485', ''))
      WHEN 'true' THEN TRUE WHEN 't' THEN TRUE WHEN '1' THEN TRUE
      WHEN 'yes' THEN TRUE WHEN 'y' THEN TRUE WHEN 'on' THEN TRUE
      ELSE FALSE END
    END)sql";
    }

    template <typename Row>
    static void fillItem(ruvia::Context& c, DeviceItemDto& item, Row&& row,
                         const DeviceActor& actor) {
        item.set<"id">(row[0].value().value_or(std::string_view{}));
        item.set<"name">(row[1].value().value_or(std::string_view{}));
        item.set<"deviceCode">(row[2].value().value_or(std::string_view{}));
        if (row[3].value().has_value())
            item.set<"linkId">(row[3].value().value_or(std::string_view{}));
        if (row[4].value().has_value())
            item.set<"targetId">(row[4].value().value_or(std::string_view{}));
        item.set<"protocolConfigId">(row[5].value().value_or(std::string_view{}));
        if (row[6].value().has_value())
            item.set<"groupId">(row[6].value().value_or(std::string_view{}));
        item.set<"status">(row[7].value().value_or(std::string_view{}));
        item.set<"onlineTimeout">(toInt(row[8].value().value_or(std::string_view{})));
        item.set<"remoteControl">(row[9].value().value_or(std::string_view{}) == "t");
        if (row[10].value().has_value())
            item.set<"modbusMode">(row[10].value().value_or(std::string_view{}));
        if (row[11].value().has_value())
            item.set<"slaveId">(toInt(row[11].value().value_or(std::string_view{})));
        item.set<"timezone">(row[12].value().value_or(std::string_view{}));
        {
            DevicePacketDto heartbeat(c);
            if (row[13].value().has_value())
                heartbeat.set<"mode">(row[13].value().value_or(std::string_view{}));
            if (row[14].value().has_value())
                heartbeat.set<"content">(row[14].value().value_or(std::string_view{}));
            item.set<"heartbeat">(std::move(heartbeat));
        }
        if (!row[30].value().has_value()) {
            DevicePacketDto registration(c);
            if (row[15].value().has_value())
                registration.set<"mode">(row[15].value().value_or(std::string_view{}));
            if (row[16].value().has_value())
                registration.set<"content">(row[16].value().value_or(std::string_view{}));
            item.set<"registration">(std::move(registration));
        }
        item.set<"remark">(row[17].value().value_or(std::string_view{}));
        item.set<"createdBy">(row[18].value().value_or(std::string_view{}));
        item.set<"createdAt">(row[19].value().value_or(std::string_view{}));
        item.set<"updatedAt">(row[20].value().value_or(std::string_view{}));
        item.set<"linkName">(row[21].value().value_or(std::string_view{}));
        item.set<"linkMode">(row[22].value().value_or(std::string_view{}));
        item.set<"linkProtocol">(row[23].value().value_or(std::string_view{}));
        item.set<"protocolName">(row[24].value().value_or(std::string_view{}));
        item.set<"protocolType">(row[25].value().value_or(std::string_view{}));
        if (row[26].value().has_value()) {
            if (const auto value = parseDouble(row[26].value().value_or(std::string_view{})))
                item.set<"readInterval">(*value);
        }
        if (row[27].value().has_value()) {
            if (const auto value = parseDouble(row[27].value().value_or(std::string_view{})))
                item.set<"storageInterval">(*value);
        }
        const auto capabilities = DeviceAccessService::capabilities(
            actor, DeviceAccessService::rank(row[29].value().value_or(std::string_view{})), row[9].value().value_or(std::string_view{}) == "t");
        item.set<"elementCount">(toInt(row[28].value().value_or(std::string_view{})));
        item.set<"connected">(false);
        item.set<"connectionState">("disconnected");
        item.set<"elements">(ruvia::BoxedArray<DeviceElementDto>(
            ruvia::ModelOptions{.resource = c.resource()}));
        item.set<"canEdit">(capabilities.canEdit);
        item.set<"canDelete">(capabilities.canDelete);
        item.set<"canShare">(capabilities.canShare);
        item.set<"canCommand">(capabilities.canCommand);
        item.set<"accessLevel">(capabilities.accessLevel);
        if (row[30].value().has_value()) {
            item.set<"edgeNodeId">(row[30].value().value_or(std::string_view{}));
            item.set<"edgeNodeName">(row[31].value().value_or(std::string_view{}));
            item.set<"edgeNodeImei">(row[32].value().value_or(std::string_view{}));
        }
        if (row[33].value().has_value())
            item.set<"edgeTransport">(row[33].value().value_or(std::string_view{}));
        if (row[34].value().has_value())
            item.set<"edgeInterface">(row[34].value().value_or(std::string_view{}));
        if (row[35].value().has_value())
            item.set<"edgeMode">(row[35].value().value_or(std::string_view{}));
        if (row[36].value().has_value())
            item.set<"edgeIp">(row[36].value().value_or(std::string_view{}));
        if (row[37].value().has_value())
            item.set<"edgePort">(toInt(row[37].value().value_or(std::string_view{})));
        if (row[38].value().has_value())
            item.set<"serialBaudRate">(toInt(row[38].value().value_or(std::string_view{})));
        if (row[39].value().has_value())
            item.set<"serialDataBits">(toInt(row[39].value().value_or(std::string_view{})));
        if (row[40].value().has_value())
            item.set<"serialStopBits">(toInt(row[40].value().value_or(std::string_view{})));
        if (row[41].value().has_value())
            item.set<"serialParity">(row[41].value().value_or(std::string_view{}));
        if (row[42].value().has_value())
            item.set<"serialRs485">(row[42].value().value_or(std::string_view{}) == "t");
    }

    template <typename Item>
    static ruvia::Task<void>
    fillLatest(ruvia::Context& c, const std::map<std::string, Item*, std::less<>>& items) {
        if (items.empty())
            co_return;
        auto pipeline = c.redis().pipeline();
        enum class ReplyKind { runtime, latest, edgeDevice };
        struct ReplyBinding {
            ReplyKind kind;
            Item* item;
        };
        std::vector<ReplyBinding> bindings;
        bindings.reserve(items.size() * 3);
        for (const auto& [id, item] : items) {
            (void)id;
            if (!item->get<"deviceCode">())
                continue;
            // The runtime hash also contains worker/session bookkeeping. The list only needs
            // these two fields, so HMGET avoids transferring and parsing the rest of the hash.
            pipeline.command("HMGET", service::telemetry::latest::runtimeKey(
                                         item->get<"deviceCode">()->view()),
                             "connection_id", "last_report_at_ms");
            bindings.push_back({ReplyKind::runtime, item});
            pipeline.hgetAll(service::telemetry::latest::latestKey(item->get<"deviceCode">()->view()));
            bindings.push_back({ReplyKind::latest, item});
            if (item->get<"edgeNodeId">() && item->get<"edgeTransport">() &&
                item->get<"edgeTransport">()->view() == "tcp") {
                pipeline.command(
                    "HMGET",
                    edgeDeviceStatusKey(item->get<"edgeNodeId">()->view(), item->get<"id">()->view()),
                    "state", "reason", "client_count", "last_activity_at_ms");
                bindings.push_back({ReplyKind::edgeDevice, item});
            }
        }
        const auto replies = co_await std::move(pipeline).exec();
        for (std::size_t index = 0; index < bindings.size() && index < replies.size(); ++index) {
            const auto& binding = bindings[index];
            if (binding.kind == ReplyKind::runtime) {
                applyRuntime(*binding.item, replies[index]);
            } else if (binding.kind == ReplyKind::latest) {
                applyLatestElements(c, *binding.item, replies[index]);
            } else {
                applyEdgeRuntime(c, *binding.item, replies[index]);
            }
        }
    }

    static ruvia::Task<void>
    fillCommandOperations(ruvia::Context& c,
                          const std::map<std::string, DeviceItemDto*, std::less<>>& items,
                          std::optional<std::string_view> onlyDevice) {
        if (items.empty())
            co_return;
        std::string filter;
        std::vector<ruvia::DbValue> params;
        if (onlyDevice) {
            filter = " AND d.id = $1::uuid";
            params.emplace_back(*onlyDevice);
        }
        const std::string sql = R"sql(
WITH command_element AS (
  SELECT d.id AS device_id, 'MODBUS_WRITE' AS operation_key, '写寄存器' AS operation_name,
         element, 1::bigint AS operation_position, element_position,
         preset, preset_position
  FROM device d
  JOIN protocol_config p ON p.id = d.protocol_config_id AND p.protocol = 'Modbus'
  CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'registers', '[]'::jsonb))
    WITH ORDINALITY AS elements(element, element_position)
  LEFT JOIN LATERAL jsonb_array_elements(
    CASE WHEN element->>'registerType' = 'COIL' THEN jsonb_build_array(
      jsonb_build_object(
        'label', COALESCE((SELECT mapping->>'label'
                           FROM jsonb_array_elements(
                             COALESCE(element->'dictConfig'->'items', '[]'::jsonb)) mapping
                           WHERE mapping->>'key' = '1' LIMIT 1), '1'),
        'value', '1'),
      jsonb_build_object(
        'label', COALESCE((SELECT mapping->>'label'
                           FROM jsonb_array_elements(
                             COALESCE(element->'dictConfig'->'items', '[]'::jsonb)) mapping
                           WHERE mapping->>'key' = '0' LIMIT 1), '0'),
        'value', '0'))
    ELSE '[]'::jsonb END)
    WITH ORDINALITY AS presets(preset, preset_position) ON TRUE
  WHERE d.deleted_at IS NULL AND p.deleted_at IS NULL AND p.enabled = TRUE
    AND CASE lower(COALESCE(element->>'writable', ''))
          WHEN 'true' THEN TRUE WHEN 't' THEN TRUE WHEN '1' THEN TRUE
          WHEN 'yes' THEN TRUE WHEN 'y' THEN TRUE WHEN 'on' THEN TRUE
          ELSE FALSE END)sql" +
                                filter + R"sql(
  UNION ALL
  SELECT d.id, 'S7_WRITE', '写寄存器', element, 2, element_position,
         preset, preset_position
  FROM device d
  JOIN protocol_config p ON p.id = d.protocol_config_id AND p.protocol = 'S7'
  CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'areas', '[]'::jsonb))
    WITH ORDINALITY AS elements(element, element_position)
  LEFT JOIN LATERAL jsonb_array_elements(
    CASE WHEN element->>'dataType' = 'BOOL'
         THEN '[{"label":"1","value":"1"},{"label":"0","value":"0"}]'::jsonb
         ELSE '[]'::jsonb END)
    WITH ORDINALITY AS presets(preset, preset_position) ON TRUE
  WHERE d.deleted_at IS NULL AND p.deleted_at IS NULL AND p.enabled = TRUE
    AND CASE lower(COALESCE(element->>'writable', ''))
          WHEN 'true' THEN TRUE WHEN 't' THEN TRUE WHEN '1' THEN TRUE
          WHEN 'yes' THEN TRUE WHEN 'y' THEN TRUE WHEN 'on' THEN TRUE
          ELSE FALSE END)sql" +
                                filter + R"sql(
  UNION ALL
  SELECT d.id, function->>'funcCode',
         COALESCE(NULLIF(function->>'name', ''), function->>'funcCode'),
         element, function_position + 2, element_position,
         preset, preset_position
  FROM device d
  JOIN protocol_config p ON p.id = d.protocol_config_id AND p.protocol = 'SL651'
  CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'funcs', '[]'::jsonb))
    WITH ORDINALITY AS functions(function, function_position)
  CROSS JOIN LATERAL jsonb_array_elements(COALESCE(function->'elements', '[]'::jsonb))
    WITH ORDINALITY AS elements(element, element_position)
  LEFT JOIN LATERAL jsonb_array_elements(COALESCE(element->'options', '[]'::jsonb))
    WITH ORDINALITY AS presets(preset, preset_position) ON TRUE
  WHERE d.deleted_at IS NULL AND p.deleted_at IS NULL AND p.enabled = TRUE
    AND function->>'dir' = 'DOWN' AND COALESCE(element->>'encode', '') <> 'JPEG')sql" +
                                filter + R"sql(
)
SELECT device_id::text, operation_key, operation_name,
       element->>'id', element->>'name', COALESCE(element->>'unit', ''),
       COALESCE(element->>'registerType', ''), COALESCE(element->>'dataType', ''),
       element->>'size', COALESCE(element->>'encode', ''),
       element->>'length', element->>'digits',
       preset->>'label', preset->>'value',
       operation_position, element_position, preset_position
FROM command_element
ORDER BY device_id, operation_position, operation_key, element_position,
         preset_position NULLS LAST)sql";
        const auto rows = co_await c.db().query(sql, params);

        struct OptionData {
            std::string label;
            std::string value;
        };
        struct ElementData {
            std::string id;
            std::string name;
            std::string unit;
            std::string registerType;
            std::string dataType;
            std::optional<std::int64_t> size;
            std::string encode;
            std::optional<std::int64_t> length;
            std::optional<std::int64_t> digits;
            std::vector<OptionData> options;
        };
        struct OperationData {
            std::string key;
            std::string name;
            std::vector<ElementData> elements;
        };
        std::map<std::string, std::vector<OperationData>, std::less<>> configured;
        for (const auto& row : rows) {
            const auto deviceId = std::string(row[0].value().value_or(std::string_view{}));
            if (!items.contains(deviceId))
                continue;
            auto& operations = configured[deviceId];
            const auto operationKey = std::string(row[1].value().value_or(std::string_view{}));
            auto operation =
                std::find_if(operations.begin(), operations.end(),
                             [&](const auto& value) { return value.key == operationKey; });
            if (operation == operations.end()) {
                operations.push_back({operationKey, std::string(row[2].value().value_or(std::string_view{})), {}});
                operation = std::prev(operations.end());
            }
            const auto elementId = std::string(row[3].value().value_or(std::string_view{}));
            auto element = std::find_if(operation->elements.begin(), operation->elements.end(),
                                        [&](const auto& value) { return value.id == elementId; });
            if (element == operation->elements.end()) {
                ElementData data;
                data.id = elementId;
                data.name = std::string(row[4].value().value_or(std::string_view{}));
                data.unit = std::string(row[5].value().value_or(std::string_view{}));
                data.registerType = std::string(row[6].value().value_or(std::string_view{}));
                data.dataType = std::string(row[7].value().value_or(std::string_view{}));
                if (row[8].value().has_value())
                    data.size = toInt(row[8].value().value_or(std::string_view{}));
                data.encode = std::string(row[9].value().value_or(std::string_view{}));
                if (row[10].value().has_value())
                    data.length = toInt(row[10].value().value_or(std::string_view{}));
                if (row[11].value().has_value())
                    data.digits = toInt(row[11].value().value_or(std::string_view{}));
                operation->elements.push_back(std::move(data));
                element = std::prev(operation->elements.end());
            }
            if (row[12].value().has_value() && row[13].value().has_value())
                element->options.push_back(
                    {std::string(row[12].value().value_or(std::string_view{})), std::string(row[13].value().value_or(std::string_view{}))});
        }

        for (auto& [deviceId, operations] : configured) {
            const auto item = items.find(deviceId);
            if (item == items.end())
                continue;
            ruvia::BoxedArray<DeviceCommandOperationDto> operationDtos(
                ruvia::ModelOptions{.resource = c.resource()});
            for (const auto& operation : operations) {
                auto& operationDto = operationDtos.emplace(c);
                operationDto.set<"name">(operation.name);
                ruvia::BoxedArray<DeviceCommandOperationElementDto> elementDtos(
                    ruvia::ModelOptions{.resource = c.resource()});
                for (const auto& element : operation.elements) {
                    auto& elementDto = elementDtos.emplace(c);
                    elementDto.set<"elementId">(element.id).set<"name">(element.name).set<"value">("");
                    if (!element.unit.empty())
                        elementDto.set<"unit">(element.unit);
                    if (!element.registerType.empty())
                        elementDto.set<"registerType">(element.registerType);
                    if (!element.dataType.empty())
                        elementDto.set<"dataType">(element.dataType);
                    if (element.size)
                        elementDto.set<"size">(*element.size);
                    if (!element.encode.empty())
                        elementDto.set<"encode">(element.encode);
                    if (element.length)
                        elementDto.set<"length">(*element.length);
                    if (element.digits)
                        elementDto.set<"digits">(*element.digits);
                    if (!element.options.empty()) {
                        ruvia::BoxedArray<DeviceCommandOptionDto> optionDtos(
                            ruvia::ModelOptions{.resource = c.resource()});
                        for (const auto& option : element.options)
                            optionDtos.emplace(c).set<"label">(option.label).set<"value">(option.value);
                        elementDto.set<"options">(std::move(optionDtos));
                    }
                }
                operationDto.set<"elements">(std::move(elementDtos));
            }
            item->second->set<"commandOperations">(std::move(operationDtos));
        }
    }

    static std::string redisHashField(const ruvia::RedisValue& value, std::string_view field) {
        if (value.kind() != ruvia::RedisValue::Kind::kArray)
            return {};
        const auto& entries = value.array();
        for (std::size_t index = 0; index + 1 < entries.size(); index += 2) {
            if (entries[index].kind() != ruvia::RedisValue::Kind::kString ||
                entries[index + 1].kind() != ruvia::RedisValue::Kind::kString)
                continue;
            if (entries[index].string() == field)
                return std::string(entries[index + 1].string());
        }
        return {};
    }

    static std::string redisArrayField(const ruvia::RedisValue& value, std::size_t index) {
        if (value.kind() != ruvia::RedisValue::Kind::kArray)
            return {};
        const auto entries = value.array();
        if (index >= entries.size() || entries[index].kind() != ruvia::RedisValue::Kind::kString)
            return {};
        return std::string(entries[index].string());
    }

    static std::optional<ruvia::JsonValue> jsonField(const ruvia::JsonValue& object,
                                                     std::string_view field) {
        if (!object.isObject())
            return std::nullopt;
        std::optional<ruvia::JsonValue> result;
        const auto valid = ruvia::detail::visitJsonObjectFields(
            ruvia::detail::ResolvedPmrResourceTag{}, object.view(),
            std::pmr::get_default_resource(),
            [&](std::string_view key, std::string_view value) {
                if (key == field)
                    result = ruvia::JsonValue::parse(value);
                return true;
            });
        return valid ? result : std::nullopt;
    }

    static std::optional<std::string> jsonString(const ruvia::JsonValue& object,
                                                 std::string_view field) {
        const auto value = object.get<ruvia::String>(field);
        if (!value)
            return std::nullopt;
        return std::string(value->view());
    }

    static std::int64_t jsonInt(const ruvia::JsonValue& object, std::string_view field,
                                std::int64_t fallback) {
        if (const auto value = object.get<ruvia::Int64>(field))
            return static_cast<std::int64_t>(*value);
        const auto raw = jsonField(object, field);
        if (!raw)
            return fallback;
        return service::common::parseInt64(std::optional<std::string_view>{raw->view()})
            .value_or(fallback);
    }

    static double jsonDouble(const ruvia::JsonValue& object, std::string_view field,
                             double fallback) {
        const auto raw = jsonField(object, field);
        if (!raw)
            return fallback;
        return toDouble(raw->view(), fallback);
    }

    template <typename Item>
    static void applyRuntime(Item& item, const ruvia::RedisValue& reply) {
        const auto reportTime = redisArrayField(reply, 1);
        if (!reportTime.empty()) {
            const auto milliseconds = toInt(reportTime);
            if (milliseconds > 0)
                item.set<"reportTime">(service::common::utcTimestampFromMilliseconds(milliseconds));
        }
        const bool connected = !redisArrayField(reply, 0).empty();
        item.set<"connected">(connected)
            .set<"connectionState">(connected ? "connected" : "disconnected");
    }

    template <typename Item>
    static void applyEdgeRuntime(ruvia::Context& c, Item& item,
                                 const ruvia::RedisValue& reply) {
        const auto state = redisArrayField(reply, 0);
        if (state.empty())
            return;
        const bool connected = state == "connected" || state == "online";
        item.set<"connected">(connected)
            .set<"connectionState">(connected ? "connected" : "disconnected");
        EdgeStatusDto status(c);
        status.set<"state">(state);
        const auto reason = redisArrayField(reply, 1);
        if (!reason.empty())
            status.set<"reason">(reason);
        const auto clientCount = redisArrayField(reply, 2);
        if (!clientCount.empty())
            status.set<"clientCount">(toInt(clientCount));
        const auto lastActivity = redisArrayField(reply, 3);
        if (!lastActivity.empty()) {
            const auto milliseconds = toInt(lastActivity);
            if (milliseconds > 0)
                status.set<"lastActivityAt">(
                    service::common::utcTimestampFromMilliseconds(milliseconds));
        }
        item.set<"edgeStatus">(std::move(status));
    }

    struct LatestElement final {
        std::int64_t sort = 0;
        std::int64_t observedAt = 0;
        double scale = 1.0;
        std::int64_t decimals = -1;
        std::string id;
        std::string name;
        std::string value{"-"};
        std::string unit;
        std::string group;
        std::string encode;
    };

    template <typename Item>
    static void applyLatestElements(ruvia::Context& c, Item& item,
                                    const ruvia::RedisValue& reply) {
        if (reply.kind() != ruvia::RedisValue::Kind::kArray)
            return;
        bool hasElementIds = false;
        std::set<std::string, std::less<>> elementIds;
        std::map<std::string, LatestElement, std::less<>> latest;

        const auto& entries = reply.array();
        for (std::size_t index = 0; index + 1 < entries.size(); index += 2) {
            if (entries[index].kind() != ruvia::RedisValue::Kind::kString ||
                entries[index + 1].kind() != ruvia::RedisValue::Kind::kString)
                continue;
            const auto field = entries[index].string();
            if (field == "_element_ids") {
                hasElementIds = true;
                const auto parsed = ruvia::JsonValue::parse(entries[index + 1].string());
                if (parsed && parsed->isObject()) {
                    (void)ruvia::detail::visitJsonObjectFields(
                        ruvia::detail::ResolvedPmrResourceTag{}, parsed->view(),
                        std::pmr::get_default_resource(),
                        [&](std::string_view key, std::string_view) {
                            if (!key.empty())
                                elementIds.emplace(key);
                            return true;
                        });
                }
                continue;
            }
            if (field.empty() || field.front() == '_')
                continue;
            const auto parsed = ruvia::JsonValue::parse(entries[index + 1].string());
            if (!parsed || !parsed->isObject())
                continue;
            LatestElement element;
            element.id = jsonString(*parsed, "id").value_or(std::string(field));
            element.name = jsonString(*parsed, "name").value_or(element.id);
            element.value = jsonString(*parsed, "value").value_or("-");
            element.unit = jsonString(*parsed, "unit").value_or("");
            element.group = jsonString(*parsed, "group").value_or("");
            element.encode = jsonString(*parsed, "encode").value_or("");
            element.scale = jsonDouble(*parsed, "scale", 1.0);
            element.decimals = jsonInt(*parsed, "decimals", -1);
            element.sort = jsonInt(*parsed, "sort", 0);
            element.observedAt = jsonInt(*parsed, "observedAt", 0);
            const auto elementId = element.id;
            latest.insert_or_assign(elementId, std::move(element));
        }

        std::vector<LatestElement> elements;
        elements.reserve(latest.size());
        for (auto& [id, element] : latest) {
            if (hasElementIds && !elementIds.contains(id))
                continue;
            elements.push_back(std::move(element));
        }
        std::sort(elements.begin(), elements.end(), [](const auto& left, const auto& right) {
            if (left.sort != right.sort)
                return left.sort < right.sort;
            return left.id < right.id;
        });
        ruvia::BoxedArray<DeviceElementDto> dtos(
            ruvia::ModelOptions{.resource = c.resource()});
        std::int64_t reportTime = 0;
        for (const auto& element : elements) {
            auto& dto = dtos.emplace(c);
            dto.set<"id">(element.id)
                .set<"name">(element.name)
                .set<"value">(element.value)
                .set<"unit">(element.unit)
                .set<"scale">(element.scale)
                .set<"decimals">(element.decimals);
            if (!element.group.empty())
                dto.set<"group">(element.group);
            if (!element.encode.empty())
                dto.set<"encode">(element.encode);
            reportTime = std::max(reportTime, element.observedAt);
        }
        item.set<"elements">(std::move(dtos));
        if (reportTime > 0 && !item.get<"reportTime">())
            item.set<"reportTime">(service::common::utcTimestampFromMilliseconds(reportTime));
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
        appendJsonString(out, packet->get<"mode">() ? packet->get<"mode">()->view() : std::string_view("OFF"));
        if (packet->get<"content">()) {
            out += ",\"content\":";
            appendJsonString(out, packet->get<"content">()->view());
        }
        out.push_back('}');
        return out;
    }

    static std::string edgeEndpointJson(const SaveDeviceBody& body) {
        if (!body.get<"edgeNodeId">() || body.get<"edgeNodeId">()->view().empty())
            return "";
        const auto quoted = [](std::string& out, std::string_view key,
                               std::string_view value, bool& first) {
            if (!first)
                out.push_back(',');
            first = false;
            appendJsonString(out, key);
            out.push_back(':');
            appendJsonString(out, value);
        };
        const auto integer = [](std::string& out, std::string_view key, std::int64_t value,
                                bool& first) {
            if (!first)
                out.push_back(',');
            first = false;
            appendJsonString(out, key);
            out.push_back(':');
            out += std::to_string(value);
        };
        std::string out{"{"};
        bool first = true;
        quoted(out, "transport",
               body.get<"edgeTransport">() ? body.get<"edgeTransport">()->view() : std::string_view{}, first);
        quoted(out, "interface",
               body.get<"edgeInterface">() ? body.get<"edgeInterface">()->view() : std::string_view{}, first);
        if (body.get<"edgeTransport">() && body.get<"edgeTransport">()->view() == "serial") {
            integer(out, "baud_rate", body.get<"serialBaudRate">()
                                          ? static_cast<std::int64_t>(*body.get<"serialBaudRate">())
                                          : 9600,
                    first);
            integer(out, "data_bits", body.get<"serialDataBits">()
                                         ? static_cast<std::int64_t>(*body.get<"serialDataBits">())
                                         : 8,
                    first);
            integer(out, "stop_bits", body.get<"serialStopBits">()
                                         ? static_cast<std::int64_t>(*body.get<"serialStopBits">())
                                         : 1,
                    first);
            quoted(out, "parity",
                   body.get<"serialParity">() ? body.get<"serialParity">()->view() : std::string_view("none"),
                   first);
            if (!first)
                out.push_back(',');
            appendJsonString(out, "rs485");
            out += body.get<"serialRs485">() && *body.get<"serialRs485">() ? ":true" : ":false";
        } else {
            quoted(out, "mode", body.get<"edgeMode">() ? body.get<"edgeMode">()->view() : std::string_view{},
                   first);
            quoted(out, "ip", body.get<"edgeIp">() ? body.get<"edgeIp">()->view() : std::string_view{}, first);
            integer(out, "port",
                    body.get<"edgePort">() ? static_cast<std::int64_t>(*body.get<"edgePort">()) : 0, first);
        }
        out.push_back('}');
        return out;
    }

    // 心跳/注册包内容校验（对应旧 SQL shape-check 的第 8、9 条，语义一致）
    static void validatePacket(const std::optional<DevicePacketBody>& packet) {
        if (!packet)
            return;
        const std::string_view mode =
            packet->get<"mode">() ? packet->get<"mode">()->view() : std::string_view("OFF");
        if (mode != "OFF" && mode != "HEX" && mode != "ASCII")
            service::common::fail(18002, "设备参数无效", 400);
        if (mode == "OFF")
            return;
        const std::string_view content =
            packet->get<"content">() ? packet->get<"content">()->view() : std::string_view{};
        if (content.empty())
            service::common::fail(18002, "设备参数无效", 400);
        if (mode == "ASCII" && content.size() > 256)
            service::common::fail(18002, "注册包或心跳包不能超过 256 字节", 400);
        if (mode == "HEX") {
            std::string stripped;
            for (const char ch : content)
                if (!std::isspace(static_cast<unsigned char>(ch)))
                    stripped.push_back(ch);
            if (stripped.empty() || stripped.size() % 2 != 0)
                service::common::fail(18002, "设备参数无效", 400);
            if (stripped.size() / 2 > 256)
                service::common::fail(18002, "注册包或心跳包不能超过 256 字节", 400);
            for (const char ch : stripped)
                if (!std::isxdigit(static_cast<unsigned char>(ch)))
                    service::common::fail(18002, "设备参数无效", 400);
        }
    }

    // 扁平字段（必填/长度/枚举/范围/UUID/timezone）由声明式校验器保证；
    // 此处只做跨字段、依赖 DB 与协议相关的校验（保留 18002/18003 域码）。
    ruvia::Task<void> validate(ruvia::Context& c, const SaveDeviceBody& body, bool required) {
        validatePacket(body.get<"heartbeat">());
        validatePacket(body.get<"registration">());
        const auto linkId = str(body.get<"linkId">());
        const auto edgeNodeId = str(body.get<"edgeNodeId">());
        const auto configId = str(body.get<"protocolConfigId">());
        if (!linkId.empty() && !edgeNodeId.empty())
            service::common::fail(18003, "本地链路和边缘节点只能选择一个", 400);
        if (required && linkId.empty() && edgeNodeId.empty())
            service::common::fail(18003, "请选择本地链路或边缘节点", 400);
        if (required && configId.empty())
            service::common::fail(18003, "请选择设备类型", 400);

        const auto& code = body.get<"deviceCode">();
        if (code) {
            if (code->view().empty() || code->view().size() > 100)
                service::common::fail(18002, "设备编码长度必须在 1 - 100 之间", 400);
            for (const auto character : code->view())
                if (!std::isalnum(static_cast<unsigned char>(character)))
                    service::common::fail(18002, "设备编码只能包含字母和数字", 400);
        }
        if (body.get<"groupId">() && !body.get<"groupId">()->view().empty()) {
            const auto group = co_await c.db().query(
                "SELECT 1 FROM device_group WHERE id = $1 AND deleted_at IS NULL",
                service::common::dbParams(body.get<"groupId">()->view()));
            if (group.empty())
                service::common::fail(18003, "设备分组不存在", 400);
        }
        if (configId.empty() || (linkId.empty() && edgeNodeId.empty()))
            co_return;

        std::string configProtocol;
        if (!edgeNodeId.empty()) {
            const auto relation = co_await c.db().query(R"sql(
SELECT p.protocol
FROM edge_node n CROSS JOIN protocol_config p
WHERE n.id = $1::uuid AND n.enrollment_status = 'approved'
  AND CASE lower(COALESCE(n.capability->>'deviceConfig', ''))
        WHEN 'true' THEN TRUE WHEN 't' THEN TRUE WHEN '1' THEN TRUE
        WHEN 'yes' THEN TRUE WHEN 'y' THEN TRUE WHEN 'on' THEN TRUE
        ELSE FALSE END
  AND p.id = $2::uuid AND p.deleted_at IS NULL LIMIT 1)sql",
                                                        service::common::dbParams(edgeNodeId,
                                                                                  configId));
            if (relation.empty())
                service::common::fail(18003, "边缘节点未批准或设备类型不存在", 400);
            configProtocol = std::string(relation.front()[0].value().value_or(std::string_view{}));
            if (configProtocol != "Modbus" && configProtocol != "S7" &&
                configProtocol != "SL651")
                service::common::fail(18003, "边缘采集协议不受支持", 400);
            co_await validateEdgeEndpoint(c, body, edgeNodeId, configProtocol);
        } else {
            const auto relation = co_await c.db().query(R"sql(
SELECT l.protocol, l.endpoint->>'mode', p.protocol
FROM link l CROSS JOIN protocol_config p
WHERE l.id = $1 AND l.deleted_at IS NULL AND l.execution = 'collector'
  AND p.id = $2 AND p.deleted_at IS NULL LIMIT 1)sql",
                                                      service::common::dbParams(linkId, configId));
            if (relation.empty())
                service::common::fail(18003, "链路或设备类型不存在", 400);
            const std::string linkProtocol(relation.front()[0].value().value_or(std::string_view{}));
            configProtocol = std::string(relation.front()[2].value().value_or(std::string_view{}));
            if (linkProtocol != configProtocol)
                service::common::fail(18003, "链路协议与设备类型不一致", 409);
        }
        if (configProtocol == "SL651" &&
            (packetEnabled(body.get<"heartbeat">()) || packetEnabled(body.get<"registration">())))
            service::common::fail(18002, "SL651 设备不支持配置注册包或心跳包", 400);
        if (configProtocol == "SL651" && code) {
            if (code->view().size() > 10)
                service::common::fail(18002, "SL651 遥测站地址最多 10 位数字", 400);
            for (const auto character : code->view())
                if (!std::isdigit(static_cast<unsigned char>(character)))
                    service::common::fail(18002, "SL651 设备编码必须是数字遥测站地址", 400);
        }
    }

    static bool packetEnabled(const std::optional<DevicePacketBody>& packet) {
        return packet && packet->get<"mode">() && packet->get<"mode">()->view() != "OFF";
    }

    static bool ipv4(std::string_view input) {
        for (int part = 0; part < 4; ++part) {
            const auto dot = input.find('.');
            const auto token = input.substr(0, dot);
            unsigned value{};
            const auto [end, error] =
                std::from_chars(token.data(), token.data() + token.size(), value);
            if (token.empty() || error != std::errc{} || end != token.data() + token.size() ||
                value > 255 || (token.size() > 1 && token.front() == '0'))
                return false;
            if (part == 3)
                return dot == std::string_view::npos;
            if (dot == std::string_view::npos)
                return false;
            input.remove_prefix(dot + 1);
        }
        return false;
    }

    static ruvia::Task<void> validateEdgeEndpoint(ruvia::Context& c,
                                                  const SaveDeviceBody& body,
                                                  std::string_view nodeId,
                                                  std::string_view protocol) {
        if (packetEnabled(body.get<"registration">()))
            service::common::fail(18002, "边缘采集设备不支持注册码", 400);
        const auto transport = str(body.get<"edgeTransport">());
        const auto interfaceName = str(body.get<"edgeInterface">());
        if (transport != "serial" && transport != "tcp")
            service::common::fail(18003, "请选择边缘节点的串口或网口", 400);
        if (interfaceName.empty())
            service::common::fail(18003, "请选择边缘节点已上报的接口", 400);
        if (protocol == "SL651" && transport != "tcp")
            service::common::fail(18003, "SL651 仅支持边缘 TCP Server 端点", 400);
        if (transport == "serial") {
            if (protocol == "S7")
                service::common::fail(18003, "S7 仅支持边缘节点 TCP Client 端点", 400);
            const auto serial = co_await c.db().query(R"sql(
SELECT available FROM edge_node_serial
WHERE node_id = $1::uuid AND path = $2 LIMIT 1)sql",
                                                      service::common::dbParams(nodeId,
                                                                                interfaceName));
            if (serial.empty() || serial.front()[0].value().value_or(std::string_view{}) != "t")
                service::common::fail(18003, "所选串口不存在或当前不可用", 409);
            if (packetEnabled(body.get<"heartbeat">()))
                service::common::fail(18002, "串口设备不支持心跳包", 400);
            co_return;
        }

        const auto network = co_await c.db().query(R"sql(
SELECT COALESCE(ipv4, ''), is_up FROM edge_node_interface
WHERE node_id = $1::uuid AND name = $2 AND COALESCE(ipv4, '') <> ''
LIMIT 1)sql",
                                                   service::common::dbParams(nodeId,
                                                                             interfaceName));
        if (network.empty())
            service::common::fail(18003, "所选网口不存在、未上报 IPv4 或属于受保护上联", 409);
        const auto interfaceIp = network.front()[0].value().value_or(std::string_view{});
        const auto mode = str(body.get<"edgeMode">());
        const auto ip = str(body.get<"edgeIp">());
        if ((mode != "TCP Client" && mode != "TCP Server") || !body.get<"edgePort">() || !ipv4(ip))
            service::common::fail(18003, "边缘 TCP 模式、IPv4 或端口无效", 400);
        if (protocol == "S7" && mode != "TCP Client")
            service::common::fail(18003, "S7 仅支持边缘节点主动连接 PLC", 400);
        if (protocol == "SL651" && mode != "TCP Server")
            service::common::fail(18003, "SL651 仅支持边缘 TCP Server 端点", 400);
        if (mode == "TCP Server") {
            if (ip != "0.0.0.0" && ip != interfaceIp)
                service::common::fail(18003, "TCP Server 监听地址必须是所选网口地址", 400);
        } else if (packetEnabled(body.get<"heartbeat">())) {
            service::common::fail(18002, "仅 TCP Server 设备支持心跳包", 400);
        }
    }

    ruvia::Task<void> validateEdgeRuntimeIdentity(ruvia::Context& c,
                                                   const SaveDeviceBody& body,
                                                   std::optional<std::string> excludedId) {
        if (!body.get<"edgeNodeId">() || body.get<"edgeNodeId">()->view().empty())
            co_return;
        const auto transport = str(body.get<"edgeTransport">());
        const auto interfaceName = str(body.get<"edgeInterface">());
        const auto mode = str(body.get<"edgeMode">());
        const auto ip = str(body.get<"edgeIp">());
        const auto port = body.get<"edgePort">() ? static_cast<std::int64_t>(*body.get<"edgePort">()) : 0;
        const auto slaveId = body.get<"slaveId">() ? static_cast<std::int64_t>(*body.get<"slaveId">()) : 1;
        const auto excluded = excludedId.value_or(std::string(kNilUuid));
        std::string protocol;
        if (body.get<"protocolConfigId">() && !body.get<"protocolConfigId">()->view().empty()) {
            const auto protocolRows = co_await c.db().query(
                "SELECT protocol FROM protocol_config WHERE id = $1::uuid AND deleted_at IS NULL",
                service::common::dbParams(body.get<"protocolConfigId">()->view()));
            if (protocolRows.empty())
                co_return;
            protocol = std::string(protocolRows.front()[0].value().value_or(std::string_view{}));
        } else if (excludedId) {
            const auto protocolRows = co_await c.db().query(R"sql(
SELECT p.protocol
FROM device d JOIN protocol_config p ON p.id = d.protocol_config_id AND p.deleted_at IS NULL
WHERE d.id = $1::uuid AND d.deleted_at IS NULL LIMIT 1)sql",
                                                            service::common::dbParams(excluded));
            if (protocolRows.empty())
                co_return;
            protocol = std::string(protocolRows.front()[0].value().value_or(std::string_view{}));
        } else {
            co_return;
        }

        if (transport == "serial") {
            const auto rows = co_await c.db().query(R"sql(
SELECT d.name
FROM device d
JOIN link l ON l.id = d.link_id AND l.execution = 'edge'
WHERE l.edge_node_id = $1::uuid AND d.id <> $2::uuid AND d.deleted_at IS NULL
  AND l.endpoint->>'transport' = 'serial'
  AND l.endpoint->>'interface' = $3
ORDER BY d.id LIMIT 1)sql",
                                                    service::common::dbParams(
                                                        body.get<"edgeNodeId">()->view(), excluded,
                                                        interfaceName));
            if (!rows.empty())
                service::common::fail(
                    18006,
                    "边缘串口已被设备占用，冲突设备: " + std::string(rows.front()[0].value().value_or(std::string_view{})),
                    409);
            co_return;
        }

        if (transport != "tcp")
            co_return;

        if (mode == "TCP Server") {
            const auto rows = co_await c.db().query(R"sql(
SELECT d.name
FROM device d
JOIN link l ON l.id = d.link_id AND l.execution = 'edge'
WHERE l.edge_node_id = $1::uuid AND d.id <> $2::uuid AND d.deleted_at IS NULL
  AND l.endpoint->>'transport' = 'tcp'
  AND l.endpoint->>'mode' = 'TCP Server'
  AND COALESCE(
        CASE WHEN COALESCE(l.endpoint->>'port', '') ~ '^[0-9]{1,5}$'
             THEN NULLIF(l.endpoint->>'port', '')::integer END, 0) = $3
  AND (
    l.endpoint->>'ip' = $4
    OR l.endpoint->>'ip' = '0.0.0.0'
    OR $4 = '0.0.0.0'
  )
ORDER BY d.id LIMIT 1)sql",
                                                    service::common::dbParams(
                                                        body.get<"edgeNodeId">()->view(), excluded, port,
                                                        ip));
            if (!rows.empty())
                service::common::fail(
                    18006,
                    "边缘 TCP Server 监听地址端口冲突，冲突设备: " +
                        std::string(rows.front()[0].value().value_or(std::string_view{})),
                    409);
            co_return;
        }

        if (mode != "TCP Client" || (protocol != "Modbus" && protocol != "S7"))
            co_return;

        const auto rows = co_await c.db().query(R"sql(
SELECT d.name,
       COALESCE(
         CASE WHEN COALESCE(d.protocol_params->>'slave_id', '') ~ '^-?[0-9]{1,18}$'
              THEN NULLIF(d.protocol_params->>'slave_id', '')::integer END, 1)
FROM device d
JOIN protocol_config p ON p.id = d.protocol_config_id AND p.deleted_at IS NULL
JOIN link l ON l.id = d.link_id AND l.execution = 'edge'
WHERE l.edge_node_id = $1::uuid AND d.id <> $2::uuid AND d.deleted_at IS NULL
  AND p.protocol = $3
  AND l.endpoint->>'transport' = 'tcp'
  AND l.endpoint->>'mode' = 'TCP Client'
  AND l.endpoint->>'ip' = $4
  AND COALESCE(
        CASE WHEN COALESCE(l.endpoint->>'port', '') ~ '^[0-9]{1,5}$'
             THEN NULLIF(l.endpoint->>'port', '')::integer END, 0) = $5
ORDER BY d.id)sql",
                                                service::common::dbParams(
                                                    body.get<"edgeNodeId">()->view(), excluded, protocol,
                                                    ip, port));
        for (const auto& row : rows) {
            const std::string name(row[0].value().value_or(std::string_view{}));
            if (protocol == "S7")
                service::common::fail(18006,
                                      "边缘 S7 TCP Client 同一目标只能关联一个设备，冲突设备: " +
                                          name,
                                      409);
            if (toInt(row[1].value().value_or(std::string_view{})) == slaveId)
                service::common::fail(
                    18006,
                    "边缘 Modbus TCP Client 同一目标下 Slave ID 重复，冲突设备: " + name,
                    409);
        }
    }

    ruvia::Task<void> validateRuntimeIdentity(ruvia::Context& c, const SaveDeviceBody& body,
                                              std::optional<std::string> excludedId) {
        if (body.get<"edgeNodeId">() && !body.get<"edgeNodeId">()->view().empty()) {
            co_await validateEdgeRuntimeIdentity(c, body, excludedId);
            co_return;
        }
        const std::string excluded = excludedId.value_or(std::string(kNilUuid));
        const std::string inLinkId = str(body.get<"linkId">());
        const std::string inTargetId = str(body.get<"targetId">());
        const std::string inConfigId = str(body.get<"protocolConfigId">());
        const std::string inSlaveId =
            body.get<"slaveId">() ? std::to_string(static_cast<std::int64_t>(*body.get<"slaveId">())) : "";
        const std::string inRegistration = packetJson(body.get<"registration">());
        const std::string inHeartbeat = packetJson(body.get<"heartbeat">());
        const auto candidate = co_await c.db().query(
            R"sql(
WITH current_device AS (
  SELECT link_id, protocol_config_id, protocol_params
  FROM device WHERE id = $2 AND deleted_at IS NULL
),
candidate AS (
  SELECT
    COALESCE(NULLIF($1, '')::uuid, current_device.link_id) AS link_id,
    COALESCE(NULLIF($3, ''), current_device.protocol_params->>'target_id', '') AS target_id,
    COALESCE(NULLIF($4, '')::uuid, current_device.protocol_config_id) AS protocol_config_id,
    COALESCE(
      CASE WHEN COALESCE(NULLIF($5, ''), '') ~ '^-?[0-9]{1,18}$'
           THEN NULLIF($5, '')::integer END,
      CASE WHEN COALESCE(current_device.protocol_params->>'slave_id', '') ~ '^-?[0-9]{1,18}$'
           THEN (current_device.protocol_params->>'slave_id')::integer END,
      1) AS slave_id,
    COALESCE(NULLIF($6, '')::jsonb, current_device.protocol_params->'registration',
             '{"mode":"OFF"}'::jsonb) AS registration,
    COALESCE(NULLIF($7, '')::jsonb, current_device.protocol_params->'heartbeat',
             '{"mode":"OFF"}'::jsonb) AS heartbeat
  FROM (SELECT 1) b LEFT JOIN current_device ON TRUE
)
SELECT candidate.link_id, link.endpoint->>'mode', protocol.protocol, candidate.target_id,
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
JOIN link link ON link.id = candidate.link_id
  AND link.deleted_at IS NULL AND link.execution = 'collector'
JOIN protocol_config protocol
  ON protocol.id = candidate.protocol_config_id AND protocol.deleted_at IS NULL
LIMIT 1)sql",
            service::common::dbParams(inLinkId, excluded, inTargetId, inConfigId, inSlaveId,
                                      inRegistration, inHeartbeat));
        if (candidate.empty())
            co_return;

        const auto& current = candidate.front();
        const std::string linkId(current[0].value().value_or(std::string_view{}));
        const std::string linkMode(current[1].value().value_or(std::string_view{}));
        const std::string protocol(current[2].value().value_or(std::string_view{}));
        const std::string targetId(current[3].value().value_or(std::string_view{}));
        const auto slaveId = toInt(current[4].value().value_or(std::string_view{}));
        const std::string registrationMode(current[5].value().value_or(std::string_view{}));
        const std::string registrationKey(current[6].value().value_or(std::string_view{}));
        const std::string heartbeatMode(current[7].value().value_or(std::string_view{}));
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
SELECT device.name,
       COALESCE(
         CASE WHEN COALESCE(device.protocol_params->>'slave_id', '') ~ '^-?[0-9]{1,18}$'
              THEN (device.protocol_params->>'slave_id')::integer END, 1),
       COALESCE(device.protocol_params->>'target_id', ''),
       upper(COALESCE(device.protocol_params->'registration'->>'mode', 'OFF')),
       CASE upper(COALESCE(device.protocol_params->'registration'->>'mode', 'OFF'))
         WHEN 'OFF' THEN 'OFF:'
         WHEN 'HEX' THEN 'HEX:' || upper(regexp_replace(
           COALESCE(device.protocol_params->'registration'->>'content', ''), '\\s', '', 'g'))
         ELSE 'ASCII:' || COALESCE(device.protocol_params->'registration'->>'content', '')
       END
FROM device device
JOIN protocol_config config
  ON config.id = device.protocol_config_id AND config.deleted_at IS NULL
WHERE device.link_id = $1 AND device.id <> $2 AND device.deleted_at IS NULL
  AND config.protocol = $3
ORDER BY device.id)sql",
                                  service::common::dbParams(linkId, excluded, protocol));

        if (linkMode == "TCP Client") {
            if (targetId.empty())
                co_return;
            for (const auto& sibling : siblings) {
                if (sibling[2].value().value_or(std::string_view{}) != targetId)
                    continue;
                const std::string name(sibling[0].value().value_or(std::string_view{}));
                if (protocol == "S7")
                    service::common::fail(
                        18006, "S7 同一目标地址只能关联一个设备，冲突设备: " + name, 409);
                if (toInt(sibling[1].value().value_or(std::string_view{})) == slaveId)
                    service::common::fail(
                        18006, "Modbus 同一目标地址下 Slave ID 重复，冲突设备: " + name, 409);
            }
            co_return;
        }
        if (linkMode != "TCP Server")
            co_return;

        if (protocol == "Modbus" && !siblings.empty()) {
            if (registrationMode == "OFF")
                service::common::fail(18006, "Modbus TCP Server 链路存在多个设备时必须配置注册包",
                                      409);
            for (const auto& sibling : siblings) {
                const std::string name(sibling[0].value().value_or(std::string_view{}));
                if (sibling[3].value().value_or(std::string_view{}) == "OFF")
                    service::common::fail(
                        18006, "Modbus TCP Server 链路存在未配置注册包的设备: " + name, 409);
                if (sibling[4].value().value_or(std::string_view{}) == registrationKey && toInt(sibling[1].value().value_or(std::string_view{})) == slaveId)
                    service::common::fail(
                        18006, "Modbus 同一链路和注册码下 Slave ID 重复，冲突设备: " + name, 409);
            }
            co_return;
        }

        if (protocol == "S7") {
            for (const auto& sibling : siblings)
                if (sibling[4].value().value_or(std::string_view{}) == registrationKey)
                    service::common::fail(18006,
                                          "S7 TCP Server 同一链路下注册码重复，冲突设备: " +
                                              std::string(sibling[0].value().value_or(std::string_view{})),
                                          409);
        }
    }

    ruvia::Task<void> ensureUnique(ruvia::Context& c, const SaveDeviceBody& body,
                                   std::optional<std::string> excludedId) {
        const auto& name = body.get<"name">();
        const auto& code = body.get<"deviceCode">();
        if (!name && !code)
            co_return;
        const std::string nameValue = str(name);
        const std::string codeValue = str(code);
        const std::string excluded = excludedId.value_or(std::string(kNilUuid));
        const auto rows = co_await c.db().query(
            R"sql(
SELECT 1 FROM device
WHERE deleted_at IS NULL AND id <> $1::uuid
  AND (($2 <> '' AND name = $2)
       OR ($3 <> '' AND protocol_params->>'device_code' = $3)) LIMIT 1)sql",
            service::common::dbParams(excluded, std::string_view(nameValue),
                                      std::string_view(codeValue)));
        if (!rows.empty())
            service::common::fail(18004, "设备名称或编码已存在", 409);
    }

    // ----- 设备分组私有工具 -----

    template <typename Row>
    static void fillGroup(DeviceGroupItemDto& item, const Row& row, const DeviceActor& actor) {
        item.set<"id">(row[0].value().value_or(std::string_view{}));
        item.set<"name">(row[1].value().value_or(std::string_view{}));
        item.set<"parentId">(row[2].value().value_or(std::string_view{}));
        item.set<"status">(row[3].value().value_or(std::string_view{}));
        item.set<"sortOrder">(toInt(row[4].value().value_or(std::string_view{})));
        item.set<"remark">(row[5].value().value_or(std::string_view{}));
        item.set<"deviceCount">(toInt(row[6].value().value_or(std::string_view{})));
        item.set<"createdAt">(row[7].value().value_or(std::string_view{}));
        item.set<"updatedAt">(row[8].value().value_or(std::string_view{}));
        item.set<"canShare">(
            actor.canGroupShare &&
            (actor.superadmin ||
             row[9].value().value_or(std::string_view{}) == actor.userId));
    }

    ruvia::Task<void> validateParent(ruvia::Context& c, const SaveDeviceGroupBody& body,
                                     std::optional<std::string> currentId) {
        const auto& parent = body.get<"parentId">();
        if (!parent || parent->view().empty())
            co_return;
        if (!service::common::isUuid(parent->view()))
            service::common::fail(17002, "上级分组必须是 UUID", 400);
        if (currentId && parent->view() == *currentId)
            service::common::fail(17003, "上级分组不能是自身", 409);
        const auto exists =
            co_await c.db().query("SELECT 1 FROM device_group WHERE id = $1 AND deleted_at IS NULL",
                                  service::common::dbParams(parent->view()));
        if (exists.empty())
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
        if (rows.front()[0].value().value_or(std::string_view{}) != "t")
            service::common::fail(17005, "只能管理自己创建的设备分组", 403);
    }

    static constexpr std::string_view kNilUuid = "00000000-0000-0000-0000-000000000000";
};

class DeviceShareService {
  public:
    static DeviceShareService& instance() {
        static DeviceShareService service;
        return service;
    }

    ruvia::Task<ruvia::BoxedArray<DeviceShareItemDto>> list(ruvia::Context& c,
                                                       std::string_view deviceId) {
        (void)co_await deviceAccessService().require(c, deviceId, DeviceAccessLevel::owner);
        const auto rows = co_await c.db().query(R"sql(
WITH RECURSIVE current_device(group_id) AS (
    SELECT device.group_id FROM device WHERE device.id = $1
), ancestor_group(id, parent_id, name) AS (
    SELECT device_group.id, device_group.parent_id, device_group.name
      FROM device_group JOIN current_device ON current_device.group_id = device_group.id
     WHERE device_group.deleted_at IS NULL
    UNION ALL
    SELECT parent.id, parent.parent_id, parent.name
      FROM device_group parent
      JOIN ancestor_group child ON child.parent_id = parent.id
     WHERE parent.deleted_at IS NULL
)
SELECT access_grant.id::text,
       CASE WHEN access_grant.user_id IS NOT NULL THEN 'user' ELSE 'department' END,
       COALESCE(access_grant.user_id::text, access_grant.department_id::text),
       CASE WHEN access_grant.user_id IS NOT NULL
            THEN COALESCE(NULLIF(target_user.nickname, ''), target_user.username, '已删除用户')
            ELSE COALESCE(target_department.name, '已删除部门') END,
       access_grant.access_level, 'device', '', '', FALSE,
       iot_utc_timestamp(access_grant.created_at),
       iot_utc_timestamp(access_grant.updated_at)
FROM device_access_grant access_grant
LEFT JOIN sys_user target_user ON target_user.id = access_grant.user_id
LEFT JOIN sys_department target_department
       ON target_department.id = access_grant.department_id
WHERE access_grant.device_id = $1
UNION ALL
SELECT group_access.id::text,
       CASE WHEN group_access.user_id IS NOT NULL THEN 'user' ELSE 'department' END,
       COALESCE(group_access.user_id::text, group_access.department_id::text),
       CASE WHEN group_access.user_id IS NOT NULL
            THEN COALESCE(NULLIF(inherited_user.nickname, ''), inherited_user.username,
                          '已删除用户')
            ELSE COALESCE(inherited_department.name, '已删除部门') END,
       group_access.access_level, 'group', ancestor.id::text, ancestor.name, TRUE,
       iot_utc_timestamp(group_access.created_at),
       iot_utc_timestamp(group_access.updated_at)
FROM device_group_access_grant group_access
JOIN ancestor_group ancestor ON ancestor.id = group_access.group_id
LEFT JOIN sys_user inherited_user ON inherited_user.id = group_access.user_id
LEFT JOIN sys_department inherited_department
       ON inherited_department.id = group_access.department_id
ORDER BY 2, 4, 9, 1)sql",
                                                service::common::dbParams(deviceId));
        ruvia::BoxedArray<DeviceShareItemDto> result(
            ruvia::ModelOptions{.resource = c.resource()});
        for (const auto& row : rows) {
            auto& item = result.emplace(c);
            item.set<"id">(row[0].value().value_or(std::string_view{}))
                .set<"subjectType">(row[1].value().value_or(std::string_view{}))
                .set<"subjectId">(row[2].value().value_or(std::string_view{}))
                .set<"subjectName">(row[3].value().value_or(std::string_view{}))
                .set<"accessLevel">(row[4].value().value_or(std::string_view{}))
                .set<"sourceType">(row[5].value().value_or(std::string_view{}))
                .set<"sourceGroupId">(row[6].value().value_or(std::string_view{}))
                .set<"sourceGroupName">(row[7].value().value_or(std::string_view{}))
                .set<"inherited">(row[8].value().value_or(std::string_view{}) == "t")
                .set<"createdAt">(row[9].value().value_or(std::string_view{}))
                .set<"updatedAt">(row[10].value().value_or(std::string_view{}));
        }
        co_return result;
    }

    ruvia::Task<ruvia::BoxedArray<DeviceShareTargetDto>> targets(ruvia::Context& c,
                                                            std::string_view deviceId) {
        (void)co_await deviceAccessService().require(c, deviceId, DeviceAccessLevel::owner);
        const auto rows = co_await c.db().query(R"sql(
SELECT 'user', target.id::text,
       COALESCE(NULLIF(target.nickname, ''), target.username)
FROM sys_user target
WHERE target.status = 'enabled' AND target.deleted_at IS NULL
  AND target.id <> (SELECT created_by FROM device WHERE id = $1)
UNION ALL
SELECT 'department', department.id::text, department.name
FROM sys_department department
WHERE department.status = 'enabled' AND department.deleted_at IS NULL
ORDER BY 1, 3, 2)sql",
                                                service::common::dbParams(deviceId));
        ruvia::BoxedArray<DeviceShareTargetDto> result(
            ruvia::ModelOptions{.resource = c.resource()});
        for (const auto& row : rows) {
            auto& item = result.emplace(c);
            item.set<"subjectType">(row[0].value().value_or(std::string_view{})).set<"subjectId">(row[1].value().value_or(std::string_view{})).set<"subjectName">(row[2].value().value_or(std::string_view{}));
        }
        co_return result;
    }

    ruvia::Task<void> replace(ruvia::Context& c, std::string_view deviceId,
                              const ReplaceDeviceSharesBody& body) {
        auto decision =
            co_await deviceAccessService().require(c, deviceId, DeviceAccessLevel::owner);
        auto shares = normalize(body);

        auto transaction = co_await c.db().beginTransaction();
        const auto deviceRows = co_await transaction.query(
            "SELECT created_by::text FROM device WHERE id = $1 AND deleted_at IS NULL FOR UPDATE",
            service::common::dbParams(deviceId));
        if (deviceRows.empty())
            service::common::fail(18001, "设备不存在", 404);
        const std::string ownerId(deviceRows.front()[0].value().value_or(std::string_view{}));

        co_await validateTargets(transaction, shares, ownerId);

        (void)co_await transaction.execute("DELETE FROM device_access_grant WHERE device_id = $1",
                                           service::common::dbParams(deviceId));
        for (const auto& share : shares) {
            const std::string userId = share.subjectType == "user" ? share.subjectId : "";
            const std::string departmentId =
                share.subjectType == "department" ? share.subjectId : "";
            const auto grantId = service::common::nextUuidV7();
            (void)co_await transaction.execute(R"sql(
INSERT INTO device_access_grant(
    id, device_id, user_id, department_id, access_level, granted_by)
VALUES ($1, $2, NULLIF($3, '')::uuid, NULLIF($4, '')::uuid, $5, $6))sql",
                                               service::common::dbParams(
                                                   grantId, deviceId, userId, departmentId,
                                                   share.accessLevel, decision.actor.userId));
        }
        const auto auditId = service::common::nextUuidV7();
        const auto shareCount = static_cast<std::int64_t>(shares.size());
        (void)co_await transaction.execute(R"sql(
INSERT INTO security_audit_log(
    id, actor_user_id, action, resource_type, resource_id, outcome, details)
VALUES ($1, $2, 'device.share.replace', 'device', $3, 'success',
        jsonb_build_object('share_count', $4::integer)))sql",
                                           service::common::dbParams(
                                               auditId, decision.actor.userId, deviceId,
                                               shareCount));
        co_await transaction.commit();
    }

    ruvia::Task<ruvia::BoxedArray<DeviceShareItemDto>> listGroup(ruvia::Context& c,
                                                            std::string_view groupId) {
        (void)co_await deviceAccessService().requireGroupOwner(c, groupId);
        const auto rows = co_await c.db().query(R"sql(
SELECT access_grant.id::text,
       CASE WHEN access_grant.user_id IS NOT NULL THEN 'user' ELSE 'department' END,
       COALESCE(access_grant.user_id::text, access_grant.department_id::text),
       CASE WHEN access_grant.user_id IS NOT NULL
            THEN COALESCE(NULLIF(target_user.nickname, ''), target_user.username, '已删除用户')
            ELSE COALESCE(target_department.name, '已删除部门') END,
       access_grant.access_level, 'group', target_group.id::text, target_group.name, FALSE,
       iot_utc_timestamp(access_grant.created_at),
       iot_utc_timestamp(access_grant.updated_at)
FROM device_group_access_grant access_grant
JOIN device_group target_group ON target_group.id = access_grant.group_id
LEFT JOIN sys_user target_user ON target_user.id = access_grant.user_id
LEFT JOIN sys_department target_department
       ON target_department.id = access_grant.department_id
WHERE access_grant.group_id = $1
ORDER BY 2, 4, access_grant.id)sql",
                                                service::common::dbParams(groupId));
        ruvia::BoxedArray<DeviceShareItemDto> result(
            ruvia::ModelOptions{.resource = c.resource()});
        for (const auto& row : rows) {
            auto& item = result.emplace(c);
            item.set<"id">(row[0].value().value_or(std::string_view{}))
                .set<"subjectType">(row[1].value().value_or(std::string_view{}))
                .set<"subjectId">(row[2].value().value_or(std::string_view{}))
                .set<"subjectName">(row[3].value().value_or(std::string_view{}))
                .set<"accessLevel">(row[4].value().value_or(std::string_view{}))
                .set<"sourceType">(row[5].value().value_or(std::string_view{}))
                .set<"sourceGroupId">(row[6].value().value_or(std::string_view{}))
                .set<"sourceGroupName">(row[7].value().value_or(std::string_view{}))
                .set<"inherited">(false)
                .set<"createdAt">(row[9].value().value_or(std::string_view{}))
                .set<"updatedAt">(row[10].value().value_or(std::string_view{}));
        }
        co_return result;
    }

    ruvia::Task<ruvia::BoxedArray<DeviceShareTargetDto>> groupTargets(ruvia::Context& c,
                                                                 std::string_view groupId) {
        (void)co_await deviceAccessService().requireGroupOwner(c, groupId);
        const auto rows = co_await c.db().query(R"sql(
SELECT 'user', target.id::text,
       COALESCE(NULLIF(target.nickname, ''), target.username)
FROM sys_user target
WHERE target.status = 'enabled' AND target.deleted_at IS NULL
  AND target.id <> (SELECT created_by FROM device_group WHERE id = $1)
UNION ALL
SELECT 'department', department.id::text, department.name
FROM sys_department department
WHERE department.status = 'enabled' AND department.deleted_at IS NULL
ORDER BY 1, 3, 2)sql",
                                                service::common::dbParams(groupId));
        ruvia::BoxedArray<DeviceShareTargetDto> result(
            ruvia::ModelOptions{.resource = c.resource()});
        for (const auto& row : rows) {
            auto& item = result.emplace(c);
            item.set<"subjectType">(row[0].value().value_or(std::string_view{})).set<"subjectId">(row[1].value().value_or(std::string_view{})).set<"subjectName">(row[2].value().value_or(std::string_view{}));
        }
        co_return result;
    }

    ruvia::Task<void> replaceGroup(ruvia::Context& c, std::string_view groupId,
                                   const ReplaceDeviceSharesBody& body) {
        auto actor = co_await deviceAccessService().requireGroupOwner(c, groupId);
        auto shares = normalize(body);
        auto transaction = co_await c.db().beginTransaction();
        const auto groupRows = co_await transaction.query(
            "SELECT created_by::text FROM device_group WHERE id = $1 AND deleted_at IS NULL "
            "FOR UPDATE",
            service::common::dbParams(groupId));
        if (groupRows.empty())
            service::common::fail(17001, "设备分组不存在", 404);
        co_await validateTargets(transaction, shares, groupRows.front()[0].value().value_or(std::string_view{}));

        (void)co_await transaction.execute(
            "DELETE FROM device_group_access_grant WHERE group_id = $1",
            service::common::dbParams(groupId));
        for (const auto& share : shares) {
            const std::string userId = share.subjectType == "user" ? share.subjectId : "";
            const std::string departmentId =
                share.subjectType == "department" ? share.subjectId : "";
            const auto grantId = service::common::nextUuidV7();
            (void)co_await transaction.execute(R"sql(
INSERT INTO device_group_access_grant(
    id, group_id, user_id, department_id, access_level, granted_by)
VALUES ($1, $2, NULLIF($3, '')::uuid, NULLIF($4, '')::uuid, $5, $6))sql",
                                               service::common::dbParams(
                                                   grantId, groupId, userId, departmentId,
                                                   share.accessLevel, actor.userId));
        }
        const auto auditId = service::common::nextUuidV7();
        const auto shareCount = static_cast<std::int64_t>(shares.size());
        (void)co_await transaction.execute(R"sql(
INSERT INTO security_audit_log(
    id, actor_user_id, action, resource_type, resource_id, outcome, details)
VALUES ($1, $2, 'device_group.share.replace', 'device_group', $3, 'success',
        jsonb_build_object('share_count', $4::integer)))sql",
                                           service::common::dbParams(auditId, actor.userId, groupId,
                                                                     shareCount));
        co_await transaction.commit();
    }

  private:
    struct NormalizedShare final {
        std::string subjectType;
        std::string subjectId;
        std::string accessLevel;
    };

    static std::vector<NormalizedShare> normalize(const ReplaceDeviceSharesBody& body) {
        if (!body.get<"shares">())
            service::common::fail(18010, "分享列表不能为空", 400);
        std::vector<NormalizedShare> shares;
        shares.reserve(body.get<"shares">()->size());
        std::set<std::string, std::less<>> uniqueSubjects;
        for (const auto& item : *body.get<"shares">()) {
            if (!item.get<"subjectType">() || !item.get<"subjectId">() || !item.get<"accessLevel">())
                service::common::fail(18010, "分享对象参数不完整", 400);
            NormalizedShare share{std::string(item.get<"subjectType">()->view()),
                                  std::string(item.get<"subjectId">()->view()),
                                  std::string(item.get<"accessLevel">()->view())};
            if (share.subjectType != "user" && share.subjectType != "department")
                service::common::fail(18010, "分享对象类型无效", 400);
            if (!service::common::isUuid(share.subjectId))
                service::common::fail(18010, "分享对象 ID 必须是 UUID", 400);
            if (share.accessLevel != "view" && share.accessLevel != "operate")
                service::common::fail(18010, "设备访问级别无效", 400);
            if (!uniqueSubjects.emplace(share.subjectType + ":" + share.subjectId).second)
                service::common::fail(18010, "分享对象不能重复", 400);
            shares.emplace_back(std::move(share));
        }
        return shares;
    }

    static ruvia::Task<void> validateTargets(ruvia::DbTransaction& transaction,
                                              const std::vector<NormalizedShare>& shares,
                                              std::string_view ownerId) {
        for (const auto& share : shares) {
            if (share.subjectType == "user") {
                if (share.subjectId == ownerId)
                    service::common::fail(18010, "不能向资源所有者重复授权", 400);
                const auto target = co_await transaction.query(
                    "SELECT 1 FROM sys_user WHERE id = $1 AND status = 'enabled' "
                    "AND deleted_at IS NULL LIMIT 1",
                    service::common::dbParams(share.subjectId));
                if (target.empty())
                    service::common::fail(18010, "包含不存在或已禁用的用户", 400);
            } else {
                const auto target = co_await transaction.query(
                    "SELECT 1 FROM sys_department WHERE id = $1 AND status = 'enabled' "
                    "AND deleted_at IS NULL LIMIT 1",
                    service::common::dbParams(share.subjectId));
                if (target.empty())
                    service::common::fail(18010, "包含不存在或已禁用的部门", 400);
            }
        }
    }
};

inline DeviceShareService& deviceShareService() { return DeviceShareService::instance(); }

inline DeviceService& deviceService() { return DeviceService::instance(); }

} // namespace service::device
