#pragma once

#include <ruvia/web/Controller.h>

#include "service/modules/system/user/user.types.h"
#include "service/common/id.validation.h"

namespace service::user {

class CreateUserValidator final : public ruvia::Middleware<CreateUserValidator> {
  public:
    RUVIA_VALIDATE_JSON(CreateUserBody,
                        RUVIA_RULE(username, RUVIA_REQUIRED("用户名不能为空"),
                                   RUVIA_MIN(2, "用户名长度需在 2 - 50 之间"),
                                   RUVIA_MAX(50, "用户名长度需在 2 - 50 之间")),
                        RUVIA_RULE(password, RUVIA_REQUIRED("密码不能为空"),
                                   RUVIA_MIN(6, "密码长度需在 6 - 100 之间"),
                                   RUVIA_MAX(100, "密码长度需在 6 - 100 之间")),
                        RUVIA_RULE(nickname, RUVIA_MAX(100, "昵称不能超过 100 个字符")),
                        RUVIA_RULE(phone, RUVIA_CUSTOM("手机号格式不正确", isPhoneNumber)),
                        RUVIA_RULE(email, RUVIA_EMAIL("邮箱格式不正确"),
                                   RUVIA_MAX(100, "邮箱不能超过 100 个字符")),
                        RUVIA_RULE(status, RUVIA_ONE_OF("状态无效", "enabled", "disabled")),
                        RUVIA_RULE_NAME("role_ids", roleIds, RUVIA_REQUIRED("至少选择一个角色"),
                                        RUVIA_MIN(1, "至少选择一个角色")))
};

class UpdateUserValidator final : public ruvia::Middleware<UpdateUserValidator> {
  public:
    RUVIA_VALIDATE_JSON(UpdateUserBody,
                        RUVIA_RULE(nickname, RUVIA_MAX(100, "昵称不能超过 100 个字符")),
                        RUVIA_RULE(password, RUVIA_MIN(6, "密码长度需在 6 - 100 之间"),
                                   RUVIA_MAX(100, "密码长度需在 6 - 100 之间")),
                        RUVIA_RULE(phone, RUVIA_CUSTOM("手机号格式不正确", isPhoneNumber)),
                        RUVIA_RULE(email, RUVIA_EMAIL("邮箱格式不正确"),
                                   RUVIA_MAX(100, "邮箱不能超过 100 个字符")),
                        RUVIA_RULE(status, RUVIA_ONE_OF("状态无效", "enabled", "disabled")),
                        RUVIA_RULE_NAME("role_ids", roleIds, RUVIA_MIN(1, "至少选择一个角色")))
};

class UserListQueryValidator final : public ruvia::Middleware<UserListQueryValidator> {
  public:
    RUVIA_VALIDATE_QUERY(UserListQuery, RUVIA_RULE(page, RUVIA_MIN(1, "page 必须大于 0")),
                         RUVIA_RULE_NAME("pageSize", pageSize,
                                         RUVIA_MIN(1, "pageSize 必须在 1 - 100 之间"),
                                         RUVIA_MAX(100, "pageSize 必须在 1 - 100 之间")),
                         RUVIA_RULE(status, RUVIA_ONE_OF("状态无效", "enabled", "disabled")))
};

class UserOptionsQueryValidator final : public ruvia::Middleware<UserOptionsQueryValidator> {
  public:
    RUVIA_VALIDATE_QUERY(UserOptionsQuery, RUVIA_RULE(keyword, RUVIA_MAX(100, "搜索关键字过长")))
};

class UserIdParamsValidator final : public ruvia::Middleware<UserIdParamsValidator> {
  public:
    RUVIA_VALIDATE_PARAM(UserIdParams,
                         RUVIA_RULE(id, RUVIA_REQUIRED("id 不能为空"),
                                    RUVIA_CUSTOM("id 必须是 UUID", service::common::isUuidField)))
};

} // namespace service::user
