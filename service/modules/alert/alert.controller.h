#pragma once
#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/middleware/auth.h"
#include "service/middleware/live.h"
#include "service/middleware/permission.h"
#include "service/modules/alert/alert.schema.h"
#include "service/modules/alert/alert.service.h"

namespace service::alert {
class AlertController final : public ruvia::Controller<AlertController> {
  public:
    RUVIA_CONTROLLER_GROUP("/v1/alert", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/rules", rules, AlertListValidator);
    RUVIA_GET("/templates", templates, AlertListValidator);
    RUVIA_GET_SSE("/events", alertEvents, AlertListValidator);
    RUVIA_GET("/records", records, AlertListValidator);
    RUVIA_GET("/records/grouped", grouped, AlertGroupedValidator);
    RUVIA_GET("/stats", stats);
    RUVIA_GET("/rules/:id", ruleDetail, AlertIdValidator);
    RUVIA_GET("/templates/:id", templateDetail, AlertIdValidator);
    RUVIA_POST("/rules", createRule);
    RUVIA_DELETE("/rules", batchRemoveRules);
    RUVIA_POST("/rules/apply-template", applyTemplate);
    RUVIA_POST("/templates", createTemplate);
    RUVIA_POST("/records/batch-ack", batchAcknowledge);
    RUVIA_PUT("/rules/:id", updateRule, AlertIdValidator);
    RUVIA_DELETE("/rules/:id", removeRule, AlertIdValidator);
    RUVIA_PUT("/templates/:id", updateTemplate, AlertIdValidator);
    RUVIA_DELETE("/templates/:id", removeTemplate, AlertIdValidator);
    RUVIA_POST("/records/:id/ack", acknowledge, AlertIdValidator);
    RUVIA_ROUTES_END

  private:
    static std::string id(ruvia::Context& c) {
        return std::string(c.req().validated<AlertIdParams>().get<"id">()->view());
    }

    ruvia::Task<ruvia::HttpResponse> rules(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await rulesSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> rulesSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:alert:query");
        co_return service::live::data(c, co_await alertService().listRules(request, c.req().validated<AlertListQuery>()));
    }

    ruvia::Task<ruvia::HttpResponse> ruleDetail(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await ruleDetailSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> ruleDetailSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:alert:query");
        co_return service::live::data(c, co_await alertService().ruleDetail(request, id(c)));
    }

    ruvia::Task<ruvia::HttpResponse> templates(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await templatesSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> templatesSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:alert:query");
        co_return service::live::data(c, co_await alertService().listTemplates(request, c.req().validated<AlertListQuery>()));
    }

    ruvia::Task<ruvia::HttpResponse> templateDetail(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await templateDetailSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> templateDetailSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:alert:query");
        co_return service::live::data(c, co_await alertService().templateDetail(request, id(c)));
    }

    ruvia::Task<ruvia::HttpResponse> records(ruvia::Context& c) {
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

    ruvia::Task<ruvia::HttpResponse> grouped(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await groupedSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> groupedSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:alert:query");
        co_return service::live::data(c, co_await alertService().grouped(request, static_cast<std::int64_t>(*c.req().validated<AlertGroupedQuery>().get<"days">())));
    }

    ruvia::Task<ruvia::HttpResponse> stats(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = co_await statsSnapshot(c, request);
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<std::string> statsSnapshot(ruvia::Context& c, service::middleware::RequestContext& request) {
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:alert:query");
        co_return service::live::data(c, co_await alertService().stats(request));
    }

    ruvia::Task<ruvia::HttpResponse> createRule(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:alert:add");
        const auto json = co_await c.req().jsonValue();
        if (!json.isObject()) {
            service::common::fail(17002, "请求体必须是对象", 400);
        }
        const auto body = AlertPayloadValidator::ruleInput(json);
        co_await alertService().createRule(request, body);
        co_return c.json(service::common::operation(c, "操作成功"));
    }

    ruvia::Task<ruvia::HttpResponse> updateRule(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:alert:edit");
        const auto json = co_await c.req().jsonValue();
        if (!json.isObject()) {
            service::common::fail(17002, "请求体必须是对象", 400);
        }
        const auto body = AlertPayloadValidator::ruleInput(json);
        co_await alertService().updateRule(request, id(c), body);
        co_return c.json(service::common::operation(c, "操作成功"));
    }

    ruvia::Task<ruvia::HttpResponse> removeRule(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:alert:delete");
        co_await alertService().removeRule(request, id(c));
        co_return c.json(service::common::operation(c, "操作成功"));
    }

    ruvia::Task<ruvia::HttpResponse> batchRemoveRules(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:alert:delete");
        const auto json = co_await c.req().jsonValue();
        if (!json.isObject()) {
            service::common::fail(17002, "请求体必须是对象", 400);
        }
        const auto body = AlertPayloadValidator::requiredUuids(json, "ids", "请选择操作对象");
        co_await alertService().batchRemoveRules(request, body);
        co_return c.json(service::common::operation(c, "操作成功"));
    }

    ruvia::Task<ruvia::HttpResponse> applyTemplate(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:alert:add");
        const auto json = co_await c.req().jsonValue();
        if (!json.isObject()) {
            service::common::fail(17002, "请求体必须是对象", 400);
        }
        const auto body = AlertPayloadValidator::applyTemplateInput(json);
        const auto payload = service::live::data(c, co_await alertService().applyTemplate(request, body));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }

    ruvia::Task<ruvia::HttpResponse> createTemplate(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:alert:add");
        const auto json = co_await c.req().jsonValue();
        if (!json.isObject()) {
            service::common::fail(17002, "请求体必须是对象", 400);
        }
        const auto body = AlertPayloadValidator::templateInput(json);
        co_await alertService().createTemplate(request, body);
        co_return c.json(service::common::operation(c, "操作成功"));
    }

    ruvia::Task<ruvia::HttpResponse> updateTemplate(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:alert:edit");
        const auto json = co_await c.req().jsonValue();
        if (!json.isObject()) {
            service::common::fail(17002, "请求体必须是对象", 400);
        }
        const auto body = AlertPayloadValidator::templateInput(json);
        co_await alertService().updateTemplate(request, id(c), body);
        co_return c.json(service::common::operation(c, "操作成功"));
    }

    ruvia::Task<ruvia::HttpResponse> removeTemplate(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:alert:delete");
        co_await alertService().removeTemplate(request, id(c));
        co_return c.json(service::common::operation(c, "操作成功"));
    }

    ruvia::Task<ruvia::HttpResponse> acknowledge(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:alert:ack");
        co_await alertService().acknowledge(request, id(c));
        co_return c.json(service::common::operation(c, "操作成功"));
    }

    ruvia::Task<ruvia::HttpResponse> batchAcknowledge(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:alert:ack");
        const auto json = co_await c.req().jsonValue();
        if (!json.isObject()) {
            service::common::fail(17002, "请求体必须是对象", 400);
        }
        const auto body = AlertPayloadValidator::requiredUuids(json, "ids", "请选择操作对象");
        co_await alertService().batchAcknowledge(request, body);
        co_return c.json(service::common::operation(c, "操作成功"));
    }
};
} // namespace service::alert
