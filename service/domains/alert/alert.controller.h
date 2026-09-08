#pragma once

#include <memory_resource>
#include <string>
#include <string_view>

#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/features/live/query.h"
#include "service/domains/alert/alert.schema.h"
#include "service/domains/alert/alert.service.h"
#include "service/middleware/auth.h"
#include "service/middleware/permission.h"

namespace service::alert {

inline ruvia::HttpResponse alertJson(ruvia::Context& c, std::string_view data,
                                     std::string_view message = "ok") {
    std::pmr::string body(c.allocator<char>());
    body.append("{\"code\":0,\"message\":");
    body.append(service::access::jsonQuoted(message));
    body.append(",\"data\":");
    body.append(data);
    body.push_back('}');
    auto response = c.body(std::move(body));
    response.header("Content-Type", "application/json; charset=UTF-8");
    return response;
}

inline std::string alertId(ruvia::Context& c) {
    return std::string(c.req().param("id").value_or(""));
}

class AlertController final : public ruvia::Controller<AlertController> {
  public:
    RUVIA_CONTROLLER_GROUP("/v1/alert", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET_SSE("/rules", rules, AlertListValidator);
    RUVIA_POST("/rules/apply-template", applyTemplate);
    RUVIA_GET_SSE("/rules/:id", ruleDetail, AlertIdValidator);
    RUVIA_POST("/rules", createRule);
    RUVIA_PUT("/rules/:id", updateRule, AlertIdValidator);
    RUVIA_DELETE("/rules/:id", removeRule, AlertIdValidator);
    RUVIA_DELETE("/rules", batchRemoveRules);
    RUVIA_GET_SSE("/templates", templates, AlertListValidator);
    RUVIA_GET_SSE("/templates/:id", templateDetail, AlertIdValidator);
    RUVIA_POST("/templates", createTemplate);
    RUVIA_PUT("/templates/:id", updateTemplate, AlertIdValidator);
    RUVIA_DELETE("/templates/:id", removeTemplate, AlertIdValidator);
    RUVIA_GET_SSE("/records/grouped", grouped);
    RUVIA_POST("/records/batch-ack", batchAcknowledge);
    RUVIA_GET_SSE("/records", records, AlertListValidator);
    RUVIA_POST("/records/:id/ack", acknowledge, AlertIdValidator);
    RUVIA_GET_SSE("/stats", stats);
    RUVIA_ROUTES_END

  private:
    ruvia::Task<void> rules(ruvia::Context& c) {
        co_await service::live::serve(c, "alert", [this, &c]() { return rulesSnapshot(c); });
    }

    ruvia::Task<std::string> rulesSnapshot(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:alert:query");
        co_return service::live::data(c, co_await alertService().listRules(c));
    }

    ruvia::Task<void> ruleDetail(ruvia::Context& c) {
        co_await service::live::serve(c, "alert", [this, &c]() { return ruleDetailSnapshot(c); });
    }

    ruvia::Task<std::string> ruleDetailSnapshot(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:alert:query");
        co_return service::live::data(c, co_await alertService().ruleDetail(c, alertId(c)));
    }

    ruvia::Task<ruvia::HttpResponse> createRule(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:alert:add");
        co_await alertService().createRule(c, co_await c.req().jsonValue());
        co_return c.json(service::common::operation(c, "创建成功"));
    }

    ruvia::Task<ruvia::HttpResponse> updateRule(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:alert:edit");
        co_await alertService().updateRule(c, alertId(c), co_await c.req().jsonValue());
        co_return c.json(service::common::operation(c, "更新成功"));
    }

    ruvia::Task<ruvia::HttpResponse> removeRule(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:alert:delete");
        co_await alertService().removeRule(c, alertId(c));
        co_return c.json(service::common::operation(c, "删除成功"));
    }

    ruvia::Task<ruvia::HttpResponse> batchRemoveRules(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:alert:delete");
        co_await alertService().batchRemoveRules(c, co_await c.req().jsonValue());
        co_return c.json(service::common::operation(c, "批量删除成功"));
    }

    ruvia::Task<void> templates(ruvia::Context& c) {
        co_await service::live::serve(c, "alert", [this, &c]() { return templatesSnapshot(c); });
    }

    ruvia::Task<std::string> templatesSnapshot(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:alert:query");
        co_return service::live::data(c, co_await alertService().listTemplates(c));
    }

    ruvia::Task<void> templateDetail(ruvia::Context& c) {
        co_await service::live::serve(c, "alert", [this, &c]() { return templateDetailSnapshot(c); });
    }

    ruvia::Task<std::string> templateDetailSnapshot(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:alert:query");
        co_return service::live::data(c, co_await alertService().templateDetail(c, alertId(c)));
    }

    ruvia::Task<ruvia::HttpResponse> createTemplate(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:alert:add");
        co_await alertService().createTemplate(c, co_await c.req().jsonValue());
        co_return c.json(service::common::operation(c, "创建成功"));
    }

    ruvia::Task<ruvia::HttpResponse> updateTemplate(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:alert:edit");
        co_await alertService().updateTemplate(c, alertId(c), co_await c.req().jsonValue());
        co_return c.json(service::common::operation(c, "更新成功"));
    }

    ruvia::Task<ruvia::HttpResponse> removeTemplate(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:alert:delete");
        co_await alertService().removeTemplate(c, alertId(c));
        co_return c.json(service::common::operation(c, "删除成功"));
    }

    ruvia::Task<ruvia::HttpResponse> applyTemplate(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:alert:add");
        co_return alertJson(c,
                           co_await alertService().applyTemplate(c, co_await c.req().jsonValue()),
                           "应用成功");
    }

    ruvia::Task<void> records(ruvia::Context& c) {
        co_await service::live::serve(c, "alert", [this, &c]() { return recordsSnapshot(c); });
    }

    ruvia::Task<std::string> recordsSnapshot(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:alert:query");
        co_return service::live::data(c, co_await alertService().listRecords(c));
    }

    ruvia::Task<ruvia::HttpResponse> acknowledge(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:alert:ack");
        co_await alertService().acknowledge(c, alertId(c));
        co_return c.json(service::common::operation(c, "确认成功"));
    }

    ruvia::Task<ruvia::HttpResponse> batchAcknowledge(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:alert:ack");
        co_await alertService().batchAcknowledge(c, co_await c.req().jsonValue());
        co_return c.json(service::common::operation(c, "批量确认成功"));
    }

    ruvia::Task<void> grouped(ruvia::Context& c) {
        co_await service::live::serve(c, "alert", [this, &c]() { return groupedSnapshot(c); });
    }

    ruvia::Task<std::string> groupedSnapshot(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:alert:query");
        co_return service::live::data(c, co_await alertService().grouped(c));
    }

    ruvia::Task<void> stats(ruvia::Context& c) {
        co_await service::live::serve(c, "alert", [this, &c]() { return statsSnapshot(c); });
    }

    ruvia::Task<std::string> statsSnapshot(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:alert:query");
        co_return service::live::data(c, co_await alertService().stats(c));
    }
};

} // namespace service::alert
