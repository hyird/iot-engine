#pragma once

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

#include <ruvia/web/Context.h>
#include <ruvia/web/ModelObject.h>
#include <ruvia/web/detail/json/JsonSkip.h>
#include <ruvia/web/db/Db.h>
#include <ruvia/web/db/DbQuery.h>

#include "service/common/http.h"
#include "service/common/uuid.h"
#include "service/middleware/rpc.h"
#include "service/middleware/auth.h"
#include "service/utils/json.h"
#include "service/utils/text.h"

namespace service::alert {

class AlertService final {
    using Query = ruvia::DbQuery;
    using Op = ruvia::DbBinaryOperator;
    using Type = ruvia::DbDataType;
  public:
    static AlertService& instance() {
        static AlertService service;
        return service;
    }

    ruvia::Task<std::string> listRules(ruvia::Context& c) {
        Query filtered(c.pool());
        filtered.select({ filtered.star("rule"), filtered.alias(filtered.column("name", "device"), "device_name") })
            .from("alert_rule", "rule")
            .join(ruvia::DbJoinType::kInner, "device", filtered.binary(filtered.column("id", "device"), Op::kEqual, filtered.column("device_id", "rule")))
            .andWhere(filtered.unary(ruvia::DbUnaryOperator::kIsNull, filtered.column("deleted_at", "rule")));
        appendTextFilter(c, "keyword", filtered, filtered.column("name", "rule"), true);
        appendUuidFilter(c, "deviceId", filtered, filtered.column("device_id", "rule"));
        appendTextFilter(c, "severity", filtered, filtered.column("severity", "rule"));
        appendTextFilter(c, "status", filtered, filtered.cast(filtered.column("status", "rule"), Type::kText));
        Query listed(c.pool());
        listed.select(listed.star()).from("page_rows");
        co_return co_await pageResult(c, filtered, listed, { "id", "name", "device_id", "device_name", "severity", "conditions", "logic",
            "silence_duration", "recovery_condition", "recovery_wait_seconds", "status", "remark", "created_at", "updated_at" }, "created_at");
    }

    ruvia::Task<std::string> ruleDetail(ruvia::Context& c, std::string_view id) {
        service::common::requireUuid(19002, id, "告警规则 ID 无效");
        Query rule(c.pool());
        rule.select({ rule.star("rule"), rule.alias(rule.column("name", "device"), "device_name") }).from("alert_rule", "rule")
            .join(ruvia::DbJoinType::kInner, "device", rule.binary(rule.column("id", "device"), Op::kEqual, rule.column("device_id", "rule")))
            .andWhere(rule.binary(rule.column("id", "rule"), Op::kEqual, rule.cast(rule.value(id), Type::kUuid)))
            .andWhere(rule.unary(ruvia::DbUnaryOperator::kIsNull, rule.column("deleted_at", "rule")));
        Query result(c.pool());
        result.select(result.cast(fieldJson(result, { "id", "name", "device_id", "device_name", "severity", "conditions", "logic",
            "silence_duration", "recovery_condition", "recovery_wait_seconds", "status", "remark", "created_at", "updated_at" }), Type::kText)).from(rule, "rule");
        co_return firstObject(co_await c.db().query(result), "告警规则不存在");
    }

    ruvia::Task<void> createRule(ruvia::Context& c, const ruvia::JsonValue& payload) {
        const auto input = ruleInput(payload);
        co_await ensureDevice(c, input.deviceId);
        co_await ensureRuleName(c, input.name, input.deviceId, std::nullopt);
        const auto principal = service::middleware::requireAuth(c);
        const auto id = service::common::nextUuidV7();
        Query query(c.pool());
        query.insertInto("alert_rule", { "id", "name", "device_id", "severity", "conditions", "logic", "silence_duration", "recovery_condition", "recovery_wait_seconds", "status", "remark", "created_by" })
            .values({ query.cast(query.value(id), Type::kUuid),
                query.value(input.name),
                query.cast(query.value(input.deviceId), Type::kUuid),
                query.value(input.severity),
                query.cast(query.value(input.conditions), Type::kJsonb),
                query.value(input.logic),
                query.cast(query.value(input.silenceDuration), Type::kInteger),
                query.value(input.recoveryCondition),
                query.cast(query.value(input.recoveryWaitSeconds), Type::kInteger),
                query.cast(query.value(input.status), { .customName = "status_enum" }),
                query.nullIf(query.value(input.remark), query.value("")),
                query.cast(query.value(principal.userId), Type::kUuid) });
        (void)co_await c.db().execute(query);
        (void)co_await service::rpc::call(c, "alert", "refresh", "{}");
    }

    ruvia::Task<void> updateRule(ruvia::Context& c, std::string_view id,
                                 const ruvia::JsonValue& payload) {
        service::common::requireUuid(19002, id, "告警规则 ID 无效");
        const auto input = ruleInput(payload);
        co_await requireRule(c, id);
        co_await ensureDevice(c, input.deviceId);
        co_await ensureRuleName(c, input.name, input.deviceId, std::string(id));
        Query query(c.pool());
        query.update("alert_rule")
            .set("name", query.value(input.name))
            .set("device_id", query.cast(query.value(input.deviceId), Type::kUuid))
            .set("severity", query.value(input.severity))
            .set("conditions", query.cast(query.value(input.conditions), Type::kJsonb))
            .set("logic", query.value(input.logic))
            .set("silence_duration", query.cast(query.value(input.silenceDuration), Type::kInteger))
            .set("recovery_condition", query.value(input.recoveryCondition))
            .set("recovery_wait_seconds", query.cast(query.value(input.recoveryWaitSeconds), Type::kInteger))
            .set("status", query.cast(query.value(input.status), { .customName = "status_enum" }))
            .set("remark", query.nullIf(query.value(input.remark), query.value("")))
            .set("updated_at", query.call("now"))
            .andWhere(query.binary(query.column("id"), Op::kEqual, query.cast(query.value(id), Type::kUuid)))
            .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull, query.column("deleted_at")));
        (void)co_await c.db().execute(query);
        (void)co_await service::rpc::call(c, "alert", "refresh", "{}");
    }

    ruvia::Task<void> removeRule(ruvia::Context& c, std::string_view id) {
        service::common::requireUuid(19002, id, "告警规则 ID 无效");
        co_await requireRule(c, id);
        auto transaction = co_await c.db().beginTransaction();
        Query records(c.pool());
        records.update("open_alert_record").set("status", records.value("resolved"))
            .set("resolved_at", records.call("now")).set("updated_at", records.call("now"))
            .andWhere(records.binary(records.column("rule_id"), Op::kEqual, records.cast(records.value(id), Type::kUuid)))
            .andWhere(records.binary(records.column("status"), Op::kIn, records.list({ records.value("active"), records.value("acknowledged") })));
        (void)co_await transaction.execute(records);
        Query rules(c.pool());
        rules.update("alert_rule").set("deleted_at", rules.call("now")).set("updated_at", rules.call("now"))
            .andWhere(rules.binary(rules.column("id"), Op::kEqual, rules.cast(rules.value(id), Type::kUuid)))
            .andWhere(rules.unary(ruvia::DbUnaryOperator::kIsNull, rules.column("deleted_at")));
        (void)co_await transaction.execute(rules);
        co_await transaction.commit();
        (void)co_await service::rpc::call(c, "alert", "refresh", "{}");
    }

    ruvia::Task<void> batchRemoveRules(ruvia::Context& c, const ruvia::JsonValue& payload) {
        const auto ids = requiredUuids(payload, "ids", "请选择要删除的规则");
        auto transaction = co_await c.db().beginTransaction();
        Query records(c.pool());
        records.update("open_alert_record").set("status", records.value("resolved"))
            .set("resolved_at", records.call("now")).set("updated_at", records.call("now"))
            .andWhere(records.binary(records.column("rule_id"), Op::kIn, uuidList(records, ids)))
            .andWhere(records.binary(records.column("status"), Op::kIn, records.list({ records.value("active"), records.value("acknowledged") })));
        (void)co_await transaction.execute(records);
        Query rules(c.pool());
        rules.update("alert_rule").set("deleted_at", rules.call("now")).set("updated_at", rules.call("now"))
            .andWhere(rules.binary(rules.column("id"), Op::kIn, uuidList(rules, ids)))
            .andWhere(rules.unary(ruvia::DbUnaryOperator::kIsNull, rules.column("deleted_at")));
        (void)co_await transaction.execute(rules);
        co_await transaction.commit();
        (void)co_await service::rpc::call(c, "alert", "refresh", "{}");
    }

    ruvia::Task<std::string> listTemplates(ruvia::Context& c) {
        Query filtered(c.pool());
        filtered.select({ filtered.star("template"), filtered.alias(filtered.column("name", "config"), "config_name"),
                filtered.alias(filtered.column("protocol", "config"), "protocol_type") })
            .from("alert_rule_template", "template")
            .join(ruvia::DbJoinType::kLeft, "protocol_config", filtered.binary(filtered.column("id", "config"), Op::kEqual,
                filtered.column("protocol_config_id", "template")), "config")
            .andWhere(filtered.unary(ruvia::DbUnaryOperator::kIsNull, filtered.column("deleted_at", "template")));
        appendTextFilter(c, "category", filtered, filtered.column("category", "template"));
        Query listed(c.pool());
        listed.select(listed.star()).from("page_rows");
        co_return co_await pageResult(c, filtered, listed, { "id", "name", "category", "description", "severity", "logic", "silence_duration",
            "protocol_config_id", "config_name", "protocol_type", "created_at" }, "created_at");
    }

    ruvia::Task<std::string> templateDetail(ruvia::Context& c, std::string_view id) {
        service::common::requireUuid(19002, id, "告警模板 ID 无效");
        Query query(c.pool());
        query.select(query.cast(fieldJson(query, { "id", "name", "category", "description", "severity", "conditions", "logic", "silence_duration",
            "recovery_condition", "recovery_wait_seconds", "applicable_protocols", "protocol_config_id", "created_by", "created_at", "updated_at" }), Type::kText))
            .from("alert_rule_template")
            .andWhere(query.binary(query.column("id"), Op::kEqual, query.cast(query.value(id), Type::kUuid)))
            .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull, query.column("deleted_at")));
        co_return firstObject(co_await c.db().query(query), "告警模板不存在");
    }

    ruvia::Task<void> createTemplate(ruvia::Context& c, const ruvia::JsonValue& payload) {
        const auto input = templateInput(payload);
        co_await ensureTemplateName(c, input.name, std::nullopt);
        if (!input.protocolConfigId.empty())
            co_await ensureProtocolConfig(c, input.protocolConfigId);
        const auto principal = service::middleware::requireAuth(c);
        const auto id = service::common::nextUuidV7();
        Query query(c.pool());
        query.insertInto("alert_rule_template", { "id", "name", "category", "description", "severity", "conditions", "logic", "silence_duration", "recovery_condition", "recovery_wait_seconds", "applicable_protocols", "protocol_config_id", "created_by" })
            .values({ query.cast(query.value(id), Type::kUuid),
                query.value(input.name),
                query.nullIf(query.value(input.category), query.value("")),
                query.nullIf(query.value(input.description), query.value("")),
                query.value(input.severity),
                query.cast(query.value(input.conditions), Type::kJsonb),
                query.value(input.logic),
                query.cast(query.value(input.silenceDuration), Type::kInteger),
                query.value(input.recoveryCondition),
                query.cast(query.value(input.recoveryWaitSeconds), Type::kInteger),
                query.cast(query.value(input.applicableProtocols), Type::kJsonb),
                query.cast(query.nullIf(query.value(input.protocolConfigId), query.value("")), Type::kUuid),
                query.cast(query.value(principal.userId), Type::kUuid) });
        (void)co_await c.db().execute(query);
    }

    ruvia::Task<void> updateTemplate(ruvia::Context& c, std::string_view id,
                                     const ruvia::JsonValue& payload) {
        service::common::requireUuid(19002, id, "告警模板 ID 无效");
        co_await requireTemplate(c, id);
        const auto input = templateInput(payload);
        co_await ensureTemplateName(c, input.name, std::string(id));
        if (!input.protocolConfigId.empty())
            co_await ensureProtocolConfig(c, input.protocolConfigId);
        Query query(c.pool());
        query.update("alert_rule_template")
            .set("name", query.value(input.name))
            .set("category", query.nullIf(query.value(input.category), query.value("")))
            .set("description", query.nullIf(query.value(input.description), query.value("")))
            .set("severity", query.value(input.severity))
            .set("conditions", query.cast(query.value(input.conditions), Type::kJsonb))
            .set("logic", query.value(input.logic))
            .set("silence_duration", query.cast(query.value(input.silenceDuration), Type::kInteger))
            .set("recovery_condition", query.value(input.recoveryCondition))
            .set("recovery_wait_seconds", query.cast(query.value(input.recoveryWaitSeconds), Type::kInteger))
            .set("applicable_protocols", query.cast(query.value(input.applicableProtocols), Type::kJsonb))
            .set("protocol_config_id", query.cast(query.nullIf(query.value(input.protocolConfigId), query.value("")), Type::kUuid))
            .set("updated_at", query.call("now"))
            .andWhere(query.binary(query.column("id"), Op::kEqual, query.cast(query.value(id), Type::kUuid)))
            .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull, query.column("deleted_at")));
        (void)co_await c.db().execute(query);
    }

    ruvia::Task<void> removeTemplate(ruvia::Context& c, std::string_view id) {
        service::common::requireUuid(19002, id, "告警模板 ID 无效");
        co_await requireTemplate(c, id);
        Query removal(c.pool());
        removal.update("alert_rule_template").set("deleted_at", removal.call("now")).set("updated_at", removal.call("now"))
            .andWhere(removal.binary(removal.column("id"), Op::kEqual, removal.cast(removal.value(id), Type::kUuid)))
            .andWhere(removal.unary(ruvia::DbUnaryOperator::kIsNull, removal.column("deleted_at")));
        (void)co_await c.db().execute(removal);
    }

    ruvia::Task<std::string> applyTemplate(ruvia::Context& c,
                                           const ruvia::JsonValue& payload) {
        const auto templateId = requiredUuid(payload, "template_id", "请选择告警模板");
        const auto deviceIds = requiredUuids(payload, "device_ids", "请选择目标设备");
        const auto principal = service::middleware::requireAuth(c);
        co_await requireTemplate(c, templateId);
        Query selected(c.pool());
        selected.select(selected.star()).from("alert_rule_template")
            .andWhere(selected.binary(selected.column("id"), Op::kEqual, selected.cast(selected.value(templateId), Type::kUuid)))
            .andWhere(selected.unary(ruvia::DbUnaryOperator::kIsNull, selected.column("deleted_at")));
        Query requested(c.pool());
        for (const auto& id : deviceIds) requested.values({ requested.cast(requested.value(id), Type::kUuid) });
        Query existing(c.pool());
        const auto existingName = existing.binary(existing.binary(existing.column("name", "selected"), Op::kConcat, existing.value(" - ")), Op::kConcat, existing.column("name", "device"));
        existing.select(existing.cast(existing.value(1), Type::kInteger)).from("alert_rule", "existing")
            .andWhere(existing.binary(existing.column("device_id", "existing"), Op::kEqual, existing.column("id", "device")))
            .andWhere(existing.binary(existing.column("name", "existing"), Op::kEqual, existingName))
            .andWhere(existing.unary(ruvia::DbUnaryOperator::kIsNull, existing.column("deleted_at", "existing")));
        Query source(c.pool());
        const auto name = source.binary(source.binary(source.column("name", "selected"), Op::kConcat, source.value(" - ")), Op::kConcat, source.column("name", "device"));
        source.select({ source.call("gen_random_uuid"), name, source.column("id", "device"), source.column("severity", "selected"),
                source.column("conditions", "selected"), source.column("logic", "selected"), source.column("silence_duration", "selected"),
                source.column("recovery_condition", "selected"), source.column("recovery_wait_seconds", "selected"),
                source.cast(source.value("enabled"), { .customName = "status_enum" }), source.column("description", "selected"), source.cast(source.value(principal.userId), Type::kUuid) })
            .from("selected").join(ruvia::DbJoinType::kCross, "requested")
            .join(ruvia::DbJoinType::kInner, "device", source.binary(source.column("id", "device"), Op::kEqual, source.column("device_id", "requested")))
            .andWhere(source.unary(ruvia::DbUnaryOperator::kIsNull, source.column("deleted_at", "device")))
            .andWhere(source.unary(ruvia::DbUnaryOperator::kNot, source.exists(existing)));
        Query created(c.pool());
        created.insertInto("alert_rule", { "id", "name", "device_id", "severity", "conditions", "logic", "silence_duration",
                "recovery_condition", "recovery_wait_seconds", "status", "remark", "created_by" })
            .insertFrom(source).returning({ created.column("id") });
        Query count(c.pool());
        count.select(count.aggregate("count", { count.star() })).from("created");
        Query ids(c.pool());
        ids.select(ids.aggregate("jsonb_agg", { ids.column("id") })).from("created");
        Query response(c.pool());
        response.with("selected", selected).with("requested", requested, { .columns = { "device_id" } }).with("created", created)
            .select(response.cast(response.call("jsonb_build_object", {
                response.cast(response.value("success"), Type::kText), response.subquery(count),
                response.cast(response.value("total"), Type::kText), response.cast(response.value(static_cast<std::int64_t>(deviceIds.size())), Type::kInteger),
                response.cast(response.value("createdIds"), Type::kText), response.coalesce({ response.subquery(ids), response.cast(response.value("[]"), Type::kJsonb) }) }), Type::kText));
        const auto result = co_await c.db().query(response);
        (void)co_await service::rpc::call(c, "alert", "refresh", "{}");
        co_return firstJson(result);
    }

    ruvia::Task<std::string> listRecords(ruvia::Context& c) {
        Query filtered(c.pool());
        filtered.select(filtered.star("record")).from("open_alert_record", "record");
        appendUuidFilter(c, "deviceId", filtered, filtered.column("device_id", "record"));
        appendUuidFilter(c, "ruleId", filtered, filtered.column("rule_id", "record"));
        appendTextFilter(c, "status", filtered, filtered.column("status", "record"));
        appendTextFilter(c, "severity", filtered, filtered.column("severity", "record"));
        Query listed(c.pool());
        listed.select({ listed.star("page_rows"), listed.alias(listed.column("name", "rule"), "rule_name"), listed.alias(listed.column("name", "device"), "device_name") })
            .from("page_rows")
            .join(ruvia::DbJoinType::kLeft, "alert_rule", listed.binary(listed.column("id", "rule"), Op::kEqual, listed.column("rule_id", "page_rows")), "rule")
            .join(ruvia::DbJoinType::kInner, "device", listed.binary(listed.column("id", "device"), Op::kEqual, listed.column("device_id", "page_rows")));
        co_return co_await pageResult(c, filtered, listed, { "id", "rule_id", "rule_name", "device_id", "device_name", "severity", "status", "message", "detail",
            "triggered_at", "acknowledged_at", "acknowledged_by", "resolved_at" }, "triggered_at");
    }

    ruvia::Task<void> acknowledge(ruvia::Context& c, std::string_view id) {
        service::common::requireUuid(19002, id, "告警记录 ID 无效");
        const auto principal = service::middleware::requireAuth(c);
        Query acknowledgement(c.pool());
        acknowledgement.update("open_alert_record").set("status", acknowledgement.value("acknowledged"))
            .set("acknowledged_at", acknowledgement.call("now"))
            .set("acknowledged_by", acknowledgement.cast(acknowledgement.value(principal.userId), Type::kUuid))
            .set("updated_at", acknowledgement.call("now"))
            .andWhere(acknowledgement.binary(acknowledgement.column("id"), Op::kEqual, acknowledgement.cast(acknowledgement.value(id), Type::kUuid)))
            .andWhere(acknowledgement.binary(acknowledgement.column("status"), Op::kEqual, acknowledgement.value("active")));
        const auto result = co_await c.db().execute(acknowledgement);
        if (result.affectedRows() == 0)
            service::common::fail(17003, "告警记录不存在或已处理", 404);
    }

    ruvia::Task<void> batchAcknowledge(ruvia::Context& c, const ruvia::JsonValue& payload) {
        const auto ids = requiredUuids(payload, "ids", "请选择要确认的告警");
        const auto principal = service::middleware::requireAuth(c);
        Query acknowledgement(c.pool());
        acknowledgement.update("open_alert_record").set("status", acknowledgement.value("acknowledged"))
            .set("acknowledged_at", acknowledgement.call("now"))
            .set("acknowledged_by", acknowledgement.cast(acknowledgement.value(principal.userId), Type::kUuid))
            .set("updated_at", acknowledgement.call("now"))
            .andWhere(acknowledgement.binary(acknowledgement.column("id"), Op::kIn, uuidList(acknowledgement, ids)))
            .andWhere(acknowledgement.binary(acknowledgement.column("status"), Op::kEqual, acknowledgement.value("active")));
        (void)co_await c.db().execute(acknowledgement);
    }

    ruvia::Task<std::string> stats(ruvia::Context& c) {
        Query unresolved(c.pool());
        const auto count = unresolved.aggregate("count", { unresolved.star() });
        unresolved.select(unresolved.alias(count, "total"));
        for (const auto severity : { "critical", "warning", "info" }) {
            unresolved.addSelect(unresolved.alias(unresolved.filter(count, unresolved.binary(unresolved.column("severity"), Op::kEqual, unresolved.value(severity))), severity));
        }
        unresolved.addSelect(unresolved.alias(unresolved.aggregate("count", { unresolved.column("device_id") }, true), "affected_devices"))
            .from("open_alert_record").andWhere(unresolved.binary(unresolved.column("status"), Op::kIn,
                unresolved.list({ unresolved.value("active"), unresolved.value("acknowledged") })));
        Query todayNew(c.pool());
        todayNew.select(todayNew.alias(todayNew.aggregate("count", { todayNew.star() }), "total")).from("open_alert_record")
            .andWhere(todayNew.binary(todayNew.column("triggered_at"), Op::kGreaterEqual, todayNew.cast(todayNew.call("now"), Type::kDate)));
        Query acknowledged(c.pool());
        acknowledged.select(acknowledged.alias(acknowledged.aggregate("count", { acknowledged.star() }), "total")).from("open_alert_record")
            .andWhere(acknowledged.binary(acknowledged.column("status"), Op::kEqual, acknowledged.value("acknowledged")));
        Query todayResolved(c.pool());
        todayResolved.select(todayResolved.alias(todayResolved.aggregate("count", { todayResolved.star() }), "total")).from("open_alert_record")
            .andWhere(todayResolved.binary(todayResolved.column("status"), Op::kEqual, todayResolved.value("resolved")))
            .andWhere(todayResolved.binary(todayResolved.column("resolved_at"), Op::kGreaterEqual, todayResolved.cast(todayResolved.call("now"), Type::kDate)));
        Query result(c.pool());
        std::vector<ruvia::DbExpression> values;
        for (const auto field : { "total", "critical", "warning", "info", "affected_devices" }) {
            values.push_back(result.cast(result.value(field), Type::kText));
            values.push_back(result.column(field, "unresolved"));
        }
        for (const auto& [key, table] : { std::pair{ "today_new", "today_new" }, std::pair{ "acknowledged", "acknowledged_summary" }, std::pair{ "today_resolved", "today_resolved" } }) {
            values.push_back(result.cast(result.value(key), Type::kText));
            values.push_back(result.column("total", table));
        }
        result.with("unresolved", unresolved).with("today_new", todayNew).with("acknowledged_summary", acknowledged).with("today_resolved", todayResolved)
            .select(result.cast(result.call("jsonb_build_object", values), Type::kText)).from("unresolved")
            .join(ruvia::DbJoinType::kCross, "today_new").join(ruvia::DbJoinType::kCross, "acknowledged_summary").join(ruvia::DbJoinType::kCross, "today_resolved");
        co_return firstJson(co_await c.db().query(result));
    }

    ruvia::Task<std::string> grouped(ruvia::Context& c) {
        const auto days = std::clamp<std::int64_t>(
            service::common::parseInt64(c.req().query("days")).value_or(7), 1, 365);
        Query groups(c.pool());
        const auto count = groups.aggregate("count", { groups.star() });
        groups.select({ groups.column("rule_id", "record"), groups.alias(groups.coalesce({ groups.column("name", "rule"), groups.value("已删除规则") }), "rule_name"),
                groups.column("device_id", "record"), groups.alias(groups.column("name", "device"), "device_name"), groups.column("severity", "record"),
                groups.alias(count, "total_count") });
        for (const auto& [status, alias] : { std::pair{ "active", "active_count" }, std::pair{ "acknowledged", "acked_count" }, std::pair{ "resolved", "resolved_count" } }) {
            groups.addSelect(groups.alias(groups.filter(count, groups.binary(groups.column("status", "record"), Op::kEqual, groups.value(status))), alias));
        }
        groups.addSelect(groups.alias(groups.aggregate("max", { groups.column("triggered_at", "record") }), "latest_trigger_time"))
            .from("open_alert_record", "record")
            .join(ruvia::DbJoinType::kLeft, "alert_rule", groups.binary(groups.column("id", "rule"), Op::kEqual, groups.column("rule_id", "record")), "rule")
            .join(ruvia::DbJoinType::kInner, "device", groups.binary(groups.column("id", "device"), Op::kEqual, groups.column("device_id", "record")))
            .andWhere(groups.binary(groups.column("triggered_at", "record"), Op::kGreaterEqual,
                groups.binary(groups.call("now"), Op::kSubtract, groups.binary(groups.cast(groups.value(days), Type::kBigInt), Op::kMultiply,
                    groups.cast(groups.value("1 day"), Type::kInterval)))))
            .groupBy({ groups.column("rule_id", "record"), groups.column("name", "rule"), groups.column("device_id", "record"), groups.column("name", "device"), groups.column("severity", "record") });
        Query result(c.pool());
        const std::vector<ruvia::DbOrderTerm> order{ { result.column("latest_trigger_time"), ruvia::DbOrderDirection::kDesc } };
        result.select(result.cast(result.coalesce({ result.aggregate("jsonb_agg", { fieldJson(result, { "rule_id", "rule_name", "device_id", "device_name", "severity",
            "total_count", "active_count", "acked_count", "resolved_count", "latest_trigger_time" }) }, false, order), result.cast(result.value("[]"), Type::kJsonb) }), Type::kText)).from(groups, "grouped");
        co_return firstJson(co_await c.db().query(result));
    }

#ifdef IOT_ENGINE_TESTING
    static void validateConditionsForTest(std::string_view raw) { validateConditions(raw); }
#endif

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

    static RuleInput ruleInput(const ruvia::JsonValue& payload) {
        RuleInput result;
        result.name = requiredString(payload, "name", "规则名称不能为空", 128);
        result.deviceId = requiredUuid(payload, "device_id", "请选择关联设备");
        result.severity = enumString(payload, "severity", "warning",
                                     {"critical", "warning", "info"});
        result.conditions = requiredConditions(payload, "conditions", "至少配置一个告警条件");
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
        result.conditions = requiredConditions(payload, "conditions", "至少配置一个告警条件");
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
        auto result = service::utils::trim(value->view());
        if (result.empty() || result.size() > maximum)
            service::common::fail(17002, std::string(message), 400);
        return result;
    }

    static std::optional<std::string> optionalString(const ruvia::JsonValue& payload,
                                                     std::string_view field,
                                                     std::size_t maximum) {
        const auto raw = service::utils::jsonField(payload, field);
        if (!raw)
            return std::nullopt;
        const auto value = payload.get<ruvia::String>(field);
        if (!value)
            service::common::fail(17002, std::string(field) + " 必须是字符串", 400);
        auto result = service::utils::trim(value->view());
        if (result.size() > maximum)
            service::common::fail(17002, std::string(field) + " 长度超出限制", 400);
        return result;
    }

    static std::string requiredUuid(const ruvia::JsonValue& payload, std::string_view field,
                                    std::string_view message) {
        const auto value = payload.get<ruvia::String>(field);
        if (!value)
            service::common::fail(17002, std::string(message), 400);
        service::common::requireUuid(19002, value->view(), message);
        return std::string(value->view());
    }

    static std::string optionalUuid(const ruvia::JsonValue& payload, std::string_view field) {
        const auto value = payload.get<ruvia::String>(field);
        if (!value || value->view().empty())
            return {};
        service::common::requireUuid(19002, value->view(), std::string(field) + " 无效");
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
            service::common::requireUuid(19002, value.view(),
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
        const auto raw = service::utils::jsonField(payload, field);
        if (!raw)
            return fallback;
        const auto value = payload.get<ruvia::Int64>(field);
        if (!value)
            service::common::fail(17002, std::string(field) + " 必须是整数", 400);
        const auto result = static_cast<std::int64_t>(*value);
        if (result < minimum || result > maximum)
            service::common::fail(17002, std::string(field) + " 超出允许范围", 400);
        return result;
    }

    static std::string array(const ruvia::JsonValue& payload, std::string_view field,
                             std::string_view fallback) {
        const auto value = service::utils::jsonField(payload, field);
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

    static std::string requiredConditions(const ruvia::JsonValue& payload, std::string_view field,
                                          std::string_view message) {
        auto result = requiredArray(payload, field, message);
        validateConditions(result);
        return result;
    }

    template <typename Visitor>
    static bool visitArrayElements(std::string_view json, Visitor&& visitor) {
        auto input = json;
        ruvia::detail::skipJsonWhitespace(input);
        if (!ruvia::detail::consumeJsonChar(input, '['))
            return false;
        ruvia::detail::skipJsonWhitespace(input);
        if (!input.empty() && input.front() == ']') {
            input.remove_prefix(1);
            ruvia::detail::skipJsonWhitespace(input);
            return input.empty();
        }
        std::size_t index = 0;
        for (;;) {
            ruvia::detail::skipJsonWhitespace(input);
            const auto before = input;
            if (!ruvia::detail::skipJsonValue(input))
                return false;
            const auto valueSize = static_cast<std::size_t>(input.data() - before.data());
            if (!visitor(index++, std::string_view(before.data(), valueSize)))
                return false;
            ruvia::detail::skipJsonWhitespace(input);
            if (!input.empty() && input.front() == ']') {
                input.remove_prefix(1);
                ruvia::detail::skipJsonWhitespace(input);
                return input.empty();
            }
            if (!ruvia::detail::consumeJsonChar(input, ','))
                return false;
        }
    }

    static void validateConditions(std::string_view raw) {
        std::size_t count = 0;
        const auto valid = visitArrayElements(raw, [&](std::size_t, std::string_view item) {
            ++count;
            validateCondition(item);
            return true;
        });
        if (!valid || count == 0)
            service::common::fail(17002, "至少配置一个告警条件", 400);
    }

    static void validateCondition(std::string_view raw) {
        const auto parsed = ruvia::JsonValue::parse(raw);
        if (!parsed || !parsed->isObject())
            service::common::fail(17002, "告警条件必须是对象", 400);
        const auto type = requiredConditionString(*parsed, "type", "告警条件类型不能为空", 32);
        if (type == "offline") {
            if (const auto duration = optionalIntegerField(*parsed, "duration");
                duration && (*duration < 1 || *duration > 86400))
                service::common::fail(17002, "离线检测时长超出允许范围", 400);
            return;
        }
        if (type == "threshold") {
            (void)requiredConditionString(*parsed, "elementKey", "请选择告警要素", 128);
            const auto op = requiredConditionString(*parsed, "operator", "告警比较符不能为空", 8);
            if (!oneOf(op, {">", ">=", "<", "<=", "==", "!="}))
                service::common::fail(17002, "告警比较符无效", 400);
            const auto value = optionalScalarText(*parsed, "value");
            if (!value || value->empty())
                service::common::fail(17002, "告警阈值不能为空", 400);
            if (oneOf(op, {">", ">=", "<", "<="}) && !decimalText(*value, true))
                service::common::fail(17002, "告警阈值必须是数字", 400);
            validateOptionalBitIndex(*parsed);
            return;
        }
        if (type == "rate_of_change") {
            (void)requiredConditionString(*parsed, "elementKey", "请选择告警要素", 128);
            const auto direction =
                optionalConditionString(*parsed, "changeDirection", 16).value_or("any");
            if (!oneOf(direction, {"any", "rise", "fall"}))
                service::common::fail(17002, "变化方向无效", 400);
            const auto rate = optionalScalarText(*parsed, "changeRate");
            if (rate && !rate->empty() && !decimalText(*rate, false))
                service::common::fail(17002, "变化率必须是非负数字", 400);
            validateOptionalBitIndex(*parsed);
            return;
        }
        service::common::fail(17002, "告警条件类型无效", 400);
    }

    static bool oneOf(std::string_view value, std::initializer_list<std::string_view> allowed) {
        return std::find(allowed.begin(), allowed.end(), value) != allowed.end();
    }

    static std::string requiredConditionString(const ruvia::JsonValue& object,
                                               std::string_view field,
                                               std::string_view message,
                                               std::size_t maximum) {
        const auto value = optionalConditionString(object, field, maximum);
        if (!value || value->empty())
            service::common::fail(17002, std::string(message), 400);
        return *value;
    }

    static std::optional<std::string> optionalConditionString(const ruvia::JsonValue& object,
                                                             std::string_view field,
                                                             std::size_t maximum) {
        const auto raw = service::utils::jsonField(object, field);
        if (!raw)
            return std::nullopt;
        const auto value = object.get<ruvia::String>(field);
        if (!value)
            service::common::fail(17002, std::string(field) + " 必须是字符串", 400);
        auto result = service::utils::trim(value->view());
        if (result.size() > maximum)
            service::common::fail(17002, std::string(field) + " 长度超出限制", 400);
        return result;
    }

    static std::optional<std::string> optionalScalarText(const ruvia::JsonValue& object,
                                                        std::string_view field) {
        const auto value = service::utils::jsonField(object, field);
        if (!value || value->isNull())
            return std::nullopt;
        if (value->isString())
            return optionalConditionString(object, field, 128);
        if (value->isNumber() || value->isBoolean())
            return service::utils::trim(value->view());
        service::common::fail(17002, std::string(field) + " 必须是标量", 400);
        return std::nullopt;
    }

    static std::optional<std::int64_t> optionalIntegerField(const ruvia::JsonValue& object,
                                                           std::string_view field) {
        const auto text = optionalScalarText(object, field);
        if (!text || text->empty())
            return std::nullopt;
        std::int64_t result = 0;
        const auto first = text->data();
        const auto last = first + text->size();
        const auto [end, error] = std::from_chars(first, last, result);
        if (error != std::errc{} || end != last)
            service::common::fail(17002, std::string(field) + " 必须是整数", 400);
        return result;
    }

    static bool decimalText(std::string_view raw, bool allowNegative) {
        const auto value = service::utils::trim(raw);
        if (value.empty() || value.size() > 64)
            return false;
        std::size_t index = 0;
        if (allowNegative && value[index] == '-') {
            ++index;
            if (index == value.size())
                return false;
        }
        bool beforeDot = false;
        bool afterDot = false;
        bool dot = false;
        for (; index < value.size(); ++index) {
            const char c = value[index];
            if (c >= '0' && c <= '9') {
                if (dot)
                    afterDot = true;
                else
                    beforeDot = true;
                continue;
            }
            if (c == '.' && !dot) {
                dot = true;
                continue;
            }
            return false;
        }
        return beforeDot && (!dot || afterDot);
    }

    static void validateOptionalBitIndex(const ruvia::JsonValue& object) {
        const auto bitIndex = optionalIntegerField(object, "bitIndex");
        if (bitIndex && (*bitIndex < 0 || *bitIndex > 62))
            service::common::fail(17002, "位索引超出允许范围", 400);
    }

    static ruvia::DbExpression uuidList(Query& query, const std::vector<std::string>& ids) {
        std::vector<ruvia::DbExpression> values;
        values.reserve(ids.size());
        for (const auto& id : ids) values.push_back(query.cast(query.value(id), Type::kUuid));
        return query.list(values);
    }

    template <typename Rows> static std::string firstJson(const Rows& rows) {
        if (rows.empty() || rows.front().empty() || !rows.front()[0].value().has_value())
            return "{}";
        return std::string(rows.front()[0].value().value_or(std::string_view{}));
    }

    template <typename Rows>
    static std::string firstObject(const Rows& rows, std::string_view notFound) {
        if (rows.empty())
            service::common::fail(17003, std::string(notFound), 404);
        return std::string(rows.front()[0].value().value_or(std::string_view{}));
    }

    static ruvia::DbExpression fieldJson(Query& query, const std::vector<std::string_view>& fields) {
        std::vector<ruvia::DbExpression> values;
        values.reserve(fields.size() * 2);
        for (const auto field : fields) {
            values.push_back(query.cast(query.value(field), Type::kText));
            auto value = query.column(field);
            if (field.ends_with("_at") || field == "latest_trigger_time") value = query.call("iot_utc_timestamp", { value });
            values.push_back(value);
        }
        return query.call("jsonb_build_object", values);
    }

    static ruvia::Task<std::string> pageResult(ruvia::Context& c, const Query& filtered,
        const Query& listed, std::vector<std::string_view> fields, std::string_view timeColumn) {
        const auto pagination = service::common::page(c.req());
        Query counted(c.pool());
        counted.select(counted.alias(counted.aggregate("count", { counted.star() }), "total")).from("filtered");
        Query page(c.pool());
        page.select(page.star()).from("filtered")
            .addOrderBy(page.column(timeColumn), ruvia::DbOrderDirection::kDesc)
            .addOrderBy(page.column("id"), ruvia::DbOrderDirection::kDesc)
            .limit(static_cast<std::uint64_t>(pagination.pageSize)).offset(static_cast<std::uint64_t>(pagination.offset));
        Query rows(c.pool());
        const std::vector<ruvia::DbOrderTerm> order{ { rows.column(timeColumn), ruvia::DbOrderDirection::kDesc }, { rows.column("id"), ruvia::DbOrderDirection::kDesc } };
        rows.select(rows.aggregate("jsonb_agg", { fieldJson(rows, fields) }, false, order)).from("listed");
        Query count(c.pool());
        count.select(count.column("total")).from("counted");
        Query result(c.pool());
        const auto total = result.coalesce({ result.subquery(count), result.value(0) });
        const auto pageSize = result.cast(result.value(pagination.pageSize), Type::kBigInt);
        const auto pages = result.caseWhen({ { result.binary(pageSize, Op::kEqual, result.value(0)), result.value(0) } },
            result.cast(result.call("ceil", { result.binary(result.cast(total, Type::kNumeric), Op::kDivide, pageSize) }), Type::kBigInt));
        result.with("filtered", filtered).with("counted", counted).with("page_rows", page).with("listed", listed)
            .select(result.cast(result.call("jsonb_build_object", {
                result.cast(result.value("list"), Type::kText), result.coalesce({ result.subquery(rows), result.cast(result.value("[]"), Type::kJsonb) }),
                result.cast(result.value("total"), Type::kText), total,
                result.cast(result.value("page"), Type::kText), result.cast(result.value(pagination.page), Type::kBigInt),
                result.cast(result.value("pageSize"), Type::kText), pageSize,
                result.cast(result.value("totalPages"), Type::kText), pages }), Type::kText));
        co_return firstJson(co_await c.db().query(result));
    }

    static void appendTextFilter(ruvia::Context& c, std::string_view parameter,
        Query& query, ruvia::DbExpression column, bool contains = false) {
        const auto value = c.req().query(parameter);
        if (!value || value->empty()) return;
        query.andWhere(query.binary(column, contains ? Op::kILike : Op::kEqual,
            query.value(contains ? "%" + std::string(*value) + "%" : std::string(*value))));
    }

    static void appendUuidFilter(ruvia::Context& c, std::string_view parameter,
        Query& query, ruvia::DbExpression column) {
        const auto value = c.req().query(parameter);
        if (!value || value->empty()) return;
        service::common::requireUuid(19002, *value, std::string(parameter) + " 无效");
        query.andWhere(query.binary(column, Op::kEqual, query.cast(query.value(*value), Type::kUuid)));
    }

    static ruvia::Task<void> ensureDevice(ruvia::Context& c, std::string_view id) {
        Query query(c.pool());
        query.select(query.cast(query.value(1), Type::kInteger)).from("device")
            .andWhere(query.binary(query.column("id"), Op::kEqual, query.cast(query.value(id), Type::kUuid)))
            .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull, query.column("deleted_at")));
        const auto rows = co_await c.db().query(query);
        if (rows.empty())
            service::common::fail(17003, "关联设备不存在", 404);
    }

    static ruvia::Task<void> ensureProtocolConfig(ruvia::Context& c, std::string_view id) {
        Query query(c.pool());
        query.select(query.cast(query.value(1), Type::kInteger)).from("protocol_config")
            .andWhere(query.binary(query.column("id"), Op::kEqual, query.cast(query.value(id), Type::kUuid)))
            .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull, query.column("deleted_at")));
        const auto rows = co_await c.db().query(query);
        if (rows.empty())
            service::common::fail(17003, "协议配置不存在", 404);
    }

    static ruvia::Task<void> requireRule(ruvia::Context& c, std::string_view id) {
        Query query(c.pool());
        query.select(query.cast(query.value(1), Type::kInteger)).from("alert_rule")
            .andWhere(query.binary(query.column("id"), Op::kEqual, query.cast(query.value(id), Type::kUuid)))
            .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull, query.column("deleted_at")));
        const auto rows = co_await c.db().query(query);
        if (rows.empty())
            service::common::fail(17003, "告警规则不存在", 404);
    }

    static ruvia::Task<void> requireTemplate(ruvia::Context& c, std::string_view id) {
        Query query(c.pool());
        query.select(query.cast(query.value(1), Type::kInteger)).from("alert_rule_template")
            .andWhere(query.binary(query.column("id"), Op::kEqual, query.cast(query.value(id), Type::kUuid)))
            .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull, query.column("deleted_at")));
        const auto rows = co_await c.db().query(query);
        if (rows.empty())
            service::common::fail(17003, "告警模板不存在", 404);
    }

    static ruvia::Task<void> ensureRuleName(ruvia::Context& c, std::string_view name,
                                            std::string_view deviceId,
                                            const std::optional<std::string>& excluded) {
        Query query(c.pool());
        query.select(query.cast(query.value(1), Type::kInteger)).from("alert_rule")
            .andWhere(query.binary(query.column("name"), Op::kEqual, query.value(name)))
            .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull, query.column("deleted_at")));
        query.andWhere(query.binary(query.column("device_id"), Op::kEqual, query.cast(query.value(deviceId), Type::kUuid)));
        if (excluded && !excluded->empty())
            query.andWhere(query.binary(query.column("id"), Op::kNotEqual, query.cast(query.value(*excluded), Type::kUuid)));
        const auto rows = co_await c.db().query(query);
        if (!rows.empty())
            service::common::fail(17009, "该设备已存在同名告警规则", 409);
    }

    static ruvia::Task<void> ensureTemplateName(ruvia::Context& c, std::string_view name,
                                                const std::optional<std::string>& excluded) {
        Query query(c.pool());
        query.select(query.cast(query.value(1), Type::kInteger)).from("alert_rule_template")
            .andWhere(query.binary(query.column("name"), Op::kEqual, query.value(name)))
            .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull, query.column("deleted_at")));
        if (excluded && !excluded->empty())
            query.andWhere(query.binary(query.column("id"), Op::kNotEqual, query.cast(query.value(*excluded), Type::kUuid)));
        const auto rows = co_await c.db().query(query);
        if (!rows.empty())
            service::common::fail(17009, "告警模板名称已存在", 409);
    }
};

inline AlertService& alertService() { return AlertService::instance(); }

} // namespace service::alert
