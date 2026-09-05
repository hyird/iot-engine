#pragma once

#include <string_view>

namespace service::edge::terminal_state {

// Close only this terminal. Retain its final browser frame for the output pump.
inline constexpr std::string_view kFailScript = R"lua(
if redis.call('GET', KEYS[1]) ~= ARGV[1] then return 0 end
redis.call('DEL', KEYS[1], KEYS[2], KEYS[3], KEYS[4])
redis.call('RPUSH', KEYS[2], ARGV[2])
redis.call('EXPIRE', KEYS[2], 120)
return 1
)lua";

// KEYS: node session, terminal owner, output queue, input ACK, output sequence.
// Sequence state must live as long as the idle terminal, not just its last I/O.
// Check both owners atomically so an old browser cannot extend a replaced session.
inline constexpr std::string_view kRefreshScript = R"lua(
if redis.call('GET', KEYS[1]) ~= ARGV[1] then return -1 end
if redis.call('GET', KEYS[2]) ~= ARGV[1] then return 0 end
for i = 2, 5 do
    redis.call('EXPIRE', KEYS[i], ARGV[2])
end
return 1
)lua";

} // namespace service::edge::terminal_state
