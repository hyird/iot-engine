#pragma once

#include <cstdint>
#include <string_view>

#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/middleware/auth.h"
#include "service/middleware/permission.h"
#include "service/modules/iot/device/device.schema.h"
#include "service/modules/iot/device/device.service.h"

namespace service::device {

class DeviceController final : public ruvia::Controller<DeviceController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api/device", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/options", options);
    RUVIA_GET("/realtime", realtime);
    RUVIA_GET("/:id", detail, DeviceIdParamsValidator);
    RUVIA_GET("/", list);
    RUVIA_POST("/", create);
    RUVIA_PUT("/:id", update, DeviceIdParamsValidator);
    RUVIA_DELETE("/:id", remove, DeviceIdParamsValidator);
    RUVIA_ROUTES_END

  private:
    static std::int64_t id(ruvia::Context& c) {
        return static_cast<std::int64_t>(*c.req().valid<DeviceIdParams>().id());
    }
    static ruvia::HttpResponse data(ruvia::Context& c, std::string_view value) {
        std::pmr::string body(c.allocator<char>());
        body.append("{\"code\":0,\"message\":\"ok\",\"data\":");
        body.append(value);
        body.push_back('}');
        auto response = c.body(std::move(body));
        response.header("Content-Type", "application/json; charset=UTF-8");
        return response;
    }
    ruvia::Task<ruvia::HttpResponse> list(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device:query");
        co_return data(c, co_await deviceService().list(c));
    }
    ruvia::Task<ruvia::HttpResponse> realtime(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device:query");
        co_return data(c, co_await deviceService().realtime(c));
    }
    ruvia::Task<ruvia::HttpResponse> options(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device:query");
        co_return data(c, co_await deviceService().options(c));
    }
    ruvia::Task<ruvia::HttpResponse> detail(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device:query");
        co_return data(c, co_await deviceService().detail(c, id(c)));
    }
    ruvia::Task<ruvia::HttpResponse> create(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device:add");
        co_await deviceService().create(c, co_await c.req().json());
        co_return c.json(service::common::operation(c, "创建成功"));
    }
    ruvia::Task<ruvia::HttpResponse> update(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device:edit");
        co_await deviceService().update(c, id(c), co_await c.req().json());
        co_return c.json(service::common::operation(c, "更新成功"));
    }
    ruvia::Task<ruvia::HttpResponse> remove(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device:delete");
        co_await deviceService().remove(c, id(c));
        co_return c.json(service::common::operation(c, "删除成功"));
    }
};

} // namespace service::device
