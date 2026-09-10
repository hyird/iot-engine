#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "service/common/database.h"
#include "service/modules/system/dept/dept.entity.h"
#include "service/modules/system/user/user.entity.h"

#include "service/common/http.h"
#include "service/common/uuid.h"
#include "service/modules/system/dept/dept.types.h"

namespace service::dept {

namespace db = service::common::database;

class DeptService {
  public:
    static DeptService &instance() {
        static DeptService service;
        return service;
    }

    ruvia::Task<DeptPageDataDto> list(ruvia::Context &c, std::int64_t page, std::int64_t pageSize,
                                      std::optional<std::string> keyword,
                                      std::optional<std::string> status,
                                      std::optional<std::string> parentId) {
        page = std::max<std::int64_t>(1, page);
        pageSize = std::clamp<std::int64_t>(pageSize, 1, 100);
        ruvia::DbFindOptions options;
        options.where = DeptEntity::column<"deleted_at">().isNull();
        if (keyword && !keyword->empty()) {
            const auto pattern = "%" + *keyword + "%";
            options.where =
                std::move(options.where) && (DeptEntity::column<"name">().ilike(pattern) ||
                                             DeptEntity::column<"code">().ilike(pattern));
        }
        if (status && (*status == "enabled" || *status == "disabled"))
            options.where = std::move(options.where) && DeptEntity::column<"status">() == *status;
        if (parentId) {
            options.where = std::move(options.where) &&
                            (parentId->empty() ? DeptEntity::column<"parent_id">().isNull()
                                               : DeptEntity::column<"parent_id">() == *parentId);
        }
        const auto total = static_cast<std::int64_t>(
            co_await c.db().getRepository<DeptEntity>().count(options.where));
        auto query = departmentSelect(c.operationResource());
        query.where(options.where.expression(query, DeptEntity::tableName(), "d"))
            .orderBy(query.column("sort_order", "d"))
            .addOrderBy(query.column("id", "d"))
            .limit(pageSize)
            .offset((page - 1) * pageSize);
        const auto rows = co_await c.db().query(query);
        ruvia::BoxedArray<DeptItemDto> departments(ruvia::ModelOptions{.resource = c.resource()});
        for (const auto &row : rows) {
            auto &item = departments.emplace(c);
            fill(item, row);
        }
        DeptPageDataDto result(c);
        result.set<"list">(std::move(departments))
            .set<"total">(total)
            .set<"page">(page)
            .set<"pageSize">(pageSize)
            .set<"totalPages">(total == 0 ? 0 : (total + pageSize - 1) / pageSize);
        co_return result;
    }

    ruvia::Task<DeptItemDto> detail(ruvia::Context &c, std::string_view id) {
        auto query = departmentSelect(c.operationResource());
        query.where(db::activeId<DeptEntity>(id).expression(query, DeptEntity::tableName(), "d"))
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty())
            service::common::fail(14001, "部门不存在", 404);
        DeptItemDto item(c);
        fill(item, rows.front());
        co_return item;
    }

    ruvia::Task<ruvia::BoxedArray<DeptOptionDto>> options(ruvia::Context &c) {
        ruvia::DbFindOptions options;
        options.where = DeptEntity::column<"deleted_at">().isNull();
        options.order = {{"sort_order"}, {"id"}};
        const auto rows = co_await c.db().getRepository<DeptEntity>().find(options);
        ruvia::BoxedArray<DeptOptionDto> result(ruvia::ModelOptions{.resource = c.resource()});
        for (const auto &row : rows) {
            auto &item = result.emplace(c);
            item.set<"id">(row.get<"id">())
                .set<"name">(row.get<"name">())
                .set<"parentId">(row.isNull<"parent_id">()
                                     ? std::string_view{}
                                     : std::string_view(row.get<"parent_id">()));
        }
        co_return result;
    }

    ruvia::Task<void> create(ruvia::Context &c, const CreateDeptBody &body) {
        const std::string name(body.get<"name">()->view());
        const std::string code = body.get<"code">() ? std::string(body.get<"code">()->view()) : "";
        const std::string parentId =
            body.get<"parentId">() ? std::string(body.get<"parentId">()->view()) : "";
        const std::string leaderId =
            body.get<"leaderId">() ? std::string(body.get<"leaderId">()->view()) : "";
        const auto sortOrder =
            body.get<"sortOrder">() ? static_cast<std::int64_t>(*body.get<"sortOrder">()) : 0;
        const std::string status =
            body.get<"status">() ? std::string(body.get<"status">()->view()) : "enabled";
        const auto id = service::common::nextUuidV7();
        co_await validateRelations(c, parentId, leaderId, std::nullopt);
        co_await ensureCodeAvailable(c, code, std::nullopt);
        DeptEntity department(c.operationResource());
        department.set<"id">(id);
        department.set<"name">(name);
        if (code.empty())
            department.setNull<"code">();
        else
            department.set<"code">(code);
        if (parentId.empty())
            department.setNull<"parent_id">();
        else
            department.set<"parent_id">(parentId);
        if (leaderId.empty())
            department.setNull<"leader_id">();
        else
            department.set<"leader_id">(leaderId);
        department.set<"sort_order">(sortOrder);
        department.set<"status">(status);
        (void)co_await c.db().getRepository<DeptEntity>().insert(department);
    }

    ruvia::Task<void> update(ruvia::Context &c, std::string_view id, const UpdateDeptBody &body) {
        ruvia::DbFindOptions existing;
        existing.where = db::activeId<DeptEntity>(id);
        if (!co_await c.db().getRepository<DeptEntity>().exists(existing))
            service::common::fail(14001, "部门不存在", 404);

        const std::string parentId =
            body.get<"parentId">() ? std::string(body.get<"parentId">()->view()) : "";
        const std::string leaderId =
            body.get<"leaderId">() ? std::string(body.get<"leaderId">()->view()) : "";
        if (body.get<"parentId">() || body.get<"leaderId">())
            co_await validateRelations(c, parentId, leaderId, std::string(id));
        if (body.get<"code">())
            co_await ensureCodeAvailable(c, std::string(body.get<"code">()->view()),
                                         std::string(id));

        ruvia::DbQuery query(c.operationResource());
        query.update(DeptEntity::tableName());
        bool changed = false;
        auto append = [&](std::string_view column, ruvia::DbExpression value) {
            query.set(column, value);
            changed = true;
        };
        if (body.get<"name">())
            append("name", query.value(body.get<"name">()->view()));
        if (body.get<"code">())
            append("code", query.nullIf(query.value(body.get<"code">()->view()), query.value("")));
        if (body.get<"parentId">())
            append("parent_id", db::nullableUuid(query, parentId));
        if (body.get<"leaderId">())
            append("leader_id", db::nullableUuid(query, leaderId));
        if (body.get<"sortOrder">())
            append("sort_order", query.value(static_cast<std::int64_t>(*body.get<"sortOrder">())));
        if (body.get<"status">())
            append("status", query.value(body.get<"status">()->view()));
        if (!changed)
            co_return;
        query.set("updated_at", query.call("now"))
            .where((DeptEntity::column<"id">() == id).expression(query));
        (void)co_await c.db().execute(query);
    }

    ruvia::Task<void> remove(ruvia::Context &c, std::string_view id) {
        ruvia::DbFindOptions existing;
        existing.where = db::activeId<DeptEntity>(id);
        if (!co_await c.db().getRepository<DeptEntity>().exists(existing))
            service::common::fail(14001, "部门不存在", 404);
        ruvia::DbFindOptions children;
        children.where =
            DeptEntity::column<"parent_id">() == id && DeptEntity::column<"deleted_at">().isNull();
        if (co_await c.db().getRepository<DeptEntity>().exists(children))
            service::common::fail(14005, "部门存在子部门，不能删除", 409);
        ruvia::DbQuery removal(c.operationResource());
        removal.update(DeptEntity::tableName())
            .set("deleted_at", removal.call("now"))
            .set("updated_at", removal.call("now"))
            .where((DeptEntity::column<"id">() == id).expression(removal));
        (void)co_await c.db().execute(removal);
    }

  private:
    static ruvia::DbQuery departmentDescendant(std::string_view currentId,
                                               std::string_view parentId,
                                               std::pmr::memory_resource *resource) {
        using namespace ruvia;
        DbQuery seed(resource);
        seed.select(seed.column("id"))
            .from(DeptEntity::tableName())
            .where((DeptEntity::column<"parent_id">() == currentId &&
                    DeptEntity::column<"deleted_at">().isNull())
                       .expression(seed));
        DbQuery recursive(resource);
        recursive.select(recursive.column("id", "d"))
            .from(DeptEntity::tableName(), "d")
            .join(DbJoinType::kInner, "descendants",
                  recursive.binary(recursive.column("parent_id", "d"), DbBinaryOperator::kEqual,
                                   recursive.column("id", "x")),
                  "x")
            .where(recursive.unary(DbUnaryOperator::kIsNull, recursive.column("deleted_at", "d")));
        seed.combine(DbSetOperation::kUnionAll, recursive);
        DbQuery query(resource);
        query.with("descendants", seed, {.recursive = true})
            .select(query.value(1))
            .from("descendants")
            .where(
                query.binary(query.column("id"), DbBinaryOperator::kEqual, query.value(parentId)))
            .limit(1);
        return query;
    }

    static ruvia::DbQuery departmentSelect(std::pmr::memory_resource *resource) {
        using namespace ruvia;
        DbQuery query(resource);
        query
            .select({query.cast(query.column("id", "d"), DbDataType::kText),
                     query.column("name", "d"), db::emptyText(query, "code", "d"),
                     db::emptyText(query, "parent_id", "d"), db::emptyText(query, "name", "parent"),
                     db::emptyText(query, "leader_id", "d"),
                     query.coalesce({query.column("nickname", "u"), query.column("username", "u"),
                                     query.value("")}),
                     query.column("sort_order", "d"), query.column("status", "d"),
                     query.call("iot_utc_timestamp", {query.column("created_at", "d")}),
                     query.call("iot_utc_timestamp", {query.column("updated_at", "d")})})
            .from(DeptEntity::tableName(), "d")
            .join(DbJoinType::kLeft, DeptEntity::tableName(),
                  query.binary(query.column("id", "parent"), DbBinaryOperator::kEqual,
                               query.column("parent_id", "d")),
                  "parent")
            .join(DbJoinType::kLeft, service::user::UserEntity::tableName(),
                  query.binary(
                      query.binary(query.column("id", "u"), DbBinaryOperator::kEqual,
                                   query.column("leader_id", "d")),
                      DbBinaryOperator::kAnd,
                      query.unary(DbUnaryOperator::kIsNull, query.column("deleted_at", "u"))),
                  "u");
        return query;
    }

    template <typename Row> static void fill(DeptItemDto &item, const Row &row) {
        item.set<"id">(row[0].value().value_or(std::string_view{}));
        item.set<"name">(row[1].value().value_or(std::string_view{}));
        item.set<"code">(row[2].value().value_or(std::string_view{}));
        item.set<"parentId">(row[3].value().value_or(std::string_view{}));
        item.set<"parentName">(row[4].value().value_or(std::string_view{}));
        item.set<"leaderId">(row[5].value().value_or(std::string_view{}));
        item.set<"leaderName">(row[6].value().value_or(std::string_view{}));
        item.set<"sortOrder">(static_cast<ruvia::Int64>(
            service::common::parseInt64(
                std::optional<std::string_view>{row[7].value().value_or(std::string_view{})})
                .value_or(0)));
        item.set<"status">(row[8].value().value_or(std::string_view{}));
        item.set<"createdAt">(row[9].value().value_or(std::string_view{}));
        item.set<"updatedAt">(row[10].value().value_or(std::string_view{}));
    }

    ruvia::Task<void> validateRelations(ruvia::Context &c, std::string_view parentId,
                                        std::string_view leaderId,
                                        std::optional<std::string> currentId) {
        if (currentId && parentId == *currentId)
            service::common::fail(14003, "上级部门不能是自身", 400);
        if (!parentId.empty()) {
            ruvia::DbFindOptions parent;
            parent.where = db::activeId<DeptEntity>(parentId);
            if (!co_await c.db().getRepository<DeptEntity>().exists(parent))
                service::common::fail(14003, "上级部门不存在", 400);
            if (currentId) {
                const auto cycle = co_await c.db().query(
                    departmentDescendant(*currentId, parentId, c.operationResource()));
                if (!cycle.empty())
                    service::common::fail(14003, "不能将部门移动到其子部门下", 400);
            }
        }
        if (!leaderId.empty()) {
            ruvia::DbFindOptions leader;
            leader.where = db::activeId<service::user::UserEntity>(leaderId) &&
                           service::user::UserEntity::column<"status">() == "enabled";
            if (!co_await c.db().getRepository<service::user::UserEntity>().exists(leader))
                service::common::fail(14004, "负责人不存在或已禁用", 400);
        }
    }

    ruvia::Task<void> ensureCodeAvailable(ruvia::Context &c, const std::string &code,
                                          std::optional<std::string> excludedId) {
        if (code.empty())
            co_return;
        ruvia::DbFindOptions options;
        options.where = DeptEntity::column<"code">() == code;
        if (excludedId)
            options.where = std::move(options.where) && DeptEntity::column<"id">() != *excludedId;
        if (co_await c.db().getRepository<DeptEntity>().exists(options))
            service::common::fail(14002, "部门编码已存在", 409);
    }
};

inline DeptService &deptService() { return DeptService::instance(); }

} // namespace service::dept
