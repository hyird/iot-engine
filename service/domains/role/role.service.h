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
        const auto total = std::stoll(std::string(countRows.rows().front()[0].text()));
        auto listParams = params;
        listParams.emplace_back(pageSize);
        const auto limitIndex = listParams.size();
        listParams.emplace_back((page - 1) * pageSize);
        const auto offsetIndex = listParams.size();
        const auto rows = co_await c.db().query(
            "SELECT id, name, code, COALESCE(description, ''), status, created_at::text, "
            "updated_at::text FROM sys_role" +
                where + " ORDER BY id DESC LIMIT $" + std::to_string(limitIndex) + " OFFSET $" +
                std::to_string(offsetIndex),
            listParams);

        ruvia::List<RoleItemDto> roles(c.resource());
        for (const auto& row : rows.rows()) {
            auto& role = roles.emplace(c);
            fillBase(role, row);
            role.permissions(co_await loadPermissions(c, role.id()->view()));
        }
        RolePageDataDto result(c);
        result.list(std::move(roles))
            .total(total)
            .page(page)
            .pageSize(pageSize)
            .totalPages(total == 0 ? 0 : (total + pageSize - 1) / pageSize);
        co_return result;
    }

    ruvia::Task<RoleItemDto> detail(ruvia::Context& c, std::string_view id) {
        const auto rows = co_await c.db().query(R"sql(
SELECT id, name, code, COALESCE(description, ''), status, created_at::text, updated_at::text
FROM sys_role WHERE id = $1 AND deleted_at IS NULL LIMIT 1)sql",
                                                service::common::dbParams(id));
        if (rows.rows().empty())
            service::common::fail(13001, "角色不存在", 404);
        RoleItemDto role(c);
        fillBase(role, rows.rows().front());
        role.permissions(co_await loadPermissions(c, id));
        co_return role;
    }

    ruvia::Task<ruvia::List<RoleOptionDto>> options(ruvia::Context& c) {
        const auto rows =
            co_await c.db().query("SELECT id, name, code FROM sys_role WHERE status = "
                                  "'enabled' AND deleted_at IS NULL ORDER BY id");
        ruvia::List<RoleOptionDto> result(c.resource());
        for (const auto& row : rows.rows()) {
            auto& item = result.emplace(c);
            item.id(row[0].text()).name(row[1].text()).code(row[2].text());
        }
        co_return result;
    }

    ruvia::Task<void> create(ruvia::Context& c, const CreateRoleBody& body) {
        const std::string code(body.code()->view());
        co_await ensureCodeAvailable(c, code, std::nullopt);
        const std::string name(body.name()->view());
        const std::string description =
            body.description() ? std::string(body.description()->view()) : "";
        const std::string status = body.status() ? std::string(body.status()->view()) : "enabled";
        const std::string permissions = permissionsText(body.permissions());
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
        if (existing.rows().empty())
            service::common::fail(13001, "角色不存在", 404);
        if (existing.rows().front()[0].text() == service::common::kSuperAdminRoleCode)
            service::common::fail(13003, "内置超级管理员角色不能修改", 400);
        if (body.code())
            co_await ensureCodeAvailable(c, std::string(body.code()->view()), std::string(id));

        std::string set;
        std::vector<ruvia::DbValue> params;
        std::optional<std::string> permissionValue;
        auto append = [&](std::string_view column, ruvia::DbValue value) {
            if (!set.empty())
                set += ", ";
            params.emplace_back(std::move(value));
            set += std::string(column) + " = $" + std::to_string(params.size());
        };
        if (body.name())
            append("name", ruvia::DbValue{body.name()->view()});
        if (body.code())
            append("code", ruvia::DbValue{body.code()->view()});
        if (body.description())
            append("description", ruvia::DbValue{body.description()->view()});
        if (body.status())
            append("status", ruvia::DbValue{body.status()->view()});
        if (body.permissions()) {
            if (!set.empty())
                set += ", ";
            permissionValue = permissionsText(body.permissions());
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
        if (rows.rows().empty())
            service::common::fail(13001, "角色不存在", 404);
        if (rows.rows().front()[0].text() == service::common::kSuperAdminRoleCode)
            service::common::fail(13003, "内置超级管理员角色不能删除", 400);
        const auto assigned = co_await c.db().query(
            "SELECT 1 FROM sys_user_role ur JOIN sys_user u ON u.id = ur.user_id "
            "WHERE ur.role_id = $1 AND u.deleted_at IS NULL LIMIT 1",
            service::common::dbParams(id));
        if (!assigned.rows().empty())
            service::common::fail(13004, "角色仍有用户使用，不能删除", 409);
        (void)co_await c.db().execute(
            "UPDATE sys_role SET deleted_at = NOW(), updated_at = NOW() WHERE id = $1",
            service::common::dbParams(id));
    }

  private:
    template <typename Row> static void fillBase(RoleItemDto& item, const Row& row) {
        item.id(row[0].text())
            .name(row[1].text())
            .code(row[2].text())
            .description(row[3].text())
            .status(row[4].text())
            .createdAt(row[5].text())
            .updatedAt(row[6].text());
    }

    static std::string
    permissionsText(const std::optional<ruvia::Array<ruvia::String>>& permissions) {
        if (!permissions)
            return {};
        std::string result;
        for (const auto& permission : *permissions) {
            const auto value = permission.view();
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
        for (const auto& row : rows.rows())
            result.emplace_back(row[0].text(), c.resource());
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
        if (!rows.rows().empty())
            service::common::fail(13002, "角色编码已存在", 409);
    }
};

inline RoleService& roleService() { return RoleService::instance(); }

inline ruvia::Task<ruvia::List<RoleOptionDto>> listRoleOptions(ruvia::Context& c) {
    co_return co_await roleService().options(c);
}

} // namespace service::role
