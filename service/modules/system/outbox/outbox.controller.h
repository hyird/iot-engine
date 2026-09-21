#pragma once

#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/common/worker.h"
#include "service/middleware/auth.h"
#include "service/middleware/live.h"
#include "service/middleware/permission.h"
#include "service/modules/system/outbox/outbox.types.h"
#include "service/modules/system/outbox/outbox.service.h"

namespace service::system {
class OutboxController final : public ruvia::Controller<OutboxController> {
  public:
    RUVIA_CONTROLLER_GROUP("/v1/system/outbox", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/dead-letters", deadLetters, service::middleware::PermissionMiddleware<"system:outbox:manage">);
    RUVIA_GET_SSE("/dead-letters/events", deadLetterEvents, service::middleware::PermissionMiddleware<"system:outbox:manage">);
    RUVIA_POST("/dead-letters/:id/replay", replay, service::middleware::PermissionMiddleware<"system:outbox:manage">, ruvia::PathModel<OutboxEventIdParams>);
    RUVIA_ROUTES_END

  private:
    ruvia::Task<OutboxDeadLetterListResponse> queryDeadLetter(service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "system:outbox:manage");
        co_return service::common::ok<OutboxDeadLetterListResponse>(request, co_await outboxService().deadLetters(request));
    }

    ruvia::Task<ruvia::HttpResponse> deadLetters(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_return c.json(service::common::ok<OutboxDeadLetterListResponse>(request, co_await outboxService().deadLetters(request)));
    }

    ruvia::Task<void> deadLetterEvents(ruvia::Context& c) {
        co_await service::live::serveSnapshots(c, "system", service::middleware::requireAuth(c).userId, [this](service::middleware::RequestContext& request) -> ruvia::Task<std::string> {
            co_return service::live::json(co_await queryDeadLetter(request));
        },
                                               [&c] {
                                                   (void)service::middleware::requireAuth(c);
                                               });
    }

    ruvia::Task<ruvia::HttpResponse> replay(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto index = c.workerState<service::ServiceWorkerTopology>().index.value();
        co_await outboxService().replay(request, c.req().validated<OutboxEventIdParams>().get<"id">().view(), index);
        co_return c.json(service::common::operation(c, "死信事件已重新入队"));
    }
};
} // namespace service::system
