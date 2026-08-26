#pragma once

#include <string>
#include <memory_resource>

#include <ruvia/http/HttpStatus.h>
#include <ruvia/web/Controller.h>

#include "service/features/collector/stream.h"
#include "service/observability/registry.h"

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
        std::pmr::string body("{\"status\":\"alive\"}", context.resource());
        auto response = context.body(std::move(body));
        response.header("Content-Type", "application/json; charset=UTF-8");
        co_return response;
    }

    ruvia::Task<ruvia::HttpResponse> ready(ruvia::Context& context) {
        auto* registry = observability::processRegistry();
        bool databaseReady = false;
        bool redisReady = false;
        try {
            const auto database = co_await context.db().query("SELECT 1");
            databaseReady = !database.empty();
            const auto redis = co_await service::message::redis::command(
                context.redis(), {"PING"});
            redisReady = redis.kind() == ruvia::RedisValue::Kind::kString &&
                         redis.string() == "PONG";
        } catch (...) {
        }
        const bool dependenciesReady = databaseReady && redisReady;
        if (registry) {
            registry->gauge("iot_engine_database_ready", databaseReady ? 1 : 0);
            registry->gauge("iot_engine_redis_ready", redisReady ? 1 : 0);
        }
        const bool isReady = registry && registry->ready() && dependenciesReady;
        if (!isReady)
            context.status(ruvia::http_status::kServiceUnavailable);
        auto json = registry
                        ? registry->healthJson()
                        : std::string{"{\"status\":\"not_ready\",\"components\":{}}"};
        if (!dependenciesReady && json.starts_with("{\"status\":\"ready\""))
            json.replace(11, 5, "not_ready");
        std::pmr::string body(json, context.resource());
        auto response = context.body(std::move(body));
        response.header("Content-Type", "application/json; charset=UTF-8");
        co_return response;
    }

    ruvia::Task<ruvia::HttpResponse> metrics(ruvia::Context& context) {
        const auto* registry = observability::processRegistry();
        const auto text = registry ? registry->prometheus()
                                   : std::string{"iot_engine_ready 0\n"};
        std::pmr::string body(text, context.resource());
        auto response = context.body(std::move(body));
        response.header("Content-Type", "text/plain; version=0.0.4; charset=UTF-8");
        co_return response;
    }
};

} // namespace service::system
