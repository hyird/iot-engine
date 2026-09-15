#pragma once

#include <cstdint>
#include <string>
#include <ruvia/web/Model.h>

namespace service::edge::gateway {

RUVIA_REQUEST_MODEL(TerminalTicketQuery,
                   RUVIA_OPTIONAL_FIELD(ticket, ruvia::String));

struct FirmwareSource final {
    std::string storagePath;
    std::uint64_t sizeBytes{};
};

} // namespace service::edge::gateway
