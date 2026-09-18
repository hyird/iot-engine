#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <ruvia/web/Controller.h>
#include "service/utils/json.h"

#include "service/common/http.h"
#include "service/middleware/auth.h"
#include "service/middleware/live.h"
#include "service/modules/protocol/protocol.types.h"
#include "service/modules/protocol/protocol.service.h"

namespace service::protocol {

class ProtocolController final : public ruvia::Controller<ProtocolController> {
  public:
    RUVIA_CONTROLLER_GROUP("/v1/protocol/configs", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/", list, ruvia::QueryModel<ProtocolListQuery>);
    RUVIA_GET("/options", options, ruvia::QueryModel<ProtocolListQuery>);
    RUVIA_GET("/:id", detail, ruvia::PathModel<ProtocolIdParams>);
    RUVIA_POST("/", create);
    RUVIA_PUT("/:id", update, ruvia::PathModel<ProtocolIdParams>);
    RUVIA_DELETE("/:id", remove, ruvia::PathModel<ProtocolIdParams>);
    RUVIA_ROUTES_END

  private:
    static std::string id(ruvia::Context& c) {
        return std::string(c.req().validated<ProtocolIdParams>().get<"id">().view());
    }

    ruvia::Task<> list(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:protocol:query");
        const auto& query = c.req().validated<ProtocolListQuery>();
        const auto& protocolValue = query.get<"protocol">();
        const auto protocol = protocolValue
            ? std::optional<std::string>(protocolValue->view())
            : std::nullopt;
        const auto data =
            co_await protocolService().list(request, static_cast<std::int64_t>(*query.get<"page">()), static_cast<std::int64_t>(*query.get<"pageSize">()), protocol);
        c.header("Content-Type", "application/json");
        const auto payload = service::live::data(c, data);
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<> options(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:protocol:query");
        const auto& query = c.req().validated<ProtocolListQuery>();
        const auto& protocol = query.get<"protocol">();
        if (!protocol) {
            service::common::fail(16003, "protocol 不能为空", 400);
        }
        const auto data = co_await protocolService().options(request, std::string(protocol->view()), static_cast<std::int64_t>(*query.get<"page">()), static_cast<std::int64_t>(*query.get<"pageSize">()));
        c.header("Content-Type", "application/json");
        const auto payload = service::live::data(c, data);
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<> detail(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:protocol:query");
        const auto modelId = id(c);
        const auto data = co_await protocolService().detail(request, modelId);
        c.header("Content-Type", "application/json");
        const auto payload = service::live::data(c, data);
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<> create(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:protocol:add");
        if (!service::utils::isJsonContentType(c.req().header("Content-Type").value_or(std::string_view{}))) {
            throw ruvia::HttpError({ .status = ruvia::http_status::kUnsupportedMediaType,
                .code = "unsupported_media_type", .message = "request body must be application/json" });
        }
        const auto rawBody = co_await c.req().text();
        auto parsedJson = ruvia::JsonValue::parse(rawBody, { .resource = c.arena() });
        if (!parsedJson) {
            throw ruvia::HttpError({ .status = ruvia::http_status::kBadRequest, .message = "invalid json body" });
        }
        const auto json = std::move(*parsedJson);
        const auto payload = CreateProtocolBody::parse(json);
        co_await protocolService().create(request, payload);
        co_return c.json(service::common::operation(c, "创建成功"));
    }

    ruvia::Task<> update(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:protocol:edit");
        if (!service::utils::isJsonContentType(c.req().header("Content-Type").value_or(std::string_view{}))) {
            throw ruvia::HttpError({ .status = ruvia::http_status::kUnsupportedMediaType,
                .code = "unsupported_media_type", .message = "request body must be application/json" });
        }
        const auto rawBody = co_await c.req().text();
        auto parsedJson = ruvia::JsonValue::parse(rawBody, { .resource = c.arena() });
        if (!parsedJson) {
            throw ruvia::HttpError({ .status = ruvia::http_status::kBadRequest, .message = "invalid json body" });
        }
        const auto json = std::move(*parsedJson);
        const auto payload = UpdateProtocolBody::parse(json);
        const auto modelId = id(c);
        co_await protocolService().update(request, modelId, payload);
        co_return c.json(service::common::operation(c, "更新成功"));
    }

    ruvia::Task<> remove(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:protocol:delete");
        const auto modelId = id(c);
        co_await protocolService().remove(request, modelId);
        co_return c.json(service::common::operation(c, "删除成功"));
    }
};

} // namespace service::protocol
