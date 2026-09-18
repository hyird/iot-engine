#pragma once

#include "service/common/uuid.h"

#include <ruvia/web/Validation.h>

namespace service::dept {

RUVIA_REQUEST_MODEL(CreateDeptBody,
    RUVIA_REQUIRED_FIELD(name, ruvia::String, RUVIA_MIN(2, "部门名称长度需在 2 - 128 之间"), RUVIA_MAX(128, "部门名称长度需在 2 - 128 之间")),
    RUVIA_OPTIONAL_FIELD(code, ruvia::String, RUVIA_MAX(64, "部门编码不能超过 64 个字符")),
    RUVIA_OPTIONAL_FIELD_NAME("parent_id", parentId, ruvia::String, RUVIA_CUSTOM("上级部门必须是 UUID", service::common::isOptionalUuidField)),
    RUVIA_OPTIONAL_FIELD_NAME("leader_id", leaderId, ruvia::String, RUVIA_CUSTOM("负责人必须是 UUID", service::common::isOptionalUuidField)),
    RUVIA_OPTIONAL_FIELD_NAME("sort_order", sortOrder, ruvia::Int64, RUVIA_MIN(0, "排序不能小于 0")),
    RUVIA_OPTIONAL_FIELD(status, ruvia::String, RUVIA_ONE_OF("状态无效", "enabled", "disabled")));

RUVIA_REQUEST_MODEL(UpdateDeptBody,
    RUVIA_OPTIONAL_FIELD(name, ruvia::String, RUVIA_MIN(2, "部门名称长度需在 2 - 128 之间"), RUVIA_MAX(128, "部门名称长度需在 2 - 128 之间")),
    RUVIA_OPTIONAL_FIELD(code, ruvia::String, RUVIA_MAX(64, "部门编码不能超过 64 个字符")),
    RUVIA_OPTIONAL_FIELD_NAME("parent_id", parentId, ruvia::String, RUVIA_CUSTOM("上级部门必须是 UUID", service::common::isOptionalUuidField)),
    RUVIA_OPTIONAL_FIELD_NAME("leader_id", leaderId, ruvia::String, RUVIA_CUSTOM("负责人必须是 UUID", service::common::isOptionalUuidField)),
    RUVIA_OPTIONAL_FIELD_NAME("sort_order", sortOrder, ruvia::Int64, RUVIA_MIN(0, "排序不能小于 0")),
    RUVIA_OPTIONAL_FIELD(status, ruvia::String, RUVIA_ONE_OF("状态无效", "enabled", "disabled")));

RUVIA_REQUEST_MODEL(DeptListQuery,
    RUVIA_OPTIONAL_FIELD(page, ruvia::Int64, RUVIA_DEFAULT(1), RUVIA_MIN(1, "page 必须大于 0")),
    RUVIA_OPTIONAL_FIELD_NAME("pageSize", pageSize, ruvia::Int64, RUVIA_DEFAULT(10), RUVIA_MIN(1, "pageSize 必须在 1 - 100 之间"), RUVIA_MAX(100, "pageSize 必须在 1 - 100 之间")),
    RUVIA_OPTIONAL_FIELD(keyword, ruvia::String),
    RUVIA_OPTIONAL_FIELD(status, ruvia::String, RUVIA_ONE_OF("状态无效", "enabled", "disabled")),
    RUVIA_OPTIONAL_FIELD_NAME("parent_id", parentId, ruvia::String, RUVIA_CUSTOM("上级部门必须是 UUID", service::common::isOptionalUuidField)));

RUVIA_REQUEST_MODEL(DeptIdParams,
    RUVIA_REQUIRED_FIELD(id, ruvia::String, RUVIA_CUSTOM("id 必须是 UUID", service::common::isUuidField)));

RUVIA_RESPONSE_MODEL(DeptOptionDto,
    RUVIA_OPTIONAL_FIELD(id, ruvia::String),
    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("parent_id", parentId, ruvia::String));

RUVIA_RESPONSE_MODEL(DeptItemDto,
    RUVIA_OPTIONAL_FIELD(id, ruvia::String),
    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD(code, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("parent_id", parentId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("parent_name", parentName, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("leader_id", leaderId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("leader_name", leaderName, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("sort_order", sortOrder, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(status, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("created_at", createdAt, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("updated_at", updatedAt, ruvia::String));

RUVIA_RESPONSE_MODEL(DeptPageDataDto,
    RUVIA_OPTIONAL_FIELD(list, ruvia::BoxedArray<DeptItemDto>),
    RUVIA_OPTIONAL_FIELD(total, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(page, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("pageSize", pageSize, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("totalPages", totalPages, ruvia::Int64));

RUVIA_RESPONSE_MODEL(DeptPageResponse,
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(message, ruvia::String),
    RUVIA_OPTIONAL_FIELD(data, DeptPageDataDto));

RUVIA_RESPONSE_MODEL(DeptDetailResponse,
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(message, ruvia::String),
    RUVIA_OPTIONAL_FIELD(data, DeptItemDto));

RUVIA_RESPONSE_MODEL(DeptOptionsResponse,
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(message, ruvia::String),
    RUVIA_OPTIONAL_FIELD(data, ruvia::BoxedArray<DeptOptionDto>));

} // namespace service::dept
