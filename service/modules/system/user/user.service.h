#pragma once

#include <memory>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "service/common/database.h"
#include "service/common/http.h"
#include "service/common/uuid.h"
#include "service/modules/system/dept/dept.entity.h"
#include "service/modules/system/role/role.entity.h"
#include "service/modules/system/role/role.types.h"
#include "service/modules/system/user/user.entity.h"
#include "service/modules/system/user/user.types.h"
#include "service/utils/password.h"

namespace service::user {

namespace db = service::common::database;

class UserService {
  public:
    static UserService& instance() {
        thread_local UserService service;
        return service;
    }

    template <typename Context>
    ruvia::Task<UserPageDataDto> list(Context& c, std::int64_t page, std::int64_t pageSize, std::optional<std::string> keyword, std::optional<std::string> status) {
        page = std::max<std::int64_t>(1, page);
        pageSize = std::clamp<std::int64_t>(pageSize, 1, 100);
        ruvia::DbFindOptions options;
        options.where = UserEntity::column<"deleted_at">().isNull();
        if (keyword && !keyword->empty()) {
            const auto pattern = "%" + *keyword + "%";
            options.where =
                std::move(options.where) && (UserEntity::column<"username">().ilike(pattern) || UserEntity::column<"nickname">().ilike(pattern) || UserEntity::column<"email">().ilike(pattern));
        }
        if (status && (*status == "enabled" || *status == "disabled")) {
            options.where = std::move(options.where) && UserEntity::column<"status">() == *status;
        }
        const auto total = static_cast<std::int64_t>(
            co_await c.db().template getRepository<UserEntity>().count(options)
        );
        auto query = userSelect(c);
        query.where(options.where)
            .orderBy("id", ruvia::DbOrderDirection::kDesc)
            .take(pageSize)
            .skip((page - 1) * pageSize);
        const auto rows = co_await query.template getMany<UserDetails>();

        ruvia::BoxedArray<UserItemDto> users(ruvia::ModelOptions{ .resource = c.arena() });
        for (const auto& row : rows) {
            auto& item = users.emplace(ruvia::ModelOptions{ .resource = c.arena() });
            fillBase(item, row);
            item.set<"roles">(co_await loadRoles(c, item.get<"id">()->view()));
        }
        UserPageDataDto result(ruvia::ModelOptions{ .resource = c.arena() });
        result.set<"list">(std::move(users))
            .template set<"total">(total)
            .template set<"page">(page)
            .template set<"pageSize">(pageSize)
            .template set<"totalPages">(total == 0 ? 0 : (total + pageSize - 1) / pageSize);
        co_return result;
    }

    template <typename Context>
    ruvia::Task<UserItemDto> detail(Context& c, std::string_view id) {
        auto query = userSelect(c);
        query.where(db::activeId<UserEntity>(id))
            .take(1);
        const auto rows = co_await query.template getMany<UserDetails>();
        if (rows.empty()) {
            service::common::fail(12001, "用户不存在", 404);
        }
        UserItemDto item(ruvia::ModelOptions{ .resource = c.arena() });
        fillBase(item, rows[0]);
        item.set<"roles">(co_await loadRoles(c, id));
        co_return item;
    }

    template <typename Context>
    ruvia::Task<ruvia::BoxedArray<UserOptionDto>> options(Context& c, std::optional<std::string> keyword) {
        auto where = UserEntity::column<"deleted_at">().isNull() &&
            UserEntity::column<"status">() == "enabled";
        if (keyword && !keyword->empty()) {
            const auto pattern = "%" + *keyword + "%";
            where = std::move(where) && (UserEntity::column<"username">().ilike(pattern) || UserEntity::column<"nickname">().ilike(pattern));
        }
        ruvia::DbFindOptions options;
        options.where = std::move(where);
        options.order = { { "username" } };
        options.take = 100;
        const auto rows = co_await c.db().template getRepository<UserEntity>().find(options);
        ruvia::BoxedArray<UserOptionDto> result(ruvia::ModelOptions{ .resource = c.arena() });
        for (const auto& row : rows) {
            auto& item = result.emplace(ruvia::ModelOptions{ .resource = c.arena() });
            item.set<"id">(row.template get<"id">())
                .template set<"username">(row.template get<"username">())
                .template set<"nickname">(row.template isNull<"nickname">() ? std::string_view{} : row.template get<"nickname">());
        }
        co_return result;
    }

    template <typename Context>
    ruvia::Task<void> create(Context& c, const CreateUserBody& body) {
        const std::string username(body.get<"username">()->view());
        ruvia::DbFindOptions exists;
        exists.where = UserEntity::column<"username">() == username &&
            UserEntity::column<"deleted_at">().isNull();
        if (co_await c.db().template getRepository<UserEntity>().exists(exists)) {
            service::common::fail(12002, "用户名已存在", 409);
        }
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
        const auto id = c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next();
        auto tx = co_await c.db().beginTransaction();
        UserEntity user(c.pool());
        user.set<"id">(id);
        user.set<"username">(username);
        user.set<"password_hash">(passwordHash);
        user.set<"status">(status);
        if (nickname.empty()) {
            user.setNull<"nickname">();
        } else {
            user.set<"nickname">(nickname);
        }
        if (phone.empty()) {
            user.setNull<"phone">();
        } else {
            user.set<"phone">(phone);
        }
        if (email.empty()) {
            user.setNull<"email">();
        } else {
            user.set<"email">(email);
        }
        if (departmentId.empty()) {
            user.setNull<"department_id">();
        } else {
            user.set<"department_id">(departmentId);
        }
        (void)co_await tx.template getRepository<UserEntity>().insert(user);
        co_await replaceRoles(tx, id, *body.get<"roleIds">(), c.pool(), *c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>());
        co_await tx.commit();
    }

    template <typename Context>
    ruvia::Task<void> update(Context& c, std::string_view id, const UpdateUserBody& body) {
        ruvia::DbFindOptions options;
        options.where = db::activeId<UserEntity>(id);
        const auto existing = co_await c.db().template getRepository<UserEntity>().findOne(options);
        if (!existing) {
            service::common::fail(12001, "用户不存在", 404);
        }
        const std::string username(existing->template get<"username">());
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
        if (body.get<"departmentId">()) {
            co_await validateDepartment(c, body.get<"departmentId">());
        }

        ruvia::DbExpressions query(c.pool());
        std::vector<ruvia::DbAssignment> changes;
        bool changed = false;
        auto append = [&](std::string_view column, std::string_view value) {
            changes.push_back({ std::string(column), query.value(value) });
            changed = true;
        };
        if (body.get<"nickname">()) {
            append("nickname", body.get<"nickname">()->view());
        }
        if (body.get<"phone">()) {
            append("phone", body.get<"phone">()->view());
        }
        if (body.get<"email">()) {
            append("email", body.get<"email">()->view());
        }
        if (body.get<"status">()) {
            append("status", body.get<"status">()->view());
        }
        if (body.get<"password">()) {
            append("password_hash", service::utils::hashPassword(body.get<"password">()->view()));
        }
        if (body.get<"departmentId">()) {
            changes.push_back({ "department_id", query.cast(query.nullIf(query.value(body.get<"departmentId">()->view()), query.value("")), ruvia::DbDataType::kUuid) });
            changed = true;
        }
        auto tx = co_await c.db().beginTransaction();
        if (changed) {
            changes.push_back({ "updated_at", query.call("now") });
            (void)co_await tx.template getRepository<UserEntity>().update(UserEntity::column<"id">() == id, changes);
        }
        if (body.get<"roleIds">()) {
            co_await replaceRoles(tx, id, *body.get<"roleIds">(), c.pool(), *c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>());
        }
        co_await tx.commit();
    }

    template <typename Context>
    ruvia::Task<void> remove(Context& c, std::string_view id, std::string_view operatorId) {
        if (id == operatorId) {
            service::common::fail(12004, "不能删除当前登录用户", 400);
        }
        ruvia::DbFindOptions options;
        options.where = db::activeId<UserEntity>(id);
        const auto user = co_await c.db().template getRepository<UserEntity>().findOne(options);
        if (!user) {
            service::common::fail(12001, "用户不存在", 404);
        }
        if (user->template get<"username">() == "admin") {
            service::common::fail(12004, "内置管理员不能删除", 400);
        }
        ruvia::DbExpressions expressions(c.pool());
        (void)co_await c.db().template getRepository<UserEntity>().update(
            UserEntity::column<"id">() == id,
            { { "deleted_at", expressions.call("now") }, { "updated_at", expressions.call("now") } }
        );
    }

  private:
    template <typename Context>
    static ruvia::DbQueryBuilder<UserEntity, ruvia::DbHandle> userSelect(Context& c) {
        using namespace ruvia;
        DbExpressions expressions(c.pool());
        auto query = c.db().template getRepository<UserEntity>().createQueryBuilder("u");
        query.select({ { "id", expressions.column("id", "u") }, { "username", expressions.column("username", "u") }, { "nickname", expressions.coalesce({ expressions.cast(expressions.column("nickname", "u"), DbDataType::kText), expressions.value("") }) }, { "phone", expressions.coalesce({ expressions.cast(expressions.column("phone", "u"), DbDataType::kText), expressions.value("") }) }, { "email", expressions.coalesce({ expressions.cast(expressions.column("email", "u"), DbDataType::kText), expressions.value("") }) }, { "status", expressions.column("status", "u") }, { "department_id", expressions.coalesce({ expressions.cast(expressions.column("department_id", "u"), DbDataType::kText), expressions.value("") }) }, { "department_name", expressions.coalesce({ expressions.cast(expressions.column("name", "d"), DbDataType::kText), expressions.value("") }) }, { "created_at", expressions.call("iot_utc_timestamp", { expressions.column("created_at", "u") }) }, { "updated_at", expressions.call("iot_utc_timestamp", { expressions.column("updated_at", "u") }) } });
        query.template join<service::dept::DeptEntity>(DbJoinType::kLeft, "d", expressions.binary(expressions.column("id", "d"), DbBinaryOperator::kEqual, expressions.column("department_id", "u")));
        return query;
    }

    static void fillBase(UserItemDto& item, const UserDetails& row) {
        item.set<"id">(row.template get<"id">());
        item.set<"username">(row.template get<"username">());
        item.set<"nickname">(row.template get<"nickname">());
        item.set<"phone">(row.template get<"phone">());
        item.set<"email">(row.template get<"email">());
        item.set<"status">(row.template get<"status">());
        item.set<"departmentId">(row.template get<"department_id">());
        item.set<"departmentName">(row.template get<"department_name">());
        item.set<"createdAt">(row.template get<"created_at">());
        item.set<"updatedAt">(row.template get<"updated_at">());
    }

    template <typename Context>
    ruvia::Task<ruvia::BoxedArray<service::role::RoleOptionDto>>
    loadRoles(Context& c, std::string_view userId) {
        ruvia::DbExpressions expressions(c.pool());
        auto query = c.db().template getRepository<service::role::RoleEntity>().createQueryBuilder("r");
        query.select({ { "id" }, { "name" }, { "code" } })
            .template join<UserRoleEntity>(ruvia::DbJoinType::kInner, "ur", expressions.binary(expressions.column("role_id", "ur"), ruvia::DbBinaryOperator::kEqual, expressions.column("id", "r")))
            .where(expressions.binary(expressions.column("user_id", "ur"), ruvia::DbBinaryOperator::kEqual, expressions.value(userId)))
            .andWhere(service::role::RoleEntity::column<"deleted_at">().isNull())
            .orderBy("id");
        const auto rows = co_await query.getMany();
        ruvia::BoxedArray<service::role::RoleOptionDto> roles(
            ruvia::ModelOptions{ .resource = c.arena() }
        );
        for (const auto& row : rows) {
            auto& role = roles.emplace(ruvia::ModelOptions{ .resource = c.arena() });
            role.set<"id">(row.template get<"id">())
                .template set<"name">(row.template get<"name">())
                .template set<"code">(row.template get<"code">());
        }
        co_return roles;
    }

    template <typename Context>
    ruvia::Task<void> validateRoles(Context& c, const ruvia::Array<ruvia::String>& roleIds) {
        std::set<std::string, std::less<>> seenRoleIds;
        for (const auto& roleId : roleIds) {
            if (!service::common::isUuid(roleId.view())) {
                service::common::fail(12005, "角色 ID 必须是 UUID", 400);
            }
            const std::string roleIdValue(roleId.view());
            if (!seenRoleIds.emplace(roleIdValue).second) {
                service::common::fail(12005, "角色不能重复", 400);
            }
            ruvia::DbFindOptions options;
            options.where = db::activeId<service::role::RoleEntity>(roleIdValue) &&
                service::role::RoleEntity::column<"status">() == "enabled";
            if (!co_await c.db().template getRepository<service::role::RoleEntity>().exists(options)) {
                service::common::fail(12005, "包含无效角色", 400);
            }
        }
    }

    template <typename Context>
    ruvia::Task<void> validateDepartment(Context& c, const std::optional<ruvia::String>& departmentId) {
        if (!departmentId || departmentId->view().empty()) {
            co_return;
        }
        ruvia::DbFindOptions options;
        options.where = db::activeId<service::dept::DeptEntity>(departmentId->view()) &&
            service::dept::DeptEntity::column<"status">() == "enabled";
        if (!co_await c.db().template getRepository<service::dept::DeptEntity>().exists(options)) {
            service::common::fail(12006, "部门不存在或已禁用", 400);
        }
    }

    template <typename Context>
    ruvia::Task<bool> containsSuperadmin(Context& c, const ruvia::Array<ruvia::String>& roleIds) {
        for (const auto& roleId : roleIds) {
            ruvia::DbFindOptions options;
            options.where =
                db::activeId<service::role::RoleEntity>(roleId.view()) &&
                service::role::RoleEntity::column<"code">() == service::role::kSuperAdminRoleCode;
            if (co_await c.db().template getRepository<service::role::RoleEntity>().exists(options)) {
                co_return true;
            }
        }
        co_return false;
    }

    static ruvia::Task<void> replaceRoles(ruvia::DbTransaction& tx, std::string_view userId, const ruvia::Array<ruvia::String>& roleIds, std::pmr::memory_resource* resource, service::common::UuidV7Generator& uuidGenerator) {
        (void)co_await tx.template getRepository<UserRoleEntity>().deleteBy(
            UserRoleEntity::column<"user_id">() == userId
        );
        for (const auto& roleId : roleIds) {
            UserRoleEntity binding(resource);
            binding.set<"id">(uuidGenerator.next());
            binding.set<"user_id">(userId);
            binding.set<"role_id">(roleId.view());
            (void)co_await tx.template getRepository<UserRoleEntity>().upsert(binding, { .doNothing = true });
        }
    }
};

inline UserService& userService() {
    return UserService::instance();
}

} // namespace service::user
