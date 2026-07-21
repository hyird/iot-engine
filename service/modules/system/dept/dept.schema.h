#pragma once

#include <ruvia/web/Controller.h>

#include "service/modules/system/dept/dept.types.h"
#include "service/common/id.validation.h"

namespace service::dept {

class CreateDeptValidator final : public ruvia::Middleware<CreateDeptValidator> {
  public:
    RUVIA_VALIDATE_JSON(CreateDeptBody,
                        RUVIA_RULE(name, RUVIA_REQUIRED("部门名称不能为空"),
                                   RUVIA_MIN(2, "部门名称长度需在 2 - 128 之间"),
                                   RUVIA_MAX(128, "部门名称长度需在 2 - 128 之间")),
                        RUVIA_RULE(code, RUVIA_MAX(64, "部门编码不能超过 64 个字符")),
                        RUVIA_RULE(parentId, RUVIA_CUSTOM("上级部门必须是 UUID",
                                                          service::common::isOptionalUuidField)),
                        RUVIA_RULE(leaderId, RUVIA_CUSTOM("负责人必须是 UUID",
                                                          service::common::isOptionalUuidField)),
                        RUVIA_RULE(sortOrder, RUVIA_MIN(0, "排序不能小于 0")),
                        RUVIA_RULE(status, RUVIA_ONE_OF("状态无效", "enabled", "disabled")))
};

class UpdateDeptValidator final : public ruvia::Middleware<UpdateDeptValidator> {
  public:
    RUVIA_VALIDATE_JSON(UpdateDeptBody,
                        RUVIA_RULE(name, RUVIA_MIN(2, "部门名称长度需在 2 - 128 之间"),
                                   RUVIA_MAX(128, "部门名称长度需在 2 - 128 之间")),
                        RUVIA_RULE(code, RUVIA_MAX(64, "部门编码不能超过 64 个字符")),
                        RUVIA_RULE(parentId, RUVIA_CUSTOM("上级部门必须是 UUID",
                                                          service::common::isOptionalUuidField)),
                        RUVIA_RULE(leaderId, RUVIA_CUSTOM("负责人必须是 UUID",
                                                          service::common::isOptionalUuidField)),
                        RUVIA_RULE(sortOrder, RUVIA_MIN(0, "排序不能小于 0")),
                        RUVIA_RULE(status, RUVIA_ONE_OF("状态无效", "enabled", "disabled")))
};

class DeptListQueryValidator final : public ruvia::Middleware<DeptListQueryValidator> {
  public:
    RUVIA_VALIDATE_QUERY(
        DeptListQuery, RUVIA_RULE(page, RUVIA_MIN(1, "page 必须大于 0")),
        RUVIA_RULE_NAME("pageSize", pageSize, RUVIA_MIN(1, "pageSize 必须在 1 - 100 之间"),
                        RUVIA_MAX(100, "pageSize 必须在 1 - 100 之间")),
        RUVIA_RULE(status, RUVIA_ONE_OF("状态无效", "enabled", "disabled")),
        RUVIA_RULE_NAME("parent_id", parentId,
                        RUVIA_CUSTOM("上级部门必须是 UUID", service::common::isOptionalUuidField)))
};

class DeptIdParamsValidator final : public ruvia::Middleware<DeptIdParamsValidator> {
  public:
    RUVIA_VALIDATE_PARAM(DeptIdParams,
                         RUVIA_RULE(id, RUVIA_REQUIRED("id 不能为空"),
                                    RUVIA_CUSTOM("id 必须是 UUID", service::common::isUuidField)))
};

} // namespace service::dept
