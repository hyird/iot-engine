#pragma once

#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/web/db/Db.h>

#include "service/common/http.h"
#include "service/common/id.validation.h"
#include "service/common/uuid.h"
#include "service/modules/northbridge/device/device.access.h"
#include "service/modules/northbridge/device/device.types.h"

namespace service::device {

class DeviceShareService {
  public:
    static DeviceShareService& instance() {
        static DeviceShareService service;
        return service;
    }

    ruvia::Task<ruvia::List<DeviceShareItemDto>> list(ruvia::Context& c,
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
       access_grant.created_at::text, access_grant.updated_at::text
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
       group_access.created_at::text, group_access.updated_at::text
FROM device_group_access_grant group_access
JOIN ancestor_group ancestor ON ancestor.id = group_access.group_id
LEFT JOIN sys_user inherited_user ON inherited_user.id = group_access.user_id
LEFT JOIN sys_department inherited_department
       ON inherited_department.id = group_access.department_id
ORDER BY 2, 4, 9, 1)sql",
                                                service::common::dbParams(deviceId));
        ruvia::List<DeviceShareItemDto> result(c.resource());
        for (const auto& row : rows.rows()) {
            auto& item = result.emplace(c);
            item.id(row[0].text())
                .subjectType(row[1].text())
                .subjectId(row[2].text())
                .subjectName(row[3].text())
                .accessLevel(row[4].text())
                .sourceType(row[5].text())
                .sourceGroupId(row[6].text())
                .sourceGroupName(row[7].text())
                .inherited(row[8].text() == "t")
                .createdAt(row[9].text())
                .updatedAt(row[10].text());
        }
        co_return result;
    }

    ruvia::Task<ruvia::List<DeviceShareTargetDto>> targets(ruvia::Context& c,
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
        ruvia::List<DeviceShareTargetDto> result(c.resource());
        for (const auto& row : rows.rows()) {
            auto& item = result.emplace(c);
            item.subjectType(row[0].text()).subjectId(row[1].text()).subjectName(row[2].text());
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
        if (deviceRows.rows().empty())
            service::common::fail(18001, "设备不存在", 404);
        const std::string ownerId(deviceRows.rows().front()[0].text());

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

    ruvia::Task<ruvia::List<DeviceShareItemDto>> listGroup(ruvia::Context& c,
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
       access_grant.created_at::text, access_grant.updated_at::text
FROM device_group_access_grant access_grant
JOIN device_group target_group ON target_group.id = access_grant.group_id
LEFT JOIN sys_user target_user ON target_user.id = access_grant.user_id
LEFT JOIN sys_department target_department
       ON target_department.id = access_grant.department_id
WHERE access_grant.group_id = $1
ORDER BY 2, 4, access_grant.id)sql",
                                                service::common::dbParams(groupId));
        ruvia::List<DeviceShareItemDto> result(c.resource());
        for (const auto& row : rows.rows()) {
            auto& item = result.emplace(c);
            item.id(row[0].text())
                .subjectType(row[1].text())
                .subjectId(row[2].text())
                .subjectName(row[3].text())
                .accessLevel(row[4].text())
                .sourceType(row[5].text())
                .sourceGroupId(row[6].text())
                .sourceGroupName(row[7].text())
                .inherited(false)
                .createdAt(row[9].text())
                .updatedAt(row[10].text());
        }
        co_return result;
    }

    ruvia::Task<ruvia::List<DeviceShareTargetDto>> groupTargets(ruvia::Context& c,
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
        ruvia::List<DeviceShareTargetDto> result(c.resource());
        for (const auto& row : rows.rows()) {
            auto& item = result.emplace(c);
            item.subjectType(row[0].text()).subjectId(row[1].text()).subjectName(row[2].text());
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
        if (groupRows.rows().empty())
            service::common::fail(17001, "设备分组不存在", 404);
        co_await validateTargets(transaction, shares, groupRows.rows().front()[0].text());

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
        if (!body.shares())
            service::common::fail(18010, "分享列表不能为空", 400);
        std::vector<NormalizedShare> shares;
        shares.reserve(body.shares()->size());
        std::set<std::string, std::less<>> uniqueSubjects;
        for (const auto& item : *body.shares()) {
            if (!item.subjectType() || !item.subjectId() || !item.accessLevel())
                service::common::fail(18010, "分享对象参数不完整", 400);
            NormalizedShare share{std::string(item.subjectType()->view()),
                                  std::string(item.subjectId()->view()),
                                  std::string(item.accessLevel()->view())};
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
                if (target.rows().empty())
                    service::common::fail(18010, "包含不存在或已禁用的用户", 400);
            } else {
                const auto target = co_await transaction.query(
                    "SELECT 1 FROM sys_department WHERE id = $1 AND status = 'enabled' "
                    "AND deleted_at IS NULL LIMIT 1",
                    service::common::dbParams(share.subjectId));
                if (target.rows().empty())
                    service::common::fail(18010, "包含不存在或已禁用的部门", 400);
            }
        }
    }
};

inline DeviceShareService& deviceShareService() { return DeviceShareService::instance(); }

} // namespace service::device
