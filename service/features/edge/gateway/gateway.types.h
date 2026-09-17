#pragma once

#include <cstdint>
#include <string>

namespace service::edge::gateway {

struct FirmwareSource final {
    std::string storagePath;
    std::uint64_t sizeBytes{};
};

} // namespace service::edge::gateway
