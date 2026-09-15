#pragma once

#include <string>

namespace service::edge {

    struct ProjectorRecoveryStream final {
        std::string stream;
        std::string lease;
        std::string token;
    };

} // namespace service::edge
