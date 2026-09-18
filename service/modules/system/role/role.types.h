#pragma once

#include "service/common/uuid.h"

#include <ruvia/web/Validation.h>

namespace service::role {

inline constexpr std::string_view kSuperAdminRoleCode{"superadmin"};

RUVIA_REQUEST_MODEL(CreateRoleBody,
    RUVIA_REQUIRED_FIELD(name, ruvia::String, RUVIA_MIN(2, "角色名称长度需在 2 - 128 之间"), RUVIA_MAX(128, "角色名称长度需在 2 - 128 之间")),
    RUVIA_REQUIRED_FIELD(code, ruvia::String, RUVIA_MIN(2, "角色编码长度需在 2 - 64 之间"), RUVIA_MAX(64, "角色编码长度需在 2 - 64 之间")),
    RUVIA_OPTIONAL_FIELD(description, ruvia::String, RUVIA_MAX(500, "角色描述不能超过 500 个字符")),
    RUVIA_OPTIONAL_FIELD(status, ruvia::String, RUVIA_ONE_OF("状态无效", "enabled", "disabled")),
    RUVIA_OPTIONAL_FIELD(permissions, ruvia::Array<ruvia::String>));

RUVIA_REQUEST_MODEL(UpdateRoleBody,
    RUVIA_OPTIONAL_FIELD(name, ruvia::String, RUVIA_MIN(2, "角色名称长度需在 2 - 128 之间"), RUVIA_MAX(128, "角色名称长度需在 2 - 128 之间")),
    RUVIA_OPTIONAL_FIELD(code, ruvia::String, RUVIA_MIN(2, "角色编码长度需在 2 - 64 之间"), RUVIA_MAX(64, "角色编码长度需在 2 - 64 之间")),
    RUVIA_OPTIONAL_FIELD(description, ruvia::String, RUVIA_MAX(500, "角色描述不能超过 500 个字符")),
    RUVIA_OPTIONAL_FIELD(status, ruvia::String, RUVIA_ONE_OF("状态无效", "enabled", "disabled")),
    RUVIA_OPTIONAL_FIELD(permissions, ruvia::Array<ruvia::String>));

RUVIA_REQUEST_MODEL(RoleListQuery,
    RUVIA_OPTIONAL_FIELD(page, ruvia::Int64, RUVIA_DEFAULT(1), RUVIA_MIN(1, "page 必须大于 0")),
    RUVIA_OPTIONAL_FIELD_NAME("pageSize", pageSize, ruvia::Int64, RUVIA_DEFAULT(10), RUVIA_MIN(1, "pageSize 必须在 1 - 100 之间"), RUVIA_MAX(100, "pageSize 必须在 1 - 100 之间")),
    RUVIA_OPTIONAL_FIELD(keyword, ruvia::String),
    RUVIA_OPTIONAL_FIELD(status, ruvia::String, RUVIA_ONE_OF("状态无效", "enabled", "disabled")));

RUVIA_REQUEST_MODEL(RoleIdParams,
    RUVIA_REQUIRED_FIELD(id, ruvia::String, RUVIA_CUSTOM("id 必须是 UUID", service::common::isUuidField)));

RUVIA_RESPONSE_MODEL(RoleOptionDto,
    RUVIA_OPTIONAL_FIELD(id, ruvia::String),
    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD(code, ruvia::String));

RUVIA_RESPONSE_MODEL(RoleItemDto,
    RUVIA_OPTIONAL_FIELD(id, ruvia::String),
    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD(code, ruvia::String),
    RUVIA_OPTIONAL_FIELD(description, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD(status, ruvia::String),
    RUVIA_OPTIONAL_FIELD(permissions, ruvia::Array<ruvia::String>),
    RUVIA_OPTIONAL_FIELD_NAME("created_at", createdAt, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("updated_at", updatedAt, ruvia::String));

RUVIA_RESPONSE_MODEL(RolePageDataDto,
    RUVIA_OPTIONAL_FIELD(list, ruvia::BoxedArray<RoleItemDto>),
    RUVIA_OPTIONAL_FIELD(total, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(page, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("pageSize", pageSize, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("totalPages", totalPages, ruvia::Int64));

RUVIA_RESPONSE_MODEL(RolePageResponse,
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(message, ruvia::String),
    RUVIA_OPTIONAL_FIELD(data, RolePageDataDto));

RUVIA_RESPONSE_MODEL(RoleDetailResponse,
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(message, ruvia::String),
    RUVIA_OPTIONAL_FIELD(data, RoleItemDto));

RUVIA_RESPONSE_MODEL(RoleOptionsResponse,
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(message, ruvia::String),
    RUVIA_OPTIONAL_FIELD(data, ruvia::BoxedArray<RoleOptionDto>));

} // namespace service::role
