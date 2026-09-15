#pragma once

#include <cstdint>

namespace service::collector::fins {

struct Connection {
    std::uint8_t destinationNetwork = 0;
    std::uint8_t destinationNode = 0;
    std::uint8_t destinationUnit = 0;
    std::uint8_t sourceNetwork = 0;
    std::uint8_t sourceNode = 0;
    std::uint8_t sourceUnit = 0;
    bool operator==(const Connection&) const = default;
};

struct Address {
    std::uint8_t memoryArea = 0x82;
    std::uint16_t word = 0;
    std::uint8_t bit = 0;
    std::uint16_t count = 1;
    bool bitAccess = false;
    bool operator==(const Address&) const = default;
};

} // namespace service::collector::fins
