#pragma once

#include <string_view>

#include <ruvia/web/db/Db.h>

#include "service/common/http.h"
#include "service/middleware/auth.h"

namespace service::middleware {

inline ruvia::Task<void> requirePermission(ruvia::Context& c, std::string_view permission) {
    const auto principal = requireAuth(c);
    const auto rows =
        co_await c.db().query(R"sql(
SELECT EXISTS (
    SELECT 1
    FROM sys_user_role ur
    JOIN sys_role r ON r.id = ur.role_id
    WHERE ur.user_id = $1
      AND r.deleted_at IS NULL
      AND r.status = 'enabled'
      AND (r.code = 'superadmin' OR r.permissions ? '*' OR r.permissions ? $2)
))sql",
                              service::common::dbParams(principal.userId, permission));
    const bool allowed = !rows.rows().empty() && !rows.rows().front().empty() &&
                         rows.rows().front()[0].text() == "t";
    if (!allowed)
        service::common::fail(service::common::kPermissionDeniedErrorCode, "无权限", 403);
}

} // namespace service::middleware
