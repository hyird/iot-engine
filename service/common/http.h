#pragma once

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/web/Context.h>
#include <ruvia/web/ConnInfo.h>
#include "service/utils/text.h"
#include <ruvia/web/Error.h>
#include <ruvia/web/db/DbTypes.h>

#include <ruvia/web/Model.h>
#include "service/common/uuid.h"

namespace service::common {

RUVIA_RESPONSE_MODEL(OperationResponse,
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(message, ruvia::String));

inline bool isUuidField(const ruvia::String& value) noexcept { return isUuid(value.view()); }

inline bool isOptionalUuidField(const ruvia::String& value) noexcept {
    return value.empty() || isUuid(value.view());
}

inline constexpr std::int64_t kValidationErrorCode{10001};
inline constexpr std::int64_t kBadRequestErrorCode{10002};
inline constexpr std::int64_t kNotFoundErrorCode{10003};
inline constexpr std::int64_t kServerErrorCode{10004};
inline constexpr std::int64_t kUnauthorizedErrorCode{11004};
inline constexpr std::int64_t kTokenExpiredErrorCode{11005};
inline constexpr std::int64_t kTokenInvalidErrorCode{11006};
inline constexpr std::int64_t kPermissionDeniedErrorCode{11007};

RUVIA_RESPONSE_MODEL(ErrorResponse,
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(message, ruvia::String));

inline std::optional<std::int64_t> parseInt64(std::optional<std::string_view> input) {
    if (!input || input->empty())
        return std::nullopt;
    std::int64_t value{};
    const auto [ptr, ec] = std::from_chars(input->data(), input->data() + input->size(), value);
    if (ec != std::errc{} || ptr != input->data() + input->size())
        return std::nullopt;
    return value;
}

struct Page final {
    std::int64_t page{1};
    std::int64_t pageSize{20};
    std::int64_t offset{0};
};

inline Page page(const ruvia::ContextRequest& request) {
    const auto integer = [&request](std::string_view name, std::int64_t fallback) {
        return parseInt64(request.query(name)).value_or(fallback);
    };
    Page result;
    result.page = std::max<std::int64_t>(1, integer("page", 1));
    result.pageSize = std::clamp<std::int64_t>(integer("pageSize", 20), 1, 100);
    result.offset = (result.page - 1) * result.pageSize;
    return result;
}

template <typename... Ts> std::vector<ruvia::DbValue> dbParams(Ts&&... values) {
    std::vector<ruvia::DbValue> params;
    params.reserve(sizeof...(Ts));
    (params.emplace_back(std::forward<Ts>(values)), ...);
    return params;
}

[[noreturn]] inline void fail(std::int64_t code, std::string message, std::uint16_t status) {
    const auto codeText = std::to_string(code);
    throw ruvia::HttpError(ruvia::HttpErrorInfoOptions{
        .status = ruvia::HttpStatusCode::fromValue(status),
        .code = codeText,
        .message = message,
    });
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

template <typename Response, typename Data> Response ok(ruvia::Context& c, Data&& data) {
    Response response(c);
    response.template set<"code">(0)
        .template set<"message">("ok")
        .template set<"data">(std::forward<Data>(data));
    return response;
}

inline OperationResponse operation(ruvia::Context& c, std::string_view message) {
    OperationResponse response(c);
    response.set<"code">(0).set<"message">(message);
    return response;
}

inline ErrorResponse error(ruvia::Context& c, std::int64_t code, std::string_view message) {
    ErrorResponse response(c);
    response.set<"code">(code).set<"message">(message);
    return response;
}

inline void requireUuid(std::int64_t code, std::string_view value, std::string_view message) {
    if (!service::common::isUuid(value))
        service::common::fail(code, std::string(message), 400);
}

inline std::string clientIp(const ruvia::Context& context) {
    if (const auto value = context.req().header("X-Real-IP")) {
        const auto resolved = service::utils::trim(*value);
        if (!resolved.empty())
            return resolved;
    }
    return std::string(ruvia::getConnInfo(context).remote().address());
}

} // namespace service::common
