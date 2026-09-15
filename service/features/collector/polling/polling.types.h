#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace service::collector::polling {

struct Reply {
    std::vector<std::uint8_t> data;
    std::vector<std::uint8_t> nextRequest;
    std::string error;
};

} // namespace service::collector::polling
