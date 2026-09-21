#pragma once

#include <cstdint>
#include <string_view>
#include <ruvia/web/Error.h>
#include <ruvia/web/FixedString.h>
#include <ruvia/web/Middleware.h>
#include <ruvia/web/Next.h>
#include "service/common/http.h"

namespace service::middleware {

struct RequestValidationErrorCodes final {
    std::int64_t code;
    std::string_view field;
    std::int64_t fieldCode;
    std::string_view issueCode;
};

inline std::int64_t requestErrorCode(const ruvia::Context& context, const ruvia::HttpErrorInfo& error) {
    if (error.code() == "validation_failed") {
        if (const auto* codes = context.tryRequestState<RequestValidationErrorCodes>()) {
            const auto issues = error.validationIssues();
            if (!codes->field.empty() && !issues.empty() && issues.front().field() == codes->field && (codes->issueCode.empty() || issues.front().code() == codes->issueCode)) return codes->fieldCode;
            return codes->code;
        }
    }
    return service::common::errorCode(error.code(), error.status().value());
}

// JsonBody 的校验失败直接进入 onError，并不向 next() 抛出异常。
// 用请求作用域绑定声明旧错误码，统一错误响应读取它；不重复解析或校验。
// 放在 PathModel 之后、JsonBody 之前，保持认证、权限及路径校验错误不变。
template <std::int64_t Code, ruvia::FixedString Field = "", std::int64_t FieldCode = Code, ruvia::FixedString IssueCode = "">
class ValidationErrorCodeMiddleware final : public ruvia::Middleware {
  public:
    ruvia::Task<void> handle(ruvia::Context& context, ruvia::Next& next) {
        const RequestValidationErrorCodes codes{Code, Field.view(), FieldCode, IssueCode.view()};
        const auto binding = context.bindRequestState(codes);
        co_await next();
    }
};

} // namespace service::middleware
