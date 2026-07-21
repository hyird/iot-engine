#pragma once

#include <ruvia/web/Model.h>

namespace service::role {

RUVIA_REQUEST_MODEL(CreateRoleBody, RUVIA_FIELD(name, ruvia::String),
                    RUVIA_FIELD(code, ruvia::String), RUVIA_FIELD(description, ruvia::String),
                    RUVIA_FIELD(status, ruvia::String),
                    RUVIA_FIELD(permissions, ruvia::Array<ruvia::String>));

RUVIA_REQUEST_MODEL(UpdateRoleBody, RUVIA_FIELD(name, ruvia::String),
                    RUVIA_FIELD(code, ruvia::String), RUVIA_FIELD(description, ruvia::String),
                    RUVIA_FIELD(status, ruvia::String),
                    RUVIA_FIELD(permissions, ruvia::Array<ruvia::String>));

RUVIA_REQUEST_MODEL(RoleListQuery, RUVIA_FIELD(page, ruvia::Int64, RUVIA_DEFAULT(1)),
                    RUVIA_FIELD_NAME("pageSize", pageSize, ruvia::Int64, RUVIA_DEFAULT(10)),
                    RUVIA_FIELD(keyword, ruvia::String), RUVIA_FIELD(status, ruvia::String));

RUVIA_REQUEST_MODEL(RoleIdParams, RUVIA_FIELD(id, ruvia::String));

RUVIA_RESPONSE_MODEL(RoleOptionDto, RUVIA_FIELD(id, ruvia::String),
                     RUVIA_FIELD(name, ruvia::String), RUVIA_FIELD(code, ruvia::String));

RUVIA_RESPONSE_MODEL(RoleItemDto, RUVIA_FIELD(id, ruvia::String), RUVIA_FIELD(name, ruvia::String),
                     RUVIA_FIELD(code, ruvia::String),
                     RUVIA_FIELD(description, ruvia::String, RUVIA_OMIT_EMPTY),
                     RUVIA_FIELD(status, ruvia::String),
                     RUVIA_FIELD(permissions, ruvia::Array<ruvia::String>),
                     RUVIA_FIELD_NAME("created_at", createdAt, ruvia::String),
                     RUVIA_FIELD_NAME("updated_at", updatedAt, ruvia::String));

RUVIA_RESPONSE_MODEL(RolePageDataDto, RUVIA_FIELD(list, ruvia::List<RoleItemDto>),
                     RUVIA_FIELD(total, ruvia::Int64), RUVIA_FIELD(page, ruvia::Int64),
                     RUVIA_FIELD_NAME("pageSize", pageSize, ruvia::Int64),
                     RUVIA_FIELD_NAME("totalPages", totalPages, ruvia::Int64));

RUVIA_RESPONSE_MODEL(RolePageResponse, RUVIA_FIELD(code, ruvia::Int64),
                     RUVIA_FIELD(message, ruvia::String), RUVIA_FIELD(data, RolePageDataDto));

RUVIA_RESPONSE_MODEL(RoleDetailResponse, RUVIA_FIELD(code, ruvia::Int64),
                     RUVIA_FIELD(message, ruvia::String), RUVIA_FIELD(data, RoleItemDto));

RUVIA_RESPONSE_MODEL(RoleOptionsResponse, RUVIA_FIELD(code, ruvia::Int64),
                     RUVIA_FIELD(message, ruvia::String),
                     RUVIA_FIELD(data, ruvia::List<RoleOptionDto>));

} // namespace service::role
