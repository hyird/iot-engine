#pragma once

#include <string>

#include <ruvia/web/db/Db.h>

#include "service/modules/system/role/role.types.h"

namespace service::role {

inline ruvia::Task<ruvia::List<RoleOptionDto>> listRoleOptions(ruvia::Context& c) {
    const auto rows = co_await c.db().query("SELECT id, name, code FROM sys_role WHERE status = "
                                            "'enabled' AND deleted_at IS NULL ORDER BY id");
    ruvia::List<RoleOptionDto> result(c.resource());
    for (const auto& row : rows.rows()) {
        auto& item = result.emplace(c);
        item.id(static_cast<ruvia::Int64>(std::stoll(std::string(row[0].text()))))
            .name(row[1].text())
            .code(row[2].text());
    }
    co_return result;
}

} // namespace service::role
