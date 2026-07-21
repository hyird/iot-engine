#pragma once

#include <cstdint>
#include <string_view>

#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/middleware/auth.h"
#include "service/middleware/permission.h"
#include "service/modules/iot/device/device.schema.h"
#include "service/modules/iot/device-group/device-group.service.h"

namespace service::device_group {

class DeviceGroupController final : public ruvia::Controller<DeviceGroupController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api/device-groups", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/tree-count", treeCount);
    RUVIA_GET("/tree", tree);
    RUVIA_GET("/:id", detail, service::device::DeviceIdParamsValidator);
    RUVIA_POST("/", create);
    RUVIA_PUT("/:id", update, service::device::DeviceIdParamsValidator);
    RUVIA_DELETE("/:id", remove, service::device::DeviceIdParamsValidator);
    RUVIA_ROUTES_END

  private:
    static std::int64_t id(ruvia::Context& c) {
        return static_cast<std::int64_t>(*c.req().valid<service::device::DeviceIdParams>().id());
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
    ruvia::Task<ruvia::HttpResponse> tree(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device-group:query");
        co_return data(c, co_await deviceGroupService().list(c, false));
    }
    ruvia::Task<ruvia::HttpResponse> treeCount(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device-group:query");
        co_return data(c, co_await deviceGroupService().list(c, true));
    }
    ruvia::Task<ruvia::HttpResponse> detail(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device-group:query");
        co_return data(c, co_await deviceGroupService().detail(c, id(c)));
    }
    ruvia::Task<ruvia::HttpResponse> create(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device-group:add");
        co_await deviceGroupService().create(c, co_await c.req().json());
        co_return c.json(service::common::operation(c, "创建成功"));
    }
    ruvia::Task<ruvia::HttpResponse> update(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device-group:edit");
        co_await deviceGroupService().update(c, id(c), co_await c.req().json());
        co_return c.json(service::common::operation(c, "更新成功"));
    }
    ruvia::Task<ruvia::HttpResponse> remove(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:device-group:delete");
        co_await deviceGroupService().remove(c, id(c));
        co_return c.json(service::common::operation(c, "删除成功"));
    }
};

} // namespace service::device_group
