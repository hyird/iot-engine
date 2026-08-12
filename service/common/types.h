#pragma once

#include <ruvia/web/Model.h>

namespace service::common {

inline constexpr std::string_view kSuperAdminRoleCode{"superadmin"};

RUVIA_RESPONSE_MODEL(OperationResponse,
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(message, ruvia::String));

RUVIA_RESPONSE_MODEL(HealthData,
    RUVIA_OPTIONAL_FIELD(status, ruvia::String));

RUVIA_RESPONSE_MODEL(HealthResponse,
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(message, ruvia::String),
    RUVIA_OPTIONAL_FIELD(data, HealthData));

} // namespace service::common
