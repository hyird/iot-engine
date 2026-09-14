#pragma once
#include "json.h"
#include <functional>
#include <stop_token>
namespace iotvpn {
Json pipeRequest(const Json& request, std::uint32_t timeoutMs = 90000, std::stop_token stop = {});
void runPipeServer(std::stop_token stop, const std::function<Json(const Json&)>& handler);


}
