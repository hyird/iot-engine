#pragma once

#include <string>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/WebWorker.h>

#include "service/modules/southbridge/runtime_config.redis.h"
#include "service/modules/northbridge/config/runtime_config.repository.h"

namespace service::northbridge::config {

template <typename Context>
inline ruvia::Task<std::string> projectRuntimeConfigLocked(Context& context) {
    auto transaction = co_await context.db().beginTransaction();
    // Every North Worker shares this transaction-scoped lock. The snapshot is loaded only after
    // earlier projections finish, so a slower request can never overwrite a newer DB state.
    (void)co_await transaction.execute(
        "SELECT pg_advisory_xact_lock(5282804697543808067::bigint)");
    auto snapshot =
        co_await service::northbridge::config::repository::loadRuntimeSnapshot(transaction);
    auto version =
        co_await service::southbridge::config_redis::project(context.redis(), snapshot);
    co_await transaction.commit();
    co_return version;
}

inline ruvia::Task<std::string> projectRuntimeConfig(ruvia::Context& context) {
    co_return co_await projectRuntimeConfigLocked(context);
}

inline ruvia::Task<std::string> projectRuntimeConfig(ruvia::WebWorkerContext& context) {
    co_return co_await projectRuntimeConfigLocked(context);
}

} // namespace service::northbridge::config
