#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/middleware/auth.h"
#include "service/middleware/permission.h"
#include "service/domains/protocol/protocol.schema.h"
#include "service/domains/protocol/protocol.service.h"

namespace service::protocol {

class ProtocolController final : public ruvia::Controller<ProtocolController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api/protocol/configs", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/", list, ProtocolListQueryValidator);
    RUVIA_GET("/options", options, ProtocolListQueryValidator);
    RUVIA_GET("/:id", detail, ProtocolIdParamsValidator);
    RUVIA_POST("/", create);
    RUVIA_PUT("/:id", update, ProtocolIdParamsValidator);
    RUVIA_DELETE("/:id", remove, ProtocolIdParamsValidator);
    RUVIA_ROUTES_END

  private:
    static std::string id(ruvia::Context& c) {
        return std::string(c.req().valid<ProtocolIdParams>().id()->view());
    }

    static ruvia::HttpResponse jsonData(ruvia::Context& c, std::string_view data) {
        std::pmr::string body(c.allocator<char>());
        body.append("{\"code\":0,\"message\":\"ok\",\"data\":");
        body.append(data);
        body.push_back('}');
        auto response = c.body(std::move(body));
        response.header("Content-Type", "application/json; charset=UTF-8");
        return response;
    }

    ruvia::Task<ruvia::HttpResponse> list(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:protocol:query");
        const auto& query = c.req().valid<ProtocolListQuery>();
        const auto protocol =
            query.protocol() ? std::optional<std::string>(query.protocol()->view()) : std::nullopt;
        const auto data =
            co_await protocolService().list(c, static_cast<std::int64_t>(*query.page()),
                                            static_cast<std::int64_t>(*query.pageSize()), protocol);
        co_return jsonData(c, data);
    }

    ruvia::Task<ruvia::HttpResponse> options(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:protocol:query");
        const auto& query = c.req().valid<ProtocolListQuery>();
        if (!query.protocol())
            service::common::fail(16003, "protocol 不能为空", 400);
        const auto data = co_await protocolService().options(
            c, std::string(query.protocol()->view()), static_cast<std::int64_t>(*query.page()),
            static_cast<std::int64_t>(*query.pageSize()));
        co_return jsonData(c, data);
    }

    ruvia::Task<ruvia::HttpResponse> detail(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:protocol:query");
        const auto data = co_await protocolService().detail(c, id(c));
        co_return jsonData(c, data);
    }

    ruvia::Task<ruvia::HttpResponse> create(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:protocol:add");
        const auto payload = co_await c.req().json();
        co_await protocolService().create(c, payload);
        co_return c.json(service::common::operation(c, "创建成功"));
    }

    ruvia::Task<ruvia::HttpResponse> update(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:protocol:edit");
        const auto payload = co_await c.req().json();
        co_await protocolService().update(c, id(c), payload);
        co_return c.json(service::common::operation(c, "更新成功"));
    }

    ruvia::Task<ruvia::HttpResponse> remove(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:protocol:delete");
        co_await protocolService().remove(c, id(c));
        co_return c.json(service::common::operation(c, "删除成功"));
    }
};

} // namespace service::protocol
