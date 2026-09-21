#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>

#include <ruvia/web/Controller.h>
#include <ruvia/web/Testing.h>

#include "service/modules/gb28181/gb28181.types.h"

namespace {
using namespace service::gb28181;

class GbRequestRoutes final : public ruvia::Controller<GbRequestRoutes> {
  public:
    RUVIA_CONTROLLER_GROUP("/gb-request")
    RUVIA_ROUTES_BEGIN
    RUVIA_POST("/name", name, ruvia::JsonBody<GbNameBody>);
    RUVIA_POST("/channel-name", channelName, ruvia::JsonBody<GbNameBody>);
    RUVIA_POST("/mapping", mapping, ruvia::JsonBody<GbMappingBody>);
    RUVIA_POST("/device/:deviceId/channel/:channelId/ptz/:action", ptz, ruvia::PathModel<GbPtzParams>, ruvia::JsonBody<GbPtzBody>);
    RUVIA_POST("/position", position, ruvia::JsonBody<GbPositionBody>);
    RUVIA_POST("/records", records, ruvia::JsonBody<GbRecordBody>);
    RUVIA_ROUTES_END

  private:
    ruvia::Task<ruvia::HttpResponse> name(ruvia::Context& c) {
        const auto name = service::utils::trim(c.req().validated<GbNameBody>().get<"name">().view());
        co_return c.text(std::string_view(name));
    }
    ruvia::Task<ruvia::HttpResponse> channelName(ruvia::Context& c) {
        const auto name = service::utils::trim(c.req().validated<GbNameBody>().get<"name">().view());
        co_return c.text(std::string_view(name));
    }
    ruvia::Task<ruvia::HttpResponse> mapping(ruvia::Context& c) {
        const auto id = service::utils::trim(c.req().validated<GbMappingBody>().get<"mappedDeviceId">().view());
        co_return c.text(std::string_view(id));
    }
    ruvia::Task<ruvia::HttpResponse> ptz(ruvia::Context& c) {
        const auto speed = std::to_string(c.req().validated<GbPtzBody>().get<"speed">().value_or(ruvia::Int64{80}).value);
        co_return c.text(std::string_view(speed));
    }
    ruvia::Task<ruvia::HttpResponse> position(ruvia::Context& c) {
        const auto& input = c.req().validated<GbPositionBody>();
        if (input.get<"pan">().value < 0 || input.get<"tilt">().value < -30 || input.get<"zoom">().value < 1) throw std::runtime_error("invalid position reached handler");
        co_return c.text("accepted");
    }
    ruvia::Task<ruvia::HttpResponse> records(ruvia::Context& c) {
        const auto& input = c.req().validated<GbRecordBody>();
        const auto times = service::common::canonicalUtcTimestamp(service::utils::trim(input.get<"startTime">().view())) + "/" + service::common::canonicalUtcTimestamp(service::utils::trim(input.get<"endTime">().view()));
        co_return c.text(std::string_view(times));
    }
};

void expect(ruvia::TestApp& app, const ruvia::TestRequest& request, int status, std::string_view body = {}) {
    const auto response = app.request(request);
    if (response.status().value() != status || (!body.empty() && response.body() != body)) {
        throw std::runtime_error("unexpected GB request result: " + std::to_string(response.status().value()) + " " + std::string(response.body()));
    }
}

void testPtzValidation(ruvia::TestApp& app) {
    const auto path = "/gb-request/device/device/channel/channel/ptz/left";
    expect(app, ruvia::TestRequest::post(path).json("{}"), 200, "80");
    for (const auto value : {"0", "255", "80"}) {
        expect(app, ruvia::TestRequest::post(path).json(std::string("{\"speed\":") + value + "}"), 200, value);
    }
    expect(app, ruvia::TestRequest::post(path).json(R"({"speed":null})"), 200, "80");
    for (const auto value : {"-1", "256", "12.5", "\"80\"", "true", "{}", "[]"}) {
        expect(app, ruvia::TestRequest::post(path).json(std::string("{\"speed\":") + value + "}"), 400);
    }
    expect(app, ruvia::TestRequest::post(path).json(R"({"speed":1,"speed":2})"), 400);
    expect(app, ruvia::TestRequest::post("/gb-request/device/device/channel/channel/ptz/arbitrary").json("{}"), 400);
    expect(app, ruvia::TestRequest::post("/gb-request/position").json(R"({"pan":360,"tilt":-30,"zoom":1000})"), 200);
    expect(app, ruvia::TestRequest::post("/gb-request/position").json(R"({"pan":null,"tilt":0,"zoom":1})"), 400);
    for (const auto body : {R"({"pan":361,"tilt":0,"zoom":1})", R"({"pan":0,"tilt":91,"zoom":1})", R"({"pan":0,"tilt":0,"zoom":0})", R"({"pan":"1","tilt":0,"zoom":1})", R"({"pan":1e999,"tilt":0,"zoom":1})", R"({"pan":0,"tilt":0})"}) {
        expect(app, ruvia::TestRequest::post("/gb-request/position").json(body), 400);
    }
}

void testIdentityAndTimeValidation(ruvia::TestApp& app) {
    expect(app, ruvia::TestRequest::post("/gb-request/name").json(R"({"name":"  camera  "})"), 200, "camera");
    expect(app, ruvia::TestRequest::post("/gb-request/channel-name").json(R"({"name":"  channel  "})"), 200, "channel");
    for (const auto body : {"{}", R"({"name":" \t "})", R"({"name":null})", R"({"name":12})", "[]", "{"}) {
        expect(app, ruvia::TestRequest::post("/gb-request/name").json(body), 400);
    }
    expect(app, ruvia::TestRequest::post("/gb-request/name").json("{\"name\":\"" + std::string(256, 'x') + "\"}"), 400);
    expect(app, ruvia::TestRequest::post("/gb-request/name").body(R"({"name":"camera"})", "text/plain"), 415);
    expect(app, ruvia::TestRequest::post("/gb-request/name").body(R"({"name":"camera"})", "Application/JSON; charset=utf-8"), 200, "camera");
    expect(app, ruvia::TestRequest::post("/gb-request/mapping").json(R"({"mapped_device_id":"invalid"})"), 400);
    expect(app, ruvia::TestRequest::post("/gb-request/mapping").json(R"({"mapped_device_id":" 00000000-0000-7000-8000-000000000001 "})"), 200, "00000000-0000-7000-8000-000000000001");
    expect(app, ruvia::TestRequest::post("/gb-request/records").json(R"({"start_time":"2026-01-01T08:00:00+08:00","end_time":"2026-01-01T09:00:00+08:00"})"), 200, "2026-01-01T00:00:00Z/2026-01-01T01:00:00Z");
    expect(app, ruvia::TestRequest::post("/gb-request/records").json(R"({"start_time":"invalid","end_time":"2026-01-01T00:00:00Z"})"), 400);
}
} // namespace

int main() {
    try {
        ruvia::TestApp app;
        testPtzValidation(app);
        testIdentityAndTimeValidation(app);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "gb28181-request-test failed: %s\n", error.what());
        return 1;
    }
    return 0;
}
