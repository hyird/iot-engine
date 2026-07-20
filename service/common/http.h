#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/web/Context.h>
#include <ruvia/web/Error.h>
#include <ruvia/web/db/DbTypes.h>

#include "service/common/types.h"

namespace service::common {

inline constexpr std::int64_t kValidationErrorCode{10001};
inline constexpr std::int64_t kBadRequestErrorCode{10002};
inline constexpr std::int64_t kNotFoundErrorCode{10003};
inline constexpr std::int64_t kServerErrorCode{10004};
inline constexpr std::int64_t kUnauthorizedErrorCode{11004};
inline constexpr std::int64_t kTokenExpiredErrorCode{11005};
inline constexpr std::int64_t kTokenInvalidErrorCode{11006};
inline constexpr std::int64_t kPermissionDeniedErrorCode{11007};

RUVIA_RESPONSE_MODEL(ErrorResponse, RUVIA_FIELD(code, ruvia::Int64),
                     RUVIA_FIELD(message, ruvia::String));

inline std::optional<std::int64_t> parseInt64(std::optional<std::string_view> input) {
    if (!input || input->empty())
        return std::nullopt;
    std::int64_t value{};
    const auto [ptr, ec] = std::from_chars(input->data(), input->data() + input->size(), value);
    if (ec != std::errc{} || ptr != input->data() + input->size())
        return std::nullopt;
    return value;
}

template <typename... Ts> inline std::vector<ruvia::DbValue> dbParams(Ts&&... values) {
    std::vector<ruvia::DbValue> params;
    params.reserve(sizeof...(Ts));
    (params.emplace_back(std::forward<Ts>(values)), ...);
    return params;
}

[[noreturn]] inline void fail(std::int64_t code, std::string message, std::uint16_t status) {
    throw ruvia::HttpError(status, std::to_string(code), std::move(message));
}

inline std::int64_t errorCode(std::string_view code, std::uint16_t status) {
    if (!code.empty()) {
        std::int64_t value{};
        const auto [ptr, ec] = std::from_chars(code.data(), code.data() + code.size(), value);
        if (ec == std::errc{} && ptr == code.data() + code.size())
            return value;
        if (code == "validation_failed")
            return kValidationErrorCode;
    }
    if (status == 401)
        return kUnauthorizedErrorCode;
    if (status == 403)
        return kPermissionDeniedErrorCode;
    if (status == 404)
        return kNotFoundErrorCode;
    return status >= 500 ? kServerErrorCode : kBadRequestErrorCode;
}

template <typename Response, typename Data> inline Response ok(ruvia::Context& c, Data&& data) {
    Response response(c);
    response.code(0).message("ok").data(std::forward<Data>(data));
    return response;
}

inline OperationResponse operation(ruvia::Context& c, std::string_view message) {
    OperationResponse response(c);
    response.code(0).message(message);
    return response;
}

inline ErrorResponse error(ruvia::Context& c, std::int64_t code, std::string_view message) {
    ErrorResponse response(c);
    response.code(code).message(message);
    return response;
}

} // namespace service::common
