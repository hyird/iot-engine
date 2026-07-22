#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include <ruvia/web/db/Db.h>

#include "service/common/http.h"
#include "service/modules/system/auth/auth.types.h"
#include "service/utils/jwt.h"
#include "service/utils/password.h"

namespace service::auth {

class LoginRateLimiter {
  public:
    static LoginRateLimiter& instance() {
        static LoginRateLimiter limiter;
        return limiter;
    }

    bool locked(const std::string& username) {
        std::lock_guard lock(mutex_);
        const auto found = records_.find(username);
        if (found == records_.end())
            return false;
        if (found->second.expiresAt <= std::chrono::steady_clock::now()) {
            records_.erase(found);
            return false;
        }
        return found->second.failures >= 5;
    }

    int failure(const std::string& username) {
        std::lock_guard lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        auto& record = records_[username];
        if (record.expiresAt <= now) {
            record.failures = 0;
            record.expiresAt = now + std::chrono::minutes(15);
        }
        return ++record.failures;
    }

    void clear(const std::string& username) {
        std::lock_guard lock(mutex_);
        records_.erase(username);
    }

  private:
    struct Record {
        int failures{};
        std::chrono::steady_clock::time_point expiresAt{};
    };
    std::mutex mutex_;
    std::unordered_map<std::string, Record> records_;
};

class AuthService {
  public:
    static AuthService& instance() {
        static AuthService service;
        return service;
    }

    ruvia::Task<LoginResultDto> login(ruvia::Context& c, const LoginBody& body) {
        const std::string username(body.username()->view());
        const std::string password(body.password()->view());
        if (LoginRateLimiter::instance().locked(username)) {
            service::common::fail(11003, "登录失败次数过多，请 15 分钟后再试", 429);
        }

        const auto rows = co_await c.db().query(R"sql(
SELECT id, username, password_hash, COALESCE(nickname, ''), status
FROM sys_user
WHERE username = $1 AND deleted_at IS NULL
LIMIT 1)sql",
                                                service::common::dbParams(username));

        if (rows.rows().empty() ||
            !service::utils::comparePassword(password, rows.rows().front()[2].text())) {
            const int remaining = 5 - LoginRateLimiter::instance().failure(username);
            const auto message = remaining > 0 ? "用户名或密码错误，还剩 " +
                                                     std::to_string(remaining) + " 次尝试机会"
                                               : "登录失败次数过多，请 15 分钟后再试";
            service::common::fail(11001, message, remaining > 0 ? 401 : 429);
        }

        const auto& row = rows.rows().front();
        if (row[4].text() != "enabled")
            service::common::fail(11002, "用户已被禁用", 403);
        LoginRateLimiter::instance().clear(username);

        const std::string userId(row[0].text());
        const std::string nickname(row[3].text());
        const std::string status(row[4].text());
        service::core::JwtPayload payload{userId, username};
        LoginResultDto result(c);
        result.token(service::utils::signAccessToken(c, payload))
            .refreshToken(service::utils::signRefreshToken(c, payload))
            .user(co_await buildUser(c, userId, username, nickname, status));
        co_return result;
    }

    ruvia::Task<LoginResultDto> refresh(ruvia::Context& c, const RefreshBody& body) {
        service::core::JwtPayload payload;
        try {
            payload = service::utils::verifyRefreshToken(c, body.refreshToken()->view());
        } catch (...) {
            service::common::fail(service::common::kTokenInvalidErrorCode, "刷新令牌无效", 401);
        }
        const auto rows = co_await c.db().query(R"sql(
SELECT id, username, COALESCE(nickname, ''), status
FROM sys_user WHERE id = $1 AND deleted_at IS NULL LIMIT 1)sql",
                                                service::common::dbParams(payload.userId));
        if (rows.rows().empty())
            service::common::fail(11008, "用户不存在", 404);
        const auto& row = rows.rows().front();
        if (row[3].text() != "enabled")
            service::common::fail(11002, "用户已被禁用", 403);

        const std::string userId(row[0].text());
        const std::string username(row[1].text());
        const std::string nickname(row[2].text());
        const std::string status(row[3].text());
        service::core::JwtPayload next{userId, username};
        LoginResultDto result(c);
        result.token(service::utils::signAccessToken(c, next))
            .refreshToken(service::utils::signRefreshToken(c, next))
            .user(co_await buildUser(c, userId, username, nickname, status));
        co_return result;
    }

    ruvia::Task<AuthUserInfoDto> current(ruvia::Context& c, std::string_view userId) {
        const auto rows = co_await c.db().query(R"sql(
SELECT username, COALESCE(nickname, ''), status
FROM sys_user WHERE id = $1 AND deleted_at IS NULL LIMIT 1)sql",
                                                service::common::dbParams(userId));
        if (rows.rows().empty())
            service::common::fail(11008, "用户不存在", 404);
        const auto& row = rows.rows().front();
        if (row[2].text() != "enabled")
            service::common::fail(11002, "用户已被禁用", 403);
        co_return co_await buildUser(c, userId, std::string(row[0].text()),
                                     std::string(row[1].text()), std::string(row[2].text()));
    }

  private:
    ruvia::Task<AuthUserInfoDto> buildUser(ruvia::Context& c, std::string_view userId,
                                           const std::string& username, const std::string& nickname,
                                           const std::string& status) {
        AuthUserInfoDto user(c);
        user.id(userId).username(username).nickname(nickname).status(status);

        const auto roles = co_await c.db().query(R"sql(
SELECT r.id, r.name, r.code
FROM sys_role r
JOIN sys_user_role ur ON ur.role_id = r.id
WHERE ur.user_id = $1 AND r.status = 'enabled' AND r.deleted_at IS NULL
ORDER BY r.id)sql",
                                                 service::common::dbParams(userId));
        auto& roleItems = user.rolesEnsure();
        for (const auto& row : roles.rows()) {
            auto& role = roleItems.emplace_back(c);
            role.id(row[0].text()).name(row[1].text()).code(row[2].text());
        }

        const auto permissions = co_await c.db().query(R"sql(
SELECT DISTINCT p.permission
FROM sys_role r
JOIN sys_user_role ur ON ur.role_id = r.id
CROSS JOIN LATERAL jsonb_array_elements_text(r.permissions) AS p(permission)
WHERE ur.user_id = $1 AND r.status = 'enabled' AND r.deleted_at IS NULL
ORDER BY p.permission)sql",
                                                       service::common::dbParams(userId));
        auto& permissionItems = user.permissionsEnsure();
        for (const auto& row : permissions.rows())
            permissionItems.emplace_back(row[0].text(), c.resource());
        co_return user;
    }
};

inline AuthService& authService() { return AuthService::instance(); }

} // namespace service::auth
