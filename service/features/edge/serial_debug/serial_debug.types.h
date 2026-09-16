#pragma once
#include <chrono>
#include <cstdint>
#include <ruvia/web/Model.h>

namespace service::edge::serial_debug {
RUVIA_REQUEST_MODEL(BrowserRequest,
    RUVIA_OPTIONAL_FIELD(action, ruvia::String),
    RUVIA_OPTIONAL_FIELD(requestId, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(baudRate, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(dataBits, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(stopBits, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(parity, ruvia::String),
    RUVIA_OPTIONAL_FIELD(rs485, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD(hex, ruvia::String));

struct BrowserSession final {
    std::uint64_t requestSequence{1};
    bool opened{};
    bool closed{};
    std::chrono::steady_clock::time_point lastInput{std::chrono::steady_clock::now()};
};
} // namespace service::edge::serial_debug
