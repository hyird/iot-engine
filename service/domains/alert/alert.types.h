#pragma once

#include <ruvia/web/Model.h>

namespace service::alert {

struct AlertIdParams final {
    RUVIA_OPTIONAL_FIELD(id, ruvia::String);
    RUVIA_MODEL(AlertIdParams, id);
};

struct AlertListQuery final {
    RUVIA_OPTIONAL_FIELD(page, ruvia::Int64, RUVIA_DEFAULT(1));
    RUVIA_OPTIONAL_FIELD_NAME("pageSize", pageSize, ruvia::Int64, RUVIA_DEFAULT(20));
    RUVIA_MODEL(AlertListQuery, page, pageSize);
};

} // namespace service::alert
