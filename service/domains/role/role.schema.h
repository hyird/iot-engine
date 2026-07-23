#pragma once

#include <ruvia/web/Controller.h>

#include "service/domains/role/role.types.h"
#include "service/common/id.validation.h"

namespace service::role {

class CreateRoleValidator final : public ruvia::Middleware<CreateRoleValidator> {
  public:
    RUVIA_VALIDATE_JSON(CreateRoleBody,
                        RUVIA_RULE(name, RUVIA_REQUIRED("角色名称不能为空"),
                                   RUVIA_MIN(2, "角色名称长度需在 2 - 128 之间"),
                                   RUVIA_MAX(128, "角色名称长度需在 2 - 128 之间")),
                        RUVIA_RULE(code, RUVIA_REQUIRED("角色编码不能为空"),
                                   RUVIA_MIN(2, "角色编码长度需在 2 - 64 之间"),
                                   RUVIA_MAX(64, "角色编码长度需在 2 - 64 之间")),
                        RUVIA_RULE(description, RUVIA_MAX(500, "角色描述不能超过 500 个字符")),
                        RUVIA_RULE(status, RUVIA_ONE_OF("状态无效", "enabled", "disabled")))
};

class UpdateRoleValidator final : public ruvia::Middleware<UpdateRoleValidator> {
  public:
    RUVIA_VALIDATE_JSON(UpdateRoleBody,
                        RUVIA_RULE(name, RUVIA_MIN(2, "角色名称长度需在 2 - 128 之间"),
                                   RUVIA_MAX(128, "角色名称长度需在 2 - 128 之间")),
                        RUVIA_RULE(code, RUVIA_MIN(2, "角色编码长度需在 2 - 64 之间"),
                                   RUVIA_MAX(64, "角色编码长度需在 2 - 64 之间")),
                        RUVIA_RULE(description, RUVIA_MAX(500, "角色描述不能超过 500 个字符")),
                        RUVIA_RULE(status, RUVIA_ONE_OF("状态无效", "enabled", "disabled")))
};

class RoleListQueryValidator final : public ruvia::Middleware<RoleListQueryValidator> {
  public:
    RUVIA_VALIDATE_QUERY(RoleListQuery, RUVIA_RULE(page, RUVIA_MIN(1, "page 必须大于 0")),
                         RUVIA_RULE_NAME("pageSize", pageSize,
                                         RUVIA_MIN(1, "pageSize 必须在 1 - 100 之间"),
                                         RUVIA_MAX(100, "pageSize 必须在 1 - 100 之间")),
                         RUVIA_RULE(status, RUVIA_ONE_OF("状态无效", "enabled", "disabled")))
};

class RoleIdParamsValidator final : public ruvia::Middleware<RoleIdParamsValidator> {
  public:
    RUVIA_VALIDATE_PARAM(RoleIdParams,
                         RUVIA_RULE(id, RUVIA_REQUIRED("id 不能为空"),
                                    RUVIA_CUSTOM("id 必须是 UUID", service::common::isUuidField)))
};

} // namespace service::role
