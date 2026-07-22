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
#include "service/modules/system/user/user.types.h"
#include "service/utils/password.h"

namespace service::user {

class UserService {
  public:
    static UserService& instance() {
        static UserService service;
        return service;
    }

    ruvia::Task<UserPageDataDto> list(ruvia::Context& c, std::int64_t page, std::int64_t pageSize,
                                      std::optional<std::string> keyword,
                                      std::optional<std::string> status) {
        page = std::max<std::int64_t>(1, page);
        pageSize = std::clamp<std::int64_t>(pageSize, 1, 100);
        std::string where = " WHERE u.deleted_at IS NULL";
        std::vector<ruvia::DbValue> filterParams;
        std::optional<std::string> keywordPattern;
        if (keyword && !keyword->empty()) {
            keywordPattern = "%" + *keyword + "%";
            filterParams.emplace_back(*keywordPattern);
            where += " AND (u.username ILIKE $1 OR COALESCE(u.nickname, '') ILIKE $1 OR "
                     "COALESCE(u.email, '') ILIKE $1)";
        }
        if (status && (*status == "enabled" || *status == "disabled")) {
            filterParams.emplace_back(*status);
            where += " AND u.status = $" + std::to_string(filterParams.size());
        }

        const auto countRows =
            co_await c.db().query("SELECT COUNT(*) FROM sys_user u" + where, filterParams);
        const auto total = std::stoll(std::string(countRows.rows().front()[0].text()));

        auto listParams = filterParams;
        listParams.emplace_back(pageSize);
        const auto limitIndex = listParams.size();
        listParams.emplace_back((page - 1) * pageSize);
        const auto offsetIndex = listParams.size();
        const auto rows = co_await c.db().query(
            "SELECT u.id, u.username, COALESCE(u.nickname, ''), COALESCE(u.phone, ''), "
            "COALESCE(u.email, ''), u.status, COALESCE(u.department_id::text, ''), "
            "COALESCE(d.name, ''), u.created_at::text, u.updated_at::text "
            "FROM sys_user u LEFT JOIN sys_department d ON d.id = u.department_id " +
                where + " ORDER BY u.id DESC LIMIT $" + std::to_string(limitIndex) + " OFFSET $" +
                std::to_string(offsetIndex),
            listParams);

        ruvia::List<UserItemDto> users(c.resource());
        for (const auto& row : rows.rows()) {
            auto& item = users.emplace(c);
            fillBase(item, row);
            item.roles(co_await loadRoles(c, item.id()->view()));
        }
        UserPageDataDto result(c);
        result.list(std::move(users))
            .total(total)
            .page(page)
            .pageSize(pageSize)
            .totalPages(total == 0 ? 0 : (total + pageSize - 1) / pageSize);
        co_return result;
    }

    ruvia::Task<UserItemDto> detail(ruvia::Context& c, std::string_view id) {
        const auto rows = co_await c.db().query(R"sql(
SELECT u.id, u.username, COALESCE(u.nickname, ''), COALESCE(u.phone, ''),
       COALESCE(u.email, ''), u.status, COALESCE(u.department_id::text, ''),
       COALESCE(d.name, ''), u.created_at::text, u.updated_at::text
FROM sys_user u
LEFT JOIN sys_department d ON d.id = u.department_id
WHERE u.id = $1 AND u.deleted_at IS NULL LIMIT 1)sql",
                                                service::common::dbParams(id));
        if (rows.rows().empty())
            service::common::fail(12001, "用户不存在", 404);
        UserItemDto item(c);
        fillBase(item, rows.rows().front());
        item.roles(co_await loadRoles(c, id));
        co_return item;
    }

    ruvia::Task<ruvia::List<UserOptionDto>> options(ruvia::Context& c,
                                                    std::optional<std::string> keyword) {
        std::string sql = "SELECT id, username, COALESCE(nickname, '') FROM sys_user WHERE "
                          "deleted_at IS NULL AND status = 'enabled'";
        std::vector<ruvia::DbValue> params;
        std::optional<std::string> pattern;
        if (keyword && !keyword->empty()) {
            pattern = "%" + *keyword + "%";
            params.emplace_back(*pattern);
            sql += " AND (username ILIKE $1 OR COALESCE(nickname, '') ILIKE $1)";
        }
        sql += " ORDER BY username LIMIT 100";
        const auto rows = co_await c.db().query(sql, params);
        ruvia::List<UserOptionDto> result(c.resource());
        for (const auto& row : rows.rows()) {
            auto& item = result.emplace(c);
            item.id(row[0].text()).username(row[1].text()).nickname(row[2].text());
        }
        co_return result;
    }

    ruvia::Task<void> create(ruvia::Context& c, const CreateUserBody& body) {
        const std::string username(body.username()->view());
        const auto exists = co_await c.db().query(
            "SELECT 1 FROM sys_user WHERE username = $1 AND deleted_at IS NULL LIMIT 1",
            service::common::dbParams(username));
        if (!exists.rows().empty())
            service::common::fail(12002, "用户名已存在", 409);
        co_await validateRoles(c, *body.roleIds());
        co_await validateDepartment(c, body.departmentId());

        const std::string passwordHash = service::utils::hashPassword(body.password()->view());
        const std::string nickname = body.nickname() ? std::string(body.nickname()->view()) : "";
        const std::string phone = body.phone() ? std::string(body.phone()->view()) : "";
        const std::string email = body.email() ? std::string(body.email()->view()) : "";
        const std::string status = body.status() ? std::string(body.status()->view()) : "enabled";
        const std::string departmentId =
            body.departmentId() ? std::string(body.departmentId()->view()) : "";
        const auto id = service::common::nextUuidV7();
        auto tx = co_await c.db().beginTransaction();
        const auto inserted = co_await tx.execute(
            R"sql(
INSERT INTO sys_user(
    id, username, password_hash, nickname, phone, email, status, department_id)
VALUES ($1, $2, $3, NULLIF($4, ''), NULLIF($5, ''), NULLIF($6, ''), $7,
        NULLIF($8, '')::uuid)
RETURNING id)sql",
            service::common::dbParams(id, username, passwordHash, nickname, phone, email, status,
                                      departmentId));
        const std::string insertedId(inserted.rows().front()[0].text());
        co_await replaceRoles(tx, insertedId, *body.roleIds());
        co_await tx.commit();
    }

    ruvia::Task<void> update(ruvia::Context& c, std::string_view id, const UpdateUserBody& body) {
        const auto existing = co_await c.db().query(
            "SELECT username FROM sys_user WHERE id = $1 AND deleted_at IS NULL LIMIT 1",
            service::common::dbParams(id));
        if (existing.rows().empty())
            service::common::fail(12001, "用户不存在", 404);
        const std::string username(existing.rows().front()[0].text());
        if (username == "admin" && body.status() && body.status()->view() != "enabled") {
            service::common::fail(12003, "内置管理员不能被禁用", 400);
        }
        if (body.roleIds()) {
            co_await validateRoles(c, *body.roleIds());
            if (username == "admin" && !co_await containsSuperadmin(c, *body.roleIds())) {
                service::common::fail(12003, "不能移除内置管理员的超级管理员角色", 400);
            }
        }
        if (body.departmentId())
            co_await validateDepartment(c, body.departmentId());

        std::string set;
        std::vector<ruvia::DbValue> params;
        std::optional<std::string> passwordHash;
        auto append = [&](std::string_view column, ruvia::DbValue value) {
            if (!set.empty())
                set += ", ";
            params.emplace_back(std::move(value));
            set += std::string(column) + " = $" + std::to_string(params.size());
        };
        if (body.nickname())
            append("nickname", ruvia::DbValue{body.nickname()->view()});
        if (body.phone())
            append("phone", ruvia::DbValue{body.phone()->view()});
        if (body.email())
            append("email", ruvia::DbValue{body.email()->view()});
        if (body.status())
            append("status", ruvia::DbValue{body.status()->view()});
        if (body.password()) {
            passwordHash = service::utils::hashPassword(body.password()->view());
            append("password_hash", ruvia::DbValue{*passwordHash});
        }
        if (body.departmentId()) {
            if (!set.empty())
                set += ", ";
            params.emplace_back(body.departmentId()->view());
            set += "department_id = NULLIF($" + std::to_string(params.size()) + ", '')::uuid";
        }

        auto tx = co_await c.db().beginTransaction();
        if (!set.empty()) {
            params.emplace_back(id);
            (void)co_await tx.execute("UPDATE sys_user SET " + set +
                                          ", updated_at = NOW() WHERE id = $" +
                                          std::to_string(params.size()),
                                      params);
        }
        if (body.roleIds())
            co_await replaceRoles(tx, id, *body.roleIds());
        co_await tx.commit();
    }

    ruvia::Task<void> remove(ruvia::Context& c, std::string_view id, std::string_view operatorId) {
        if (id == operatorId)
            service::common::fail(12004, "不能删除当前登录用户", 400);
        const auto rows = co_await c.db().query(
            "SELECT username FROM sys_user WHERE id = $1 AND deleted_at IS NULL LIMIT 1",
            service::common::dbParams(id));
        if (rows.rows().empty())
            service::common::fail(12001, "用户不存在", 404);
        if (rows.rows().front()[0].text() == "admin")
            service::common::fail(12004, "内置管理员不能删除", 400);
        (void)co_await c.db().execute(
            "UPDATE sys_user SET deleted_at = NOW(), updated_at = NOW() WHERE id = $1",
            service::common::dbParams(id));
    }

  private:
    template <typename Row> static void fillBase(UserItemDto& item, const Row& row) {
        item.id(row[0].text())
            .username(row[1].text())
            .nickname(row[2].text())
            .phone(row[3].text())
            .email(row[4].text())
            .status(row[5].text())
            .departmentId(row[6].text())
            .departmentName(row[7].text())
            .createdAt(row[8].text())
            .updatedAt(row[9].text());
    }

    ruvia::Task<ruvia::List<service::role::RoleOptionDto>> loadRoles(ruvia::Context& c,
                                                                     std::string_view userId) {
        const auto rows = co_await c.db().query(R"sql(
SELECT r.id, r.name, r.code FROM sys_role r
JOIN sys_user_role ur ON ur.role_id = r.id
WHERE ur.user_id = $1 AND r.deleted_at IS NULL ORDER BY r.id)sql",
                                                service::common::dbParams(userId));
        ruvia::List<service::role::RoleOptionDto> roles(c.resource());
        for (const auto& row : rows.rows()) {
            auto& role = roles.emplace(c);
            role.id(row[0].text()).name(row[1].text()).code(row[2].text());
        }
        co_return roles;
    }

    ruvia::Task<void> validateRoles(ruvia::Context& c, const ruvia::Array<ruvia::String>& roleIds) {
        for (const auto& roleId : roleIds) {
            if (!service::common::isUuid(roleId.view()))
                service::common::fail(12005, "角色 ID 必须是 UUID", 400);
            const auto rows =
                co_await c.db().query("SELECT 1 FROM sys_role WHERE id = $1 AND status = 'enabled' "
                                      "AND deleted_at IS NULL",
                                      service::common::dbParams(roleId.view()));
            if (rows.rows().empty())
                service::common::fail(12005, "包含无效角色", 400);
        }
    }

    ruvia::Task<void> validateDepartment(
        ruvia::Context& c, const std::optional<ruvia::String>& departmentId) {
        if (!departmentId || departmentId->view().empty())
            co_return;
        const auto rows = co_await c.db().query(
            "SELECT 1 FROM sys_department WHERE id = $1 AND status = 'enabled' "
            "AND deleted_at IS NULL LIMIT 1",
            service::common::dbParams(departmentId->view()));
        if (rows.rows().empty())
            service::common::fail(12006, "部门不存在或已禁用", 400);
    }

    ruvia::Task<bool> containsSuperadmin(ruvia::Context& c,
                                         const ruvia::Array<ruvia::String>& roleIds) {
        for (const auto& roleId : roleIds) {
            const auto rows = co_await c.db().query(
                "SELECT code FROM sys_role WHERE id = $1 AND deleted_at IS NULL",
                service::common::dbParams(roleId.view()));
            if (!rows.rows().empty() &&
                rows.rows().front()[0].text() == service::common::kSuperAdminRoleCode)
                co_return true;
        }
        co_return false;
    }

    static ruvia::Task<void> replaceRoles(ruvia::DbTransaction& tx, std::string_view userId,
                                          const ruvia::Array<ruvia::String>& roleIds) {
        (void)co_await tx.execute("DELETE FROM sys_user_role WHERE user_id = $1",
                                  service::common::dbParams(userId));
        for (const auto& roleId : roleIds) {
            const auto id = service::common::nextUuidV7();
            (void)co_await tx.execute("INSERT INTO sys_user_role(id, user_id, role_id) VALUES ($1, "
                                      "$2, $3) ON CONFLICT DO "
                                      "NOTHING",
                                      service::common::dbParams(id, userId, roleId.view()));
        }
    }
};

inline UserService& userService() { return UserService::instance(); }

} // namespace service::user
