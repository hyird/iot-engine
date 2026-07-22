#pragma once

#include <string>
#include <string_view>

#include <ruvia/web/Context.h>

#include "service/modules/northbridge/config/runtime_config.reconciler.h"

namespace service::bridge {

inline ruvia::Task<void> publishConfigEvent(ruvia::Context& context, std::string_view aggregate,
                                            std::string_view action, std::string_view aggregateId) {
    // CRUD only marks the PostgreSQL-backed projection dirty. The single reconciler publishes one
    // notification to every worker-specific config Stream after the versioned snapshot is ready.
    // Publishing here would race the transactionally projected active version.
    (void)context;
    (void)aggregate;
    (void)action;
    (void)aggregateId;
    service::northbridge::config::requestRuntimeConfigProjection();
    co_return;
}

} // namespace service::bridge
