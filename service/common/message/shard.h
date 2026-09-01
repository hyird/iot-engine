#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace service::message::shard {

// Fixed shards keep queued work addressable when the Service Worker count changes.
// At runtime shard N belongs exclusively to Worker (N % workerCount).
inline constexpr std::size_t kCount = 64;

inline std::size_t index(std::string_view key) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto character : key) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ULL;
    }
    return static_cast<std::size_t>(hash % kCount);
}

} // namespace service::message::shard
