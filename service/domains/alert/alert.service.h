#pragma once

#include <algorithm>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/web/Context.h>
#include <ruvia/web/ModelObject.h>
#include <ruvia/web/db/Db.h>

#include "service/common/http.h"
#include "service/common/uuid.h"
#include "service/features/access/contract.h"
#include "service/middleware/auth.h"

namespace service::alert {

class AlertService final {
  public:
    static AlertService& instance() {
        static AlertService service;
        return service;
    }

    ruvia::Task<std::string> listRules(ruvia::Context& c) {
        const auto pagination = service::access::page(c.req());
        std::string where = " WHERE rule.deleted_at IS NULL";
        std::vector<ruvia::DbValue> params;
        appendTextFilter(c, "keyword", "rule.name ILIKE", where, params, true);
        appendUuidFilter(c, "deviceId", "rule.device_id", where, params);
        appendTextFilter(c, "severity", "rule.severity =", where, params, false);
        appendTextFilter(c, "status", "rule.status::text =", where, params, false);
        const auto limit = addPageParams(params, pagination);
        co_return firstJson(co_await c.db().query(
            R"sql(
WITH filtered AS (
  SELECT rule.*, device.name AS device_name
  FROM alert_rule rule
  JOIN device ON device.id = rule.device_id
)sql" + where + R"sql(
), counted AS (SELECT COUNT(*) AS total FROM filtered), listed AS (
  SELECT * FROM filtered ORDER BY created_at DESC, id DESC
  LIMIT $)sql" + std::to_string(limit.first) + "::bigint OFFSET $" +
                std::to_string(limit.second) + R"sql(::bigint
)
SELECT jsonb_build_object(
  'list', COALESCE((SELECT jsonb_agg(jsonb_build_object(
    'id', id, 'name', name, 'device_id', device_id, 'device_name', device_name,
    'severity', severity, 'conditions', conditions, 'logic', logic,
    'silence_duration', silence_duration, 'recovery_condition', recovery_condition,
    'recovery_wait_seconds', recovery_wait_seconds, 'status', status,
    'remark', remark, 'created_at', created_at, 'updated_at', updated_at)
    ORDER BY created_at DESC, id DESC) FROM listed), '[]'::jsonb),
  'total', COALESCE((SELECT total FROM counted), 0),
  'page', $)sql" + std::to_string(limit.third) + R"sql(::bigint,
  'pageSize', $)sql" + std::to_string(limit.first) + R"sql(::bigint,
  'totalPages', CASE WHEN $)sql" + std::to_string(limit.first) +
                R"sql(::bigint = 0 THEN 0 ELSE CEIL(
    COALESCE((SELECT total FROM counted), 0)::numeric / $)sql" +
                std::to_string(limit.first) + "::bigint)::bigint END)::text",
            params));
    }

    ruvia::Task<std::string> ruleDetail(ruvia::Context& c, std::string_view id) {
        service::access::requireUuid(id, "告警规则 ID 无效");
        co_return firstObject(co_await c.db().query(R"sql(
SELECT jsonb_build_object(
  'id', rule.id, 'name', rule.name, 'device_id', rule.device_id,
  'device_name', device.name, 'severity', rule.severity,
  'conditions', rule.conditions, 'logic', rule.logic,
  'silence_duration', rule.silence_duration,
  'recovery_condition', rule.recovery_condition,
  'recovery_wait_seconds', rule.recovery_wait_seconds,
  'status', rule.status, 'remark', rule.remark,
  'created_at', rule.created_at, 'updated_at', rule.updated_at)::text
FROM alert_rule rule JOIN device ON device.id = rule.device_id
WHERE rule.id = $1::uuid AND rule.deleted_at IS NULL)sql",
                                                      service::common::dbParams(id)),
                           "告警规则不存在");
    }

    ruvia::Task<void> createRule(ruvia::Context& c, const ruvia::JsonValue& payload) {
        const auto input = ruleInput(payload);
        co_await ensureDevice(c, input.deviceId);
        co_await ensureRuleName(c, input.name, input.deviceId, std::nullopt);
        const auto principal = service::middleware::requireAuth(c);
        const auto id = service::common::nextUuidV7();
        (void)co_await c.db().execute(R"sql(
INSERT INTO alert_rule(
  id, name, device_id, severity, conditions, logic, silence_duration,
  recovery_condition, recovery_wait_seconds, status, remark, created_by)
VALUES ($1::uuid, $2, $3::uuid, $4, $5::jsonb, $6, $7::integer,
        $8, $9::integer, $10::status_enum, NULLIF($11, ''), $12::uuid))sql",
                                      service::common::dbParams(
                                          id, input.name, input.deviceId, input.severity,
                                          input.conditions, input.logic,
                                          input.silenceDuration, input.recoveryCondition,
                                          input.recoveryWaitSeconds, input.status, input.remark,
                                          principal.userId));
    }

    ruvia::Task<void> updateRule(ruvia::Context& c, std::string_view id,
                                 const ruvia::JsonValue& payload) {
        service::access::requireUuid(id, "告警规则 ID 无效");
        const auto input = ruleInput(payload);
        co_await requireRule(c, id);
        co_await ensureDevice(c, input.deviceId);
        co_await ensureRuleName(c, input.name, input.deviceId, std::string(id));
        (void)co_await c.db().execute(R"sql(
UPDATE alert_rule SET
  name = $2, device_id = $3::uuid, severity = $4, conditions = $5::jsonb,
  logic = $6, silence_duration = $7::integer, recovery_condition = $8,
  recovery_wait_seconds = $9::integer, status = $10::status_enum,
  remark = NULLIF($11, ''), updated_at = NOW()
WHERE id = $1::uuid AND deleted_at IS NULL)sql",
                                      service::common::dbParams(
                                          id, input.name, input.deviceId, input.severity,
                                          input.conditions, input.logic, input.silenceDuration,
                                          input.recoveryCondition, input.recoveryWaitSeconds,
                                          input.status, input.remark));
    }

    ruvia::Task<void> removeRule(ruvia::Context& c, std::string_view id) {
        service::access::requireUuid(id, "告警规则 ID 无效");
        co_await requireRule(c, id);
        auto transaction = co_await c.db().beginTransaction();
        (void)co_await transaction.execute(
            "UPDATE open_alert_record SET status = 'resolved', resolved_at = NOW(), "
            "updated_at = NOW() WHERE rule_id = $1::uuid AND status IN ('active','acknowledged')",
            service::common::dbParams(id));
        (void)co_await transaction.execute(
            "UPDATE alert_rule SET deleted_at = NOW(), updated_at = NOW() "
            "WHERE id = $1::uuid AND deleted_at IS NULL",
            service::common::dbParams(id));
        co_await transaction.commit();
    }

    ruvia::Task<void> batchRemoveRules(ruvia::Context& c, const ruvia::JsonValue& payload) {
        const auto ids = requiredUuids(payload, "ids", "请选择要删除的规则");
        auto transaction = co_await c.db().beginTransaction();
        const auto array = uuidArrayLiteral(ids);
        (void)co_await transaction.execute(
            "UPDATE open_alert_record SET status = 'resolved', resolved_at = NOW(), "
            "updated_at = NOW() WHERE rule_id = ANY($1::uuid[]) "
            "AND status IN ('active','acknowledged')",
            service::common::dbParams(array));
        (void)co_await transaction.execute(
            "UPDATE alert_rule SET deleted_at = NOW(), updated_at = NOW() "
            "WHERE id = ANY($1::uuid[]) AND deleted_at IS NULL",
            service::common::dbParams(array));
        co_await transaction.commit();
    }

    ruvia::Task<std::string> listTemplates(ruvia::Context& c) {
        const auto pagination = service::access::page(c.req());
        std::string where = " WHERE template.deleted_at IS NULL";
        std::vector<ruvia::DbValue> params;
        appendTextFilter(c, "category", "template.category =", where, params, false);
        const auto limit = addPageParams(params, pagination);
        co_return firstJson(co_await c.db().query(
            R"sql(
WITH filtered AS (
  SELECT template.*, config.name AS config_name, config.protocol AS protocol_type
  FROM alert_rule_template template
  LEFT JOIN protocol_config config ON config.id = template.protocol_config_id
)sql" + where + R"sql(
), counted AS (SELECT COUNT(*) AS total FROM filtered), listed AS (
  SELECT * FROM filtered ORDER BY created_at DESC, id DESC
  LIMIT $)sql" + std::to_string(limit.first) + "::bigint OFFSET $" +
                std::to_string(limit.second) + R"sql(::bigint
)
SELECT jsonb_build_object(
  'list', COALESCE((SELECT jsonb_agg(jsonb_build_object(
    'id', id, 'name', name, 'category', category, 'description', description,
    'severity', severity, 'logic', logic, 'silence_duration', silence_duration,
    'protocol_config_id', protocol_config_id, 'config_name', config_name,
    'protocol_type', protocol_type, 'created_at', created_at)
    ORDER BY created_at DESC, id DESC) FROM listed), '[]'::jsonb),
  'total', COALESCE((SELECT total FROM counted), 0),
  'page', $)sql" + std::to_string(limit.third) + R"sql(::bigint,
  'pageSize', $)sql" + std::to_string(limit.first) + R"sql(::bigint,
  'totalPages', CASE WHEN $)sql" + std::to_string(limit.first) +
                R"sql(::bigint = 0 THEN 0 ELSE CEIL(
    COALESCE((SELECT total FROM counted), 0)::numeric / $)sql" +
                std::to_string(limit.first) + "::bigint)::bigint END)::text",
            params));
    }

    ruvia::Task<std::string> templateDetail(ruvia::Context& c, std::string_view id) {
        service::access::requireUuid(id, "告警模板 ID 无效");
        co_return firstObject(co_await c.db().query(R"sql(
SELECT jsonb_build_object(
  'id', id, 'name', name, 'category', category, 'description', description,
  'severity', severity, 'conditions', conditions, 'logic', logic,
  'silence_duration', silence_duration, 'recovery_condition', recovery_condition,
  'recovery_wait_seconds', recovery_wait_seconds,
  'applicable_protocols', applicable_protocols,
  'protocol_config_id', protocol_config_id, 'created_by', created_by,
  'created_at', created_at, 'updated_at', updated_at)::text
FROM alert_rule_template WHERE id = $1::uuid AND deleted_at IS NULL)sql",
                                                      service::common::dbParams(id)),
                           "告警模板不存在");
    }

    ruvia::Task<void> createTemplate(ruvia::Context& c, const ruvia::JsonValue& payload) {
        const auto input = templateInput(payload);
        co_await ensureTemplateName(c, input.name, std::nullopt);
        if (!input.protocolConfigId.empty())
            co_await ensureProtocolConfig(c, input.protocolConfigId);
        const auto principal = service::middleware::requireAuth(c);
        const auto id = service::common::nextUuidV7();
        (void)co_await c.db().execute(R"sql(
INSERT INTO alert_rule_template(
  id, name, category, description, severity, conditions, logic, silence_duration,
  recovery_condition, recovery_wait_seconds, applicable_protocols,
  protocol_config_id, created_by)
VALUES ($1::uuid, $2, NULLIF($3, ''), NULLIF($4, ''), $5, $6::jsonb, $7,
        $8::integer, $9, $10::integer, $11::jsonb, NULLIF($12, '')::uuid, $13::uuid))sql",
                                      service::common::dbParams(
                                          id, input.name, input.category, input.description,
                                          input.severity, input.conditions, input.logic,
                                          input.silenceDuration,
                                          input.recoveryCondition, input.recoveryWaitSeconds,
                                          input.applicableProtocols, input.protocolConfigId,
                                          principal.userId));
    }

    ruvia::Task<void> updateTemplate(ruvia::Context& c, std::string_view id,
                                     const ruvia::JsonValue& payload) {
        service::access::requireUuid(id, "告警模板 ID 无效");
        co_await requireTemplate(c, id);
        const auto input = templateInput(payload);
        co_await ensureTemplateName(c, input.name, std::string(id));
        if (!input.protocolConfigId.empty())
            co_await ensureProtocolConfig(c, input.protocolConfigId);
        (void)co_await c.db().execute(R"sql(
UPDATE alert_rule_template SET
  name = $2, category = NULLIF($3, ''), description = NULLIF($4, ''),
  severity = $5, conditions = $6::jsonb, logic = $7,
  silence_duration = $8::integer, recovery_condition = $9,
  recovery_wait_seconds = $10::integer, applicable_protocols = $11::jsonb,
  protocol_config_id = NULLIF($12, '')::uuid, updated_at = NOW()
WHERE id = $1::uuid AND deleted_at IS NULL)sql",
                                      service::common::dbParams(
                                          id, input.name, input.category, input.description,
                                          input.severity, input.conditions, input.logic,
                                          input.silenceDuration, input.recoveryCondition,
                                          input.recoveryWaitSeconds, input.applicableProtocols,
                                          input.protocolConfigId));
    }

    ruvia::Task<void> removeTemplate(ruvia::Context& c, std::string_view id) {
        service::access::requireUuid(id, "告警模板 ID 无效");
        co_await requireTemplate(c, id);
        (void)co_await c.db().execute(
            "UPDATE alert_rule_template SET deleted_at = NOW(), updated_at = NOW() "
            "WHERE id = $1::uuid AND deleted_at IS NULL",
            service::common::dbParams(id));
    }

    ruvia::Task<std::string> applyTemplate(ruvia::Context& c,
                                           const ruvia::JsonValue& payload) {
        const auto templateId = requiredUuid(payload, "template_id", "请选择告警模板");
        const auto deviceIds = requiredUuids(payload, "device_ids", "请选择目标设备");
        const auto principal = service::middleware::requireAuth(c);
        co_await requireTemplate(c, templateId);
        const auto deviceIdArray = uuidArrayLiteral(deviceIds);
        const auto result = co_await c.db().query(R"sql(
WITH selected AS (
  SELECT * FROM alert_rule_template
  WHERE id = $1::uuid AND deleted_at IS NULL
), requested AS (
  SELECT unnest($2::uuid[]) AS device_id
), created AS (
  INSERT INTO alert_rule(
    id, name, device_id, severity, conditions, logic, silence_duration,
    recovery_condition, recovery_wait_seconds, status, remark, created_by)
  SELECT gen_random_uuid(), selected.name || ' - ' || device.name, device.id,
         selected.severity, selected.conditions, selected.logic,
         selected.silence_duration, selected.recovery_condition,
         selected.recovery_wait_seconds, 'enabled', selected.description, $3::uuid
  FROM selected JOIN requested ON TRUE
  JOIN device ON device.id = requested.device_id AND device.deleted_at IS NULL
  WHERE NOT EXISTS (
    SELECT 1 FROM alert_rule existing
    WHERE existing.device_id = device.id AND existing.name = selected.name || ' - ' || device.name
      AND existing.deleted_at IS NULL)
  RETURNING id
)
SELECT jsonb_build_object(
  'success', (SELECT COUNT(*) FROM created),
  'total', cardinality($2::uuid[]),
  'createdIds', COALESCE((SELECT jsonb_agg(id) FROM created), '[]'::jsonb))::text)sql",
                                                  service::common::dbParams(
                                                      templateId, deviceIdArray, principal.userId));
        co_return firstJson(result);
    }

    ruvia::Task<std::string> listRecords(ruvia::Context& c) {
        const auto pagination = service::access::page(c.req());
        std::string where = " WHERE TRUE";
        std::vector<ruvia::DbValue> params;
        appendUuidFilter(c, "deviceId", "record.device_id", where, params);
        appendUuidFilter(c, "ruleId", "record.rule_id", where, params);
        appendTextFilter(c, "status", "record.status =", where, params, false);
        appendTextFilter(c, "severity", "record.severity =", where, params, false);
        const auto limit = addPageParams(params, pagination);
        co_return firstJson(co_await c.db().query(
            R"sql(
WITH filtered AS (
  SELECT record.*, rule.name AS rule_name, device.name AS device_name
  FROM open_alert_record record
  LEFT JOIN alert_rule rule ON rule.id = record.rule_id
  JOIN device ON device.id = record.device_id
)sql" + where + R"sql(
), counted AS (SELECT COUNT(*) AS total FROM filtered), listed AS (
  SELECT * FROM filtered ORDER BY triggered_at DESC, id DESC
  LIMIT $)sql" + std::to_string(limit.first) + "::bigint OFFSET $" +
                std::to_string(limit.second) + R"sql(::bigint
)
SELECT jsonb_build_object(
  'list', COALESCE((SELECT jsonb_agg(jsonb_build_object(
    'id', id, 'rule_id', rule_id, 'rule_name', rule_name,
    'device_id', device_id, 'device_name', device_name,
    'severity', severity, 'status', status, 'message', message, 'detail', detail,
    'triggered_at', triggered_at, 'acknowledged_at', acknowledged_at,
    'acknowledged_by', acknowledged_by, 'resolved_at', resolved_at)
    ORDER BY triggered_at DESC, id DESC) FROM listed), '[]'::jsonb),
  'total', COALESCE((SELECT total FROM counted), 0),
  'page', $)sql" + std::to_string(limit.third) + R"sql(::bigint,
  'pageSize', $)sql" + std::to_string(limit.first) + R"sql(::bigint,
  'totalPages', CASE WHEN $)sql" + std::to_string(limit.first) +
                R"sql(::bigint = 0 THEN 0 ELSE CEIL(
    COALESCE((SELECT total FROM counted), 0)::numeric / $)sql" +
                std::to_string(limit.first) + "::bigint)::bigint END)::text",
            params));
    }

    ruvia::Task<void> acknowledge(ruvia::Context& c, std::string_view id) {
        service::access::requireUuid(id, "告警记录 ID 无效");
        const auto principal = service::middleware::requireAuth(c);
        const auto result = co_await c.db().execute(R"sql(
UPDATE open_alert_record
SET status = 'acknowledged', acknowledged_at = NOW(), acknowledged_by = $2::uuid,
    updated_at = NOW()
WHERE id = $1::uuid AND status = 'active')sql",
                                                   service::common::dbParams(id, principal.userId));
        if (result.affectedRows() == 0)
            service::common::fail(17003, "告警记录不存在或已处理", 404);
    }

    ruvia::Task<void> batchAcknowledge(ruvia::Context& c, const ruvia::JsonValue& payload) {
        const auto ids = requiredUuids(payload, "ids", "请选择要确认的告警");
        const auto principal = service::middleware::requireAuth(c);
        const auto idArray = uuidArrayLiteral(ids);
        (void)co_await c.db().execute(R"sql(
UPDATE open_alert_record
SET status = 'acknowledged', acknowledged_at = NOW(), acknowledged_by = $2::uuid,
    updated_at = NOW()
WHERE id = ANY($1::uuid[]) AND status = 'active')sql",
                                      service::common::dbParams(idArray, principal.userId));
    }

    ruvia::Task<std::string> stats(ruvia::Context& c) {
        co_return firstJson(co_await c.db().query(R"sql(
SELECT jsonb_build_object(
  'total', COUNT(*) FILTER (WHERE status IN ('active','acknowledged')),
  'critical', COUNT(*) FILTER (WHERE status IN ('active','acknowledged') AND severity = 'critical'),
  'warning', COUNT(*) FILTER (WHERE status IN ('active','acknowledged') AND severity = 'warning'),
  'info', COUNT(*) FILTER (WHERE status IN ('active','acknowledged') AND severity = 'info'),
  'today_new', COUNT(*) FILTER (WHERE triggered_at >= CURRENT_DATE),
  'acknowledged', COUNT(*) FILTER (WHERE status = 'acknowledged'),
  'today_resolved', COUNT(*) FILTER (
    WHERE status = 'resolved' AND resolved_at >= CURRENT_DATE),
  'affected_devices', COUNT(DISTINCT device_id) FILTER (
    WHERE status IN ('active','acknowledged')))::text
FROM open_alert_record)sql"));
    }

    ruvia::Task<std::string> grouped(ruvia::Context& c) {
        const auto days = std::clamp<std::int64_t>(
            service::common::parseInt64(c.req().query("days")).value_or(7), 1, 365);
        co_return firstJson(co_await c.db().query(R"sql(
SELECT COALESCE(jsonb_agg(jsonb_build_object(
  'rule_id', grouped.rule_id, 'rule_name', grouped.rule_name,
  'device_id', grouped.device_id, 'device_name', grouped.device_name,
  'severity', grouped.severity, 'total_count', grouped.total_count,
  'active_count', grouped.active_count, 'acked_count', grouped.acked_count,
  'resolved_count', grouped.resolved_count,
  'latest_trigger_time', grouped.latest_trigger_time)
  ORDER BY grouped.latest_trigger_time DESC), '[]'::jsonb)::text
FROM (
  SELECT record.rule_id, COALESCE(rule.name, '已删除规则') AS rule_name,
         record.device_id, device.name AS device_name, record.severity,
         COUNT(*) AS total_count,
         COUNT(*) FILTER (WHERE record.status = 'active') AS active_count,
         COUNT(*) FILTER (WHERE record.status = 'acknowledged') AS acked_count,
         COUNT(*) FILTER (WHERE record.status = 'resolved') AS resolved_count,
         MAX(record.triggered_at) AS latest_trigger_time
  FROM open_alert_record record
  LEFT JOIN alert_rule rule ON rule.id = record.rule_id
  JOIN device ON device.id = record.device_id
  WHERE record.triggered_at >= NOW() - ($1::bigint * interval '1 day')
  GROUP BY record.rule_id, rule.name, record.device_id, device.name, record.severity
) grouped)sql",
                                                  service::common::dbParams(days)));
    }

  private:
    struct RuleInput final {
        std::string name;
        std::string deviceId;
        std::string severity;
        std::string conditions;
        std::string logic;
        std::int64_t silenceDuration{};
        std::string recoveryCondition;
        std::int64_t recoveryWaitSeconds{};
        std::string status;
        std::string remark;
    };

    struct TemplateInput final {
        std::string name;
        std::string category;
        std::string description;
        std::string severity;
        std::string conditions;
        std::string logic;
        std::int64_t silenceDuration{};
        std::string recoveryCondition;
        std::int64_t recoveryWaitSeconds{};
        std::string applicableProtocols;
        std::string protocolConfigId;
    };

    struct PageParams final {
        std::size_t first{};
        std::size_t second{};
        std::size_t third{};
    };

    static RuleInput ruleInput(const ruvia::JsonValue& payload) {
        RuleInput result;
        result.name = requiredString(payload, "name", "规则名称不能为空", 128);
        result.deviceId = requiredUuid(payload, "device_id", "请选择关联设备");
        result.severity = enumString(payload, "severity", "warning",
                                     {"critical", "warning", "info"});
        result.conditions = requiredArray(payload, "conditions", "至少配置一个告警条件");
        result.logic = enumString(payload, "logic", "and", {"and", "or"});
        result.silenceDuration = integer(payload, "silence_duration", 300, 0, 86400);
        result.recoveryCondition =
            optionalString(payload, "recovery_condition", 32).value_or("reverse");
        result.recoveryWaitSeconds =
            integer(payload, "recovery_wait_seconds", 60, 0, 86400);
        result.status = enumString(payload, "status", "enabled", {"enabled", "disabled"});
        result.remark = optionalString(payload, "remark", 500).value_or("");
        return result;
    }

    static TemplateInput templateInput(const ruvia::JsonValue& payload) {
        TemplateInput result;
        result.name = requiredString(payload, "name", "模板名称不能为空", 128);
        result.category = optionalString(payload, "category", 64).value_or("");
        result.description = optionalString(payload, "description", 500).value_or("");
        result.severity = enumString(payload, "severity", "warning",
                                     {"critical", "warning", "info"});
        result.conditions = requiredArray(payload, "conditions", "至少配置一个告警条件");
        result.logic = enumString(payload, "logic", "and", {"and", "or"});
        result.silenceDuration = integer(payload, "silence_duration", 300, 0, 86400);
        result.recoveryCondition =
            optionalString(payload, "recovery_condition", 32).value_or("reverse");
        result.recoveryWaitSeconds =
            integer(payload, "recovery_wait_seconds", 60, 0, 86400);
        result.applicableProtocols = array(payload, "applicable_protocols", "[]");
        result.protocolConfigId = optionalUuid(payload, "protocol_config_id");
        return result;
    }

    static std::string requiredString(const ruvia::JsonValue& payload, std::string_view field,
                                      std::string_view message, std::size_t maximum) {
        const auto value = payload.get<ruvia::String>(field);
        if (!value)
            service::common::fail(17002, std::string(message), 400);
        auto result = service::access::trim(value->view());
        if (result.empty() || result.size() > maximum)
            service::common::fail(17002, std::string(message), 400);
        return result;
    }

    static std::optional<std::string> optionalString(const ruvia::JsonValue& payload,
                                                     std::string_view field,
                                                     std::size_t maximum) {
        const auto value = payload.get<ruvia::String>(field);
        if (!value)
            return std::nullopt;
        auto result = service::access::trim(value->view());
        if (result.size() > maximum)
            service::common::fail(17002, std::string(field) + " 长度超出限制", 400);
        return result;
    }

    static std::string requiredUuid(const ruvia::JsonValue& payload, std::string_view field,
                                    std::string_view message) {
        const auto value = payload.get<ruvia::String>(field);
        if (!value)
            service::common::fail(17002, std::string(message), 400);
        service::access::requireUuid(value->view(), message);
        return std::string(value->view());
    }

    static std::string optionalUuid(const ruvia::JsonValue& payload, std::string_view field) {
        const auto value = payload.get<ruvia::String>(field);
        if (!value || value->view().empty())
            return {};
        service::access::requireUuid(value->view(), std::string(field) + " 无效");
        return std::string(value->view());
    }

    static std::vector<std::string> requiredUuids(const ruvia::JsonValue& payload,
                                                  std::string_view field,
                                                  std::string_view message) {
        const auto values = payload.get<ruvia::Array<ruvia::String>>(field);
        if (!values || values->empty() || values->size() > 1000)
            service::common::fail(17002, std::string(message), 400);
        std::set<std::string, std::less<>> unique;
        for (const auto& value : *values) {
            service::access::requireUuid(value.view(),
                                         std::string(field) + " 包含无效 UUID");
            unique.emplace(value.view());
        }
        return {unique.begin(), unique.end()};
    }

    static std::string enumString(const ruvia::JsonValue& payload, std::string_view field,
                                  std::string_view fallback,
                                  std::initializer_list<std::string_view> allowed) {
        const auto value = optionalString(payload, field, 32).value_or(std::string(fallback));
        if (std::find(allowed.begin(), allowed.end(), value) == allowed.end())
            service::common::fail(17002, std::string(field) + " 无效", 400);
        return value;
    }

    static std::int64_t integer(const ruvia::JsonValue& payload, std::string_view field,
                                std::int64_t fallback, std::int64_t minimum,
                                std::int64_t maximum) {
        const auto value = payload.get<ruvia::Int64>(field);
        const auto result = value ? static_cast<std::int64_t>(*value) : fallback;
        if (result < minimum || result > maximum)
            service::common::fail(17002, std::string(field) + " 超出允许范围", 400);
        return result;
    }

    static std::string array(const ruvia::JsonValue& payload, std::string_view field,
                             std::string_view fallback) {
        const auto value = service::access::jsonField(payload, field);
        if (!value)
            return std::string(fallback);
        if (!value->isArray())
            service::common::fail(17002, std::string(field) + " 必须是数组", 400);
        return std::string(value->view());
    }

    static std::string requiredArray(const ruvia::JsonValue& payload, std::string_view field,
                                     std::string_view message) {
        const auto result = array(payload, field, "[]");
        if (result == "[]")
            service::common::fail(17002, std::string(message), 400);
        return result;
    }

    static std::string uuidArrayLiteral(const std::vector<std::string>& values) {
        std::string result{"{"};
        for (std::size_t index = 0; index < values.size(); ++index) {
            if (index != 0)
                result.push_back(',');
            result += values[index];
        }
        result.push_back('}');
        return result;
    }

    template <typename Rows> static std::string firstJson(const Rows& rows) {
        if (rows.rows().empty() || rows.rows().front().empty() || rows.rows().front()[0].isNull())
            return "{}";
        return std::string(rows.rows().front()[0].text());
    }

    template <typename Rows>
    static std::string firstObject(const Rows& rows, std::string_view notFound) {
        if (rows.rows().empty())
            service::common::fail(17003, std::string(notFound), 404);
        return std::string(rows.rows().front()[0].text());
    }

    static PageParams addPageParams(std::vector<ruvia::DbValue>& params,
                                    const service::access::Page& pagination) {
        params.emplace_back(pagination.pageSize);
        const auto limit = params.size();
        params.emplace_back(pagination.offset);
        const auto offset = params.size();
        params.emplace_back(pagination.page);
        return {.first = limit, .second = offset, .third = params.size()};
    }

    static void appendTextFilter(ruvia::Context& c, std::string_view query,
                                 std::string_view expression, std::string& where,
                                 std::vector<ruvia::DbValue>& params, bool contains) {
        const auto value = c.req().query(query);
        if (!value || value->empty())
            return;
        params.emplace_back(*value);
        where += " AND " + std::string(expression) + " ";
        if (contains)
            where += "'%' || ";
        where += "$" + std::to_string(params.size());
        if (contains)
            where += " || '%'";
    }

    static void appendUuidFilter(ruvia::Context& c, std::string_view query,
                                 std::string_view expression, std::string& where,
                                 std::vector<ruvia::DbValue>& params) {
        const auto value = c.req().query(query);
        if (!value || value->empty())
            return;
        service::access::requireUuid(*value, std::string(query) + " 无效");
        params.emplace_back(*value);
        where += " AND " + std::string(expression) + " = $" + std::to_string(params.size()) +
                 "::uuid";
    }

    static ruvia::Task<void> ensureDevice(ruvia::Context& c, std::string_view id) {
        const auto rows = co_await c.db().query(
            "SELECT 1 FROM device WHERE id = $1::uuid AND deleted_at IS NULL",
            service::common::dbParams(id));
        if (rows.rows().empty())
            service::common::fail(17003, "关联设备不存在", 404);
    }

    static ruvia::Task<void> ensureProtocolConfig(ruvia::Context& c, std::string_view id) {
        const auto rows = co_await c.db().query(
            "SELECT 1 FROM protocol_config WHERE id = $1::uuid AND deleted_at IS NULL",
            service::common::dbParams(id));
        if (rows.rows().empty())
            service::common::fail(17003, "协议配置不存在", 404);
    }

    static ruvia::Task<void> requireRule(ruvia::Context& c, std::string_view id) {
        const auto rows = co_await c.db().query(
            "SELECT 1 FROM alert_rule WHERE id = $1::uuid AND deleted_at IS NULL",
            service::common::dbParams(id));
        if (rows.rows().empty())
            service::common::fail(17003, "告警规则不存在", 404);
    }

    static ruvia::Task<void> requireTemplate(ruvia::Context& c, std::string_view id) {
        const auto rows = co_await c.db().query(
            "SELECT 1 FROM alert_rule_template WHERE id = $1::uuid AND deleted_at IS NULL",
            service::common::dbParams(id));
        if (rows.rows().empty())
            service::common::fail(17003, "告警模板不存在", 404);
    }

    static ruvia::Task<void> ensureRuleName(ruvia::Context& c, std::string_view name,
                                            std::string_view deviceId,
                                            const std::optional<std::string>& excluded) {
        const auto excludedId = excluded.value_or("");
        const auto rows = co_await c.db().query(
            "SELECT 1 FROM alert_rule WHERE device_id = $1::uuid AND name = $2 "
            "AND deleted_at IS NULL AND ($3 = '' OR id <> NULLIF($3, '')::uuid)",
            service::common::dbParams(deviceId, name, excludedId));
        if (!rows.rows().empty())
            service::common::fail(17009, "该设备已存在同名告警规则", 409);
    }

    static ruvia::Task<void> ensureTemplateName(ruvia::Context& c, std::string_view name,
                                                const std::optional<std::string>& excluded) {
        const auto excludedId = excluded.value_or("");
        const auto rows = co_await c.db().query(
            "SELECT 1 FROM alert_rule_template WHERE name = $1 AND deleted_at IS NULL "
            "AND ($2 = '' OR id <> NULLIF($2, '')::uuid)",
            service::common::dbParams(name, excludedId));
        if (!rows.rows().empty())
            service::common::fail(17009, "告警模板名称已存在", 409);
    }
};

inline AlertService& alertService() { return AlertService::instance(); }

} // namespace service::alert
