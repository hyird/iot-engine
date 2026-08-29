#pragma once

#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

#include <ruvia/web/App.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/auth/Jwt.h>

#include "service/common/uuid.h"

namespace service::core {

struct JwtPayload {
    std::string userId;
    std::string username;
};

} // namespace service::core

namespace service::utils {

class JwtExpiredError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};
class JwtInvalidError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

namespace jwt_detail {

inline std::string requiredSecret(const ruvia::Env& env, std::string_view name) {
    const auto value = env.get(name);
    if (!value || value->size() < 32) {
        throw std::runtime_error(std::string(name) + " must contain at least 32 characters");
    }
    return std::string(*value);
}

inline std::chrono::seconds duration(std::string_view value, std::chrono::seconds fallback) {
    if (value.empty())
        return fallback;
    std::int64_t multiplier = 1;
    const char suffix = value.back();
    if (suffix == 'm')
        multiplier = 60;
    else if (suffix == 'h')
        multiplier = 3600;
    else if (suffix == 'd')
        multiplier = 86400;
    const bool hasSuffix = suffix == 's' || suffix == 'm' || suffix == 'h' || suffix == 'd';
    const auto number = hasSuffix ? value.substr(0, value.size() - 1) : value;
    std::int64_t amount = 0;
    const auto [end, error] =
        std::from_chars(number.data(), number.data() + number.size(), amount);
    if (number.empty() || error != std::errc{} || end != number.data() + number.size() ||
        amount <= 0 || amount > std::numeric_limits<std::int64_t>::max() / multiplier)
        return fallback;
    return std::chrono::seconds(amount * multiplier);
}

inline std::string sign(ruvia::Context& c, const core::JwtPayload& payload,
                        std::string_view secretName, std::string_view type,
                        std::string_view durationName, std::chrono::seconds fallback) {
    const auto secret = requiredSecret(c.env(), secretName);
    ruvia::JwtSignOptions options;
    options.secret = secret;
    options.issuer.assign("iot-engine");
    options.audience.assign("iot-engine-web");
    options.subject.assign(payload.userId);
    options.expiresIn = duration(c.env().get(durationName).value_or(""), fallback);
    options.claims.emplace_back(
        ruvia::JwtClaimOptions{.name = "user_id", .value = payload.userId});
    options.claims.emplace_back(
        ruvia::JwtClaimOptions{.name = "username", .value = payload.username});
    options.claims.emplace_back(ruvia::JwtClaimOptions{.name = "token_type", .value = type});
    options.resource = c.resource();
    const auto token = ruvia::jwtSign(options);
    return std::string(token.data(), token.size());
}

inline core::JwtPayload verify(ruvia::Context& c, std::string_view token,
                               std::string_view secretName, std::string_view expectedType) {
    try {
        const auto secret = requiredSecret(c.env(), secretName);
        ruvia::JwtVerifyOptions options;
        options.token = token;
        options.secret = secret;
        options.issuer.assign("iot-engine");
        options.audience.assign("iot-engine-web");
        options.leeway = std::chrono::seconds(15);
        options.resource = c.resource();
        const auto decoded = ruvia::jwtVerify(options);
        const auto id = decoded.claim("user_id");
        const auto username = decoded.claim("username");
        const auto type = decoded.claim("token_type");
        if (!id || !username || !type || *type != expectedType) {
            throw JwtInvalidError("invalid token payload");
        }
        core::JwtPayload result;
        result.userId = std::string(*id);
        result.username = std::string(*username);
        if (!service::common::isUuid(result.userId) || result.username.empty())
            throw JwtInvalidError("invalid token payload");
        return result;
    } catch (const JwtInvalidError&) {
        throw;
    } catch (const std::exception& error) {
        const std::string message(error.what());
        if (message.find("expired") != std::string::npos)
            throw JwtExpiredError(message);
        throw JwtInvalidError(message);
    }
}

} // namespace jwt_detail

inline std::string signAccessToken(ruvia::Context& c, const core::JwtPayload& payload) {
    return jwt_detail::sign(c, payload, "JWT_SECRET", "access", "JWT_EXPIRES_IN",
                            std::chrono::hours(1));
}

inline std::string signRefreshToken(ruvia::Context& c, const core::JwtPayload& payload) {
    return jwt_detail::sign(c, payload, "JWT_REFRESH_SECRET", "refresh", "JWT_REFRESH_EXPIRES_IN",
                            std::chrono::hours(24 * 30));
}

inline core::JwtPayload verifyAccessToken(ruvia::Context& c, std::string_view token) {
    return jwt_detail::verify(c, token, "JWT_SECRET", "access");
}

inline core::JwtPayload verifyRefreshToken(ruvia::Context& c, std::string_view token) {
    return jwt_detail::verify(c, token, "JWT_REFRESH_SECRET", "refresh");
}

} // namespace service::utils
