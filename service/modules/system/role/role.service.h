#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "service/common/database.h"
#include "service/modules/system/role/role.entity.h"
#include "service/modules/system/user/user.entity.h"

#include "service/common/http.h"
#include "service/modules/system/role/role.types.h"
#include "service/common/uuid.h"

namespace service::role {

namespace db = service::common::database;

class RoleService {
  public:
    static RoleService &instance() {
        static RoleService service;
        return service;
    }

    ruvia::Task<RolePageDataDto> list(ruvia::Context &c, std::int64_t page, std::int64_t pageSize,
                                      std::optional<std::string> keyword,
                                      std::optional<std::string> status) {
        page = std::max<std::int64_t>(1, page);
        pageSize = std::clamp<std::int64_t>(pageSize, 1, 100);
        auto where = RoleEntity::column<"deleted_at">().isNull();
        if (keyword && !keyword->empty()) {
            const auto pattern = "%" + *keyword + "%";
            where = std::move(where) && (RoleEntity::column<"name">().ilike(pattern) ||
                                         RoleEntity::column<"code">().ilike(pattern));
        }
        if (status && (*status == "enabled" || *status == "disabled"))
            where = std::move(where) && RoleEntity::column<"status">() == *status;
        ruvia::DbFindOptions countOptions;
        countOptions.where = std::move(where);
        const auto total = static_cast<std::int64_t>(
            co_await c.db().getRepository<RoleEntity>().count(countOptions.where));
        auto query = roleSelect(c.pool());
        query.where(countOptions.where.expression(query))
            .orderBy(query.column("id"), ruvia::DbOrderDirection::kDesc)
            .limit(pageSize)
            .offset((page - 1) * pageSize);
        const auto rows = co_await c.db().query(query);

        ruvia::BoxedArray<RoleItemDto> roles(ruvia::ModelOptions{.resource = c.arena()});
        for (const auto &row : rows) {
            auto &role = roles.emplace(ruvia::ModelOptions{.resource = c.arena()});
            fillBase(role, row);
            role.set<"permissions">(co_await loadPermissions(c, role.get<"id">()->view()));
        }
        RolePageDataDto result(ruvia::ModelOptions{.resource = c.arena()});
        result.set<"list">(std::move(roles))
            .set<"total">(total)
            .set<"page">(page)
            .set<"pageSize">(pageSize)
            .set<"totalPages">(total == 0 ? 0 : (total + pageSize - 1) / pageSize);
        co_return result;
    }

    ruvia::Task<RoleItemDto> detail(ruvia::Context &c, std::string_view id) {
        auto query = roleSelect(c.pool());
        query.where(db::activeId<RoleEntity>(id).expression(query)).limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty())
            service::common::fail(13001, "角色不存在", 404);
        RoleItemDto role(ruvia::ModelOptions{.resource = c.arena()});
        fillBase(role, rows.front());
        role.set<"permissions">(co_await loadPermissions(c, id));
        co_return role;
    }

    ruvia::Task<ruvia::BoxedArray<RoleOptionDto>> options(ruvia::Context &c) {
        ruvia::DbFindOptions options;

        options.where = RoleEntity::column<"status">() == "enabled" &&
                        RoleEntity::column<"deleted_at">().isNull();
        options.order = {{"id"}};
        const auto rows = co_await c.db().getRepository<RoleEntity>().find(options);
        ruvia::BoxedArray<RoleOptionDto> result(ruvia::ModelOptions{.resource = c.arena()});
        for (const auto &row : rows) {
            auto &item = result.emplace(ruvia::ModelOptions{.resource = c.arena()});
            item.set<"id">(row.get<"id">())
                .set<"name">(row.get<"name">())
                .set<"code">(row.get<"code">());
        }
        co_return result;
    }

    ruvia::Task<void> create(ruvia::Context &c, const CreateRoleBody &body) {
        const std::string code(body.get<"code">()->view());
        co_await ensureCodeAvailable(c, code, std::nullopt);
        const std::string name(body.get<"name">()->view());
        const std::string description =
            body.get<"description">() ? std::string(body.get<"description">()->view()) : "";
        const std::string status =
            body.get<"status">() ? std::string(body.get<"status">()->view()) : "enabled";
        const std::string permissions = permissionsText(body.get<"permissions">());
        const auto id = service::common::nextUuidV7();
        ruvia::DbQuery query(c.pool());
        query
            .insertInto(RoleEntity::tableName(),
                        {"id", "name", "code", "description", "status", "permissions"})
            .values({query.value(id), query.value(name), query.value(code),
                     query.nullIf(query.value(description), query.value("")), query.value(status),
                     permissionArray(query, permissions)});
        (void)co_await c.db().execute(query);
    }

    ruvia::Task<void> update(ruvia::Context &c, std::string_view id, const UpdateRoleBody &body) {
        ruvia::DbFindOptions existingOptions;

        existingOptions.where = db::activeId<RoleEntity>(id);
        const auto existing = co_await c.db().getRepository<RoleEntity>().findOne(existingOptions);
        if (!existing)
            service::common::fail(13001, "角色不存在", 404);
        if (existing->get<"code">() == service::role::kSuperAdminRoleCode)
            service::common::fail(13003, "内置超级管理员角色不能修改", 400);
        if (body.get<"code">())
            co_await ensureCodeAvailable(c, std::string(body.get<"code">()->view()),
                                         std::string(id));

        ruvia::DbQuery query(c.pool());
        query.update(RoleEntity::tableName());
        bool changed = false;
        auto append = [&](std::string_view column, std::string_view value) {
            query.set(column, query.value(value));
            changed = true;
        };
        if (body.get<"name">())
            append("name", body.get<"name">()->view());
        if (body.get<"code">())
            append("code", body.get<"code">()->view());
        if (body.get<"description">())
            append("description", body.get<"description">()->view());
        if (body.get<"status">())
            append("status", body.get<"status">()->view());
        if (body.get<"permissions">()) {
            query.set("permissions",
                      permissionArray(query, permissionsText(body.get<"permissions">())));
            changed = true;
        }
        if (!changed)
            co_return;
        query.set("updated_at", query.call("now"))
            .where((RoleEntity::column<"id">() == id).expression(query));
        (void)co_await c.db().execute(query);
    }

    ruvia::Task<void> remove(ruvia::Context &c, std::string_view id) {
        ruvia::DbFindOptions options;

        options.where = db::activeId<RoleEntity>(id);
        const auto role = co_await c.db().getRepository<RoleEntity>().findOne(options);
        if (!role)
            service::common::fail(13001, "角色不存在", 404);
        if (role->get<"code">() == service::role::kSuperAdminRoleCode)
            service::common::fail(13003, "内置超级管理员角色不能删除", 400);
        ruvia::DbQuery assigned(c.pool());
        assigned.select(assigned.value(1))
            .from(service::user::UserRoleEntity::tableName(), "ur")
            .join(ruvia::DbJoinType::kInner, service::user::UserEntity::tableName(),
                  assigned.binary(assigned.column("id", "u"), ruvia::DbBinaryOperator::kEqual,
                                  assigned.column("user_id", "ur")),
                  "u")
            .where(assigned.binary(assigned.column("role_id", "ur"),
                                   ruvia::DbBinaryOperator::kEqual, assigned.value(id)))
            .andWhere(
                assigned.unary(ruvia::DbUnaryOperator::kIsNull, assigned.column("deleted_at", "u")))
            .limit(1);
        if (!(co_await c.db().query(assigned)).empty())
            service::common::fail(13004, "角色仍有用户使用，不能删除", 409);
        ruvia::DbQuery removal(c.pool());
        removal.update(RoleEntity::tableName())
            .set("deleted_at", removal.call("now"))
            .set("updated_at", removal.call("now"))
            .where((RoleEntity::column<"id">() == id).expression(removal));
        (void)co_await c.db().execute(removal);
    }

  private:
    static ruvia::DbExpression permissionArray(ruvia::DbQuery &query,
                                               std::string_view permissions) {
        using namespace ruvia;
        DbQuery aggregate(query.resource());
        aggregate.select(aggregate.aggregate("jsonb_agg", {aggregate.column("permission")}))
            .fromFunction(
                aggregate.call("unnest",
                               {aggregate.call("string_to_array",
                                               {aggregate.nullIf(aggregate.value(permissions),
                                                                 aggregate.value("")),
                                                aggregate.value(",")})}),
                "values", {.columns = {{.name = "permission"}}});
        return query.coalesce(
            {query.subquery(aggregate), query.cast(query.value("[]"), DbDataType::kJsonb)});
    }

    static ruvia::DbQuery roleSelect(std::pmr::memory_resource *resource) {
        using namespace ruvia;
        DbQuery query(resource);
        query
            .select({query.column("id"), query.column("name"), query.column("code"),
                     db::emptyText(query, "description"), query.column("status"),
                     query.call("iot_utc_timestamp", {query.column("created_at")}),
                     query.call("iot_utc_timestamp", {query.column("updated_at")})})
            .from(RoleEntity::tableName());
        return query;
    }

    template <typename Row> static void fillBase(RoleItemDto &item, const Row &row) {
        item.set<"id">(row[0].value().value_or(std::string_view{}));
        item.set<"name">(row[1].value().value_or(std::string_view{}));
        item.set<"code">(row[2].value().value_or(std::string_view{}));
        item.set<"description">(row[3].value().value_or(std::string_view{}));
        item.set<"status">(row[4].value().value_or(std::string_view{}));
        item.set<"createdAt">(row[5].value().value_or(std::string_view{}));
        item.set<"updatedAt">(row[6].value().value_or(std::string_view{}));
    }

    static std::string
    permissionsText(const std::optional<ruvia::Array<ruvia::String>> &permissions) {
        if (!permissions)
            return {};
        std::string result;
        for (const auto &permission : *permissions) {
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

    ruvia::Task<ruvia::Array<ruvia::String>> loadPermissions(ruvia::Context &c,
                                                             std::string_view id) {
        ruvia::DbQuery query(c.pool());
        query.select(query.column("permission"))
            .from(RoleEntity::tableName())
            .joinFunction(ruvia::DbJoinType::kCross,
                          query.call("jsonb_array_elements_text", {query.column("permissions")}),
                          {}, "values", {.lateral = true, .columns = {{.name = "permission"}}})
            .where((RoleEntity::column<"id">() == id).expression(query))
            .orderBy(query.column("permission"));
        const auto rows = co_await c.db().query(query);
        ruvia::Array<ruvia::String> result(c.arena());
        for (const auto &row : rows)
            result.emplace_back(row[0].value().value_or(std::string_view{}),
                                ruvia::ModelOptions{.resource = c.arena()});
        co_return result;
    }

    ruvia::Task<void> ensureCodeAvailable(ruvia::Context &c, const std::string &code,
                                          std::optional<std::string> excludedId) {
        ruvia::DbFindOptions options;
        options.where = RoleEntity::column<"code">() == code;
        if (excludedId)
            options.where = std::move(options.where) && RoleEntity::column<"id">() != *excludedId;
        if (co_await c.db().getRepository<RoleEntity>().exists(options))
            service::common::fail(13002, "角色编码已存在", 409);
    }
};

inline RoleService &roleService() { return RoleService::instance(); }

inline ruvia::Task<ruvia::BoxedArray<RoleOptionDto>> listRoleOptions(ruvia::Context &c) {
    co_return co_await roleService().options(c);
}

} // namespace service::role
