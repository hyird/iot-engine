#pragma once
#include <ruvia/web/Model.h>

namespace service::edge::serial_debug {
RUVIA_MODEL(BrowserRequest,
    RUVIA_OPTIONAL_FIELD(action, ruvia::String),
    RUVIA_OPTIONAL_FIELD(requestId, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(baudRate, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(dataBits, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(stopBits, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(parity, ruvia::String),
    RUVIA_OPTIONAL_FIELD(rs485, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD(hex, ruvia::String));

} // namespace service::edge::serial_debug
