#pragma once
#include <ruvia/web/db/DbEntity.h>
#include "service/common/message.h"
namespace service::system {
RUVIA_DB_ENTITY(WorkerSnapshotEntity, service::message::worker_metrics::kSnapshotTable,
    RUVIA_DB_COLUMN(id, ruvia::String, ruvia::DbColumnOptions{.primaryKey = true}),
    RUVIA_DB_COLUMN(metrics, ruvia::String),
    RUVIA_DB_COLUMN(ready, bool),
    RUVIA_DB_COLUMN(health, ruvia::String));
}
