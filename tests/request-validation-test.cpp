#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>

#include <ruvia/web/Testing.h>
#include <ruvia/web/Controller.h>

#include "service/modules/device/device.types.h"
#include "service/modules/edge_node/edge_node.types.h"
#include "service/modules/gb28181/gb28181.types.h"
#include "service/modules/system/user/user.types.h"
#include "service/utils/json.h"
#include "service/middleware/request_context.h"

namespace {
static_assert(std::is_same_v<ruvia::Task<ruvia::HttpResponse>::value_type, ruvia::HttpResponse>);
RUVIA_RESPONSE_MODEL(CreatedResponse,
    RUVIA_REQUIRED_FIELD(created, ruvia::Bool),
    RUVIA_REQUIRED_FIELD(name, ruvia::String));

class ValidationRoutes final : public ruvia::Controller<ValidationRoutes> {
  public:
    RUVIA_CONTROLLER_GROUP("/validation")
    RUVIA_ROUTES_BEGIN
    RUVIA_POST("/typed-response", typedResponse);
    RUVIA_POST("/devices", createDevice, ruvia::JsonBody<service::device::CreateDeviceBody>);
    RUVIA_PUT("/devices", updateDevice, ruvia::JsonBody<service::device::UpdateDeviceBody>);
    RUVIA_POST("/debug", debug, ruvia::JsonBody<service::device::DeviceDebugBody>);
    RUVIA_POST("/network", network, ruvia::JsonBody<service::edge::NetworkBody>);
    RUVIA_POST("/user", createUser, ruvia::JsonBody<service::user::CreateUserBody>);
    RUVIA_GET("/users", query, ruvia::QueryModel<service::user::UserListQuery>);
    RUVIA_GET("/device/:deviceId", device, ruvia::PathModel<service::gb28181::GbDeviceParams>);
    RUVIA_GET("/device/:deviceId/channel/:channelId", channel, ruvia::PathModel<service::gb28181::GbChannelParams>);
    RUVIA_ROUTES_END

  private:
    ruvia::Task<ruvia::HttpResponse> typedResponse(ruvia::Context& c) {
        service::middleware::RequestContext request(c, "test-user");
        if (request.arena() == c.arena()) throw std::runtime_error("业务操作未隔离临时内存");
        service::middleware::RequestContext snapshot(c, "test-user");
        if (snapshot.arena() == c.arena()) throw std::runtime_error("事件快照未隔离临时内存");
        CreatedResponse response({ .resource = request.arena() });
        response.set<"created">(true).set<"name">(std::string(8192, 'x') + "设备\"一");
        c.status(ruvia::http_status::kCreated);
        co_return c.json(response);
    }

    ruvia::Task<ruvia::HttpResponse> createDevice(ruvia::Context& c) {

        co_return c.text("accepted");
    }
    ruvia::Task<ruvia::HttpResponse> updateDevice(ruvia::Context& c) {

        co_return c.text("accepted");
    }
    ruvia::Task<ruvia::HttpResponse> network(ruvia::Context& c) {

        co_return c.text("accepted");
    }
    ruvia::Task<ruvia::HttpResponse> createUser(ruvia::Context& c) {

        co_return c.text("accepted");
    }
    ruvia::Task<ruvia::HttpResponse> device(ruvia::Context& c) {

        co_return c.text("accepted");
    }
    ruvia::Task<ruvia::HttpResponse> channel(ruvia::Context& c) {

        co_return c.text("accepted");
    }
    ruvia::Task<ruvia::HttpResponse> debug(ruvia::Context& c) {

        co_return c.text(std::string_view(c.req().validated<service::device::DeviceDebugBody>().get<"enabled">() ? "on" : "off"));
    }
    ruvia::Task<ruvia::HttpResponse> query(ruvia::Context& c) {
        const auto& body = c.req().validated<service::user::UserListQuery>();
        const auto page = std::to_string(body.get<"page">()->value);
        co_return c.text(std::string_view(page));
    }
};

void expect(ruvia::TestApp& app, const ruvia::TestRequest& request, int status,
            std::string_view message = {}) {
    const auto response = app.request(request);
    if (response.status().value() != status ||
        (!message.empty() && response.body().find(message) == std::string_view::npos)) {
        throw std::runtime_error("请求校验结果不符: " + std::to_string(response.status().value()) +
                                 " " + std::string(response.body()));
    }
}
} // namespace

int main() {
    try {
        for (const auto value : {"application/json", "Application/JSON; charset=utf-8", "\tapplication/json \t; charset=utf-8"}) {
            if (!service::utils::isJsonContentType(value)) throw std::runtime_error("合法 JSON 媒体类型被拒绝");
        }
        for (const auto value : {"", " \t", "application/jsonp", "application/problem+json", "text/plain"}) {
            if (service::utils::isJsonContentType(value)) throw std::runtime_error("非 JSON 媒体类型被接受");
        }
        ruvia::TestApp app;
        const auto typed = app.request(ruvia::TestRequest::post("/validation/typed-response"));
        if (typed.status().value() != 201 ||
            !typed.header("Content-Type").value_or("").starts_with("application/json") ||
            typed.body() != std::string(R"({"created":true,"name":")") + std::string(8192, 'x') + R"(设备\"一"})") {
            throw std::runtime_error("类型化响应未保持状态码或 JSON 序列化");
        }
        expect(app, ruvia::TestRequest::post("/validation/devices").json("{}"), 400, "name");
        expect(app, ruvia::TestRequest::put("/validation/devices").json("{}"), 200);
        expect(app, ruvia::TestRequest::put("/validation/devices").json(R"({"name":""})"), 400);
        expect(app, ruvia::TestRequest::put("/validation/devices").json(R"({"protocol_config_id":""})"), 200);
        expect(app, ruvia::TestRequest::post("/validation/devices").json(R"({"name":"device","device_code":"device","protocol_config_id":""})"), 400);
        expect(app, ruvia::TestRequest::post("/validation/devices").json(R"({"name":"device","device_code":"device","protocol_config_id":"01994172-9ad0-7000-8000-000000000001"})"), 200);
        expect(app, ruvia::TestRequest::post("/validation/debug").json(R"({"enabled":false})"), 200, "off");
        for (const auto body : {"{}", R"({"enabled":null})", R"({"enabled":"false"})", R"({"enabled":true,"enabled":false})", "{"}) {
            expect(app, ruvia::TestRequest::post("/validation/debug").json(body), 400);
        }
        expect(app, ruvia::TestRequest::post("/validation/debug").body(R"({"enabled":false})", "text/plain"), 415);
        expect(app, ruvia::TestRequest::post("/validation/debug").body(R"({"enabled":false})", "Application/JSON; charset=utf-8"), 200);
        expect(app, ruvia::TestRequest::post("/validation/network").json(R"({"interfaces":[{"operation":"upsert","name":"lan"}]})"), 200);
        expect(app, ruvia::TestRequest::post("/validation/network").json(R"({"interfaces":[]})"), 400);
        expect(app, ruvia::TestRequest::post("/validation/network").json(R"({"interfaces":[{"operation":"upsert","name":"bad-name"}]})"), 400, "interfaces[0].name");
        expect(app, ruvia::TestRequest::post("/validation/network").json(R"({"interfaces":[{"operation":"upsert","name":"lan","prefixLength":31}]})"), 400, "interfaces[0].prefixLength");
        expect(app, ruvia::TestRequest::post("/validation/network").json(R"({"interfaces":[null]})"), 400);
        expect(app, ruvia::TestRequest::post("/validation/user").json(R"({"username":"tester","password":"123456","role_ids":["role"],"email":"invalid"})"), 400, "email");
        expect(app, ruvia::TestRequest::get("/validation/users"), 200, "1");
        expect(app, ruvia::TestRequest::get("/validation/users?page=2&pageSize=100"), 200, "2");
        expect(app, ruvia::TestRequest::get("/validation/users?pageSize=101"), 400);
        expect(app, ruvia::TestRequest::get("/validation/users?page=invalid"), 400);
        expect(app, ruvia::TestRequest::get("/validation/device/camera"), 200);
        expect(app, ruvia::TestRequest::get("/validation/device/camera/channel/channel1"), 200);
        expect(app, ruvia::TestRequest::get("/validation/device/%20"), 400);
        std::cout << "HTTP 请求绑定、字段约束、操作差异及嵌套校验通过\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
