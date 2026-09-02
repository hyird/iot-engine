#pragma once

#include <ruvia/web/Model.h>

namespace service::vpn {

RUVIA_REQUEST_MODEL(VpnIdParams,
    RUVIA_OPTIONAL_FIELD(id, ruvia::String));

RUVIA_REQUEST_MODEL(VpnListQuery,
    RUVIA_OPTIONAL_FIELD(page, ruvia::Int64, RUVIA_DEFAULT(1)),
    RUVIA_OPTIONAL_FIELD_NAME("pageSize", pageSize, ruvia::Int64, RUVIA_DEFAULT(20)),
    RUVIA_OPTIONAL_FIELD(keyword, ruvia::String),
    RUVIA_OPTIONAL_FIELD(status, ruvia::String));

RUVIA_REQUEST_MODEL(VpnClientConfigQuery,
    RUVIA_OPTIONAL_FIELD_NAME("peerId", peerId, ruvia::String));

} // namespace service::vpn
