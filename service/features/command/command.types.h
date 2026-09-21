#pragma once

#include <string_view>
#include <ruvia/web/ModelJson.h>

namespace service::command {

RUVIA_MODEL(PrepareCommandBody,
    RUVIA_REQUIRED_FIELD(deviceId, ruvia::String),
    RUVIA_REQUIRED_FIELD(elements, ruvia::Array<ruvia::Array<ruvia::String>>));

inline bool terminalState(std::string_view state) {
    return state == "SUCCEEDED" || state == "REJECTED" || state == "UNKNOWN" ||
           state == "READBACK_MISMATCH" || state == "FAILED";
}

// A transport timeout is not evidence that a physical operation did not happen.
inline std::string_view collectorResultState(bool success, std::string_view reason) {
    if (success) return "SUCCEEDED";
    if (reason == "device_offline" || reason == "command_invalid" ||
        reason == "stale_session_epoch" || reason == "dispatch_deadline_expired" ||
        reason == "connection_route_missing") return "REJECTED";
    if (reason.find("readback") != std::string_view::npos)
        return "READBACK_MISMATCH";
    return "UNKNOWN";
}

} // namespace service::command
