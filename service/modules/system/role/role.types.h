#pragma once

#include <ruvia/web/Model.h>

namespace service::role {

RUVIA_RESPONSE_MODEL(RoleOptionDto, RUVIA_FIELD(id, ruvia::Int64), RUVIA_FIELD(name, ruvia::String),
                     RUVIA_FIELD(code, ruvia::String));

RUVIA_RESPONSE_MODEL(RoleOptionsResponse, RUVIA_FIELD(code, ruvia::Int64),
                     RUVIA_FIELD(message, ruvia::String),
                     RUVIA_FIELD(data, ruvia::List<RoleOptionDto>));

} // namespace service::role
