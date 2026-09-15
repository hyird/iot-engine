#pragma once

#include <span>
#include "service/features/packet_log/packet_log.config.h"

namespace service::packet_log {

void initialize(Config config);
void shutdown() noexcept;

void write(Level level, std::string_view event, const Context& context = {},
           std::span<const std::uint8_t> bytes = {}, std::string_view reason = {}) noexcept;

} // namespace service::packet_log
