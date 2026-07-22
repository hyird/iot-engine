#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <ruvia/web/db/Db.h>

#include "service/common/http.h"
#include "service/middleware/auth.h"

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
        if (rows.rows().empty())
            service::common::fail(service::common::kTokenInvalidErrorCode, "用户状态无效", 401);
        const auto& row = rows.rows().front();
        DeviceActor result;
        result.userId = principal.userId;
        result.departmentId = std::string(row[0].text());
        result.superadmin = isTrue(row[1].text());
        result.canEdit = isTrue(row[2].text());
        result.canDelete = isTrue(row[3].text());
        result.canShare = isTrue(row[4].text());
        result.canCommand = isTrue(row[5].text());
        result.canGroupShare = isTrue(row[6].text());
        co_return result;
    }

    ruvia::Task<DeviceActor> requireGroupOwner(ruvia::Context& c,
                                                std::string_view groupId) const {
        auto currentActor = co_await actor(c);
        const auto rows = co_await c.db().query(
            "SELECT created_by::text FROM device_group WHERE id = $1 "
            "AND deleted_at IS NULL LIMIT 1",
            service::common::dbParams(groupId));
        if (rows.rows().empty())
            service::common::fail(17001, "设备分组不存在", 404);
        if (!currentActor.superadmin && rows.rows().front()[0].text() != currentActor.userId)
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
        if (rows.rows().empty())
            service::common::fail(18001, "设备不存在", 404);
        const auto level = rank(rows.rows().front()[0].text());
        if (level == DeviceAccessLevel::none)
            service::common::fail(18001, "设备不存在", 404);
        if (level < minimum)
            service::common::fail(18005, "设备权限不足", 403);
        co_return DeviceAccessDecision{std::move(currentActor), level};
    }

    static std::string scopedDevicesCte() {
        return "WITH RECURSIVE scoped_device AS (SELECT source.*, " + effectiveRankSql("source") +
               " AS access_rank FROM device source WHERE source.deleted_at IS NULL) ";
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
        const auto parsed = std::stoll(std::string(value));
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

} // namespace service::device
