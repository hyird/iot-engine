#pragma once

#include <string>
#include <string_view>

#include <ruvia/web/Context.h>

#include "service/features/runtime/reconciler.h"

namespace service::message {

inline ruvia::Task<void> publishConfigEvent(ruvia::Context& context, std::string_view aggregate,
                                            std::string_view action, std::string_view aggregateId) {
    // CRUD only marks the PostgreSQL-backed projection dirty. The single reconciler publishes one
    // notification to every worker-specific config Stream after the versioned snapshot is ready.
    // Publishing here would race the transactionally projected active version.
    (void)context;
    (void)aggregate;
    (void)action;
    (void)aggregateId;
    service::runtime::requestProjection();
    co_return;
}

} // namespace service::message
