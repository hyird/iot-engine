#pragma once
#include <ruvia/web/db/DbEntity.h>
#include <ruvia/web/redis/RedisEntity.h>
#include "service/common/message.h"
namespace service::system {
RUVIA_REDIS_ENTITY(WorkerSnapshotEntity, service::message::worker_metrics::kSnapshotTable,
    RUVIA_REDIS_COLUMN(id, ruvia::String, ruvia::RedisColumnOptions{.primaryKey = true}),
    RUVIA_REDIS_COLUMN(metrics, ruvia::String),
    RUVIA_REDIS_COLUMN(ready, bool),
    RUVIA_REDIS_COLUMN(health, ruvia::String));
}
