#pragma once

#include <cstddef>
#include <optional>

namespace service {

struct ServiceWorkerTopology final {
    const std::size_t count;
    std::optional<std::size_t> index;
};

} // namespace service
