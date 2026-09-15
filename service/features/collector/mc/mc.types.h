#pragma once

#include <cstdint>

namespace service::collector::mc {

enum class FrameFormat { Binary3E, Binary4E };

struct Connection {
    FrameFormat frame = FrameFormat::Binary3E;
    std::uint8_t network = 0;
    std::uint8_t station = 255;
    std::uint16_t moduleIo = 1023;
    std::uint8_t multidrop = 0;
    std::uint16_t monitoringTimer = 16;
    bool operator==(const Connection&) const = default;
};

struct Address {
    std::uint8_t deviceCode = 0xa8;
    std::uint32_t number = 0;
    std::uint16_t count = 1;
    bool bitAccess = false;
    bool operator==(const Address&) const = default;
};

} // namespace service::collector::mc
