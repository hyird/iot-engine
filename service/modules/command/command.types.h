#pragma once

#include <ruvia/web/Model.h>

namespace service::command {

RUVIA_REQUEST_MODEL(PreparedElement,
    RUVIA_OPTIONAL_FIELD(elementId, ruvia::String),
    RUVIA_OPTIONAL_FIELD(value, ruvia::String));

RUVIA_REQUEST_MODEL(PreparedOperation,
    RUVIA_OPTIONAL_FIELD(id, ruvia::String),
    RUVIA_OPTIONAL_FIELD(deviceId, ruvia::String),
    RUVIA_OPTIONAL_FIELD(deviceCode, ruvia::String),
    RUVIA_OPTIONAL_FIELD(protocol, ruvia::String),
    RUVIA_OPTIONAL_FIELD(elements, ruvia::BoxedArray<PreparedElement>),
    RUVIA_OPTIONAL_FIELD(payload, ruvia::Array<ruvia::String>));

RUVIA_REQUEST_MODEL(PreparedBatch,
    RUVIA_OPTIONAL_FIELD(queue, ruvia::String),
    RUVIA_OPTIONAL_FIELD(kind, ruvia::String),
    RUVIA_OPTIONAL_FIELD(maximum, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(nodeId, ruvia::String),
    RUVIA_OPTIONAL_FIELD(commands, ruvia::BoxedArray<PreparedOperation>));

} // namespace service::command
