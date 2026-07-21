#pragma once

#include <ruvia/web/Model.h>

namespace service::protocol {

RUVIA_REQUEST_MODEL(ProtocolListQuery, RUVIA_FIELD(page, ruvia::Int64, RUVIA_DEFAULT(1)),
                    RUVIA_FIELD_NAME("pageSize", pageSize, ruvia::Int64, RUVIA_DEFAULT(10)),
                    RUVIA_FIELD(protocol, ruvia::String));

RUVIA_REQUEST_MODEL(ProtocolIdParams, RUVIA_FIELD(id, ruvia::String));

} // namespace service::protocol
