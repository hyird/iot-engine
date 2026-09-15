#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace service::edge::terminal_state {

enum class InputAckStatus { Pending, Acknowledged, OwnershipLost };

struct ConnectionIdentity final {
    std::string_view nodeId;
    std::uint64_t epoch{};
    std::uint32_t protocolVersion{};
    std::size_t workerIndex{};
};

} // namespace service::edge::terminal_state
