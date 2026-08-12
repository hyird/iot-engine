#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/web/db/Db.h>

#include "service/common/http.h"
#include "service/common/types.h"
#include "service/common/uuid.h"
#include "service/domains/role/role.types.h"

namespace service::role {

class RoleService {
  public:
    static RoleService& instance() {
        static RoleService service;
        return service;
    }

    ruvia::Task<RolePageDataDto> list(ruvia::Context& c, std::int64_t page, std::int64_t pageSize,
                                      std::optional<std::string> keyword,
                                      std::optional<std::string> status) {
        page = std::max<std::int64_t>(1, page);
        pageSize = std::clamp<std::int64_t>(pageSize, 1, 100);
        std::string where = " WHERE deleted_at IS NULL";
        std::vector<ruvia::DbValue> params;
        std::optional<std::string> pattern;
        if (keyword && !keyword->empty()) {
            pattern = "%" + *keyword + "%";
            params.emplace_back(*pattern);
            where += " AND (name ILIKE $1 OR code ILIKE $1)";
        }
        if (status && (*status == "enabled" || *status == "disabled")) {
            params.emplace_back(*status);
            where += " AND status = $" + std::to_string(params.size());
        }

        const auto countRows =
            co_await c.db().query("SELECT COUNT(*) FROM sys_role" + where, params);
        const auto total =
            service::common::parseInt64(
                std::optional<std::string_view>{countRows.front()[0].value().value_or(std::string_view{})})
                .value_or(0);
        auto listParams = params;
        listParams.emplace_back(pageSize);
        const auto limitIndex = listParams.size();
        listParams.emplace_back((page - 1) * pageSize);
        const auto offsetIndex = listParams.size();
        const auto rows = co_await c.db().query(
            "SELECT id, name, code, COALESCE(description, ''), status, "
            "iot_utc_timestamp(created_at), iot_utc_timestamp(updated_at) FROM sys_role" +
                where + " ORDER BY id DESC LIMIT $" + std::to_string(limitIndex) + " OFFSET $" +
                std::to_string(offsetIndex),
            listParams);

        ruvia::BoxedArray<RoleItemDto> roles(c.resource());
        for (const auto& row : rows) {
            auto& role = roles.emplace(c);
            fillBase(role, row);
            role.set<"permissions">(co_await loadPermissions(c, role.get<"id">()->view()));
        }
        RolePageDataDto result(c);
        result.set<"list">(std::move(roles))
            .set<"total">(total)
            .set<"page">(page)
            .set<"pageSize">(pageSize)
            .set<"totalPages">(total == 0 ? 0 : (total + pageSize - 1) / pageSize);
        co_return result;
    }

    ruvia::Task<RoleItemDto> detail(ruvia::Context& c, std::string_view id) {
        const auto rows = co_await c.db().query(R"sql(
SELECT id, name, code, COALESCE(description, ''), status,
       iot_utc_timestamp(created_at), iot_utc_timestamp(updated_at)
FROM sys_role WHERE id = $1 AND deleted_at IS NULL LIMIT 1)sql",
                                                service::common::dbParams(id));
        if (rows.empty())
            service::common::fail(13001, "角色不存在", 404);
        RoleItemDto role(c);
        fillBase(role, rows.front());
        role.set<"permissions">(co_await loadPermissions(c, id));
        co_return role;
    }

    ruvia::Task<ruvia::BoxedArray<RoleOptionDto>> options(ruvia::Context& c) {
        const auto rows =
            co_await c.db().query("SELECT id, name, code FROM sys_role WHERE status = "
                                  "'enabled' AND deleted_at IS NULL ORDER BY id");
        ruvia::BoxedArray<RoleOptionDto> result(c.resource());
        for (const auto& row : rows) {
            auto& item = result.emplace(c);
            item.set<"id">(row[0].value().value_or(std::string_view{})).set<"name">(row[1].value().value_or(std::string_view{})).set<"code">(row[2].value().value_or(std::string_view{}));
        }
        co_return result;
    }

    ruvia::Task<void> create(ruvia::Context& c, const CreateRoleBody& body) {
        const std::string code(body.get<"code">()->view());
        co_await ensureCodeAvailable(c, code, std::nullopt);
        const std::string name(body.get<"name">()->view());
        const std::string description =
            body.get<"description">() ? std::string(body.get<"description">()->view()) : "";
        const std::string status = body.get<"status">() ? std::string(body.get<"status">()->view()) : "enabled";
        const std::string permissions = permissionsText(body.get<"permissions">());
        const auto id = service::common::nextUuidV7();
        (void)co_await c.db().execute(
            R"sql(
INSERT INTO sys_role(id, name, code, description, status, permissions)
VALUES ($1, $2, $3, NULLIF($4, ''), $5,
        COALESCE((SELECT jsonb_agg(permission)
                  FROM unnest(string_to_array(NULLIF($6, ''), ',')) AS values(permission)),
                 '[]'::jsonb))
)sql",
            service::common::dbParams(id, name, code, description, status, permissions));
    }

    ruvia::Task<void> update(ruvia::Context& c, std::string_view id, const UpdateRoleBody& body) {
        const auto existing = co_await c.db().query(
            "SELECT code FROM sys_role WHERE id = $1 AND deleted_at IS NULL LIMIT 1",
            service::common::dbParams(id));
        if (existing.empty())
            service::common::fail(13001, "角色不存在", 404);
        if (existing.front()[0].value().value_or(std::string_view{}) == service::common::kSuperAdminRoleCode)
            service::common::fail(13003, "内置超级管理员角色不能修改", 400);
        if (body.get<"code">())
            co_await ensureCodeAvailable(c, std::string(body.get<"code">()->view()), std::string(id));

        std::string set;
        std::vector<ruvia::DbValue> params;
        std::optional<std::string> permissionValue;
        auto append = [&](std::string_view column, ruvia::DbValue value) {
            if (!set.empty())
                set += ", ";
            params.emplace_back(std::move(value));
            set += std::string(column) + " = $" + std::to_string(params.size());
        };
        if (body.get<"name">())
            append("name", ruvia::DbValue{body.get<"name">()->view()});
        if (body.get<"code">())
            append("code", ruvia::DbValue{body.get<"code">()->view()});
        if (body.get<"description">())
            append("description", ruvia::DbValue{body.get<"description">()->view()});
        if (body.get<"status">())
            append("status", ruvia::DbValue{body.get<"status">()->view()});
        if (body.get<"permissions">()) {
            if (!set.empty())
                set += ", ";
            permissionValue = permissionsText(body.get<"permissions">());
            params.emplace_back(*permissionValue);
            set += "permissions = COALESCE((SELECT jsonb_agg(permission) FROM "
                   "unnest(string_to_array(NULLIF($" +
                   std::to_string(params.size()) +
                   ", ''), ',')) AS values(permission)), '[]'::jsonb)";
        }
        if (set.empty())
            co_return;
        params.emplace_back(id);
        (void)co_await c.db().execute("UPDATE sys_role SET " + set +
                                          ", updated_at = NOW() WHERE id = $" +
                                          std::to_string(params.size()),
                                      params);
    }

    ruvia::Task<void> remove(ruvia::Context& c, std::string_view id) {
        const auto rows = co_await c.db().query(
            "SELECT code FROM sys_role WHERE id = $1 AND deleted_at IS NULL LIMIT 1",
            service::common::dbParams(id));
        if (rows.empty())
            service::common::fail(13001, "角色不存在", 404);
        if (rows.front()[0].value().value_or(std::string_view{}) == service::common::kSuperAdminRoleCode)
            service::common::fail(13003, "内置超级管理员角色不能删除", 400);
        const auto assigned = co_await c.db().query(
            "SELECT 1 FROM sys_user_role ur JOIN sys_user u ON u.id = ur.user_id "
            "WHERE ur.role_id = $1 AND u.deleted_at IS NULL LIMIT 1",
            service::common::dbParams(id));
        if (!assigned.empty())
            service::common::fail(13004, "角色仍有用户使用，不能删除", 409);
        (void)co_await c.db().execute(
            "UPDATE sys_role SET deleted_at = NOW(), updated_at = NOW() WHERE id = $1",
            service::common::dbParams(id));
    }

  private:
    template <typename Row> static void fillBase(RoleItemDto& item, const Row& row) {
        item.set<"id">(row[0].value().value_or(std::string_view{}));
        item.set<"name">(row[1].value().value_or(std::string_view{}));
        item.set<"code">(row[2].value().value_or(std::string_view{}));
        item.set<"description">(row[3].value().value_or(std::string_view{}));
        item.set<"status">(row[4].value().value_or(std::string_view{}));
        item.set<"createdAt">(row[5].value().value_or(std::string_view{}));
        item.set<"updatedAt">(row[6].value().value_or(std::string_view{}));
    }

    static std::string
    permissionsText(const std::optional<ruvia::Array<ruvia::String>>& permissions) {
        if (!permissions)
            return {};
        std::string result;
        for (const auto& permission : *permissions) {
            const auto value = permission.view();
            if (value.empty() || value.size() > 128)
                service::common::fail(13005, "权限编码不能为空且不能超过 128 个字符", 400);
            if (value.find(',') != std::string_view::npos)
                service::common::fail(13005, "权限编码不能包含逗号", 400);
            if (!result.empty())
                result += ',';
            result.append(value);
        }
        return result;
    }

    ruvia::Task<ruvia::Array<ruvia::String>> loadPermissions(ruvia::Context& c,
                                                             std::string_view id) {
        const auto rows = co_await c.db().query(R"sql(
SELECT permission FROM sys_role,
LATERAL jsonb_array_elements_text(permissions) AS values(permission)
WHERE id = $1 ORDER BY permission)sql",
                                                service::common::dbParams(id));
        ruvia::Array<ruvia::String> result(c.resource());
        for (const auto& row : rows)
            result.emplace_back(row[0].value().value_or(std::string_view{}), c.resource());
        co_return result;
    }

    ruvia::Task<void> ensureCodeAvailable(ruvia::Context& c, const std::string& code,
                                          std::optional<std::string> excludedId) {
        auto sql = std::string("SELECT 1 FROM sys_role WHERE code = $1");
        auto params = service::common::dbParams(code);
        if (excludedId) {
            sql += " AND id <> $2";
            params.emplace_back(*excludedId);
        }
        sql += " LIMIT 1";
        const auto rows = co_await c.db().query(sql, params);
        if (!rows.empty())
            service::common::fail(13002, "角色编码已存在", 409);
    }
};

inline RoleService& roleService() { return RoleService::instance(); }

inline ruvia::Task<ruvia::BoxedArray<RoleOptionDto>> listRoleOptions(ruvia::Context& c) {
    co_return co_await roleService().options(c);
}

} // namespace service::role
