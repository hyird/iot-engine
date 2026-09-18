#pragma once

#include "service/common/uuid.h"

#include <string_view>

#include <ruvia/web/Validation.h>

#include "service/modules/system/role/role.types.h"

namespace service::user {

inline bool isPhoneNumber(const ruvia::String& value) {
    const auto text = value.view();
    if (text.empty())
        return true;
    if (text.size() < 7 || text.size() > 20)
        return false;
    for (const char ch : text) {
        if ((ch < '0' || ch > '9') && ch != '+' && ch != '-' && ch != ' ')
            return false;
    }
    return true;
}

RUVIA_REQUEST_MODEL(CreateUserBody,
    RUVIA_REQUIRED_FIELD(username, ruvia::String, RUVIA_MIN(2, "用户名长度需在 2 - 50 之间"), RUVIA_MAX(50, "用户名长度需在 2 - 50 之间")),
    RUVIA_REQUIRED_FIELD(password, ruvia::String, RUVIA_MIN(6, "密码长度需在 6 - 100 之间"), RUVIA_MAX(100, "密码长度需在 6 - 100 之间")),
    RUVIA_OPTIONAL_FIELD(nickname, ruvia::String, RUVIA_MAX(100, "昵称不能超过 100 个字符")),
    RUVIA_OPTIONAL_FIELD(phone, ruvia::String, RUVIA_CUSTOM("手机号格式不正确", isPhoneNumber)),
    RUVIA_OPTIONAL_FIELD(email, ruvia::String, RUVIA_EMAIL("邮箱格式不正确"), RUVIA_MAX(100, "邮箱不能超过 100 个字符")),
    RUVIA_OPTIONAL_FIELD(status, ruvia::String, RUVIA_ONE_OF("状态无效", "enabled", "disabled")),
    RUVIA_OPTIONAL_FIELD_NAME("department_id", departmentId, ruvia::String, RUVIA_CUSTOM("部门 ID 必须是 UUID", service::common::isOptionalUuidField)),
    RUVIA_REQUIRED_FIELD_NAME("role_ids", roleIds, ruvia::Array<ruvia::String>, RUVIA_MIN(1, "至少选择一个角色")));

RUVIA_REQUEST_MODEL(UpdateUserBody,
    RUVIA_OPTIONAL_FIELD(nickname, ruvia::String, RUVIA_MAX(100, "昵称不能超过 100 个字符")),
    RUVIA_OPTIONAL_FIELD(phone, ruvia::String, RUVIA_CUSTOM("手机号格式不正确", isPhoneNumber)),
    RUVIA_OPTIONAL_FIELD(email, ruvia::String, RUVIA_EMAIL("邮箱格式不正确"), RUVIA_MAX(100, "邮箱不能超过 100 个字符")),
    RUVIA_OPTIONAL_FIELD(status, ruvia::String, RUVIA_ONE_OF("状态无效", "enabled", "disabled")),
    RUVIA_OPTIONAL_FIELD(password, ruvia::String, RUVIA_MIN(6, "密码长度需在 6 - 100 之间"), RUVIA_MAX(100, "密码长度需在 6 - 100 之间")),
    RUVIA_OPTIONAL_FIELD_NAME("department_id", departmentId, ruvia::String, RUVIA_CUSTOM("部门 ID 必须是 UUID", service::common::isOptionalUuidField)),
    RUVIA_OPTIONAL_FIELD_NAME("role_ids", roleIds, ruvia::Array<ruvia::String>, RUVIA_MIN(1, "至少选择一个角色")));

RUVIA_REQUEST_MODEL(UserListQuery,
    RUVIA_OPTIONAL_FIELD(page, ruvia::Int64, RUVIA_DEFAULT(1), RUVIA_MIN(1, "page 必须大于 0")),
    RUVIA_OPTIONAL_FIELD_NAME("pageSize", pageSize, ruvia::Int64, RUVIA_DEFAULT(10), RUVIA_MIN(1, "pageSize 必须在 1 - 100 之间"), RUVIA_MAX(100, "pageSize 必须在 1 - 100 之间")),
    RUVIA_OPTIONAL_FIELD(keyword, ruvia::String),
    RUVIA_OPTIONAL_FIELD(status, ruvia::String, RUVIA_ONE_OF("状态无效", "enabled", "disabled")));

RUVIA_REQUEST_MODEL(UserOptionsQuery,
    RUVIA_OPTIONAL_FIELD(keyword, ruvia::String, RUVIA_MAX(100, "搜索关键字过长")));

RUVIA_REQUEST_MODEL(UserIdParams,
    RUVIA_REQUIRED_FIELD(id, ruvia::String, RUVIA_CUSTOM("id 必须是 UUID", service::common::isUuidField)));

RUVIA_RESPONSE_MODEL(UserOptionDto,
    RUVIA_OPTIONAL_FIELD(id, ruvia::String),
    RUVIA_OPTIONAL_FIELD(username, ruvia::String),
    RUVIA_OPTIONAL_FIELD(nickname, ruvia::String, RUVIA_OMIT_EMPTY));

RUVIA_RESPONSE_MODEL(UserItemDto,
    RUVIA_OPTIONAL_FIELD(id, ruvia::String),
    RUVIA_OPTIONAL_FIELD(username, ruvia::String),
    RUVIA_OPTIONAL_FIELD(nickname, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD(phone, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD(email, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD(status, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("department_id", departmentId, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("department_name", departmentName, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD(roles, ruvia::BoxedArray<service::role::RoleOptionDto>),
    RUVIA_OPTIONAL_FIELD_NAME("created_at", createdAt, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("updated_at", updatedAt, ruvia::String));

RUVIA_RESPONSE_MODEL(UserPageDataDto,
    RUVIA_OPTIONAL_FIELD(list, ruvia::BoxedArray<UserItemDto>),
    RUVIA_OPTIONAL_FIELD(total, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(page, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("pageSize", pageSize, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("totalPages", totalPages, ruvia::Int64));

RUVIA_RESPONSE_MODEL(UserPageResponse,
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(message, ruvia::String),
    RUVIA_OPTIONAL_FIELD(data, UserPageDataDto));

RUVIA_RESPONSE_MODEL(UserDetailResponse,
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(message, ruvia::String),
    RUVIA_OPTIONAL_FIELD(data, UserItemDto));

RUVIA_RESPONSE_MODEL(UserOptionsResponse,
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(message, ruvia::String),
    RUVIA_OPTIONAL_FIELD(data, ruvia::BoxedArray<UserOptionDto>));

} // namespace service::user
