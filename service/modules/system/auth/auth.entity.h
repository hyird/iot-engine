#pragma once

#include <string>
#include <string_view>

namespace service::auth {

// 首次失败设置过期时间，后续原子递增不延长窗口；Redis ORM 不支持
// INCR 与条件 PEXPIRE 的原子组合，使用此映射维护同一条计数记录。
struct LoginFailureCounter final {
    static constexpr int limit = 5;
    static constexpr std::string_view windowMilliseconds = "900000";

    static std::string key(std::string_view username) {
        return "iot:auth:login-failures:" + std::string(username);
    }
};

} // namespace service::auth
