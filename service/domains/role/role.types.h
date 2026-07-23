#pragma once

#include <ruvia/web/Model.h>

namespace service::role {

struct CreateRoleBody final {
    RUVIA_OPTIONAL_FIELD(name, ruvia::String);
    RUVIA_OPTIONAL_FIELD(code, ruvia::String);
    RUVIA_OPTIONAL_FIELD(description, ruvia::String);
    RUVIA_OPTIONAL_FIELD(status, ruvia::String);
    RUVIA_OPTIONAL_FIELD(permissions, ruvia::Array<ruvia::String>);
    RUVIA_MODEL(CreateRoleBody, name, code, description, status, permissions);
};

struct UpdateRoleBody final {
    RUVIA_OPTIONAL_FIELD(name, ruvia::String);
    RUVIA_OPTIONAL_FIELD(code, ruvia::String);
    RUVIA_OPTIONAL_FIELD(description, ruvia::String);
    RUVIA_OPTIONAL_FIELD(status, ruvia::String);
    RUVIA_OPTIONAL_FIELD(permissions, ruvia::Array<ruvia::String>);
    RUVIA_MODEL(UpdateRoleBody, name, code, description, status, permissions);
};

struct RoleListQuery final {
    RUVIA_OPTIONAL_FIELD(page, ruvia::Int64, RUVIA_DEFAULT(1));
    RUVIA_OPTIONAL_FIELD_NAME("pageSize", pageSize, ruvia::Int64, RUVIA_DEFAULT(10));
    RUVIA_OPTIONAL_FIELD(keyword, ruvia::String);
    RUVIA_OPTIONAL_FIELD(status, ruvia::String);
    RUVIA_MODEL(RoleListQuery, page, pageSize, keyword, status);
};

struct RoleIdParams final {
    RUVIA_OPTIONAL_FIELD(id, ruvia::String);
    RUVIA_MODEL(RoleIdParams, id);
};

struct RoleOptionDto final {
    RUVIA_OPTIONAL_FIELD(id, ruvia::String);
    RUVIA_OPTIONAL_FIELD(name, ruvia::String);
    RUVIA_OPTIONAL_FIELD(code, ruvia::String);
    RUVIA_MODEL(RoleOptionDto, id, name, code);
};

struct RoleItemDto final {
    RUVIA_OPTIONAL_FIELD(id, ruvia::String);
    RUVIA_OPTIONAL_FIELD(name, ruvia::String);
    RUVIA_OPTIONAL_FIELD(code, ruvia::String);
    RUVIA_OPTIONAL_FIELD(description, ruvia::String, RUVIA_OMIT_EMPTY);
    RUVIA_OPTIONAL_FIELD(status, ruvia::String);
    RUVIA_OPTIONAL_FIELD(permissions, ruvia::Array<ruvia::String>);
    RUVIA_OPTIONAL_FIELD_NAME("created_at", createdAt, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("updated_at", updatedAt, ruvia::String);
    RUVIA_MODEL(RoleItemDto, id, name, code, description, status, permissions, createdAt, updatedAt);
};

struct RolePageDataDto final {
    RUVIA_OPTIONAL_FIELD(list, ruvia::List<RoleItemDto>);
    RUVIA_OPTIONAL_FIELD(total, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(page, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD_NAME("pageSize", pageSize, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD_NAME("totalPages", totalPages, ruvia::Int64);
    RUVIA_MODEL(RolePageDataDto, list, total, page, pageSize, totalPages);
};

struct RolePageResponse final {
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(message, ruvia::String);
    RUVIA_OPTIONAL_FIELD(data, RolePageDataDto);
    RUVIA_MODEL(RolePageResponse, code, message, data);
};

struct RoleDetailResponse final {
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(message, ruvia::String);
    RUVIA_OPTIONAL_FIELD(data, RoleItemDto);
    RUVIA_MODEL(RoleDetailResponse, code, message, data);
};

struct RoleOptionsResponse final {
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(message, ruvia::String);
    RUVIA_OPTIONAL_FIELD(data, ruvia::List<RoleOptionDto>);
    RUVIA_MODEL(RoleOptionsResponse, code, message, data);
};

} // namespace service::role
