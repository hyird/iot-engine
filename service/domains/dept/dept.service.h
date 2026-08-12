#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/web/db/Db.h>

#include "service/common/http.h"
#include "service/common/uuid.h"
#include "service/domains/dept/dept.types.h"

namespace service::dept {

class DeptService {
  public:
    static DeptService& instance() {
        static DeptService service;
        return service;
    }

    ruvia::Task<DeptPageDataDto> list(ruvia::Context& c, std::int64_t page, std::int64_t pageSize,
                                      std::optional<std::string> keyword,
                                      std::optional<std::string> status,
                                      std::optional<std::string> parentId) {
        page = std::max<std::int64_t>(1, page);
        pageSize = std::clamp<std::int64_t>(pageSize, 1, 100);
        std::string where = " WHERE d.deleted_at IS NULL";
        std::vector<ruvia::DbValue> params;
        std::optional<std::string> pattern;
        if (keyword && !keyword->empty()) {
            pattern = "%" + *keyword + "%";
            params.emplace_back(*pattern);
            where += " AND (d.name ILIKE $1 OR COALESCE(d.code, '') ILIKE $1)";
        }
        if (status && (*status == "enabled" || *status == "disabled")) {
            params.emplace_back(*status);
            where += " AND d.status = $" + std::to_string(params.size());
        }
        if (parentId) {
            if (parentId->empty()) {
                where += " AND d.parent_id IS NULL";
            } else {
                params.emplace_back(*parentId);
                where += " AND d.parent_id = $" + std::to_string(params.size());
            }
        }

        const auto countRows =
            co_await c.db().query("SELECT COUNT(*) FROM sys_department d" + where, params);
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
            "SELECT d.id::text, d.name, COALESCE(d.code, ''), "
            "COALESCE(d.parent_id::text, ''), COALESCE(parent.name, ''), "
            "COALESCE(d.leader_id::text, ''), "
            "COALESCE(u.nickname, u.username, ''), d.sort_order, d.status, "
            "iot_utc_timestamp(d.created_at), iot_utc_timestamp(d.updated_at) "
            "FROM sys_department d "
            "LEFT JOIN sys_department parent ON parent.id = d.parent_id "
            "LEFT JOIN sys_user u ON u.id = d.leader_id AND u.deleted_at IS NULL" +
                where + " ORDER BY d.sort_order, d.id LIMIT $" + std::to_string(limitIndex) +
                " OFFSET $" + std::to_string(offsetIndex),
            listParams);
        ruvia::BoxedArray<DeptItemDto> departments(c.resource());
        for (const auto& row : rows) {
            auto& item = departments.emplace(c);
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

    ruvia::Task<DeptItemDto> detail(ruvia::Context& c, std::string_view id) {
        const auto rows = co_await c.db().query(R"sql(
SELECT d.id::text, d.name, COALESCE(d.code, ''), COALESCE(d.parent_id::text, ''),
       COALESCE(parent.name, ''), COALESCE(d.leader_id::text, ''),
       COALESCE(u.nickname, u.username, ''), d.sort_order, d.status,
       iot_utc_timestamp(d.created_at), iot_utc_timestamp(d.updated_at)
FROM sys_department d
LEFT JOIN sys_department parent ON parent.id = d.parent_id
LEFT JOIN sys_user u ON u.id = d.leader_id AND u.deleted_at IS NULL
WHERE d.id = $1 AND d.deleted_at IS NULL LIMIT 1)sql",
                                                service::common::dbParams(id));
        if (rows.empty())
            service::common::fail(14001, "部门不存在", 404);
        DeptItemDto item(c);
        fill(item, rows.front());
        co_return item;
    }

    ruvia::Task<ruvia::BoxedArray<DeptOptionDto>> options(ruvia::Context& c) {
        const auto rows = co_await c.db().query(
            "SELECT id::text, name, COALESCE(parent_id::text, '') FROM sys_department "
            "WHERE deleted_at IS NULL "
            "ORDER BY sort_order, id");
        ruvia::BoxedArray<DeptOptionDto> result(c.resource());
        for (const auto& row : rows) {
            auto& item = result.emplace(c);
            item.set<"id">(row[0].value().value_or(std::string_view{})).set<"name">(row[1].value().value_or(std::string_view{})).set<"parentId">(row[2].value().value_or(std::string_view{}));
        }
        co_return result;
    }

    ruvia::Task<void> create(ruvia::Context& c, const CreateDeptBody& body) {
        const std::string name(body.get<"name">()->view());
        const std::string code = body.get<"code">() ? std::string(body.get<"code">()->view()) : "";
        const std::string parentId = body.get<"parentId">() ? std::string(body.get<"parentId">()->view()) : "";
        const std::string leaderId = body.get<"leaderId">() ? std::string(body.get<"leaderId">()->view()) : "";
        const auto sortOrder = body.get<"sortOrder">() ? static_cast<std::int64_t>(*body.get<"sortOrder">()) : 0;
        const std::string status = body.get<"status">() ? std::string(body.get<"status">()->view()) : "enabled";
        const auto id = service::common::nextUuidV7();
        co_await validateRelations(c, parentId, leaderId, std::nullopt);
        co_await ensureCodeAvailable(c, code, std::nullopt);
        (void)co_await c.db().execute(
            R"sql(
INSERT INTO sys_department(id, name, code, parent_id, leader_id, sort_order, status)
VALUES ($1, $2, NULLIF($3, ''), NULLIF($4, '')::uuid, NULLIF($5, '')::uuid, $6, $7)
)sql",
            service::common::dbParams(id, name, code, parentId, leaderId, sortOrder, status));
    }

    ruvia::Task<void> update(ruvia::Context& c, std::string_view id, const UpdateDeptBody& body) {
        const auto existing = co_await c.db().query(
            "SELECT 1 FROM sys_department WHERE id = $1 AND deleted_at IS NULL LIMIT 1",
            service::common::dbParams(id));
        if (existing.empty())
            service::common::fail(14001, "部门不存在", 404);

        const std::string parentId = body.get<"parentId">() ? std::string(body.get<"parentId">()->view()) : "";
        const std::string leaderId = body.get<"leaderId">() ? std::string(body.get<"leaderId">()->view()) : "";
        if (body.get<"parentId">() || body.get<"leaderId">())
            co_await validateRelations(c, parentId, leaderId, std::string(id));
        if (body.get<"code">())
            co_await ensureCodeAvailable(c, std::string(body.get<"code">()->view()), std::string(id));

        std::string set;
        std::vector<ruvia::DbValue> params;
        auto append = [&](std::string_view expression, ruvia::DbValue value) {
            if (!set.empty())
                set += ", ";
            params.emplace_back(std::move(value));
            set += std::string(expression) + "$" + std::to_string(params.size());
        };
        if (body.get<"name">())
            append("name = ", ruvia::DbValue{body.get<"name">()->view()});
        if (body.get<"code">())
            append("code = NULLIF(", ruvia::DbValue{body.get<"code">()->view()}), set += ", '')";
        if (body.get<"parentId">())
            append("parent_id = NULLIF(", ruvia::DbValue{parentId}), set += ", '')::uuid";
        if (body.get<"leaderId">())
            append("leader_id = NULLIF(", ruvia::DbValue{leaderId}), set += ", '')::uuid";
        if (body.get<"sortOrder">())
            append("sort_order = ", ruvia::DbValue{static_cast<std::int64_t>(*body.get<"sortOrder">())});
        if (body.get<"status">())
            append("status = ", ruvia::DbValue{body.get<"status">()->view()});
        if (set.empty())
            co_return;
        params.emplace_back(id);
        (void)co_await c.db().execute("UPDATE sys_department SET " + set +
                                          ", updated_at = NOW() WHERE id = $" +
                                          std::to_string(params.size()),
                                      params);
    }

    ruvia::Task<void> remove(ruvia::Context& c, std::string_view id) {
        const auto existing = co_await c.db().query(
            "SELECT 1 FROM sys_department WHERE id = $1 AND deleted_at IS NULL LIMIT 1",
            service::common::dbParams(id));
        if (existing.empty())
            service::common::fail(14001, "部门不存在", 404);
        const auto children = co_await c.db().query(
            "SELECT 1 FROM sys_department WHERE parent_id = $1 AND deleted_at IS NULL LIMIT 1",
            service::common::dbParams(id));
        if (!children.empty())
            service::common::fail(14005, "部门存在子部门，不能删除", 409);
        (void)co_await c.db().execute(
            "UPDATE sys_department SET deleted_at = NOW(), updated_at = NOW() WHERE id = $1",
            service::common::dbParams(id));
    }

  private:
    template <typename Row> static void fill(DeptItemDto& item, const Row& row) {
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

    ruvia::Task<void> validateRelations(ruvia::Context& c, std::string_view parentId,
                                        std::string_view leaderId,
                                        std::optional<std::string> currentId) {
        if (currentId && parentId == *currentId)
            service::common::fail(14003, "上级部门不能是自身", 400);
        if (!parentId.empty()) {
            const auto parent = co_await c.db().query(
                "SELECT 1 FROM sys_department WHERE id = $1 AND deleted_at IS NULL LIMIT 1",
                service::common::dbParams(parentId));
            if (parent.empty())
                service::common::fail(14003, "上级部门不存在", 400);
            if (currentId) {
                const auto cycle =
                    co_await c.db().query(R"sql(
WITH RECURSIVE descendants AS (
    SELECT id FROM sys_department WHERE parent_id = $1 AND deleted_at IS NULL
    UNION ALL
    SELECT d.id FROM sys_department d JOIN descendants x ON d.parent_id = x.id
    WHERE d.deleted_at IS NULL
)
SELECT 1 FROM descendants WHERE id = $2 LIMIT 1)sql",
                                          service::common::dbParams(*currentId, parentId));
                if (!cycle.empty())
                    service::common::fail(14003, "不能将部门移动到其子部门下", 400);
            }
        }
        if (!leaderId.empty()) {
            const auto leader =
                co_await c.db().query("SELECT 1 FROM sys_user WHERE id = $1 AND status = 'enabled' "
                                      "AND deleted_at IS NULL LIMIT 1",
                                      service::common::dbParams(leaderId));
            if (leader.empty())
                service::common::fail(14004, "负责人不存在或已禁用", 400);
        }
    }

    ruvia::Task<void> ensureCodeAvailable(ruvia::Context& c, const std::string& code,
                                          std::optional<std::string> excludedId) {
        if (code.empty())
            co_return;
        auto sql = std::string("SELECT 1 FROM sys_department WHERE code = $1");
        auto params = service::common::dbParams(code);
        if (excludedId) {
            sql += " AND id <> $2";
            params.emplace_back(*excludedId);
        }
        sql += " LIMIT 1";
        const auto rows = co_await c.db().query(sql, params);
        if (!rows.empty())
            service::common::fail(14002, "部门编码已存在", 409);
    }
};

inline DeptService& deptService() { return DeptService::instance(); }

} // namespace service::dept
