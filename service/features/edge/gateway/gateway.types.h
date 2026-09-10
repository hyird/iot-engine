#pragma once

#include <ruvia/web/Model.h>

namespace service::edge::gateway {

RUVIA_REQUEST_MODEL(TerminalTicketQuery,
                   RUVIA_OPTIONAL_FIELD(ticket, ruvia::String));

} // namespace service::edge::gateway
