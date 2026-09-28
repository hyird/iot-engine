#include <ruvia/web/Controller.h>
#include <ruvia/web/Testing.h>
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include "service/middleware/validation.h"
#include "service/modules/protocol/protocol.types.h"
#include "service/modules/alert/alert.types.h"
#include "service/modules/open_access/open_access.types.h"
#include "service/modules/vpn/vpn.types.h"
#include "service/modules/link/link.types.h"

namespace {
constexpr std::string_view kWebhookHeaderDiagnosticRequest = R"({"accessKeyId":"00000000-0000-7000-8000-000000000001","name":"sink","url":"https://example.test/hook","headers":{"X-Example":"ok"}})";
template <typename Body>
void printWebhookHeaderFailure(std::string_view route, const Body& body, const ruvia::JsonObject& headers,
    std::string_view rawRequest, std::pmr::memory_resource* resource);

RUVIA_MODEL(PresenceBody,
    RUVIA_REQUIRED_FIELD(requiredText, ruvia::String, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(enabled, ruvia::Bool, RUVIA_DEFAULT(true)),
    RUVIA_OPTIONAL_FIELD(nullableDefault, ruvia::Int64, RUVIA_NULLABLE, RUVIA_DEFAULT(7)));

class ModelMigrationRoutes final : public ruvia::Controller<ModelMigrationRoutes> {
  public:
    RUVIA_CONTROLLER_GROUP("/model-migration")
    RUVIA_ROUTES_BEGIN
    RUVIA_POST("/presence", presence, ruvia::JsonBody<PresenceBody>);
    RUVIA_PUT("/errors/:id", accept, ruvia::PathModel<service::protocol::ProtocolIdParams>, service::middleware::ValidationErrorCodeMiddleware<16002, "enabled", 16004>, ruvia::JsonBody<service::protocol::UpdateProtocolBody>);
    RUVIA_POST("/protocol", createProtocol, ruvia::JsonBody<service::protocol::CreateProtocolBody>);
    RUVIA_PUT("/protocol", updateProtocol, ruvia::JsonBody<service::protocol::UpdateProtocolBody>);
    RUVIA_POST("/rule", rule, ruvia::JsonBody<service::alert::RuleInput>);
    RUVIA_POST("/template", accept, ruvia::JsonBody<service::alert::TemplateInput>);
    RUVIA_POST("/batch", accept, ruvia::JsonBody<service::alert::AlertBatchBody>);
    RUVIA_POST("/key", accept, ruvia::JsonBody<service::access::CreateAccessKeyBody>);
    RUVIA_PUT("/key", updateKey, ruvia::JsonBody<service::access::UpdateAccessKeyBody>);
    RUVIA_POST("/webhook", webhook, ruvia::JsonBody<service::access::CreateWebhookBody>);
    RUVIA_PUT("/webhook", updateWebhook, ruvia::JsonBody<service::access::UpdateWebhookBody>);
    RUVIA_POST("/vpn", vpn, ruvia::JsonBody<service::vpn::VpnDesktopSelectionBody>);
    RUVIA_POST("/link", accept, service::middleware::ValidationErrorCodeMiddleware<10001, "name", 15002, "too_small">, ruvia::JsonBody<service::link::SaveLinkBody>);
    RUVIA_ROUTES_END
  private:
    ruvia::Task<ruvia::HttpResponse> accept(ruvia::Context& c) { co_return c.text("accepted"); }
    ruvia::Task<ruvia::HttpResponse> presence(ruvia::Context& c) {
        const auto& body = c.req().validated<PresenceBody>();
        if (!body.isPresent<"requiredText">()) throw std::runtime_error("required presence lost");
        if (!body.isPresent<"enabled">() && (!body.get<"enabled">() || !body.get<"enabled">()->value)) throw std::runtime_error("missing default lost");
        if (body.isNull<"nullableDefault">()) {
            if (!body.isPresent<"nullableDefault">() || body.get<"nullableDefault">()) throw std::runtime_error("null replaced by default");
            co_return c.text("null");
        }
        if (body.isPresent<"nullableDefault">()) co_return c.text("present");
        if (!body.get<"nullableDefault">() || body.get<"nullableDefault">()->value != 7) throw std::runtime_error("missing nullable default lost");
        co_return c.text("default");
    }
    ruvia::Task<ruvia::HttpResponse> createProtocol(ruvia::Context& c) {
        const auto& enabled = c.req().validated<service::protocol::CreateProtocolBody>().get<"enabled">();
        co_return c.text(std::string_view(enabled ? (enabled->value ? "true" : "false") : "null"));
    }
    ruvia::Task<ruvia::HttpResponse> updateProtocol(ruvia::Context& c) {
        const auto& body = c.req().validated<service::protocol::UpdateProtocolBody>();
        const auto& config = body.get<"config">();
        if (config) co_return c.text(config->view());
        if (!body.isPresent<"remark">()) co_return c.text("unchanged");
        if (body.isNull<"remark">()) co_return c.text("cleared");
        const auto& remark = body.get<"remark">();
        co_return c.text(remark ? remark->view() : "cleared");
    }
    ruvia::Task<ruvia::HttpResponse> rule(ruvia::Context& c) {
        const auto& body = c.req().validated<service::alert::RuleInput>();
        if (body.get<"silenceDuration">()->value != 300 || body.get<"recoveryWaitSeconds">()->value != 60 || body.isPresent<"silenceDuration">()) throw std::runtime_error("alert defaults or input presence changed");
        service::alert::alertRequest::validateConditions(body.get<"conditions">().view());
        co_return c.text(body.get<"severity">()->view());
    }
    ruvia::Task<ruvia::HttpResponse> updateKey(ruvia::Context& c) {
        const auto& body = c.req().validated<service::access::UpdateAccessKeyBody>();
        if (!body.isPresent<"remark">()) co_return c.text("unchanged");
        if (body.isNull<"remark">()) co_return c.text("cleared");
        const auto& remark = body.get<"remark">();
        co_return c.text(remark ? remark->view() : "cleared");
    }
    ruvia::Task<ruvia::HttpResponse> webhook(ruvia::Context& c) {
        const auto& body = c.req().validated<service::access::CreateWebhookBody>();
        if (body.get<"timeoutSeconds">()->value != 5 || body.get<"skipTlsVerify">()->value) throw std::runtime_error("webhook defaults changed");
        const auto& headers = body.get<"headers">();
        const bool validHeaders = !headers || service::access::isWebhookHeaders(*headers);
        if (!validHeaders) {
            const auto validatedRequest = c.req().validatedJson<service::access::CreateWebhookBody>();
            printWebhookHeaderFailure("POST", body, *headers, validatedRequest.raw(), c.arena());
            service::common::fail(19002, "非法 Header", 400);
        }
        co_return c.text("accepted");
    }
    ruvia::Task<ruvia::HttpResponse> updateWebhook(ruvia::Context& c) {
        const auto& body = c.req().validated<service::access::UpdateWebhookBody>();
        const auto& headers = body.get<"headers">();
        const bool validHeaders = !headers || service::access::isWebhookHeaders(*headers);
        if (!validHeaders) {
            const auto validatedRequest = c.req().validatedJson<service::access::UpdateWebhookBody>();
            printWebhookHeaderFailure("PUT", body, *headers, validatedRequest.raw(), c.arena());
            service::common::fail(19002, "非法 Header", 400);
        }
        co_return c.text("accepted");
    }
    ruvia::Task<ruvia::HttpResponse> vpn(ruvia::Context& c) {
        const auto ids = service::vpn::normalizeVpnNodeIds(c.req().validated<service::vpn::VpnDesktopSelectionBody>().get<"edgeNodeIds">());
        co_return c.text(ids.empty() ? std::string_view("empty") : std::string_view(ids.front()));
    }
};
void expect(ruvia::TestApp& app, const ruvia::TestRequest& request, int status, std::string_view text = {}) {
    const auto result = app.request(request);
    if (result.status().value() != status || (!text.empty() && result.body() != text)) throw std::runtime_error("model request: " + std::to_string(result.status().value()) + " " + std::string(result.body()));
}
void testValidationCodes(ruvia::TestApp& app) {
    const auto path = "/model-migration/errors/00000000-0000-7000-8000-000000000001";
    expect(app, ruvia::TestRequest::put(path).json(R"({"name":42})"), 400, "16002");
    expect(app, ruvia::TestRequest::put(path).json(R"({"enabled":"false"})"), 400, "16004");
    expect(app, ruvia::TestRequest::put("/model-migration/errors/invalid").json(R"({"name":42})"), 400, "10001");
    expect(app, ruvia::TestRequest::put(path).json("{}"), 200, "accepted");
    expect(app, ruvia::TestRequest::post("/model-migration/presence").json("{}"), 400, "10001");
}
void testPresence(ruvia::TestApp& app) {
    expect(app, ruvia::TestRequest::post("/model-migration/presence").json("{}"), 400);
    expect(app, ruvia::TestRequest::post("/model-migration/presence").json(R"({"requiredText":null})"), 200, "default");
    expect(app, ruvia::TestRequest::post("/model-migration/presence").json(R"({"requiredText":"ok","nullableDefault":null})"), 200, "null");
    expect(app, ruvia::TestRequest::post("/model-migration/presence").json(R"({"requiredText":"ok","nullableDefault":3})"), 200, "present");
    expect(app, ruvia::TestRequest::post("/model-migration/presence").json(R"({"requiredText":"ok","enabled":null})"), 400);
    expect(app, ruvia::TestRequest::post("/model-migration/presence").json(R"({"requiredText":42})"), 400);
}
void testProtocol(ruvia::TestApp& app) {
    expect(app, ruvia::TestRequest::post("/model-migration/protocol").json(R"({"protocol":"Modbus","name":"pump"})"), 200, "true");
    expect(app, ruvia::TestRequest::post("/model-migration/protocol").json(R"({"protocol":"Modbus","name":"pump","enabled":false})"), 200, "false");
    expect(app, ruvia::TestRequest::post("/model-migration/protocol").json(R"({"protocol":"Modbus","name":"pump","enabled":null})"), 200, "null");
    for (const auto invalid : {R"({"protocol":null,"name":"x"})", R"({"protocol":"Modbus","name":null})", R"({"protocol":"Modbus","name":"x","enabled":"false"})", R"({"protocol":"Modbus","name":"x","remark":3})"}) expect(app, ruvia::TestRequest::post("/model-migration/protocol").json(invalid), 400);
    expect(app, ruvia::TestRequest::put("/model-migration/protocol").json("{}"), 200, "unchanged");
    expect(app, ruvia::TestRequest::put("/model-migration/protocol").json(R"({"remark":null})"), 200, "cleared");
    expect(app, ruvia::TestRequest::put("/model-migration/protocol").json(R"({"remark":"note"})"), 200, "note");
    expect(app, ruvia::TestRequest::put("/model-migration/protocol").json(R"({"config":{ "vendor": [null, true, {"x":1}] }})"), 200, R"({ "vendor": [null, true, {"x":1}] })");
    for (const auto invalid : {R"({"remark":false})", R"({"name":4})", R"({"protocol":[]})", R"({"enabled":"false"})", R"({"remark":"one","remark":"two"})", "[]"}) expect(app, ruvia::TestRequest::put("/model-migration/protocol").json(invalid), 400);
}
void testAlert(ruvia::TestApp& app) {
    const std::string fields = R"("name":"alarm","device_id":"00000000-0000-7000-8000-000000000001","conditions":[{"type":"offline"}])";
    expect(app, ruvia::TestRequest::post("/model-migration/rule").json("{" + fields + "}"), 200, "warning");
    for (const auto invalid : {R"("remark":null)", R"("remark":42)", R"("silence_duration":null)", R"("silence_duration":"300")", R"("silence_duration":-1)", R"("status":"invalid")"}) expect(app, ruvia::TestRequest::post("/model-migration/rule").json("{" + fields + "," + invalid + "}"), 400);
    expect(app, ruvia::TestRequest::post("/model-migration/template").json(R"({"name":"alarm","conditions":[{"type":"offline"}]})"), 200);
    for (const auto invalid : {"{}", R"({"ids":[]})", R"({"ids":["bad"]})", R"({"ids":[1]})"}) expect(app, ruvia::TestRequest::post("/model-migration/batch").json(invalid), 400);
}
std::string hexBytes(std::string_view bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (const char byte : bytes) {
        if (!result.empty()) result.push_back(' ');
        const auto value = static_cast<unsigned char>(byte);
        result.push_back(digits[value >> 4]);
        result.push_back(digits[value & 0x0f]);
    }
    return result;
}
bool isDiagnosticHeaderToken(std::string_view key) {
    return !key.empty() && std::ranges::all_of(key, [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || std::string_view("!#$%&'*+.^_`|~-").find(ch) != std::string_view::npos;
    });
}
bool isDiagnosticReservedHeader(std::string_view key) {
    constexpr std::array<std::string_view, 15> reserved{
        "host", "content-length", "connection", "x-iot-event", "x-iot-timestamp", "x-iot-delivery", "x-iot-signature", "content-type", "user-agent", "transfer-encoding", "trailer", "te", "upgrade", "expect", "proxy-connection"
    };
    std::string lower(key);
    for (auto& ch : lower) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    return std::ranges::find(reserved, lower) != reserved.end();
}
// Called synchronously by webhook handlers only for this exact, non-sensitive test request.
template <typename Body>
void printWebhookHeaderFailure(std::string_view route, const Body& body, const ruvia::JsonObject& headers,
    std::string_view rawRequest, std::pmr::memory_resource* resource) {
    if (rawRequest != kWebhookHeaderDiagnosticRequest) return;

    constexpr std::string_view expectedHeaders = R"({"X-Example":"ok"})";
    const auto headersView = headers.view();
    std::size_t callbackCount = 0;
    std::string callbackKey;
    std::string callbackValueJson;
    std::string callbackStringValue;
    std::string headerLookupValue;
    bool callbackStringPresent = false;
    bool headerLookupPresent = false;
    bool valuesEquivalent = false;
    bool keyTokenValid = false;
    bool keyReserved = false;
    bool callbackNewlineCheckAvailable = false;
    bool callbackValueHasNewline = false;
    const bool fieldsCompleted = headers.forEachField([&](std::string_view key, const ruvia::JsonValue& value) {
        ++callbackCount;
        if (callbackCount == 1) {
            callbackKey.assign(key);
            callbackValueJson.assign(value.view());
            keyTokenValid = isDiagnosticHeaderToken(key);
            keyReserved = isDiagnosticReservedHeader(key);
            const auto callbackString = value.get<ruvia::String>();
            const auto headerLookup = headers.get<ruvia::String>(key);
            callbackStringPresent = callbackString.has_value();
            headerLookupPresent = headerLookup.has_value();
            if (callbackString) {
                callbackStringValue.assign(callbackString->view());
                callbackNewlineCheckAvailable = true;
                callbackValueHasNewline = callbackString->view().find_first_of("\r\n") != std::string_view::npos;
            }
            if (headerLookup) headerLookupValue.assign(headerLookup->view());
            valuesEquivalent = callbackStringPresent == headerLookupPresent &&
                (!callbackStringPresent || callbackString->view() == headerLookup->view());
        }
        return true;
    });

    bool reparsedRoot = false;
    bool reparsedHeaders = false;
    bool reparsedHeadersValid = false;
    if (const auto root = ruvia::JsonValue::parse(rawRequest, {.resource = resource})) {
        reparsedRoot = true;
        if (const auto parsedHeaders = root->get<ruvia::JsonObject>("headers")) {
            reparsedHeaders = true;
            reparsedHeadersValid = service::access::isWebhookHeaders(*parsedHeaders);
        }
    }

    std::cerr << "model-migration webhook header failure in " << route << " /model-migration/webhook (fixed test payload only)\n"
              << "validated raw request=" << rawRequest << '\n'
              << "validated model isWebhookHeaders=false; body.headers: present=" << (body.template isPresent<"headers">() ? "yes" : "no")
              << " null=" << (body.template isNull<"headers">() ? "yes" : "no")
              << " optional=" << (body.template get<"headers">().has_value() ? "engaged" : "empty")
              << " view_bytes=" << headersView.size() << " expected_bytes=" << expectedHeaders.size()
              << " view_matches_expected=" << (headersView == expectedHeaders ? "yes" : "no")
              << " view_hex=" << hexBytes(headersView) << '\n'
              << "validated object inspection: forEachField_return=" << (fieldsCompleted ? "true" : "false")
              << " callbacks=" << callbackCount;
    if (callbackCount != 0) {
        std::cerr << " first_key_copy=" << callbackKey << " key_hex=" << hexBytes(callbackKey)
                  << " token_valid=" << (keyTokenValid ? "yes" : "no")
                  << " reserved=" << (keyReserved ? "yes" : "no")
                  << " value_json_copy=" << callbackValueJson
                  << " callback_get<String>=" << (callbackStringPresent ? "present" : "absent")
                  << " callback_string_copy_hex=" << hexBytes(callbackStringValue)
                  << " headers_get<String>(key)=" << (headerLookupPresent ? "present" : "absent")
                  << " lookup_string_copy_hex=" << hexBytes(headerLookupValue)
                  << " values_equivalent=" << (valuesEquivalent ? "yes" : "no")
                  << " callback_value_has_cr_or_lf=" << (callbackNewlineCheckAvailable ? (callbackValueHasNewline ? "yes" : "no") : "unavailable");
    }
    std::cerr << '\n'
              << "reparse on handler arena: root_parsed=" << (reparsedRoot ? "yes" : "no")
              << " headers_found=" << (reparsedHeaders ? "yes" : "no")
              << " isWebhookHeaders=" << (reparsedHeaders ? (reparsedHeadersValid ? "true" : "false") : "unavailable") << '\n';
}
void testAccess(ruvia::TestApp& app) {
    const std::string key = R"({"name":"integration","scopes":["device:realtime"],"deviceIds":["00000000-0000-7000-8000-000000000001"]})";
    expect(app, ruvia::TestRequest::post("/model-migration/key").json(key), 200);
    expect(app, ruvia::TestRequest::post("/model-migration/key").json("{}"), 400);
    expect(app, ruvia::TestRequest::put("/model-migration/key").json("{}"), 200, "unchanged");
    expect(app, ruvia::TestRequest::put("/model-migration/key").json(R"({"remark":null})"), 200, "cleared");
    for (const auto invalid : {R"({"name":" "})", R"({"scopes":["admin"]})", R"({"deviceIds":["bad"]})", R"({"remark":false})"}) expect(app, ruvia::TestRequest::put("/model-migration/key").json(invalid), 400);
    const std::string fields = R"("accessKeyId":"00000000-0000-7000-8000-000000000001","name":"sink","url":"https://example.test/hook")";
    const std::string webhookWithoutHeaders = "{" + fields + "}";
    const auto noHeadersResponse = app.request(ruvia::TestRequest::post("/model-migration/webhook").json(webhookWithoutHeaders));
    const int noHeadersStatus = noHeadersResponse.status().value();
    if (noHeadersStatus != 200) throw std::runtime_error("webhook without headers: expected status=200; actual status=" + std::to_string(noHeadersStatus) + " body=" + std::string(noHeadersResponse.body()));
    const std::string webhookWithHeaders = "{" + fields + R"(,"headers":{"X-Example":"ok"}})";
    const auto headersResponse = app.request(ruvia::TestRequest::post("/model-migration/webhook").json(webhookWithHeaders));
    if (headersResponse.status().value() != 200) {
        throw std::runtime_error("webhook with valid test headers: expected status=200; actual status=" + std::to_string(headersResponse.status().value()) + " body=" + std::string(headersResponse.body()));
    }
    for (const auto invalid : {R"({"url":"file:///tmp/x"})", R"({"url":"https://user@example.test"})", R"({"headers":{"Host":"attacker"}})", R"({"headers":{"X-Test":"x\r\ny"}})", R"({"headers":{"X-Test":3}})", R"({"headers":{"bad name":"x"}})", R"({"timeoutSeconds":31})", R"({"skipTlsVerify":"false"})", R"({"eventTypes":[]})", R"({"eventTypes":["bogus"]})"}) expect(app, ruvia::TestRequest::put("/model-migration/webhook").json(invalid), 400);
}
void testLink(ruvia::TestApp& app) {
    expect(app, ruvia::TestRequest::post("/model-migration/link").json("{}"), 400, "10001");
    expect(app, ruvia::TestRequest::post("/model-migration/link").json(R"({"name":"","protocol":"Modbus","endpoint":{}})"), 400, "15002");
    expect(app, ruvia::TestRequest::post("/model-migration/link").json(R"({"name":"valid","protocol":"invalid","endpoint":{}})"), 400, "10001");
}
void testVpn(ruvia::TestApp& app) {
    expect(app, ruvia::TestRequest::post("/model-migration/vpn").json(R"({"edgeNodeIds":[]})"), 200, "empty");
    expect(app, ruvia::TestRequest::post("/model-migration/vpn").json(R"({"edgeNodeIds":["AAAAAAAA-AAAA-7AAA-8AAA-AAAAAAAAAAAA"]})"), 200, "aaaaaaaa-aaaa-7aaa-8aaa-aaaaaaaaaaaa");
    for (const auto invalid : {"{}", R"({"edgeNodeIds":null})", R"({"edgeNodeIds":["bad"]})", R"({"edgeNodeIds":["AAAAAAAA-AAAA-7AAA-8AAA-AAAAAAAAAAAA","aaaaaaaa-aaaa-7aaa-8aaa-aaaaaaaaaaaa"]})"}) expect(app, ruvia::TestRequest::post("/model-migration/vpn").json(invalid), 400);
}
}
int main() {
    try {
        ruvia::TestApp app;
        app.onError([](ruvia::Context& context, ruvia::HttpErrorInfo error) -> ruvia::Task<ruvia::HttpResponse> {
            context.status(error.status());
            std::pmr::string code(context.arena());
            code = std::to_string(service::middleware::requestErrorCode(context, error));
            co_return context.text(std::move(code));
        });
        testValidationCodes(app);
        testPresence(app);
        testProtocol(app);
        testAlert(app);
        testAccess(app);
        testVpn(app);
        testLink(app);
        std::cout << "model migration tests passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
