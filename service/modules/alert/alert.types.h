#pragma once

#include <ruvia/web/Model.h>

namespace service::alert {

RUVIA_REQUEST_MODEL(AlertIdParams,
    RUVIA_OPTIONAL_FIELD(id, ruvia::String));

RUVIA_REQUEST_MODEL(AlertListQuery,
    RUVIA_OPTIONAL_FIELD(page, ruvia::Int64, RUVIA_DEFAULT(1)),
    RUVIA_OPTIONAL_FIELD_NAME("pageSize", pageSize, ruvia::Int64, RUVIA_DEFAULT(20)));

} // namespace service::alert
