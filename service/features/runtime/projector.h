#pragma once

#include <string>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/WebWorker.h>

#include "service/features/collector/config.h"
#include "service/features/runtime/repository.h"

namespace service::runtime {

template <typename Context>
inline ruvia::Task<std::string> projectLocked(Context& context) {
    auto transaction = co_await context.db().beginTransaction();
    // Every Service Worker shares this transaction-scoped lock. The snapshot is loaded only after
    // earlier projections finish, so a slower request can never overwrite a newer DB state.
    (void)co_await transaction.execute(
        "SELECT pg_advisory_xact_lock(5282804697543808067::bigint)");
    auto snapshot =
        co_await service::runtime::repository::loadRuntimeSnapshot(transaction);
    auto version =
        co_await service::collector::config::project(context.redis(), snapshot);
    co_await transaction.commit();
    co_return version;
}

inline ruvia::Task<std::string> project(ruvia::Context& context) {
    co_return co_await projectLocked(context);
}

inline ruvia::Task<std::string> project(ruvia::WebWorkerContext& context) {
    co_return co_await projectLocked(context);
}

} // namespace service::runtime
