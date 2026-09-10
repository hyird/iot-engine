#pragma once

#include <ruvia/web/Model.h>

namespace service::dept {

RUVIA_REQUEST_MODEL(CreateDeptBody,
    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD(code, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("parent_id", parentId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("leader_id", leaderId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("sort_order", sortOrder, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(status, ruvia::String));

RUVIA_REQUEST_MODEL(UpdateDeptBody,
    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD(code, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("parent_id", parentId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("leader_id", leaderId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("sort_order", sortOrder, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(status, ruvia::String));

RUVIA_REQUEST_MODEL(DeptListQuery,
    RUVIA_OPTIONAL_FIELD(page, ruvia::Int64, RUVIA_DEFAULT(1)),
    RUVIA_OPTIONAL_FIELD_NAME("pageSize", pageSize, ruvia::Int64, RUVIA_DEFAULT(10)),
    RUVIA_OPTIONAL_FIELD(keyword, ruvia::String),
    RUVIA_OPTIONAL_FIELD(status, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("parent_id", parentId, ruvia::String));

RUVIA_REQUEST_MODEL(DeptIdParams,
    RUVIA_OPTIONAL_FIELD(id, ruvia::String));

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
