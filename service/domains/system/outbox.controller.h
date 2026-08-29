#pragma once

#include <charconv>
#include <optional>
#include <string>
#include <string_view>

#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/domains/system/outbox.schema.h"
#include "service/middleware/auth.h"
#include "service/middleware/permission.h"
#include "service/observability/registry.h"

namespace service::system {

class OutboxController final : public ruvia::Controller<OutboxController> {
public:
  RUVIA_CONTROLLER_GROUP("/v1/system/outbox",
                         service::middleware::AuthMiddleware)
  RUVIA_ROUTES_BEGIN
  RUVIA_GET("/dead-letters", deadLetters);
  RUVIA_POST("/dead-letters/:id/replay", replay, OutboxEventIdValidator);
  RUVIA_ROUTES_END

private:
  static std::int64_t integer(std::string_view value) {
    std::int64_t result{};
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), result);
    return error == std::errc{} && end == value.data() + value.size() ? result
                                                                      : 0;
  }

  static std::string id(ruvia::Context &context) {
    return std::string(
        context.req().validated<OutboxEventIdParams>().get<"id">()->view());
  }

  ruvia::Task<ruvia::HttpResponse> deadLetters(ruvia::Context &context) {
    co_await service::middleware::requirePermission(context,
                                                    "system:outbox:manage");
    const auto rows = co_await context.db().query(R"sql(
SELECT id::text, event_type, aggregate_type, aggregate_id, action,
       schema_version::text, attempts::text, COALESCE(last_error, ''),
       occurred_at::text, dead_lettered_at::text
FROM outbox_event
WHERE published_at IS NULL AND dead_lettered_at IS NOT NULL
ORDER BY dead_lettered_at, occurred_at, id
LIMIT 100)sql");
    ruvia::BoxedArray<OutboxDeadLetterDto> items(
        ruvia::ModelOptions{.resource = context.resource()});
    for (const auto &row : rows) {
      auto &item = items.emplace(context);
      item.set<"id">(row[0].value().value_or(std::string_view{}))
          .set<"eventType">(row[1].value().value_or(std::string_view{}))
          .set<"aggregateType">(row[2].value().value_or(std::string_view{}))
          .set<"aggregateId">(row[3].value().value_or(std::string_view{}))
          .set<"action">(row[4].value().value_or(std::string_view{}))
          .set<"schemaVersion">(
              integer(row[5].value().value_or(std::string_view{})))
          .set<"attempts">(integer(row[6].value().value_or(std::string_view{})))
          .set<"lastError">(row[7].value().value_or(std::string_view{}))
          .set<"occurredAt">(row[8].value().value_or(std::string_view{}))
          .set<"deadLetteredAt">(row[9].value().value_or(std::string_view{}));
    }
    co_return context.json(service::common::ok<OutboxDeadLetterListResponse>(
        context, std::move(items)));
  }

  ruvia::Task<ruvia::HttpResponse> replay(ruvia::Context &context) {
    co_await service::middleware::requirePermission(context,
                                                    "system:outbox:manage");
    const auto eventId = id(context);
    const auto rows =
        co_await context.db().query(R"sql(
UPDATE outbox_event
SET attempts = 0, last_error = NULL, available_at = NOW(), dead_lettered_at = NULL
WHERE id = $1::uuid AND published_at IS NULL AND dead_lettered_at IS NOT NULL
RETURNING id::text)sql",
                                    service::common::dbParams(eventId));
    if (rows.empty())
      service::common::fail(service::common::kNotFoundErrorCode,
                            "死信事件不存在或已重放", 404);
    if (auto *registry = service::observability::processRegistry())
      registry->increment("iot_engine_outbox_dead_letter_replays_total");
    co_return context.json(
        service::common::operation(context, "死信事件已重新入队"));
  }
};

} // namespace service::system
