#pragma once

#include <ruvia/web/Model.h>

namespace service::system {

RUVIA_REQUEST_MODEL(OutboxEventIdParams,
                    RUVIA_OPTIONAL_FIELD(id, ruvia::String));

RUVIA_RESPONSE_MODEL(
    OutboxDeadLetterDto, RUVIA_OPTIONAL_FIELD(id, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("event_type", eventType, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("aggregate_type", aggregateType, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("aggregate_id", aggregateId, ruvia::String),
    RUVIA_OPTIONAL_FIELD(action, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("schema_version", schemaVersion, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(attempts, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("last_error", lastError, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("occurred_at", occurredAt, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("dead_lettered_at", deadLetteredAt,
                              ruvia::String));

RUVIA_RESPONSE_MODEL(
    OutboxDeadLetterListResponse, RUVIA_OPTIONAL_FIELD(code, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(message, ruvia::String),
    RUVIA_OPTIONAL_FIELD(data, ruvia::BoxedArray<OutboxDeadLetterDto>));

} // namespace service::system
