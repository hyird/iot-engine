#pragma once

#include <ruvia/web/Model.h>

namespace service::common {

inline constexpr std::string_view kSuperAdminRoleCode{"superadmin"};

struct OperationResponse final {
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(message, ruvia::String);
    RUVIA_MODEL(OperationResponse, code, message);
};

struct HealthData final {
    RUVIA_OPTIONAL_FIELD(status, ruvia::String);
    RUVIA_MODEL(HealthData, status);
};

struct HealthResponse final {
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(message, ruvia::String);
    RUVIA_OPTIONAL_FIELD(data, HealthData);
    RUVIA_MODEL(HealthResponse, code, message, data);
};

} // namespace service::common
