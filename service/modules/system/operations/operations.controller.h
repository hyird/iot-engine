#pragma once

#include <string>
#include <string_view>
#include <memory_resource>

#include <ruvia/http/HttpStatus.h>
#include <ruvia/web/Controller.h>

#include "service/modules/system/operations/operations.service.h"

namespace service::system {

class OperationsController final : public ruvia::Controller<OperationsController> {
  public:
    RUVIA_CONTROLLER_GROUP("/internal")
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/health/live", live);
    RUVIA_GET("/health/ready", ready);
    RUVIA_GET("/metrics", metrics);
    RUVIA_ROUTES_END

  private:
    ruvia::Task<ruvia::HttpResponse> live(ruvia::Context& context) {
        std::pmr::string body("{\"status\":\"alive\"}", context.arena());
        auto response = context.body(std::move(body));
        response.header("Content-Type", "application/json; charset=UTF-8");
        co_return response;
    }

    ruvia::Task<ruvia::HttpResponse> ready(ruvia::Context& context) {
        const auto result = co_await OperationsService::readiness(context);
        if (!result.ready)
            context.status(ruvia::http_status::kServiceUnavailable);
        std::pmr::string body(result.json, context.arena());
        auto response = context.body(std::move(body));
        response.header("Content-Type", "application/json; charset=UTF-8");
        co_return response;
    }

    ruvia::Task<ruvia::HttpResponse> metrics(ruvia::Context& context) {
        const auto text = co_await OperationsService::metrics(context);
        std::pmr::string body(text, context.arena());
        auto response = context.body(std::move(body));
        response.header("Content-Type", "text/plain; version=0.0.4; charset=UTF-8");
        co_return response;
    }
};

} // namespace service::system
