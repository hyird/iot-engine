#pragma once

#include <ruvia/web/Model.h>

namespace service::dept {

struct CreateDeptBody final {
    RUVIA_OPTIONAL_FIELD(name, ruvia::String);
    RUVIA_OPTIONAL_FIELD(code, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("parent_id", parentId, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("leader_id", leaderId, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("sort_order", sortOrder, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(status, ruvia::String);
    RUVIA_MODEL(CreateDeptBody, name, code, parentId, leaderId, sortOrder, status);
};

struct UpdateDeptBody final {
    RUVIA_OPTIONAL_FIELD(name, ruvia::String);
    RUVIA_OPTIONAL_FIELD(code, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("parent_id", parentId, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("leader_id", leaderId, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("sort_order", sortOrder, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(status, ruvia::String);
    RUVIA_MODEL(UpdateDeptBody, name, code, parentId, leaderId, sortOrder, status);
};

struct DeptListQuery final {
    RUVIA_OPTIONAL_FIELD(page, ruvia::Int64, RUVIA_DEFAULT(1));
    RUVIA_OPTIONAL_FIELD_NAME("pageSize", pageSize, ruvia::Int64, RUVIA_DEFAULT(10));
    RUVIA_OPTIONAL_FIELD(keyword, ruvia::String);
    RUVIA_OPTIONAL_FIELD(status, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("parent_id", parentId, ruvia::String);
    RUVIA_MODEL(DeptListQuery, page, pageSize, keyword, status, parentId);
};

struct DeptIdParams final {
    RUVIA_OPTIONAL_FIELD(id, ruvia::String);
    RUVIA_MODEL(DeptIdParams, id);
};

struct DeptOptionDto final {
    RUVIA_OPTIONAL_FIELD(id, ruvia::String);
    RUVIA_OPTIONAL_FIELD(name, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("parent_id", parentId, ruvia::String);
    RUVIA_MODEL(DeptOptionDto, id, name, parentId);
};

struct DeptItemDto final {
    RUVIA_OPTIONAL_FIELD(id, ruvia::String);
    RUVIA_OPTIONAL_FIELD(name, ruvia::String);
    RUVIA_OPTIONAL_FIELD(code, ruvia::String, RUVIA_OMIT_EMPTY);
    RUVIA_OPTIONAL_FIELD_NAME("parent_id", parentId, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("parent_name", parentName, ruvia::String, RUVIA_OMIT_EMPTY);
    RUVIA_OPTIONAL_FIELD_NAME("leader_id", leaderId, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("leader_name", leaderName, ruvia::String, RUVIA_OMIT_EMPTY);
    RUVIA_OPTIONAL_FIELD_NAME("sort_order", sortOrder, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(status, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("created_at", createdAt, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("updated_at", updatedAt, ruvia::String);
    RUVIA_MODEL(DeptItemDto, id, name, code, parentId, parentName, leaderId, leaderName, sortOrder,
                status, createdAt, updatedAt);
};

struct DeptPageDataDto final {
    RUVIA_OPTIONAL_FIELD(list, ruvia::List<DeptItemDto>);
    RUVIA_OPTIONAL_FIELD(total, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(page, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD_NAME("pageSize", pageSize, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD_NAME("totalPages", totalPages, ruvia::Int64);
    RUVIA_MODEL(DeptPageDataDto, list, total, page, pageSize, totalPages);
};

struct DeptPageResponse final {
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(message, ruvia::String);
    RUVIA_OPTIONAL_FIELD(data, DeptPageDataDto);
    RUVIA_MODEL(DeptPageResponse, code, message, data);
};

struct DeptDetailResponse final {
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(message, ruvia::String);
    RUVIA_OPTIONAL_FIELD(data, DeptItemDto);
    RUVIA_MODEL(DeptDetailResponse, code, message, data);
};

struct DeptOptionsResponse final {
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(message, ruvia::String);
    RUVIA_OPTIONAL_FIELD(data, ruvia::List<DeptOptionDto>);
    RUVIA_MODEL(DeptOptionsResponse, code, message, data);
};

} // namespace service::dept
