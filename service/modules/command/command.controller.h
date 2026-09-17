#pragma once
#include <ruvia/web/Controller.h>

#include "service/middleware/auth.h"
#include "service/modules/command/command.schema.h"
#include "service/modules/command/command.service.h"

namespace service::command {
class CommandController final : public ruvia::Controller<CommandController> {
  public:
    RUVIA_CONTROLLER_GROUP("/v1/device", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_POST("/:id/commands", create, CommandStatusValidator, SubmitCommandValidator);
    RUVIA_ROUTES_END

  private:
    static ruvia::Task<void> authorize(service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:device:command");
    }

    ruvia::Task<ruvia::HttpResponse> create(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await authorize(request);
        const auto& id = c.req().validated<CommandStatusQuery>().get<"id">();
        co_return c.json(service::common::ok<service::device::DeviceCommandCreateResponse>(request, co_await commandService().create(request, id.view(), c.req().validated<service::device::DeviceCommandBody>())));
    }
};
} // namespace service::command
