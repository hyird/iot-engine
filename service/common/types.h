#pragma once

#include <ruvia/web/Model.h>

namespace service::common {

inline constexpr std::string_view kSuperAdminRoleCode{"superadmin"};

RUVIA_RESPONSE_MODEL(OperationResponse, RUVIA_FIELD(code, ruvia::Int64),
                     RUVIA_FIELD(message, ruvia::String));

RUVIA_RESPONSE_MODEL(HealthData, RUVIA_FIELD(status, ruvia::String));

RUVIA_RESPONSE_MODEL(HealthResponse, RUVIA_FIELD(code, ruvia::Int64),
                     RUVIA_FIELD(message, ruvia::String), RUVIA_FIELD(data, HealthData));

} // namespace service::common
