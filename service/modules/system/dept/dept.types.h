#pragma once

#include <ruvia/web/Model.h>

namespace service::dept {

RUVIA_REQUEST_MODEL(CreateDeptBody, RUVIA_FIELD(name, ruvia::String),
                    RUVIA_FIELD(code, ruvia::String),
                    RUVIA_FIELD_NAME("parent_id", parentId, ruvia::Int64),
                    RUVIA_FIELD_NAME("leader_id", leaderId, ruvia::Int64),
                    RUVIA_FIELD_NAME("sort_order", sortOrder, ruvia::Int64),
                    RUVIA_FIELD(status, ruvia::String));

RUVIA_REQUEST_MODEL(UpdateDeptBody, RUVIA_FIELD(name, ruvia::String),
                    RUVIA_FIELD(code, ruvia::String),
                    RUVIA_FIELD_NAME("parent_id", parentId, ruvia::Int64),
                    RUVIA_FIELD_NAME("leader_id", leaderId, ruvia::Int64),
                    RUVIA_FIELD_NAME("sort_order", sortOrder, ruvia::Int64),
                    RUVIA_FIELD(status, ruvia::String));

RUVIA_REQUEST_MODEL(DeptListQuery, RUVIA_FIELD(page, ruvia::Int64, RUVIA_DEFAULT(1)),
                    RUVIA_FIELD_NAME("pageSize", pageSize, ruvia::Int64, RUVIA_DEFAULT(10)),
                    RUVIA_FIELD(keyword, ruvia::String), RUVIA_FIELD(status, ruvia::String),
                    RUVIA_FIELD_NAME("parent_id", parentId, ruvia::Int64));

RUVIA_REQUEST_MODEL(DeptIdParams, RUVIA_FIELD(id, ruvia::Int64));

RUVIA_RESPONSE_MODEL(DeptOptionDto, RUVIA_FIELD(id, ruvia::Int64), RUVIA_FIELD(name, ruvia::String),
                     RUVIA_FIELD_NAME("parent_id", parentId, ruvia::Int64));

RUVIA_RESPONSE_MODEL(DeptItemDto, RUVIA_FIELD(id, ruvia::Int64), RUVIA_FIELD(name, ruvia::String),
                     RUVIA_FIELD(code, ruvia::String, RUVIA_OMIT_EMPTY),
                     RUVIA_FIELD_NAME("parent_id", parentId, ruvia::Int64),
                     RUVIA_FIELD_NAME("parent_name", parentName, ruvia::String, RUVIA_OMIT_EMPTY),
                     RUVIA_FIELD_NAME("leader_id", leaderId, ruvia::Int64),
                     RUVIA_FIELD_NAME("leader_name", leaderName, ruvia::String, RUVIA_OMIT_EMPTY),
                     RUVIA_FIELD_NAME("sort_order", sortOrder, ruvia::Int64),
                     RUVIA_FIELD(status, ruvia::String),
                     RUVIA_FIELD_NAME("created_at", createdAt, ruvia::String),
                     RUVIA_FIELD_NAME("updated_at", updatedAt, ruvia::String));

RUVIA_RESPONSE_MODEL(DeptPageDataDto, RUVIA_FIELD(list, ruvia::List<DeptItemDto>),
                     RUVIA_FIELD(total, ruvia::Int64), RUVIA_FIELD(page, ruvia::Int64),
                     RUVIA_FIELD_NAME("pageSize", pageSize, ruvia::Int64),
                     RUVIA_FIELD_NAME("totalPages", totalPages, ruvia::Int64));

RUVIA_RESPONSE_MODEL(DeptPageResponse, RUVIA_FIELD(code, ruvia::Int64),
                     RUVIA_FIELD(message, ruvia::String), RUVIA_FIELD(data, DeptPageDataDto));

RUVIA_RESPONSE_MODEL(DeptDetailResponse, RUVIA_FIELD(code, ruvia::Int64),
                     RUVIA_FIELD(message, ruvia::String), RUVIA_FIELD(data, DeptItemDto));

RUVIA_RESPONSE_MODEL(DeptOptionsResponse, RUVIA_FIELD(code, ruvia::Int64),
                     RUVIA_FIELD(message, ruvia::String),
                     RUVIA_FIELD(data, ruvia::List<DeptOptionDto>));

} // namespace service::dept
