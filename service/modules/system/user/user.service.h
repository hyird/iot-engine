#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "service/common/database.h"
#include "service/modules/system/role/role.entity.h"
#include "service/modules/system/dept/dept.entity.h"
#include "service/modules/system/user/user.entity.h"

#include "service/common/http.h"
#include "service/modules/system/role/role.types.h"
#include "service/common/uuid.h"
#include "service/modules/system/user/user.types.h"
#include "service/utils/password.h"

namespace service::user {

namespace db = service::common::database;

class UserService {
  public:
    static UserService &instance() {
        static UserService service;
        return service;
    }

    ruvia::Task<UserPageDataDto> list(ruvia::Context &c, std::int64_t page, std::int64_t pageSize,
                                      std::optional<std::string> keyword,
                                      std::optional<std::string> status) {
        page = std::max<std::int64_t>(1, page);
        pageSize = std::clamp<std::int64_t>(pageSize, 1, 100);
        ruvia::DbFindOptions options;
        options.where = UserEntity::column<"deleted_at">().isNull();
        if (keyword && !keyword->empty()) {
            const auto pattern = "%" + *keyword + "%";
            options.where =
                std::move(options.where) && (UserEntity::column<"username">().ilike(pattern) ||
                                             UserEntity::column<"nickname">().ilike(pattern) ||
                                             UserEntity::column<"email">().ilike(pattern));
        }
        if (status && (*status == "enabled" || *status == "disabled"))
            options.where = std::move(options.where) && UserEntity::column<"status">() == *status;
        const auto total = static_cast<std::int64_t>(
            co_await c.db().getRepository<UserEntity>().count(options.where));
        auto query = userSelect(c.operationResource());
        query.where(options.where.expression(query, UserEntity::tableName(), "u"))
            .orderBy(query.column("id", "u"), ruvia::DbOrderDirection::kDesc)
            .limit(pageSize)
            .offset((page - 1) * pageSize);
        const auto rows = co_await c.db().query(query);

        ruvia::BoxedArray<UserItemDto> users(ruvia::ModelOptions{.resource = c.resource()});
        for (const auto &row : rows) {
            auto &item = users.emplace(c);
            fillBase(item, row);
            item.set<"roles">(co_await loadRoles(c, item.get<"id">()->view()));
        }
        UserPageDataDto result(c);
        result.set<"list">(std::move(users))
            .set<"total">(total)
            .set<"page">(page)
            .set<"pageSize">(pageSize)
            .set<"totalPages">(total == 0 ? 0 : (total + pageSize - 1) / pageSize);
        co_return result;
    }

    ruvia::Task<UserItemDto> detail(ruvia::Context &c, std::string_view id) {
        auto query = userSelect(c.operationResource());
        query.where(db::activeId<UserEntity>(id).expression(query, UserEntity::tableName(), "u"))
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty())
            service::common::fail(12001, "用户不存在", 404);
        UserItemDto item(c);
        fillBase(item, rows.front());
        item.set<"roles">(co_await loadRoles(c, id));
        co_return item;
    }

    ruvia::Task<ruvia::BoxedArray<UserOptionDto>> options(ruvia::Context &c,
                                                          std::optional<std::string> keyword) {
        ruvia::DbQuery query(c.operationResource());
        auto where = UserEntity::column<"deleted_at">().isNull() &&
                     UserEntity::column<"status">() == "enabled";
        if (keyword && !keyword->empty()) {
            const auto pattern = "%" + *keyword + "%";
            where = std::move(where) && (UserEntity::column<"username">().ilike(pattern) ||
                                         UserEntity::column<"nickname">().ilike(pattern));
        }
        query
            .select(
                {query.column("id"), query.column("username"), db::emptyText(query, "nickname")})
            .from(UserEntity::tableName())
            .where(where.expression(query))
            .orderBy(query.column("username"))
            .limit(100);
        const auto rows = co_await c.db().query(query);
        ruvia::BoxedArray<UserOptionDto> result(ruvia::ModelOptions{.resource = c.resource()});
        for (const auto &row : rows) {
            auto &item = result.emplace(c);
            item.set<"id">(row[0].value().value_or(std::string_view{}))
                .set<"username">(row[1].value().value_or(std::string_view{}))
                .set<"nickname">(row[2].value().value_or(std::string_view{}));
        }
        co_return result;
    }

    ruvia::Task<void> create(ruvia::Context &c, const CreateUserBody &body) {
        const std::string username(body.get<"username">()->view());
        ruvia::DbFindOptions exists;
        exists.where = UserEntity::column<"username">() == username &&
                       UserEntity::column<"deleted_at">().isNull();
        if (co_await c.db().getRepository<UserEntity>().exists(exists))
            service::common::fail(12002, "用户名已存在", 409);
        co_await validateRoles(c, *body.get<"roleIds">());
        co_await validateDepartment(c, body.get<"departmentId">());

        const std::string passwordHash =
            service::utils::hashPassword(body.get<"password">()->view());
        const std::string nickname =
            body.get<"nickname">() ? std::string(body.get<"nickname">()->view()) : "";
        const std::string phone =
            body.get<"phone">() ? std::string(body.get<"phone">()->view()) : "";
        const std::string email =
            body.get<"email">() ? std::string(body.get<"email">()->view()) : "";
        const std::string status =
            body.get<"status">() ? std::string(body.get<"status">()->view()) : "enabled";
        const std::string departmentId =
            body.get<"departmentId">() ? std::string(body.get<"departmentId">()->view()) : "";
        const auto id = service::common::nextUuidV7();
        auto tx = co_await c.db().beginTransaction();
        ruvia::DbQuery query(c.operationResource());
        query
            .insertInto(UserEntity::tableName(), {"id", "username", "password_hash", "nickname",
                                                  "phone", "email", "status", "department_id"})
            .values({query.value(id), query.value(username), query.value(passwordHash),
                     query.nullIf(query.value(nickname), query.value("")),
                     query.nullIf(query.value(phone), query.value("")),
                     query.nullIf(query.value(email), query.value("")), query.value(status),
                     db::nullableUuid(query, departmentId)})
            .returning({query.column("id")});
        const auto inserted = co_await tx.query(query);
        const std::string insertedId(inserted.front()[0].value().value_or(std::string_view{}));
        co_await replaceRoles(tx, insertedId, *body.get<"roleIds">(), c.operationResource());
        co_await tx.commit();
    }

    ruvia::Task<void> update(ruvia::Context &c, std::string_view id, const UpdateUserBody &body) {
        ruvia::DbQuery lookup(c.operationResource());
        lookup.select(lookup.column("username"))
            .from(UserEntity::tableName())
            .where(db::activeId<UserEntity>(id).expression(lookup))
            .limit(1);
        const auto existing = co_await c.db().query(lookup);
        if (existing.empty())
            service::common::fail(12001, "用户不存在", 404);
        const std::string username(existing.front()[0].value().value_or(std::string_view{}));
        if (username == "admin" && body.get<"status">() &&
            body.get<"status">()->view() != "enabled") {
            service::common::fail(12003, "内置管理员不能被禁用", 400);
        }
        if (body.get<"roleIds">()) {
            co_await validateRoles(c, *body.get<"roleIds">());
            if (username == "admin" && !co_await containsSuperadmin(c, *body.get<"roleIds">())) {
                service::common::fail(12003, "不能移除内置管理员的超级管理员角色", 400);
            }
        }
        if (body.get<"departmentId">())
            co_await validateDepartment(c, body.get<"departmentId">());

        ruvia::DbQuery query(c.operationResource());
        query.update(UserEntity::tableName());
        bool changed = false;
        auto append = [&](std::string_view column, std::string_view value) {
            query.set(column, query.value(value));
            changed = true;
        };
        if (body.get<"nickname">())
            append("nickname", body.get<"nickname">()->view());
        if (body.get<"phone">())
            append("phone", body.get<"phone">()->view());
        if (body.get<"email">())
            append("email", body.get<"email">()->view());
        if (body.get<"status">())
            append("status", body.get<"status">()->view());
        if (body.get<"password">())
            append("password_hash", service::utils::hashPassword(body.get<"password">()->view()));
        if (body.get<"departmentId">()) {
            query.set("department_id", db::nullableUuid(query, body.get<"departmentId">()->view()));
            changed = true;
        }
        auto tx = co_await c.db().beginTransaction();
        if (changed) {
            query.set("updated_at", query.call("now"))
                .where((UserEntity::column<"id">() == id).expression(query));
            (void)co_await tx.execute(query);
        }
        if (body.get<"roleIds">())
            co_await replaceRoles(tx, id, *body.get<"roleIds">(), c.operationResource());
        co_await tx.commit();
    }

    ruvia::Task<void> remove(ruvia::Context &c, std::string_view id, std::string_view operatorId) {
        if (id == operatorId)
            service::common::fail(12004, "不能删除当前登录用户", 400);
        ruvia::DbQuery query(c.operationResource());
        query.select(query.column("username"))
            .from(UserEntity::tableName())
            .where(db::activeId<UserEntity>(id).expression(query))
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty())
            service::common::fail(12001, "用户不存在", 404);
        if (rows.front()[0].value().value_or(std::string_view{}) == "admin")
            service::common::fail(12004, "内置管理员不能删除", 400);
        ruvia::DbQuery removal(c.operationResource());
        removal.update(UserEntity::tableName())
            .set("deleted_at", removal.call("now"))
            .set("updated_at", removal.call("now"))
            .where((UserEntity::column<"id">() == id).expression(removal));
        (void)co_await c.db().execute(removal);
    }

  private:
    static ruvia::DbQuery userSelect(std::pmr::memory_resource *resource) {
        using namespace ruvia;
        DbQuery query(resource);
        query
            .select({query.column("id", "u"), query.column("username", "u"),
                     db::emptyText(query, "nickname", "u"), db::emptyText(query, "phone", "u"),
                     db::emptyText(query, "email", "u"), query.column("status", "u"),
                     db::emptyText(query, "department_id", "u"), db::emptyText(query, "name", "d"),
                     query.call("iot_utc_timestamp", {query.column("created_at", "u")}),
                     query.call("iot_utc_timestamp", {query.column("updated_at", "u")})})
            .from(UserEntity::tableName(), "u")
            .join(DbJoinType::kLeft, service::dept::DeptEntity::tableName(),
                  query.binary(query.column("id", "d"), DbBinaryOperator::kEqual,
                               query.column("department_id", "u")),
                  "d");
        return query;
    }

    template <typename Row> static void fillBase(UserItemDto &item, const Row &row) {
        item.set<"id">(row[0].value().value_or(std::string_view{}));
        item.set<"username">(row[1].value().value_or(std::string_view{}));
        item.set<"nickname">(row[2].value().value_or(std::string_view{}));
        item.set<"phone">(row[3].value().value_or(std::string_view{}));
        item.set<"email">(row[4].value().value_or(std::string_view{}));
        item.set<"status">(row[5].value().value_or(std::string_view{}));
        item.set<"departmentId">(row[6].value().value_or(std::string_view{}));
        item.set<"departmentName">(row[7].value().value_or(std::string_view{}));
        item.set<"createdAt">(row[8].value().value_or(std::string_view{}));
        item.set<"updatedAt">(row[9].value().value_or(std::string_view{}));
    }

    ruvia::Task<ruvia::BoxedArray<service::role::RoleOptionDto>>
    loadRoles(ruvia::Context &c, std::string_view userId) {
        ruvia::DbQuery query(c.operationResource());
        query
            .select({query.column("id", "r"), query.column("name", "r"), query.column("code", "r")})
            .from(service::role::RoleEntity::tableName(), "r")
            .join(ruvia::DbJoinType::kInner, UserRoleEntity::tableName(),
                  query.binary(query.column("role_id", "ur"), ruvia::DbBinaryOperator::kEqual,
                               query.column("id", "r")),
                  "ur")
            .where(query.binary(query.column("user_id", "ur"), ruvia::DbBinaryOperator::kEqual,
                                query.value(userId)))
            .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull, query.column("deleted_at", "r")))
            .orderBy(query.column("id", "r"));
        const auto rows = co_await c.db().query(query);
        ruvia::BoxedArray<service::role::RoleOptionDto> roles(
            ruvia::ModelOptions{.resource = c.resource()});
        for (const auto &row : rows) {
            auto &role = roles.emplace(c);
            role.set<"id">(row[0].value().value_or(std::string_view{}))
                .set<"name">(row[1].value().value_or(std::string_view{}))
                .set<"code">(row[2].value().value_or(std::string_view{}));
        }
        co_return roles;
    }

    ruvia::Task<void> validateRoles(ruvia::Context &c, const ruvia::Array<ruvia::String> &roleIds) {
        std::set<std::string, std::less<>> seenRoleIds;
        for (const auto &roleId : roleIds) {
            if (!service::common::isUuid(roleId.view()))
                service::common::fail(12005, "角色 ID 必须是 UUID", 400);
            const std::string roleIdValue(roleId.view());
            if (!seenRoleIds.emplace(roleIdValue).second)
                service::common::fail(12005, "角色不能重复", 400);
            ruvia::DbFindOptions options;
            options.where = db::activeId<service::role::RoleEntity>(roleIdValue) &&
                            service::role::RoleEntity::column<"status">() == "enabled";
            if (!co_await c.db().getRepository<service::role::RoleEntity>().exists(options))
                service::common::fail(12005, "包含无效角色", 400);
        }
    }

    ruvia::Task<void> validateDepartment(ruvia::Context &c,
                                         const std::optional<ruvia::String> &departmentId) {
        if (!departmentId || departmentId->view().empty())
            co_return;
        ruvia::DbFindOptions options;
        options.where = db::activeId<service::dept::DeptEntity>(departmentId->view()) &&
                        service::dept::DeptEntity::column<"status">() == "enabled";
        if (!co_await c.db().getRepository<service::dept::DeptEntity>().exists(options))
            service::common::fail(12006, "部门不存在或已禁用", 400);
    }

    ruvia::Task<bool> containsSuperadmin(ruvia::Context &c,
                                         const ruvia::Array<ruvia::String> &roleIds) {
        for (const auto &roleId : roleIds) {
            ruvia::DbFindOptions options;
            options.where =
                db::activeId<service::role::RoleEntity>(roleId.view()) &&
                service::role::RoleEntity::column<"code">() == service::role::kSuperAdminRoleCode;
            if (co_await c.db().getRepository<service::role::RoleEntity>().exists(options))
                co_return true;
        }
        co_return false;
    }

    static ruvia::Task<void> replaceRoles(ruvia::DbTransaction &tx, std::string_view userId,
                                          const ruvia::Array<ruvia::String> &roleIds,
                                          std::pmr::memory_resource *resource) {
        (void)co_await tx.getRepository<UserRoleEntity>().deleteBy(
            UserRoleEntity::column<"user_id">() == userId);
        for (const auto &roleId : roleIds) {
            UserRoleEntity binding(resource);
            binding.set<"id">(service::common::nextUuidV7());
            binding.set<"user_id">(userId);
            binding.set<"role_id">(roleId.view());
            (void)co_await tx.getRepository<UserRoleEntity>().upsert(binding, {.doNothing = true});
        }
    }
};

inline UserService &userService() { return UserService::instance(); }

} // namespace service::user
