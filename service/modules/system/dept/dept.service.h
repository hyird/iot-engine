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
#include "service/modules/system/dept/dept.types.h"

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
        const auto total = std::stoll(std::string(countRows.rows().front()[0].text()));
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
            "d.created_at::text, d.updated_at::text FROM sys_department d "
            "LEFT JOIN sys_department parent ON parent.id = d.parent_id "
            "LEFT JOIN sys_user u ON u.id = d.leader_id AND u.deleted_at IS NULL" +
                where + " ORDER BY d.sort_order, d.id LIMIT $" + std::to_string(limitIndex) +
                " OFFSET $" + std::to_string(offsetIndex),
            listParams);
        ruvia::List<DeptItemDto> departments(c.resource());
        for (const auto& row : rows.rows()) {
            auto& item = departments.emplace(c);
            fill(item, row);
        }
        DeptPageDataDto result(c);
        result.list(std::move(departments))
            .total(total)
            .page(page)
            .pageSize(pageSize)
            .totalPages(total == 0 ? 0 : (total + pageSize - 1) / pageSize);
        co_return result;
    }

    ruvia::Task<DeptItemDto> detail(ruvia::Context& c, std::string_view id) {
        const auto rows = co_await c.db().query(R"sql(
SELECT d.id::text, d.name, COALESCE(d.code, ''), COALESCE(d.parent_id::text, ''),
       COALESCE(parent.name, ''), COALESCE(d.leader_id::text, ''),
       COALESCE(u.nickname, u.username, ''), d.sort_order, d.status,
       d.created_at::text, d.updated_at::text
FROM sys_department d
LEFT JOIN sys_department parent ON parent.id = d.parent_id
LEFT JOIN sys_user u ON u.id = d.leader_id AND u.deleted_at IS NULL
WHERE d.id = $1 AND d.deleted_at IS NULL LIMIT 1)sql",
                                                service::common::dbParams(id));
        if (rows.rows().empty())
            service::common::fail(14001, "部门不存在", 404);
        DeptItemDto item(c);
        fill(item, rows.rows().front());
        co_return item;
    }

    ruvia::Task<ruvia::List<DeptOptionDto>> options(ruvia::Context& c) {
        const auto rows = co_await c.db().query(
            "SELECT id::text, name, COALESCE(parent_id::text, '') FROM sys_department "
            "WHERE deleted_at IS NULL "
            "ORDER BY sort_order, id");
        ruvia::List<DeptOptionDto> result(c.resource());
        for (const auto& row : rows.rows()) {
            auto& item = result.emplace(c);
            item.id(row[0].text()).name(row[1].text()).parentId(row[2].text());
        }
        co_return result;
    }

    ruvia::Task<void> create(ruvia::Context& c, const CreateDeptBody& body) {
        const std::string name(body.name()->view());
        const std::string code = body.code() ? std::string(body.code()->view()) : "";
        const std::string parentId = body.parentId() ? std::string(body.parentId()->view()) : "";
        const std::string leaderId = body.leaderId() ? std::string(body.leaderId()->view()) : "";
        const auto sortOrder = body.sortOrder() ? static_cast<std::int64_t>(*body.sortOrder()) : 0;
        const std::string status = body.status() ? std::string(body.status()->view()) : "enabled";
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
        if (existing.rows().empty())
            service::common::fail(14001, "部门不存在", 404);

        const std::string parentId = body.parentId() ? std::string(body.parentId()->view()) : "";
        const std::string leaderId = body.leaderId() ? std::string(body.leaderId()->view()) : "";
        if (body.parentId() || body.leaderId())
            co_await validateRelations(c, parentId, leaderId, std::string(id));
        if (body.code())
            co_await ensureCodeAvailable(c, std::string(body.code()->view()), std::string(id));

        std::string set;
        std::vector<ruvia::DbValue> params;
        auto append = [&](std::string_view expression, ruvia::DbValue value) {
            if (!set.empty())
                set += ", ";
            params.emplace_back(std::move(value));
            set += std::string(expression) + "$" + std::to_string(params.size());
        };
        if (body.name())
            append("name = ", ruvia::DbValue{body.name()->view()});
        if (body.code())
            append("code = NULLIF(", ruvia::DbValue{body.code()->view()}), set += ", '')";
        if (body.parentId())
            append("parent_id = NULLIF(", ruvia::DbValue{parentId}), set += ", '')::uuid";
        if (body.leaderId())
            append("leader_id = NULLIF(", ruvia::DbValue{leaderId}), set += ", '')::uuid";
        if (body.sortOrder())
            append("sort_order = ", ruvia::DbValue{static_cast<std::int64_t>(*body.sortOrder())});
        if (body.status())
            append("status = ", ruvia::DbValue{body.status()->view()});
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
        if (existing.rows().empty())
            service::common::fail(14001, "部门不存在", 404);
        const auto children = co_await c.db().query(
            "SELECT 1 FROM sys_department WHERE parent_id = $1 AND deleted_at IS NULL LIMIT 1",
            service::common::dbParams(id));
        if (!children.rows().empty())
            service::common::fail(14005, "部门存在子部门，不能删除", 409);
        (void)co_await c.db().execute(
            "UPDATE sys_department SET deleted_at = NOW(), updated_at = NOW() WHERE id = $1",
            service::common::dbParams(id));
    }

  private:
    template <typename Row> static void fill(DeptItemDto& item, const Row& row) {
        item.id(row[0].text())
            .name(row[1].text())
            .code(row[2].text())
            .parentId(row[3].text())
            .parentName(row[4].text())
            .leaderId(row[5].text())
            .leaderName(row[6].text())
            .sortOrder(static_cast<ruvia::Int64>(std::stoll(std::string(row[7].text()))))
            .status(row[8].text())
            .createdAt(row[9].text())
            .updatedAt(row[10].text());
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
            if (parent.rows().empty())
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
                if (!cycle.rows().empty())
                    service::common::fail(14003, "不能将部门移动到其子部门下", 400);
            }
        }
        if (!leaderId.empty()) {
            const auto leader =
                co_await c.db().query("SELECT 1 FROM sys_user WHERE id = $1 AND status = 'enabled' "
                                      "AND deleted_at IS NULL LIMIT 1",
                                      service::common::dbParams(leaderId));
            if (leader.rows().empty())
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
        if (!rows.rows().empty())
            service::common::fail(14002, "部门编码已存在", 409);
    }
};

inline DeptService& deptService() { return DeptService::instance(); }

} // namespace service::dept
