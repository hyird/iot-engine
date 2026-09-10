#pragma once

#include <string>
#include <string_view>

#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/middleware/live.h"
#include "service/modules/system/outbox/outbox.schema.h"
#include "service/modules/system/outbox/outbox.service.h"
#include "service/middleware/auth.h"

namespace service::system {

class OutboxController final : public ruvia::Controller<OutboxController> {
public:
  RUVIA_CONTROLLER_GROUP("/v1/system/outbox",
                         service::middleware::AuthMiddleware)
  RUVIA_ROUTES_BEGIN
  RUVIA_GET_SSE("/dead-letters", deadLetters);
  RUVIA_POST("/dead-letters/:id/replay", replay, OutboxEventIdValidator);
  RUVIA_ROUTES_END

private:
  static std::string id(ruvia::Context &context) {
    return std::string(
        context.req().validated<OutboxEventIdParams>().get<"id">()->view());
  }

  ruvia::Task<void> deadLetters(ruvia::Context& context) {
    co_await service::live::serve(context, "system",
                                  [this, &context]() { return deadLettersSnapshot(context); });
  }

  ruvia::Task<std::string> deadLettersSnapshot(ruvia::Context& context) {
    co_return service::live::json(service::common::ok<OutboxDeadLetterListResponse>(
        context, co_await outboxService().deadLetters(context)));
  }

  ruvia::Task<ruvia::HttpResponse> replay(ruvia::Context &context) {
    const auto eventId = id(context);
    co_await outboxService().replay(context, eventId);
    co_return context.json(service::common::operation(context, "死信事件已重新入队"));
  }
};

} // namespace service::system
