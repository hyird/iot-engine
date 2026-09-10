#pragma once

#include <cstdint>
#include <string>

#include "service/common/database.h"
#include "service/modules/system/user/user.entity.h"
#include "service/modules/system/role/role.entity.h"

#include "service/common/http.h"
#include "service/modules/system/auth/auth.types.h"
#include "service/utils/redis.h"
#include "service/utils/jwt.h"
#include "service/utils/password.h"

namespace service::auth {

class LoginRateLimiter {
  public:
    virtual ~LoginRateLimiter() = default;

    virtual ruvia::Task<bool> locked(ruvia::Context& context, std::string_view username) {
        const auto reply =
            co_await service::message::redis::command(context.redis(), {"GET", key(username)});
        if (reply.null())
            co_return false;
        if (reply.kind() != ruvia::RedisValue::Kind::kString)
            throw std::runtime_error("invalid login rate limit state");
        co_return service::common::parseInt64(reply.string()).value_or(0) >= 5;
    }

    virtual ruvia::Task<int> failure(ruvia::Context& context, std::string_view username) {
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

    virtual ruvia::Task<void> clear(ruvia::Context& context, std::string_view username) {
        (void)co_await service::message::redis::command(context.redis(), {"DEL", key(username)});
    }

  private:
    static std::string key(std::string_view username) {
        return "iot:auth:login-failures:" + std::string(username);
    }
};

class AuthService {
  public:
    explicit AuthService(LoginRateLimiter& limiter) : limiter_(limiter) {}

    static ruvia::Task<void> requirePermission(ruvia::Context& c,
                                              std::string_view userId,
                                              std::string_view permission) {
        auto query = enabledRoles(userId, c.operationResource());
        query.select(query.value(1))
            .join(ruvia::DbJoinType::kInner, service::user::UserEntity::tableName(),
                  query.binary(query.column("id", "u"), ruvia::DbBinaryOperator::kEqual,
                               query.column("user_id", "ur")), "u")
            .andWhere((service::user::UserEntity::column<"status">() == "enabled" &&
                       service::user::UserEntity::column<"deleted_at">().isNull())
                          .expression(query, service::user::UserEntity::tableName(), "u"))
            .andWhere(query.binary(
                query.binary(query.column("code", "r"), ruvia::DbBinaryOperator::kEqual,
                             query.value("superadmin")),
                ruvia::DbBinaryOperator::kOr,
                query.binary(
                    query.binary(query.column("permissions", "r"),
                                 ruvia::DbBinaryOperator::kJsonHasKey, query.value("*")),
                    ruvia::DbBinaryOperator::kOr,
                    query.binary(query.column("permissions", "r"),
                                 ruvia::DbBinaryOperator::kJsonHasKey, query.value(permission)))))
            .limit(1);
        if ((co_await c.db().query(query)).empty())
            service::common::fail(service::common::kPermissionDeniedErrorCode, "无权限", 403);
    }

    ruvia::Task<LoginResultDto> login(ruvia::Context& c, const LoginBody& body) {
        const std::string username(body.get<"username">()->view());
        const std::string password(body.get<"password">()->view());
        if (co_await limiter_.locked(c, username)) {
            service::common::fail(11003, "登录失败次数过多，请 15 分钟后再试", 429);
        }

        ruvia::DbQuery query(c.operationResource());
        query
            .select({query.column("id"), query.column("username"), query.column("password_hash"),
                     service::common::database::emptyText(query, "nickname"),
                     query.column("status")})
            .from(service::user::UserEntity::tableName())
            .where((service::user::UserEntity::column<"username">() == username &&
                    service::user::UserEntity::column<"deleted_at">().isNull())
                       .expression(query))
            .limit(1);
        const auto rows = co_await c.db().query(query);

        if (rows.empty() || !service::utils::comparePassword(
                                password, rows.front()[2].value().value_or(std::string_view{}))) {
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
        ruvia::DbQuery query(c.operationResource());
        query
            .select({query.column("id"), query.column("username"),
                     service::common::database::emptyText(query, "nickname"),
                     query.column("status")})
            .from(service::user::UserEntity::tableName())
            .where(service::common::database::activeId<service::user::UserEntity>(payload.userId)
                       .expression(query))
            .limit(1);
        const auto rows = co_await c.db().query(query);
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
        ruvia::DbQuery query(c.operationResource());
        query
            .select({query.column("username"),
                     service::common::database::emptyText(query, "nickname"),
                     query.column("status")})
            .from(service::user::UserEntity::tableName())
            .where(
                service::common::database::activeId<service::user::UserEntity>(userId).expression(
                    query))
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty())
            service::common::fail(11008, "用户不存在", 404);
        const auto& row = rows.front();
        if (row[2].value().value_or(std::string_view{}) != "enabled")
            service::common::fail(11002, "用户已被禁用", 403);
        co_return co_await buildUser(c, userId,
                                     std::string(row[0].value().value_or(std::string_view{})),
                                     std::string(row[1].value().value_or(std::string_view{})),
                                     std::string(row[2].value().value_or(std::string_view{})));
    }

  private:
    static ruvia::DbQuery enabledRoles(std::string_view userId,
                                       std::pmr::memory_resource* resource) {
        ruvia::DbQuery query(resource);
        query.from(service::role::RoleEntity::tableName(), "r")
            .join(ruvia::DbJoinType::kInner, service::user::UserRoleEntity::tableName(),
                  query.binary(query.column("role_id", "ur"), ruvia::DbBinaryOperator::kEqual,
                               query.column("id", "r")),
                  "ur")
            .where(query.binary(query.column("user_id", "ur"), ruvia::DbBinaryOperator::kEqual,
                                query.value(userId)))
            .andWhere((service::role::RoleEntity::column<"status">() == "enabled" &&
                       service::role::RoleEntity::column<"deleted_at">().isNull())
                          .expression(query, service::role::RoleEntity::tableName(), "r"));
        return query;
    }

    ruvia::Task<AuthUserInfoDto> buildUser(ruvia::Context& c, std::string_view userId,
                                           const std::string& username, const std::string& nickname,
                                           const std::string& status) {
        AuthUserInfoDto user(c);
        user.set<"id">(userId).set<"username">(username).set<"nickname">(nickname).set<"status">(
            status);

        auto roleQuery = enabledRoles(userId, c.operationResource());
        roleQuery
            .select({roleQuery.column("id", "r"), roleQuery.column("name", "r"),
                     roleQuery.column("code", "r")})
            .orderBy(roleQuery.column("id", "r"));
        const auto roles = co_await c.db().query(roleQuery);
        auto& roleItems = user.ensure<"roles">();
        for (const auto& row : roles) {
            auto& role = roleItems.emplace_back(c);
            role.set<"id">(row[0].value().value_or(std::string_view{}))
                .set<"name">(row[1].value().value_or(std::string_view{}))
                .set<"code">(row[2].value().value_or(std::string_view{}));
        }

        auto permissionQuery = enabledRoles(userId, c.operationResource());
        permissionQuery.select(permissionQuery.column("permission", "p"))
            .distinct()
            .joinFunction(ruvia::DbJoinType::kCross,
                          permissionQuery.call("jsonb_array_elements_text",
                                               {permissionQuery.column("permissions", "r")}),
                          {}, "p", {.lateral = true, .columns = {{.name = "permission"}}})
            .orderBy(permissionQuery.column("permission", "p"));
        const auto permissions = co_await c.db().query(permissionQuery);
        auto& permissionItems = user.ensure<"permissions">();
        for (const auto& row : permissions)
            permissionItems.emplace_back(row[0].value().value_or(std::string_view{}),
                                         ruvia::ModelOptions{.resource = c.resource()});
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
