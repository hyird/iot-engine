#pragma once

#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>

#include <ruvia/web/App.h>
#include <ruvia/web/auth/Jwt.h>

#include "service/common/database.h"
#include "service/common/http.h"
#include "service/modules/system/auth/auth.entity.h"
#include "service/modules/system/auth/auth.error.h"
#include "service/modules/system/auth/auth.types.h"
#include "service/modules/system/role/role.entity.h"
#include "service/modules/system/user/user.entity.h"
#include "service/utils/password.h"
#include "service/utils/redis.h"

namespace service::auth {

class AuthTokenService final {
  public:
    static std::string requiredSecret(const ruvia::Env& env, std::string_view name) {
        const auto value = env.get(name);
        if (!value || value->size() < 32) {
            throw std::runtime_error(std::string(name) + " must contain at least 32 characters");
        }
        return std::string(*value);
    }

    static std::chrono::seconds duration(std::string_view value, std::chrono::seconds fallback) {
        if (value.empty()) {
            return fallback;
        }
        std::int64_t multiplier = 1;
        const char suffix = value.back();
        if (suffix == 'm') {
            multiplier = 60;
        } else if (suffix == 'h') {
            multiplier = 3600;
        } else if (suffix == 'd') {
            multiplier = 86400;
        }
        const bool hasSuffix = suffix == 's' || suffix == 'm' || suffix == 'h' || suffix == 'd';
        const auto number = hasSuffix ? value.substr(0, value.size() - 1) : value;
        std::int64_t amount = 0;
        const auto [end, error] =
            std::from_chars(number.data(), number.data() + number.size(), amount);
        if (number.empty() || error != std::errc{} || end != number.data() + number.size() ||
            amount <= 0 || amount > std::numeric_limits<std::int64_t>::max() / multiplier) {
            return fallback;
        }
        return std::chrono::seconds(amount * multiplier);
    }

    template <typename Context>
    static std::string sign(Context& c, const JwtPayload& payload, std::string_view secretName, std::string_view type, std::string_view durationName, std::chrono::seconds fallback) {
        const auto secret = requiredSecret(c.env(), secretName);
        ruvia::JwtSignOptions options;
        options.secret = secret;
        options.issuer.assign("iot-engine");
        options.audience.assign("iot-engine-web");
        options.subject.assign(payload.userId);
        options.expiresIn = duration(c.env().get(durationName).value_or(""), fallback);
        options.claims.emplace_back(
            ruvia::JwtClaimOptions{ .name = "user_id", .value = payload.userId }
        );
        options.claims.emplace_back(
            ruvia::JwtClaimOptions{ .name = "username", .value = payload.username }
        );
        options.claims.emplace_back(ruvia::JwtClaimOptions{ .name = "token_type", .value = type });
        options.resource = c.pool();
        const auto token = ruvia::jwtSign(options);
        return std::string(token.data(), token.size());
    }

    template <typename Context>
    static JwtPayload verify(Context& c, std::string_view token, std::string_view secretName, std::string_view expectedType) {
        try {
            const auto secret = requiredSecret(c.env(), secretName);
            ruvia::JwtVerifyOptions options;
            options.token = token;
            options.secret = secret;
            options.issuer.assign("iot-engine");
            options.audience.assign("iot-engine-web");
            options.leeway = std::chrono::seconds(15);
            options.resource = c.pool();
            const auto decoded = ruvia::jwtVerify(options);
            const auto id = decoded.claim("user_id");
            const auto username = decoded.claim("username");
            const auto type = decoded.claim("token_type");
            if (!id || !username || !type || *type != expectedType) {
                throw JwtInvalidError("invalid token payload");
            }
            JwtPayload result;
            result.userId = std::string(*id);
            result.username = std::string(*username);
            if (!service::common::isUuid(result.userId) || result.username.empty()) {
                throw JwtInvalidError("invalid token payload");
            }
            return result;
        } catch (const JwtInvalidError&) {
            throw;
        } catch (const std::exception& error) {
            const std::string message(error.what());
            if (message.find("expired") != std::string::npos) {
                throw JwtExpiredError(message);
            }
            throw JwtInvalidError(message);
        }
    }

    template <typename Context>
    static std::string signAccessToken(Context& c, const JwtPayload& payload) {
        return sign(c, payload, "JWT_SECRET", "access", "JWT_EXPIRES_IN", std::chrono::hours(1));
    }

    template <typename Context>
    static std::string signRefreshToken(Context& c, const JwtPayload& payload) {
        return sign(c, payload, "JWT_REFRESH_SECRET", "refresh", "JWT_REFRESH_EXPIRES_IN", std::chrono::hours(24 * 30));
    }

    template <typename Context>
    static JwtPayload verifyAccessToken(Context& c, std::string_view token) {
        return verify(c, token, "JWT_SECRET", "access");
    }

    template <typename Context>
    static JwtPayload verifyRefreshToken(Context& c, std::string_view token) {
        return verify(c, token, "JWT_REFRESH_SECRET", "refresh");
    }
};

class LoginRateLimiter {
  public:
    static constexpr int limit = 5;
    static constexpr std::string_view windowMilliseconds = "900000";

    template <typename Context>
    ruvia::Task<bool> locked(Context& context, std::string_view username) {
        const auto reply =
            co_await service::message::redis::command(context.redis(), { "GET", LoginFailureCounter::key(username) });
        if (reply.null()) {
            co_return false;
        }
        if (reply.kind() != ruvia::RedisValue::Kind::kString) {
            throw std::runtime_error("invalid login rate limit state");
        }
        co_return LoginFailureCounter::decode(reply.string()).failures >= limit;
    }

    template <typename Context>
    ruvia::Task<int> failure(Context& context, std::string_view username) {
        static constexpr std::string_view script = R"lua(
local failures = redis.call('INCR', KEYS[1])
if failures == 1 then redis.call('PEXPIRE', KEYS[1], ARGV[1]) end
return failures
)lua";
        const auto reply = co_await service::message::redis::command(
            context.redis(),
            { "EVAL", std::string(script), "1", LoginFailureCounter::key(username), std::string(windowMilliseconds) }
        );
        if (reply.kind() != ruvia::RedisValue::Kind::kInteger) {
            throw std::runtime_error("invalid login rate limit increment");
        }
        co_return static_cast<int>(reply.integer());
    }

    template <typename Context>
    ruvia::Task<void> clear(Context& context, std::string_view username) {
        (void)co_await service::message::redis::command(context.redis(), { "DEL", LoginFailureCounter::key(username) });
    }
};

class AuthService {
  public:
    explicit AuthService(LoginRateLimiter& limiter) : limiter_(limiter) {}

    template <typename Context>
    static ruvia::Task<void> requirePermission(Context& c, std::string_view userId, std::string_view permission) {
        ruvia::DbExpressions expressions(c.pool());
        auto query = enabledRoles(c, userId);
        query.template join<service::user::UserEntity>(ruvia::DbJoinType::kInner, "u", expressions.binary(expressions.column("id", "u"), ruvia::DbBinaryOperator::kEqual, expressions.column("user_id", "ur")))
            .andWhere(expressions.binary(
                expressions.binary(expressions.column("status", "u"), ruvia::DbBinaryOperator::kEqual, expressions.value("enabled")),
                ruvia::DbBinaryOperator::kAnd,
                expressions.unary(ruvia::DbUnaryOperator::kIsNull, expressions.column("deleted_at", "u"))
            ))
            .andWhere(expressions.binary(
                expressions.binary(expressions.column("code", "r"), ruvia::DbBinaryOperator::kEqual, expressions.value("superadmin")),
                ruvia::DbBinaryOperator::kOr,
                expressions.binary(
                    expressions.binary(expressions.column("permissions", "r"), ruvia::DbBinaryOperator::kJsonHasKey, expressions.value("*")),
                    ruvia::DbBinaryOperator::kOr,
                    expressions.binary(expressions.column("permissions", "r"), ruvia::DbBinaryOperator::kJsonHasKey, expressions.value(permission))
                )
            ));
        if (!co_await query.getExists()) {
            service::common::fail(service::common::kPermissionDeniedErrorCode, "无权限", 403);
        }
    }

    template <typename Context>
    ruvia::Task<LoginResultDto> login(Context& c, const LoginBody& body) {
        const std::string username(body.get<"username">()->view());
        const std::string password(body.get<"password">()->view());
        if (co_await limiter_.locked(c, username)) {
            service::common::fail(11003, "登录失败次数过多，请 15 分钟后再试", 429);
        }

        ruvia::DbFindOptions options;
        options.where = service::user::UserEntity::column<"username">() == username &&
            service::user::UserEntity::column<"deleted_at">().isNull();
        const auto user = co_await c.db().template getRepository<service::user::UserEntity>().findOne(options);

        if (!user || !service::utils::comparePassword(password, user->template get<"password_hash">())) {
            const int remaining = LoginRateLimiter::limit - co_await limiter_.failure(c, username);
            const auto message = remaining > 0 ? "用户名或密码错误，还剩 " +
                    std::to_string(remaining) + " 次尝试机会"
                                               : "登录失败次数过多，请 15 分钟后再试";
            service::common::fail(11001, message, remaining > 0 ? 401 : 429);
        }

        if (user->template get<"status">() != "enabled") {
            service::common::fail(11002, "用户已被禁用", 403);
        }
        co_await limiter_.clear(c, username);

        const std::string userId(user->template get<"id">());
        const std::string nickname(user->template isNull<"nickname">() ? std::string_view{} : user->template get<"nickname">());
        const std::string status(user->template get<"status">());
        JwtPayload payload{ userId, username };
        LoginResultDto result(ruvia::ModelOptions{ .resource = c.arena() });
        result.set<"token">(AuthTokenService::signAccessToken(c, payload))
            .template set<"refreshToken">(AuthTokenService::signRefreshToken(c, payload))
            .template set<"user">(co_await buildUser(c, userId, username, nickname, status));
        co_return result;
    }

    template <typename Context>
    ruvia::Task<LoginResultDto> refresh(Context& c, const RefreshBody& body) {
        JwtPayload payload;
        try {
            payload = AuthTokenService::verifyRefreshToken(c, body.get<"refreshToken">()->view());
        } catch (...) {
            service::common::fail(service::common::kTokenInvalidErrorCode, "刷新令牌无效", 401);
        }
        ruvia::DbFindOptions options;
        options.where = service::common::database::activeId<service::user::UserEntity>(payload.userId);
        const auto user = co_await c.db().template getRepository<service::user::UserEntity>().findOne(options);
        if (!user) {
            service::common::fail(11008, "用户不存在", 404);
        }
        if (user->template get<"status">() != "enabled") {
            service::common::fail(11002, "用户已被禁用", 403);
        }

        const std::string userId(user->template get<"id">());
        const std::string username(user->template get<"username">());
        const std::string nickname(user->template isNull<"nickname">() ? std::string_view{} : user->template get<"nickname">());
        const std::string status(user->template get<"status">());
        JwtPayload next{ userId, username };
        LoginResultDto result(ruvia::ModelOptions{ .resource = c.arena() });
        result.set<"token">(AuthTokenService::signAccessToken(c, next))
            .template set<"refreshToken">(AuthTokenService::signRefreshToken(c, next))
            .template set<"user">(co_await buildUser(c, userId, username, nickname, status));
        co_return result;
    }

    template <typename Context>
    ruvia::Task<AuthUserInfoDto> current(Context& c, std::string_view userId) {
        ruvia::DbFindOptions options;
        options.where = service::common::database::activeId<service::user::UserEntity>(userId);
        const auto user = co_await c.db().template getRepository<service::user::UserEntity>().findOne(options);
        if (!user) {
            service::common::fail(11008, "用户不存在", 404);
        }
        if (user->template get<"status">() != "enabled") {
            service::common::fail(11002, "用户已被禁用", 403);
        }
        co_return co_await buildUser(c, userId, std::string(user->template get<"username">()), std::string(user->template isNull<"nickname">() ? std::string_view{} : user->template get<"nickname">()), std::string(user->template get<"status">()));
    }

  private:
    template <typename Context>
    static ruvia::DbQueryBuilder<service::role::RoleEntity, ruvia::DbHandle>
    enabledRoles(Context& c, std::string_view userId) {
        ruvia::DbExpressions expressions(c.pool());
        auto query = c.db().template getRepository<service::role::RoleEntity>().createQueryBuilder("r");
        query.template join<service::user::UserRoleEntity>(ruvia::DbJoinType::kInner, "ur", expressions.binary(expressions.column("role_id", "ur"), ruvia::DbBinaryOperator::kEqual, expressions.column("id", "r")))
            .where(expressions.binary(expressions.column("user_id", "ur"), ruvia::DbBinaryOperator::kEqual, expressions.value(userId)))
            .andWhere(service::role::RoleEntity::column<"status">() == "enabled" && service::role::RoleEntity::column<"deleted_at">().isNull());
        return query;
    }

    template <typename Context>
    ruvia::Task<AuthUserInfoDto> buildUser(Context& c, std::string_view userId, const std::string& username, const std::string& nickname, const std::string& status) {
        AuthUserInfoDto user(ruvia::ModelOptions{ .resource = c.arena() });
        user.set<"id">(userId).set<"username">(username).set<"nickname">(nickname).set<"status">(
            status
        );

        auto roleQuery = enabledRoles(c, userId);
        roleQuery.select({ { "id" }, { "name" }, { "code" } }).orderBy("id");
        const auto roles = co_await roleQuery.getMany();
        auto& roleItems = user.ensure<"roles">();
        for (const auto& row : roles) {
            auto& role = roleItems.emplace_back(ruvia::ModelOptions{ .resource = c.arena() });
            role.set<"id">(row.template get<"id">())
                .template set<"name">(row.template get<"name">())
                .template set<"code">(row.template get<"code">());
        }

        ruvia::DbExpressions expressions(c.pool());
        auto permissionQuery = enabledRoles(c, userId);
        permissionQuery.select({ { "permission", expressions.column("permission", "p") } })
            .distinct()
            .joinFunction(ruvia::DbJoinType::kCross, expressions.call("jsonb_array_elements_text", { expressions.column("permissions", "r") }), "p", {}, { .lateral = true, .columns = { { .name = "permission" } } })
            .orderBy(expressions.column("permission", "p"));
        const auto permissions = co_await permissionQuery.template getMany<service::role::RolePermission>();
        auto& permissionItems = user.ensure<"permissions">();
        for (const auto& row : permissions) {
            permissionItems.emplace_back(row.template get<"permission">(), ruvia::ModelOptions{ .resource = c.arena() });
        }
        co_return user;
    }

  private:
    LoginRateLimiter& limiter_;
};

inline AuthService& authService() {
    thread_local LoginRateLimiter limiter;
    thread_local AuthService service(limiter);
    return service;
}

} // namespace service::auth
