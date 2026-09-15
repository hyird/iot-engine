#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace service::collector {

struct LinkState {
    struct Target {
        std::string id;
        std::string state;
        std::string reason;
        std::string error;
        std::int64_t lastActivityAtMs = 0;
    };

    std::string linkId;
    std::size_t workerIndex = 0;
    std::string state;
    std::string reason;
    std::string error;
    std::vector<std::string> remoteEndpoints;
    std::vector<Target> targets;
    std::int64_t lastActivityAtMs = 0;
};

} // namespace service::collector
