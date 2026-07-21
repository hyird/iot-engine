#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <ruvia/web/ModelObject.h>
#include <ruvia/web/db/Db.h>

#include "service/common/http.h"
#include "service/middleware/auth.h"

namespace service::device_group {

class DeviceGroupService {
  public:
    static DeviceGroupService& instance() {
        static DeviceGroupService service;
        return service;
    }

    ruvia::Task<std::string> list(ruvia::Context& c, bool withCount) {
        const auto countExpr =
            withCount
                ? R"sql((SELECT COUNT(*) FROM iot_device d WHERE d.group_id = g.id AND d.deleted_at IS NULL))sql"
                : "0";
        const std::string sql =
            std::string("SELECT jsonb_build_object('id', g.id, 'name', g.name, 'parent_id', "
                        "g.parent_id, 'status', g.status, 'sort_order', g.sort_order, 'remark', "
                        "COALESCE(g.remark, ''), 'deviceCount', ") +
            countExpr +
            ", 'created_at', g.created_at, 'updated_at', g.updated_at)::text "
            "FROM iot_device_group g WHERE g.deleted_at IS NULL "
            "ORDER BY g.sort_order, g.id";
        const auto rows = co_await c.db().query(sql);
        std::string result = "[";
        bool first = true;
        for (const auto& row : rows.rows()) {
            if (!first)
                result.push_back(',');
            first = false;
            result.append(row[0].text());
        }
        result.push_back(']');
        co_return result;
    }

    ruvia::Task<std::string> detail(ruvia::Context& c, std::int64_t id) {
        const auto rows = co_await c.db().query(R"sql(
SELECT jsonb_build_object(
    'id', id, 'name', name, 'parent_id', parent_id, 'status', status,
    'sort_order', sort_order, 'remark', COALESCE(remark, ''),
    'created_at', created_at, 'updated_at', updated_at)::text
FROM iot_device_group WHERE id = $1 AND deleted_at IS NULL LIMIT 1)sql",
                                                service::common::dbParams(id));
        if (rows.rows().empty())
            service::common::fail(17001, "设备分组不存在", 404);
        co_return std::string(rows.rows().front()[0].text());
    }

    ruvia::Task<void> create(ruvia::Context& c, const ruvia::JsonValue& payload) {
        co_await validate(c, payload, std::nullopt);
        const auto principal = service::middleware::requireAuth(c);
        (void)co_await c.db().execute(R"sql(
WITH body AS (SELECT $1::jsonb AS value)
INSERT INTO iot_device_group(name, parent_id, status, sort_order, remark, created_by)
SELECT value->>'name', NULLIF(value->>'parent_id', '')::bigint,
       COALESCE(value->>'status', 'enabled'), COALESCE((value->>'sort_order')::integer, 0),
       NULLIF(value->>'remark', ''), $2 FROM body)sql",
                                      service::common::dbParams(payload.view(), principal.userId));
    }

    ruvia::Task<void> update(ruvia::Context& c, std::int64_t id, const ruvia::JsonValue& payload) {
        const auto rows = co_await c.db().query(
            "SELECT created_by FROM iot_device_group WHERE id = $1 AND deleted_at IS NULL",
            service::common::dbParams(id));
        if (rows.rows().empty())
            service::common::fail(17001, "设备分组不存在", 404);
        co_await requireOwner(c, toInt(rows.rows().front()[0].text()));
        co_await validate(c, payload, id);
        (void)co_await c.db().execute(R"sql(
WITH body AS (SELECT $1::jsonb AS value)
UPDATE iot_device_group g SET
    name = CASE WHEN body.value ? 'name' THEN body.value->>'name' ELSE g.name END,
    parent_id = CASE WHEN body.value ? 'parent_id' THEN NULLIF(body.value->>'parent_id', '')::bigint ELSE g.parent_id END,
    status = CASE WHEN body.value ? 'status' THEN body.value->>'status' ELSE g.status END,
    sort_order = CASE WHEN body.value ? 'sort_order' THEN (body.value->>'sort_order')::integer ELSE g.sort_order END,
    remark = CASE WHEN body.value ? 'remark' THEN NULLIF(body.value->>'remark', '') ELSE g.remark END,
    updated_at = NOW()
FROM body WHERE g.id = $2)sql",
                                      service::common::dbParams(payload.view(), id));
    }

    ruvia::Task<void> remove(ruvia::Context& c, std::int64_t id) {
        const auto rows = co_await c.db().query(
            "SELECT created_by FROM iot_device_group WHERE id = $1 AND deleted_at IS NULL",
            service::common::dbParams(id));
        if (rows.rows().empty())
            service::common::fail(17001, "设备分组不存在", 404);
        co_await requireOwner(c, toInt(rows.rows().front()[0].text()));
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

    ruvia::Task<void> validate(ruvia::Context& c, const ruvia::JsonValue& payload,
                               std::optional<std::int64_t> currentId) {
        if (!payload.isObject())
            service::common::fail(17002, "请求体必须是对象", 400);
        const auto name = payload.get<ruvia::String>("name");
        if (!currentId && (!name || name->view().empty()))
            service::common::fail(17002, "分组名称不能为空", 400);
        if (name && (name->view().empty() || name->view().size() > 100))
            service::common::fail(17002, "分组名称长度必须在 1 - 100 之间", 400);
        const auto check = co_await c.db().query(R"sql(
WITH body AS (SELECT $1::jsonb AS value)
SELECT
  NOT (value ? 'status') OR value->>'status' IN ('enabled', 'disabled'),
  NOT (value ? 'sort_order') OR
      (jsonb_typeof(value->'sort_order') = 'number' AND (value->>'sort_order')::numeric >= 0),
  NOT (value ? 'parent_id') OR value->'parent_id' = 'null'::jsonb OR
      (jsonb_typeof(value->'parent_id') = 'number' AND (value->>'parent_id')::bigint > 0)
FROM body)sql",
                                                 service::common::dbParams(payload.view()));
        const auto& row = check.rows().front();
        if (row[0].text() != "t" || row[1].text() != "t" || row[2].text() != "t")
            service::common::fail(17002, "设备分组参数无效", 400);
        const auto parent = payload.get<ruvia::Int64>("parent_id");
        if (parent) {
            if (currentId && *parent == *currentId)
                service::common::fail(17003, "上级分组不能是自身", 409);
            const auto exists = co_await c.db().query(
                "SELECT 1 FROM iot_device_group WHERE id = $1 AND deleted_at IS NULL",
                service::common::dbParams(static_cast<std::int64_t>(*parent)));
            if (exists.rows().empty())
                service::common::fail(17003, "上级分组不存在", 400);
        }
    }

    ruvia::Task<void> requireOwner(ruvia::Context& c, std::int64_t ownerId) {
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
};

inline DeviceGroupService& deviceGroupService() { return DeviceGroupService::instance(); }

} // namespace service::device_group
