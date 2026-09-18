#pragma once

#include <memory>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "service/common/database.h"
#include "service/common/http.h"
#include "service/common/uuid.h"
#include "service/modules/system/dept/dept.entity.h"
#include "service/modules/system/dept/dept.types.h"
#include "service/modules/system/user/user.entity.h"

namespace service::dept {

namespace db = service::common::database;

class DeptService {
  public:
    static DeptService& instance() {
        thread_local DeptService service;
        return service;
    }

    template <typename Context>
    ruvia::Task<DeptPageDataDto> list(Context& c, std::int64_t page, std::int64_t pageSize, std::optional<std::string> keyword, std::optional<std::string> status, std::optional<std::string> parentId) {
        page = std::max<std::int64_t>(1, page);
        pageSize = std::clamp<std::int64_t>(pageSize, 1, 100);
        ruvia::DbFindOptions options;
        options.where = DeptEntity::column<"deleted_at">().isNull();
        if (keyword && !keyword->empty()) {
            const auto pattern = "%" + *keyword + "%";
            options.where =
                std::move(options.where) && (DeptEntity::column<"name">().ilike(pattern) || DeptEntity::column<"code">().ilike(pattern));
        }
        if (status && (*status == "enabled" || *status == "disabled")) {
            options.where = std::move(options.where) && DeptEntity::column<"status">() == *status;
        }
        if (parentId) {
            options.where = std::move(options.where) &&
                (parentId->empty() ? DeptEntity::column<"parent_id">().isNull()
                                   : DeptEntity::column<"parent_id">() == *parentId);
        }
        const auto total = static_cast<std::int64_t>(
            co_await c.db().template getRepository<DeptEntity>().count(options)
        );
        auto query = departmentSelect(c);
        query.where(options.where)
            .orderBy("sort_order")
            .addOrderBy("id")
            .take(pageSize)
            .skip((page - 1) * pageSize);
        const auto rows = co_await query.template getMany<DepartmentDetails>();
        ruvia::BoxedArray<DeptItemDto> departments(ruvia::ModelOptions{ .resource = c.arena() });
        for (const auto& row : rows) {
            auto& item = departments.emplace(ruvia::ModelOptions{ .resource = c.arena() });
            fill(item, row);
        }
        DeptPageDataDto result(ruvia::ModelOptions{ .resource = c.arena() });
        result.set<"list">(std::move(departments))
            .template set<"total">(total)
            .template set<"page">(page)
            .template set<"pageSize">(pageSize)
            .template set<"totalPages">(total == 0 ? 0 : (total + pageSize - 1) / pageSize);
        co_return result;
    }

    template <typename Context>
    ruvia::Task<DeptItemDto> detail(Context& c, std::string_view id) {
        auto query = departmentSelect(c);
        query.where(db::activeId<DeptEntity>(id))
            .take(1);
        const auto rows = co_await query.template getMany<DepartmentDetails>();
        if (rows.empty()) {
            service::common::fail(14001, "部门不存在", 404);
        }
        DeptItemDto item(ruvia::ModelOptions{ .resource = c.arena() });
        fill(item, rows[0]);
        co_return item;
    }

    template <typename Context>
    ruvia::Task<ruvia::BoxedArray<DeptOptionDto>> options(Context& c) {
        ruvia::DbFindOptions options;
        options.where = DeptEntity::column<"deleted_at">().isNull();
        options.order = { { "sort_order" }, { "id" } };
        const auto rows = co_await c.db().template getRepository<DeptEntity>().find(options);
        ruvia::BoxedArray<DeptOptionDto> result(ruvia::ModelOptions{ .resource = c.arena() });
        for (const auto& row : rows) {
            auto& item = result.emplace(ruvia::ModelOptions{ .resource = c.arena() });
            item.set<"id">(row.template get<"id">())
                .template set<"name">(row.template get<"name">())
                .template set<"parentId">(row.template isNull<"parent_id">() ? std::string_view{} : std::string_view(row.template get<"parent_id">()));
        }
        co_return result;
    }

    template <typename Context>
    ruvia::Task<void> create(Context& c, const CreateDeptBody& body) {
        const std::string name(body.get<"name">().view());
        const std::string code = body.get<"code">() ? std::string(body.get<"code">()->view()) : "";
        const std::string parentId =
            body.get<"parentId">() ? std::string(body.get<"parentId">()->view()) : "";
        const std::string leaderId =
            body.get<"leaderId">() ? std::string(body.get<"leaderId">()->view()) : "";
        const auto sortOrder =
            body.get<"sortOrder">() ? static_cast<std::int64_t>(*body.get<"sortOrder">()) : 0;
        const std::string status =
            body.get<"status">() ? std::string(body.get<"status">()->view()) : "enabled";
        const auto id = c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next();
        co_await validateRelations(c, parentId, leaderId, std::nullopt);
        co_await ensureCodeAvailable(c, code, std::nullopt);
        DeptEntity department(c.pool());
        department.set<"id">(id);
        department.set<"name">(name);
        if (code.empty()) {
            department.setNull<"code">();
        } else {
            department.set<"code">(code);
        }
        if (parentId.empty()) {
            department.setNull<"parent_id">();
        } else {
            department.set<"parent_id">(parentId);
        }
        if (leaderId.empty()) {
            department.setNull<"leader_id">();
        } else {
            department.set<"leader_id">(leaderId);
        }
        department.set<"sort_order">(sortOrder);
        department.set<"status">(status);
        (void)co_await c.db().template getRepository<DeptEntity>().insert(department);
    }

    template <typename Context>
    ruvia::Task<void> update(Context& c, std::string_view id, const UpdateDeptBody& body) {
        ruvia::DbFindOptions existing;
        existing.where = db::activeId<DeptEntity>(id);
        if (!co_await c.db().template getRepository<DeptEntity>().exists(existing)) {
            service::common::fail(14001, "部门不存在", 404);
        }

        const std::string parentId =
            body.get<"parentId">() ? std::string(body.get<"parentId">()->view()) : "";
        const std::string leaderId =
            body.get<"leaderId">() ? std::string(body.get<"leaderId">()->view()) : "";
        if (body.get<"parentId">() || body.get<"leaderId">()) {
            co_await validateRelations(c, parentId, leaderId, std::string(id));
        }
        if (body.get<"code">()) {
            co_await ensureCodeAvailable(c, std::string(body.get<"code">()->view()), std::string(id));
        }

        ruvia::DbExpressions query(c.pool());
        std::vector<ruvia::DbAssignment> changes;
        bool changed = false;
        auto append = [&](std::string_view column, ruvia::DbExpression value) {
            changes.push_back({ std::string(column), value });
            changed = true;
        };
        if (body.get<"name">()) {
            append("name", query.value(body.get<"name">()->view()));
        }
        if (body.get<"code">()) {
            append("code", query.nullIf(query.value(body.get<"code">()->view()), query.value("")));
        }
        if (body.get<"parentId">()) {
            append("parent_id", query.cast(query.nullIf(query.value(parentId), query.value("")), ruvia::DbDataType::kUuid));
        }
        if (body.get<"leaderId">()) {
            append("leader_id", query.cast(query.nullIf(query.value(leaderId), query.value("")), ruvia::DbDataType::kUuid));
        }
        if (body.get<"sortOrder">()) {
            append("sort_order", query.value(static_cast<std::int64_t>(*body.get<"sortOrder">())));
        }
        if (body.get<"status">()) {
            append("status", query.value(body.get<"status">()->view()));
        }
        if (!changed) {
            co_return;
        }
        changes.push_back({ "updated_at", query.call("now") });
        (void)co_await c.db().template getRepository<DeptEntity>().update(DeptEntity::column<"id">() == id, changes);
    }

    template <typename Context>
    ruvia::Task<void> remove(Context& c, std::string_view id) {
        ruvia::DbFindOptions existing;
        existing.where = db::activeId<DeptEntity>(id);
        if (!co_await c.db().template getRepository<DeptEntity>().exists(existing)) {
            service::common::fail(14001, "部门不存在", 404);
        }
        ruvia::DbFindOptions children;
        children.where =
            DeptEntity::column<"parent_id">() == id && DeptEntity::column<"deleted_at">().isNull();
        if (co_await c.db().template getRepository<DeptEntity>().exists(children)) {
            service::common::fail(14005, "部门存在子部门，不能删除", 409);
        }
        ruvia::DbExpressions expressions(c.pool());
        (void)co_await c.db().template getRepository<DeptEntity>().update(
            DeptEntity::column<"id">() == id,
            { { "deleted_at", expressions.call("now") }, { "updated_at", expressions.call("now") } }
        );
    }

  private:
    template <typename Context>
    static ruvia::DbQueryBuilder<DeptEntity, ruvia::DbHandle>
    departmentDescendant(Context& c, std::string_view currentId, std::string_view parentId) {
        using namespace ruvia;
        DbExpressions expressions(c.pool());
        auto departments = c.db().template getRepository<DeptEntity>();
        auto seed = departments.createQueryBuilder("seed");
        seed.select({ { "id" } })
            .where(DeptEntity::column<"parent_id">() == currentId && DeptEntity::column<"deleted_at">().isNull());
        auto recursive = departments.createQueryBuilder("d");
        recursive.select({ { "id" } })
            .joinCte(DbJoinType::kInner, "descendants", "x", expressions.binary(expressions.column("parent_id", "d"), DbBinaryOperator::kEqual, expressions.column("id", "x")))
            .where(DeptEntity::column<"deleted_at">().isNull());
        seed.combine(DbSetOperation::kUnionAll, recursive);
        auto query = departments.createQueryBuilder("candidate");
        query.with("descendants", seed, { .recursive = true })
            .joinCte(DbJoinType::kInner, "descendants", "x", expressions.binary(expressions.column("id", "candidate"), DbBinaryOperator::kEqual, expressions.column("id", "x")))
            .where(DeptEntity::column<"id">() == parentId);
        return query;
    }

    template <typename Context>
    static ruvia::DbQueryBuilder<DeptEntity, ruvia::DbHandle> departmentSelect(Context& c) {
        using namespace ruvia;
        DbExpressions expressions(c.pool());
        auto query = c.db().template getRepository<DeptEntity>().createQueryBuilder("d");
        query.select({ { "id", expressions.cast(expressions.column("id", "d"), DbDataType::kText) }, { "name", expressions.column("name", "d") }, { "code", expressions.coalesce({ expressions.cast(expressions.column("code", "d"), DbDataType::kText), expressions.value("") }) }, { "parent_id", expressions.coalesce({ expressions.cast(expressions.column("parent_id", "d"), DbDataType::kText), expressions.value("") }) }, { "parent_name", expressions.coalesce({ expressions.cast(expressions.column("name", "parent"), DbDataType::kText), expressions.value("") }) }, { "leader_id", expressions.coalesce({ expressions.cast(expressions.column("leader_id", "d"), DbDataType::kText), expressions.value("") }) }, { "leader_name", expressions.coalesce({ expressions.column("nickname", "u"), expressions.column("username", "u"), expressions.value("") }) }, { "sort_order", expressions.column("sort_order", "d") }, { "status", expressions.column("status", "d") }, { "created_at", expressions.call("iot_utc_timestamp", { expressions.column("created_at", "d") }) }, { "updated_at", expressions.call("iot_utc_timestamp", { expressions.column("updated_at", "d") }) } });
        query.template join<DeptEntity>(DbJoinType::kLeft, "parent", expressions.binary(expressions.column("id", "parent"), DbBinaryOperator::kEqual, expressions.column("parent_id", "d")))
            .template join<service::user::UserEntity>(DbJoinType::kLeft, "u", expressions.binary(expressions.binary(expressions.column("id", "u"), DbBinaryOperator::kEqual, expressions.column("leader_id", "d")), DbBinaryOperator::kAnd, expressions.unary(DbUnaryOperator::kIsNull, expressions.column("deleted_at", "u"))));
        return query;
    }

    static void fill(DeptItemDto& item, const DepartmentDetails& row) {
        item.set<"id">(row.template get<"id">());
        item.set<"name">(row.template get<"name">());
        item.set<"code">(row.template get<"code">());
        item.set<"parentId">(row.template get<"parent_id">());
        item.set<"parentName">(row.template get<"parent_name">());
        item.set<"leaderId">(row.template get<"leader_id">());
        item.set<"leaderName">(row.template get<"leader_name">());
        item.set<"sortOrder">(row.template get<"sort_order">());
        item.set<"status">(row.template get<"status">());
        item.set<"createdAt">(row.template get<"created_at">());
        item.set<"updatedAt">(row.template get<"updated_at">());
    }

    template <typename Context>
    ruvia::Task<void> validateRelations(Context& c, std::string_view parentId, std::string_view leaderId, std::optional<std::string> currentId) {
        if (currentId && parentId == *currentId) {
            service::common::fail(14003, "上级部门不能是自身", 400);
        }
        if (!parentId.empty()) {
            ruvia::DbFindOptions parent;
            parent.where = db::activeId<DeptEntity>(parentId);
            if (!co_await c.db().template getRepository<DeptEntity>().exists(parent)) {
                service::common::fail(14003, "上级部门不存在", 400);
            }
            if (currentId) {
                auto descendants = departmentDescendant(c, *currentId, parentId);
                if (co_await descendants.getExists()) {
                    service::common::fail(14003, "不能将部门移动到其子部门下", 400);
                }
            }
        }
        if (!leaderId.empty()) {
            ruvia::DbFindOptions leader;
            leader.where = db::activeId<service::user::UserEntity>(leaderId) &&
                service::user::UserEntity::column<"status">() == "enabled";
            if (!co_await c.db().template getRepository<service::user::UserEntity>().exists(leader)) {
                service::common::fail(14004, "负责人不存在或已禁用", 400);
            }
        }
    }

    template <typename Context>
    ruvia::Task<void> ensureCodeAvailable(Context& c, const std::string& code, std::optional<std::string> excludedId) {
        if (code.empty()) {
            co_return;
        }
        ruvia::DbFindOptions options;
        options.where = DeptEntity::column<"code">() == code;
        if (excludedId) {
            options.where = std::move(options.where) && DeptEntity::column<"id">() != *excludedId;
        }
        if (co_await c.db().template getRepository<DeptEntity>().exists(options)) {
            service::common::fail(14002, "部门编码已存在", 409);
        }
    }
};

inline DeptService& deptService() {
    return DeptService::instance();
}

} // namespace service::dept
