#pragma once
#include <ruvia/web/Controller.h>
#include "service/utils/json.h"

#include "service/common/http.h"
#include "service/middleware/auth.h"
#include "service/middleware/live.h"
#include "service/middleware/permission.h"
#include "service/modules/alert/alert.types.h"
#include "service/modules/alert/alert.service.h"

namespace service::alert {
class AlertController final : public ruvia::Controller<AlertController> {
  public:
    RUVIA_CONTROLLER_GROUP("/v1/alert", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/rules", rules, ruvia::QueryModel<AlertListQuery>);
    RUVIA_GET("/templates", templates, ruvia::QueryModel<AlertListQuery>);
    RUVIA_GET_SSE("/events", alertEvents, ruvia::QueryModel<AlertListQuery>);
    RUVIA_GET("/records", records, ruvia::QueryModel<AlertListQuery>);
    RUVIA_GET("/records/grouped", grouped, ruvia::QueryModel<AlertGroupedQuery>);
    RUVIA_GET("/stats", stats);
    RUVIA_GET("/rules/:id", ruleDetail, ruvia::PathModel<AlertIdParams>);
    RUVIA_GET("/templates/:id", templateDetail, ruvia::PathModel<AlertIdParams>);
    RUVIA_POST("/rules", createRule);
    RUVIA_DELETE("/rules", batchRemoveRules);
    RUVIA_POST("/rules/apply-template", applyTemplate);
    RUVIA_POST("/templates", createTemplate);
    RUVIA_POST("/records/batch-ack", batchAcknowledge);
    RUVIA_PUT("/rules/:id", updateRule, ruvia::PathModel<AlertIdParams>);
    RUVIA_DELETE("/rules/:id", removeRule, ruvia::PathModel<AlertIdParams>);
    RUVIA_PUT("/templates/:id", updateTemplate, ruvia::PathModel<AlertIdParams>);
    RUVIA_DELETE("/templates/:id", removeTemplate, ruvia::PathModel<AlertIdParams>);
    RUVIA_POST("/records/:id/ack", acknowledge, ruvia::PathModel<AlertIdParams>);
    RUVIA_ROUTES_END

  private:
    static std::string id(ruvia::Context& c) {
        return std::string(c.req().validated<AlertIdParams>().get<"id">().view());
    }

    ruvia::Task<> rules(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await rulesSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> rulesSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:alert:query");
        co_return service::live::data(c, co_await alertService().listRules(request, c.req().validated<AlertListQuery>()));
    }

    ruvia::Task<> ruleDetail(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await ruleDetailSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> ruleDetailSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:alert:query");
        co_return service::live::data(c, co_await alertService().ruleDetail(request, id(c)));
    }

    ruvia::Task<> templates(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await templatesSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> templatesSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:alert:query");
        co_return service::live::data(c, co_await alertService().listTemplates(request, c.req().validated<AlertListQuery>()));
    }

    ruvia::Task<> templateDetail(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await templateDetailSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> templateDetailSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:alert:query");
        co_return service::live::data(c, co_await alertService().templateDetail(request, id(c)));
    }

    ruvia::Task<> records(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await recordsSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> recordsSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:alert:query");
        co_return service::live::data(c, co_await alertService().listRecords(request, c.req().validated<AlertListQuery>()));
    }

    ruvia::Task<void> alertEvents(ruvia::Context& c) {
        service::middleware::RequestContext access(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(access, access.userId, "iot:alert:query");
        std::vector<service::live::SnapshotChannel> channels;
        channels.push_back({ "records", "alert", [this, &c](service::middleware::RequestContext& request) {
                                return recordsSnapshot(c, request);
                            } });
        channels.push_back({ "stats", "alert", [this, &c](service::middleware::RequestContext& request) {
                                return statsSnapshot(c, request);
                            } });
        co_await service::live::serveSnapshotChannels(c, service::middleware::requireAuth(c).userId, std::move(channels), [&c] {
            (void)service::middleware::requireAuth(c);
        });
    }

    ruvia::Task<> grouped(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await groupedSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> groupedSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:alert:query");
        co_return service::live::data(c, co_await alertService().grouped(request, static_cast<std::int64_t>(*c.req().validated<AlertGroupedQuery>().get<"days">())));
    }

    ruvia::Task<> stats(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await statsSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> statsSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:alert:query");
        co_return service::live::data(c, co_await alertService().stats(request));
    }

    ruvia::Task<> createRule(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:alert:add");
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
        if (!json.isObject()) {
            service::common::fail(17002, "请求体必须是对象", 400);
        }
        const auto body = alertRequest::ruleInput(json);
        co_await alertService().createRule(request, body);
        co_return c.json(service::common::operation(c, "操作成功"));
    }

    ruvia::Task<> updateRule(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:alert:edit");
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
        if (!json.isObject()) {
            service::common::fail(17002, "请求体必须是对象", 400);
        }
        const auto body = alertRequest::ruleInput(json);
        co_await alertService().updateRule(request, id(c), body);
        co_return c.json(service::common::operation(c, "操作成功"));
    }

    ruvia::Task<> removeRule(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:alert:delete");
        co_await alertService().removeRule(request, id(c));
        co_return c.json(service::common::operation(c, "操作成功"));
    }

    ruvia::Task<> batchRemoveRules(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:alert:delete");
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
        if (!json.isObject()) {
            service::common::fail(17002, "请求体必须是对象", 400);
        }
        const auto body = alertRequest::requiredUuids(json, "ids", "请选择操作对象");
        co_await alertService().batchRemoveRules(request, body);
        co_return c.json(service::common::operation(c, "操作成功"));
    }

    ruvia::Task<> applyTemplate(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:alert:add");
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
        if (!json.isObject()) {
            service::common::fail(17002, "请求体必须是对象", 400);
        }
        const auto body = alertRequest::applyTemplateInput(json);
        const auto payload = service::live::data(c, co_await alertService().applyTemplate(request, body));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<> createTemplate(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:alert:add");
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
        if (!json.isObject()) {
            service::common::fail(17002, "请求体必须是对象", 400);
        }
        const auto body = alertRequest::templateInput(json);
        co_await alertService().createTemplate(request, body);
        co_return c.json(service::common::operation(c, "操作成功"));
    }

    ruvia::Task<> updateTemplate(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:alert:edit");
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
        if (!json.isObject()) {
            service::common::fail(17002, "请求体必须是对象", 400);
        }
        const auto body = alertRequest::templateInput(json);
        co_await alertService().updateTemplate(request, id(c), body);
        co_return c.json(service::common::operation(c, "操作成功"));
    }

    ruvia::Task<> removeTemplate(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:alert:delete");
        co_await alertService().removeTemplate(request, id(c));
        co_return c.json(service::common::operation(c, "操作成功"));
    }

    ruvia::Task<> acknowledge(ruvia::Context& c) {

        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:alert:ack");
        co_await alertService().acknowledge(request, id(c));
        co_return c.json(service::common::operation(c, "操作成功"));
    }

    ruvia::Task<> batchAcknowledge(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:alert:ack");
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
        if (!json.isObject()) {
            service::common::fail(17002, "请求体必须是对象", 400);
        }
        const auto body = alertRequest::requiredUuids(json, "ids", "请选择操作对象");
        co_await alertService().batchAcknowledge(request, body);
        co_return c.json(service::common::operation(c, "操作成功"));
    }
};
} // namespace service::alert
