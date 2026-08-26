#pragma once

#include <cstdint>
#include <string>

#include <ruvia/web/db/Db.h>

#include "service/common/http.h"
#include "service/domains/auth/auth.types.h"
#include "service/features/collector/stream.h"
#include "service/utils/jwt.h"
#include "service/utils/password.h"

namespace service::auth {

class LoginRateLimiter {
  public:
    virtual ~LoginRateLimiter() = default;

    virtual ruvia::Task<bool> locked(ruvia::Context& context,
                                     std::string_view username) {
        const auto reply = co_await service::message::redis::command(
            context.redis(), {"GET", key(username)});
        if (reply.null())
            co_return false;
        if (reply.kind() != ruvia::RedisValue::Kind::kString)
            throw std::runtime_error("invalid login rate limit state");
        co_return service::common::parseInt64(reply.string()).value_or(0) >= 5;
    }

    virtual ruvia::Task<int> failure(ruvia::Context& context,
                                     std::string_view username) {
        static constexpr std::string_view script = R"lua(
local failures = redis.call('INCR', KEYS[1])
if failures == 1 then redis.call('PEXPIRE', KEYS[1], ARGV[1]) end
return failures
)lua";
        const auto reply = co_await service::message::redis::command(
            context.redis(), {"EVAL", std::string(script), "1", key(username), "900000"});
        if (reply.kind() != ruvia::RedisValue::Kind::kInteger)
            throw std::runtime_error("invalid login rate limit increment");
        co_return static_cast<int>(reply.integer());
    }

    virtual ruvia::Task<void> clear(ruvia::Context& context,
                                    std::string_view username) {
        (void)co_await service::message::redis::command(
            context.redis(), {"DEL", key(username)});
    }

  private:
    static std::string key(std::string_view username) {
        return "iot:auth:login-failures:" + std::string(username);
    }
};

class AuthService {
  public:
    explicit AuthService(LoginRateLimiter& limiter) : limiter_(limiter) {}

    ruvia::Task<LoginResultDto> login(ruvia::Context& c, const LoginBody& body) {
        const std::string username(body.get<"username">()->view());
        const std::string password(body.get<"password">()->view());
        if (co_await limiter_.locked(c, username)) {
            service::common::fail(11003, "登录失败次数过多，请 15 分钟后再试", 429);
        }

        const auto rows = co_await c.db().query(R"sql(
SELECT id, username, password_hash, COALESCE(nickname, ''), status
FROM sys_user
WHERE username = $1 AND deleted_at IS NULL
LIMIT 1)sql",
                                                service::common::dbParams(username));

        if (rows.empty() ||
            !service::utils::comparePassword(password, rows.front()[2].value().value_or(std::string_view{}))) {
            const int remaining = 5 - co_await limiter_.failure(c, username);
            const auto message = remaining > 0 ? "用户名或密码错误，还剩 " +
                                                     std::to_string(remaining) + " 次尝试机会"
                                               : "登录失败次数过多，请 15 分钟后再试";
            service::common::fail(11001, message, remaining > 0 ? 401 : 429);
        }

        const auto& row = rows.front();
        if (row[4].value().value_or(std::string_view{}) != "enabled")
            service::common::fail(11002, "用户已被禁用", 403);
        co_await limiter_.clear(c, username);

        const std::string userId(row[0].value().value_or(std::string_view{}));
        const std::string nickname(row[3].value().value_or(std::string_view{}));
        const std::string status(row[4].value().value_or(std::string_view{}));
        service::core::JwtPayload payload{userId, username};
        LoginResultDto result(c);
        result.set<"token">(service::utils::signAccessToken(c, payload))
            .set<"refreshToken">(service::utils::signRefreshToken(c, payload))
            .set<"user">(co_await buildUser(c, userId, username, nickname, status));
        co_return result;
    }

    ruvia::Task<LoginResultDto> refresh(ruvia::Context& c, const RefreshBody& body) {
        service::core::JwtPayload payload;
        try {
            payload = service::utils::verifyRefreshToken(c, body.get<"refreshToken">()->view());
        } catch (...) {
            service::common::fail(service::common::kTokenInvalidErrorCode, "刷新令牌无效", 401);
        }
        const auto rows = co_await c.db().query(R"sql(
SELECT id, username, COALESCE(nickname, ''), status
FROM sys_user WHERE id = $1 AND deleted_at IS NULL LIMIT 1)sql",
                                                service::common::dbParams(payload.userId));
        if (rows.empty())
            service::common::fail(11008, "用户不存在", 404);
        const auto& row = rows.front();
        if (row[3].value().value_or(std::string_view{}) != "enabled")
            service::common::fail(11002, "用户已被禁用", 403);

        const std::string userId(row[0].value().value_or(std::string_view{}));
        const std::string username(row[1].value().value_or(std::string_view{}));
        const std::string nickname(row[2].value().value_or(std::string_view{}));
        const std::string status(row[3].value().value_or(std::string_view{}));
        service::core::JwtPayload next{userId, username};
        LoginResultDto result(c);
        result.set<"token">(service::utils::signAccessToken(c, next))
            .set<"refreshToken">(service::utils::signRefreshToken(c, next))
            .set<"user">(co_await buildUser(c, userId, username, nickname, status));
        co_return result;
    }

    ruvia::Task<AuthUserInfoDto> current(ruvia::Context& c, std::string_view userId) {
        const auto rows = co_await c.db().query(R"sql(
SELECT username, COALESCE(nickname, ''), status
FROM sys_user WHERE id = $1 AND deleted_at IS NULL LIMIT 1)sql",
                                                service::common::dbParams(userId));
        if (rows.empty())
            service::common::fail(11008, "用户不存在", 404);
        const auto& row = rows.front();
        if (row[2].value().value_or(std::string_view{}) != "enabled")
            service::common::fail(11002, "用户已被禁用", 403);
        co_return co_await buildUser(c, userId, std::string(row[0].value().value_or(std::string_view{})),
                                     std::string(row[1].value().value_or(std::string_view{})), std::string(row[2].value().value_or(std::string_view{})));
    }

  private:
    ruvia::Task<AuthUserInfoDto> buildUser(ruvia::Context& c, std::string_view userId,
                                           const std::string& username, const std::string& nickname,
                                           const std::string& status) {
        AuthUserInfoDto user(c);
        user.set<"id">(userId).set<"username">(username).set<"nickname">(nickname).set<"status">(status);

        const auto roles = co_await c.db().query(R"sql(
SELECT r.id, r.name, r.code
FROM sys_role r
JOIN sys_user_role ur ON ur.role_id = r.id
WHERE ur.user_id = $1 AND r.status = 'enabled' AND r.deleted_at IS NULL
ORDER BY r.id)sql",
                                                 service::common::dbParams(userId));
        auto& roleItems = user.ensure<"roles">();
        for (const auto& row : roles) {
            auto& role = roleItems.emplace_back(c);
            role.set<"id">(row[0].value().value_or(std::string_view{})).set<"name">(row[1].value().value_or(std::string_view{})).set<"code">(row[2].value().value_or(std::string_view{}));
        }

        const auto permissions = co_await c.db().query(R"sql(
SELECT DISTINCT p.permission
FROM sys_role r
JOIN sys_user_role ur ON ur.role_id = r.id
CROSS JOIN LATERAL jsonb_array_elements_text(r.permissions) AS p(permission)
WHERE ur.user_id = $1 AND r.status = 'enabled' AND r.deleted_at IS NULL
ORDER BY p.permission)sql",
                                                       service::common::dbParams(userId));
        auto& permissionItems = user.ensure<"permissions">();
        for (const auto& row : permissions)
            permissionItems.emplace_back(row[0].value().value_or(std::string_view{}), c.resource());
        co_return user;
    }
  private:
    LoginRateLimiter& limiter_;
};

inline AuthService& authService() {
    static LoginRateLimiter limiter;
    static AuthService service(limiter);
    return service;
}

} // namespace service::auth
