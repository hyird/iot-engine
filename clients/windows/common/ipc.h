#pragma once
#include "json.h"
#include <functional>
#include <stdexcept>
#include <stop_token>
#include <string>
namespace iotvpn {
struct LocalServiceError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct ServiceRepairResult {
    bool succeeded = false;
    bool cancelled = false;
    std::string message;
};
Json pipeRequest(const Json& request, std::uint32_t timeoutMs = 90000, std::stop_token stop = {});
ServiceRepairResult repairLocalService();
void runPipeServer(std::stop_token stop, const std::function<Json(const Json&)>& handler);
}
