#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/middleware/live.h"
#include "service/middleware/auth.h"
#include "service/modules/protocol/protocol.schema.h"
#include "service/modules/protocol/protocol.service.h"

namespace service::protocol {

class ProtocolController final : public ruvia::Controller<ProtocolController> {
  public:
    RUVIA_CONTROLLER_GROUP("/v1/protocol/configs", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET_SSE("/", list, ProtocolListQueryValidator);
    RUVIA_GET_SSE("/options", options, ProtocolListQueryValidator);
    RUVIA_GET_SSE("/:id", detail, ProtocolIdParamsValidator);
    RUVIA_GET_SSE("/:id/revisions", revisions, ProtocolIdParamsValidator);
    RUVIA_POST("/", create);
    RUVIA_POST("/:id/revisions", publish, ProtocolIdParamsValidator);
    RUVIA_DELETE("/:id", remove, ProtocolIdParamsValidator);
    RUVIA_ROUTES_END

  private:
    static std::string id(ruvia::Context& c) {
        return std::string(c.req().validated<ProtocolIdParams>().get<"id">()->view());
    }

    ruvia::Task<void> list(ruvia::Context& c) {
        co_await service::live::serve(c, "protocol", [this, &c]() { return listSnapshot(c); });
    }

    ruvia::Task<std::string> listSnapshot(ruvia::Context& c) {
        const auto& query = c.req().validated<ProtocolListQuery>();
        const auto& protocolValue = query.get<"protocol">();
        const auto protocol = protocolValue
                                  ? std::optional<std::string>(protocolValue->view())
                                  : std::nullopt;
        const auto data =
            co_await protocolService().list(c, static_cast<std::int64_t>(*query.get<"page">()),
                                            static_cast<std::int64_t>(*query.get<"pageSize">()), protocol);
        co_return service::live::data(c, data);
    }

    ruvia::Task<void> options(ruvia::Context& c) {
        co_await service::live::serve(c, "protocol", [this, &c]() { return optionsSnapshot(c); });
    }

    ruvia::Task<std::string> optionsSnapshot(ruvia::Context& c) {
        const auto& query = c.req().validated<ProtocolListQuery>();
        const auto& protocol = query.get<"protocol">();
        if (!protocol)
            service::common::fail(16003, "protocol 不能为空", 400);
        const auto data = co_await protocolService().options(
            c, std::string(protocol->view()), static_cast<std::int64_t>(*query.get<"page">()),
            static_cast<std::int64_t>(*query.get<"pageSize">()));
        co_return service::live::data(c, data);
    }

    ruvia::Task<void> detail(ruvia::Context& c) {
        co_await service::live::serve(c, "protocol", [this, &c]() { return detailSnapshot(c); });
    }

    ruvia::Task<std::string> detailSnapshot(ruvia::Context& c) {
        const auto modelId = id(c);
        const auto data = co_await protocolService().detail(c, modelId);
        co_return service::live::data(c, data);
    }

    ruvia::Task<ruvia::HttpResponse> create(ruvia::Context& c) {
        const auto payload = co_await c.req().jsonValue();
        co_await protocolService().create(c, payload);
        co_return c.json(service::common::operation(c, "创建成功"));
    }

    ruvia::Task<void> revisions(ruvia::Context& c) {
        co_await service::live::serve(c, "protocol", [this, &c]() {
            return revisionsSnapshot(c);
        });
    }

    ruvia::Task<std::string> revisionsSnapshot(ruvia::Context& c) {
        const auto modelId = id(c);
        co_return service::live::data(c, co_await protocolService().revisions(c, modelId));
    }

    ruvia::Task<ruvia::HttpResponse> publish(ruvia::Context& c) {
        const auto payload = co_await c.req().jsonValue();
        const auto modelId = id(c);
        co_await protocolService().update(c, modelId, payload);
        co_return c.json(service::common::operation(c, "新版本已发布，设备需显式切换版本"));
    }

    ruvia::Task<ruvia::HttpResponse> remove(ruvia::Context& c) {
        const auto modelId = id(c);
        co_await protocolService().remove(c, modelId);
        co_return c.json(service::common::operation(c, "删除成功"));
    }
};

} // namespace service::protocol
