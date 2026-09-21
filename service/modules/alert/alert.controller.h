#pragma once
#include <ruvia/web/Controller.h>
#include "service/common/http.h"
#include "service/middleware/auth.h"
#include "service/middleware/live.h"
#include "service/middleware/permission.h"
#include "service/middleware/validation.h"
#include "service/modules/alert/alert.types.h"
#include "service/modules/alert/alert.service.h"

namespace service::alert {
class AlertController final : public ruvia::Controller<AlertController> {
  public:
    RUVIA_CONTROLLER_GROUP("/v1/alert", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/rules", rules, service::middleware::PermissionMiddleware<"iot:alert:query">, ruvia::QueryModel<AlertListQuery>);
    RUVIA_GET("/templates", templates, service::middleware::PermissionMiddleware<"iot:alert:query">, ruvia::QueryModel<AlertListQuery>);
    RUVIA_GET_SSE("/events", alertEvents, service::middleware::PermissionMiddleware<"iot:alert:query">, ruvia::QueryModel<AlertListQuery>);
    RUVIA_GET("/records", records, service::middleware::PermissionMiddleware<"iot:alert:query">, ruvia::QueryModel<AlertListQuery>);
    RUVIA_GET("/records/grouped", grouped, service::middleware::PermissionMiddleware<"iot:alert:query">, ruvia::QueryModel<AlertGroupedQuery>);
    RUVIA_GET("/stats", stats, service::middleware::PermissionMiddleware<"iot:alert:query">);
    RUVIA_GET("/rules/:id", ruleDetail, service::middleware::PermissionMiddleware<"iot:alert:query">, ruvia::PathModel<AlertIdParams>);
    RUVIA_GET("/templates/:id", templateDetail, service::middleware::PermissionMiddleware<"iot:alert:query">, ruvia::PathModel<AlertIdParams>);
    RUVIA_POST("/rules", createRule, service::middleware::PermissionMiddleware<"iot:alert:add">, service::middleware::ValidationErrorCodeMiddleware<17002>, ruvia::JsonBody<RuleInput>);
    RUVIA_DELETE("/rules", batchRemoveRules, service::middleware::PermissionMiddleware<"iot:alert:delete">, service::middleware::ValidationErrorCodeMiddleware<17002>, ruvia::JsonBody<AlertBatchBody>);
    RUVIA_POST("/rules/apply-template", applyTemplate, service::middleware::PermissionMiddleware<"iot:alert:add">, service::middleware::ValidationErrorCodeMiddleware<17002>, ruvia::JsonBody<ApplyTemplateInput>);
    RUVIA_POST("/templates", createTemplate, service::middleware::PermissionMiddleware<"iot:alert:add">, service::middleware::ValidationErrorCodeMiddleware<17002>, ruvia::JsonBody<TemplateInput>);
    RUVIA_POST("/records/batch-ack", batchAcknowledge, service::middleware::PermissionMiddleware<"iot:alert:ack">, service::middleware::ValidationErrorCodeMiddleware<17002>, ruvia::JsonBody<AlertBatchBody>);
    RUVIA_PUT("/rules/:id", updateRule, service::middleware::PermissionMiddleware<"iot:alert:edit">, ruvia::PathModel<AlertIdParams>, service::middleware::ValidationErrorCodeMiddleware<17002>, ruvia::JsonBody<RuleInput>);
    RUVIA_DELETE("/rules/:id", removeRule, service::middleware::PermissionMiddleware<"iot:alert:delete">, ruvia::PathModel<AlertIdParams>);
    RUVIA_PUT("/templates/:id", updateTemplate, service::middleware::PermissionMiddleware<"iot:alert:edit">, ruvia::PathModel<AlertIdParams>, service::middleware::ValidationErrorCodeMiddleware<17002>, ruvia::JsonBody<TemplateInput>);
    RUVIA_DELETE("/templates/:id", removeTemplate, service::middleware::PermissionMiddleware<"iot:alert:delete">, ruvia::PathModel<AlertIdParams>);
    RUVIA_POST("/records/:id/ack", acknowledge, service::middleware::PermissionMiddleware<"iot:alert:ack">, ruvia::PathModel<AlertIdParams>);
    RUVIA_ROUTES_END

  private:
    static std::string id(ruvia::Context& c) { return std::string(c.req().validated<AlertIdParams>().get<"id">().view()); }
    ruvia::Task<ruvia::HttpResponse> rules(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = service::live::data(c, co_await alertService().listRules(request, c.req().validated<AlertListQuery>()));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }
    ruvia::Task<ruvia::HttpResponse> ruleDetail(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = service::live::data(c, co_await alertService().ruleDetail(request, id(c)));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }
    ruvia::Task<ruvia::HttpResponse> templates(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = service::live::data(c, co_await alertService().listTemplates(request, c.req().validated<AlertListQuery>()));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }
    ruvia::Task<ruvia::HttpResponse> templateDetail(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = service::live::data(c, co_await alertService().templateDetail(request, id(c)));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }
    ruvia::Task<ruvia::HttpResponse> records(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = service::live::data(c, co_await alertService().listRecords(request, c.req().validated<AlertListQuery>()));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }
    ruvia::Task<ruvia::HttpResponse> stats(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = service::live::data(c, co_await alertService().stats(request));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }
    ruvia::Task<ruvia::HttpResponse> grouped(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = service::live::data(c, co_await alertService().grouped(request, c.req().validated<AlertGroupedQuery>().get<"days">()->value));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }
    ruvia::Task<void> alertEvents(ruvia::Context& c) {
        std::vector<service::live::SnapshotChannel> channels{
            {"records", "alert", [&c](service::middleware::RequestContext& request) -> ruvia::Task<std::string> {
                co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:alert:query");
                co_return service::live::data(c, co_await alertService().listRecords(request, c.req().validated<AlertListQuery>()));
            }},
            {"stats", "alert", [&c](service::middleware::RequestContext& request) -> ruvia::Task<std::string> {
                co_await service::auth::AuthService::requirePermission(request, request.userId, "iot:alert:query");
                co_return service::live::data(c, co_await alertService().stats(request));
            }}
        };
        co_await service::live::serveSnapshotChannels(c, service::middleware::requireAuth(c).userId, std::move(channels), [&c] { (void)service::middleware::requireAuth(c); });
    }
    ruvia::Task<ruvia::HttpResponse> createRule(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await alertService().createRule(request, c.req().validated<RuleInput>());
        co_return c.json(service::common::operation(c, "操作成功"));
    }
    ruvia::Task<ruvia::HttpResponse> updateRule(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await alertService().updateRule(request, id(c), c.req().validated<RuleInput>());
        co_return c.json(service::common::operation(c, "操作成功"));
    }
    ruvia::Task<ruvia::HttpResponse> removeRule(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await alertService().removeRule(request, id(c));
        co_return c.json(service::common::operation(c, "操作成功"));
    }
    ruvia::Task<ruvia::HttpResponse> batchRemoveRules(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await alertService().batchRemoveRules(request, uniqueAlertIds(c.req().validated<AlertBatchBody>().get<"ids">()));
        co_return c.json(service::common::operation(c, "操作成功"));
    }
    ruvia::Task<ruvia::HttpResponse> applyTemplate(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        const auto payload = service::live::data(c, co_await alertService().applyTemplate(request, c.req().validated<ApplyTemplateInput>()));
        c.header("Content-Type", "application/json");
        co_return c.body(std::string_view(payload));
    }
    ruvia::Task<ruvia::HttpResponse> createTemplate(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await alertService().createTemplate(request, c.req().validated<TemplateInput>());
        co_return c.json(service::common::operation(c, "操作成功"));
    }
    ruvia::Task<ruvia::HttpResponse> updateTemplate(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await alertService().updateTemplate(request, id(c), c.req().validated<TemplateInput>());
        co_return c.json(service::common::operation(c, "操作成功"));
    }
    ruvia::Task<ruvia::HttpResponse> removeTemplate(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await alertService().removeTemplate(request, id(c));
        co_return c.json(service::common::operation(c, "操作成功"));
    }
    ruvia::Task<ruvia::HttpResponse> acknowledge(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await alertService().acknowledge(request, id(c));
        co_return c.json(service::common::operation(c, "操作成功"));
    }
    ruvia::Task<ruvia::HttpResponse> batchAcknowledge(ruvia::Context& c) {
        service::middleware::RequestContext request(c, service::middleware::requireAuth(c).userId);
        co_await alertService().batchAcknowledge(request, uniqueAlertIds(c.req().validated<AlertBatchBody>().get<"ids">()));
        co_return c.json(service::common::operation(c, "操作成功"));
    }
};
} // namespace service::alert
