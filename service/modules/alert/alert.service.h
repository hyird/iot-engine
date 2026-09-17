#pragma once

#include <memory>
#include <algorithm>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/web/Context.h>
#include <ruvia/web/ModelObject.h>
#include <ruvia/web/db/Db.h>
#include <ruvia/web/db/DbQuery.h>

#include "service/common/http.h"
#include "service/common/uuid.h"
#include "service/middleware/rpc.h"
#include "service/modules/alert/alert.entity.h"
#include "service/modules/alert/alert.types.h"
#include "service/utils/json.h"
#include "service/utils/number.h"
#include "service/utils/text.h"

namespace service::alert {

class AlertService final {
    using Query = ruvia::DbQuery;
    using Op = ruvia::DbBinaryOperator;
    using Type = ruvia::DbDataType;

  public:
    static AlertService& instance() {
        static thread_local AlertService service;
        return service;
    }

    template <typename Context>
    ruvia::Task<std::string> listRules(Context& c, const AlertListQuery& parameters) {
        Query filtered(c.pool());
        filtered.select({ filtered.star("rule"), filtered.alias(filtered.column(service::alert::entities::DeviceEntity::columnName<"name">(), "device"), "device_name") })
            .from(service::alert::entities::AlertRuleEntity::tableName(), "rule")
            .join(ruvia::DbJoinType::kInner, service::alert::entities::DeviceEntity::tableName(), filtered.binary(filtered.column(service::alert::entities::DeviceEntity::columnName<"id">(), "device"), Op::kEqual, filtered.column(service::alert::entities::AlertRuleEntity::columnName<"device_id">(), "rule")))
            .andWhere(filtered.unary(ruvia::DbUnaryOperator::kIsNull, filtered.column(service::alert::entities::AlertRuleEntity::columnName<"deleted_at">(), "rule")));
        appendTextFilter(parameters.get<"keyword">(), "keyword", filtered, filtered.column(service::alert::entities::AlertRuleEntity::columnName<"name">(), "rule"), true);
        appendUuidFilter(parameters.get<"deviceId">(), "deviceId", filtered, filtered.column(service::alert::entities::AlertRuleEntity::columnName<"device_id">(), "rule"));
        appendTextFilter(parameters.get<"severity">(), "severity", filtered, filtered.column(service::alert::entities::AlertRuleEntity::columnName<"severity">(), "rule"));
        appendTextFilter(parameters.get<"status">(), "status", filtered, filtered.cast(filtered.column(service::alert::entities::AlertRuleEntity::columnName<"status">(), "rule"), Type::kText));
        Query listed(c.pool());
        listed.select(listed.star()).from("page_rows");
        co_return co_await pageResult(c, parameters, filtered, listed, { "id", "name", "device_id", "device_name", "severity", "conditions", "logic", "silence_duration", "recovery_condition", "recovery_wait_seconds", "status", "remark", "created_at", "updated_at" }, "created_at");
    }

    template <typename Context>
    ruvia::Task<std::string> ruleDetail(Context& c, std::string_view id) {
        service::common::requireUuid(19002, id, "告警规则 ID 无效");
        Query rule(c.pool());
        rule.select({ rule.star("rule"), rule.alias(rule.column(service::alert::entities::DeviceEntity::columnName<"name">(), "device"), "device_name") }).from(service::alert::entities::AlertRuleEntity::tableName(), "rule").join(ruvia::DbJoinType::kInner, service::alert::entities::DeviceEntity::tableName(), rule.binary(rule.column(service::alert::entities::DeviceEntity::columnName<"id">(), "device"), Op::kEqual, rule.column(service::alert::entities::AlertRuleEntity::columnName<"device_id">(), "rule"))).andWhere(rule.binary(rule.column(service::alert::entities::AlertRuleEntity::columnName<"id">(), "rule"), Op::kEqual, rule.cast(rule.value(id), Type::kUuid))).andWhere(rule.unary(ruvia::DbUnaryOperator::kIsNull, rule.column(service::alert::entities::AlertRuleEntity::columnName<"deleted_at">(), "rule")));
        Query result(c.pool());
        result.select(result.cast(fieldJson(result, { "id", "name", "device_id", "device_name", "severity", "conditions", "logic", "silence_duration", "recovery_condition", "recovery_wait_seconds", "status", "remark", "created_at", "updated_at" }), Type::kText)).from(rule, "rule");
        co_return firstObject(co_await c.db().query(result), "告警规则不存在");
    }

    template <typename Context>
    ruvia::Task<void> createRule(Context& c, const RuleInput& input) {
        co_await ensureDevice(c, input.deviceId);
        co_await ensureRuleName(c, input.name, input.deviceId, std::nullopt);
        const auto id = c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next();
        Query query(c.pool());
        query.insertInto(service::alert::entities::AlertRuleEntity::tableName(), { "id", "name", "device_id", "severity", "conditions", "logic", "silence_duration", "recovery_condition", "recovery_wait_seconds", "status", "remark", "created_by" })
            .values({ query.cast(query.value(id), Type::kUuid), query.value(input.name), query.cast(query.value(input.deviceId), Type::kUuid), query.value(input.severity), query.cast(query.value(input.conditions), Type::kJsonb), query.value(input.logic), query.cast(query.value(input.silenceDuration), Type::kInteger), query.value(input.recoveryCondition), query.cast(query.value(input.recoveryWaitSeconds), Type::kInteger), query.cast(query.value(input.status), { .customName = "status_enum" }), query.nullIf(query.value(input.remark), query.value("")), query.cast(query.value(c.userId), Type::kUuid) });
        (void)co_await c.db().execute(query);
        (void)co_await service::rpc::call(c, "alert", "refresh", "{}");
    }

    template <typename Context>
    ruvia::Task<void> updateRule(Context& c, std::string_view id, const RuleInput& input) {
        service::common::requireUuid(19002, id, "告警规则 ID 无效");
        co_await requireRule(c, id);
        co_await ensureDevice(c, input.deviceId);
        co_await ensureRuleName(c, input.name, input.deviceId, std::string(id));
        Query query(c.pool());
        query.update(service::alert::entities::AlertRuleEntity::tableName())
            .set(service::alert::entities::AlertRuleEntity::columnName<"name">(), query.value(input.name))
            .set(service::alert::entities::AlertRuleEntity::columnName<"device_id">(), query.cast(query.value(input.deviceId), Type::kUuid))
            .set(service::alert::entities::AlertRuleEntity::columnName<"severity">(), query.value(input.severity))
            .set(service::alert::entities::AlertRuleEntity::columnName<"conditions">(), query.cast(query.value(input.conditions), Type::kJsonb))
            .set(service::alert::entities::AlertRuleEntity::columnName<"logic">(), query.value(input.logic))
            .set(service::alert::entities::AlertRuleEntity::columnName<"silence_duration">(), query.cast(query.value(input.silenceDuration), Type::kInteger))
            .set(service::alert::entities::AlertRuleEntity::columnName<"recovery_condition">(), query.value(input.recoveryCondition))
            .set(service::alert::entities::AlertRuleEntity::columnName<"recovery_wait_seconds">(), query.cast(query.value(input.recoveryWaitSeconds), Type::kInteger))
            .set(service::alert::entities::AlertRuleEntity::columnName<"status">(), query.cast(query.value(input.status), { .customName = "status_enum" }))
            .set(service::alert::entities::AlertRuleEntity::columnName<"remark">(), query.nullIf(query.value(input.remark), query.value("")))
            .set(service::alert::entities::AlertRuleEntity::columnName<"updated_at">(), query.call("now"))
            .andWhere(query.binary(query.column(service::alert::entities::AlertRuleEntity::columnName<"id">()), Op::kEqual, query.cast(query.value(id), Type::kUuid)))
            .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull, query.column(service::alert::entities::AlertRuleEntity::columnName<"deleted_at">())));
        (void)co_await c.db().execute(query);
        (void)co_await service::rpc::call(c, "alert", "refresh", "{}");
    }

    template <typename Context>
    ruvia::Task<void> removeRule(Context& c, std::string_view id) {
        service::common::requireUuid(19002, id, "告警规则 ID 无效");
        co_await requireRule(c, id);
        auto transaction = co_await c.db().beginTransaction();
        Query records(c.pool());
        records.update(service::alert::entities::OpenAlertRecordEntity::tableName()).set(service::alert::entities::OpenAlertRecordEntity::columnName<"status">(), records.value("resolved")).set(service::alert::entities::OpenAlertRecordEntity::columnName<"resolved_at">(), records.call("now")).set(service::alert::entities::OpenAlertRecordEntity::columnName<"updated_at">(), records.call("now")).andWhere(records.binary(records.column(service::alert::entities::OpenAlertRecordEntity::columnName<"rule_id">()), Op::kEqual, records.cast(records.value(id), Type::kUuid))).andWhere(records.binary(records.column(service::alert::entities::OpenAlertRecordEntity::columnName<"status">()), Op::kIn, records.list({ records.value("active"), records.value("acknowledged") })));
        (void)co_await transaction.execute(records);
        Query rules(c.pool());
        rules.update(service::alert::entities::AlertRuleEntity::tableName()).set(service::alert::entities::AlertRuleEntity::columnName<"deleted_at">(), rules.call("now")).set(service::alert::entities::AlertRuleEntity::columnName<"updated_at">(), rules.call("now")).andWhere(rules.binary(rules.column(service::alert::entities::AlertRuleEntity::columnName<"id">()), Op::kEqual, rules.cast(rules.value(id), Type::kUuid))).andWhere(rules.unary(ruvia::DbUnaryOperator::kIsNull, rules.column(service::alert::entities::AlertRuleEntity::columnName<"deleted_at">())));
        (void)co_await transaction.execute(rules);
        co_await transaction.commit();
        (void)co_await service::rpc::call(c, "alert", "refresh", "{}");
    }

    template <typename Context>
    ruvia::Task<void> batchRemoveRules(Context& c, const std::vector<std::string>& ids) {
        auto transaction = co_await c.db().beginTransaction();
        Query records(c.pool());
        records.update(service::alert::entities::OpenAlertRecordEntity::tableName()).set(service::alert::entities::OpenAlertRecordEntity::columnName<"status">(), records.value("resolved")).set(service::alert::entities::OpenAlertRecordEntity::columnName<"resolved_at">(), records.call("now")).set(service::alert::entities::OpenAlertRecordEntity::columnName<"updated_at">(), records.call("now")).andWhere(records.binary(records.column(service::alert::entities::OpenAlertRecordEntity::columnName<"rule_id">()), Op::kIn, uuidList(records, ids))).andWhere(records.binary(records.column(service::alert::entities::OpenAlertRecordEntity::columnName<"status">()), Op::kIn, records.list({ records.value("active"), records.value("acknowledged") })));
        (void)co_await transaction.execute(records);
        Query rules(c.pool());
        rules.update(service::alert::entities::AlertRuleEntity::tableName()).set(service::alert::entities::AlertRuleEntity::columnName<"deleted_at">(), rules.call("now")).set(service::alert::entities::AlertRuleEntity::columnName<"updated_at">(), rules.call("now")).andWhere(rules.binary(rules.column(service::alert::entities::AlertRuleEntity::columnName<"id">()), Op::kIn, uuidList(rules, ids))).andWhere(rules.unary(ruvia::DbUnaryOperator::kIsNull, rules.column(service::alert::entities::AlertRuleEntity::columnName<"deleted_at">())));
        (void)co_await transaction.execute(rules);
        co_await transaction.commit();
        (void)co_await service::rpc::call(c, "alert", "refresh", "{}");
    }

    template <typename Context>
    ruvia::Task<std::string> listTemplates(Context& c, const AlertListQuery& parameters) {
        Query filtered(c.pool());
        filtered.select({ filtered.star("template"), filtered.alias(filtered.column(service::alert::entities::ProtocolConfigEntity::columnName<"name">(), "config"), "config_name"), filtered.alias(filtered.column(service::alert::entities::ProtocolConfigEntity::columnName<"protocol">(), "config"), "protocol_type") })
            .from(service::alert::entities::AlertRuleTemplateEntity::tableName(), "template")
            .join(ruvia::DbJoinType::kLeft, service::alert::entities::ProtocolConfigEntity::tableName(), filtered.binary(filtered.column(service::alert::entities::ProtocolConfigEntity::columnName<"id">(), "config"), Op::kEqual, filtered.column(service::alert::entities::AlertRuleTemplateEntity::columnName<"protocol_config_id">(), "template")), "config")
            .andWhere(filtered.unary(ruvia::DbUnaryOperator::kIsNull, filtered.column(service::alert::entities::AlertRuleTemplateEntity::columnName<"deleted_at">(), "template")));
        appendTextFilter(parameters.get<"category">(), "category", filtered, filtered.column(service::alert::entities::AlertRuleTemplateEntity::columnName<"category">(), "template"));
        Query listed(c.pool());
        listed.select(listed.star()).from("page_rows");
        co_return co_await pageResult(c, parameters, filtered, listed, { "id", "name", "category", "description", "severity", "logic", "silence_duration", "protocol_config_id", "config_name", "protocol_type", "created_at" }, "created_at");
    }

    template <typename Context>
    ruvia::Task<std::string> templateDetail(Context& c, std::string_view id) {
        service::common::requireUuid(19002, id, "告警模板 ID 无效");
        Query query(c.pool());
        query.select(query.cast(fieldJson(query, { "id", "name", "category", "description", "severity", "conditions", "logic", "silence_duration", "recovery_condition", "recovery_wait_seconds", "applicable_protocols", "protocol_config_id", "created_by", "created_at", "updated_at" }), Type::kText))
            .from(service::alert::entities::AlertRuleTemplateEntity::tableName())
            .andWhere(query.binary(query.column(service::alert::entities::AlertRuleTemplateEntity::columnName<"id">()), Op::kEqual, query.cast(query.value(id), Type::kUuid)))
            .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull, query.column(service::alert::entities::AlertRuleTemplateEntity::columnName<"deleted_at">())));
        co_return firstObject(co_await c.db().query(query), "告警模板不存在");
    }

    template <typename Context>
    ruvia::Task<void> createTemplate(Context& c, const TemplateInput& input) {
        co_await ensureTemplateName(c, input.name, std::nullopt);
        if (!input.protocolConfigId.empty()) {
            co_await ensureProtocolConfig(c, input.protocolConfigId);
        }
        const auto id = c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next();
        Query query(c.pool());
        query.insertInto(service::alert::entities::AlertRuleTemplateEntity::tableName(), { "id", "name", "category", "description", "severity", "conditions", "logic", "silence_duration", "recovery_condition", "recovery_wait_seconds", "applicable_protocols", "protocol_config_id", "created_by" })
            .values({ query.cast(query.value(id), Type::kUuid), query.value(input.name), query.nullIf(query.value(input.category), query.value("")), query.nullIf(query.value(input.description), query.value("")), query.value(input.severity), query.cast(query.value(input.conditions), Type::kJsonb), query.value(input.logic), query.cast(query.value(input.silenceDuration), Type::kInteger), query.value(input.recoveryCondition), query.cast(query.value(input.recoveryWaitSeconds), Type::kInteger), query.cast(query.value(input.applicableProtocols), Type::kJsonb), query.cast(query.nullIf(query.value(input.protocolConfigId), query.value("")), Type::kUuid), query.cast(query.value(c.userId), Type::kUuid) });
        (void)co_await c.db().execute(query);
    }

    template <typename Context>
    ruvia::Task<void> updateTemplate(Context& c, std::string_view id, const TemplateInput& input) {
        service::common::requireUuid(19002, id, "告警模板 ID 无效");
        co_await requireTemplate(c, id);
        co_await ensureTemplateName(c, input.name, std::string(id));
        if (!input.protocolConfigId.empty()) {
            co_await ensureProtocolConfig(c, input.protocolConfigId);
        }
        Query query(c.pool());
        query.update(service::alert::entities::AlertRuleTemplateEntity::tableName())
            .set(service::alert::entities::AlertRuleTemplateEntity::columnName<"name">(), query.value(input.name))
            .set(service::alert::entities::AlertRuleTemplateEntity::columnName<"category">(), query.nullIf(query.value(input.category), query.value("")))
            .set(service::alert::entities::AlertRuleTemplateEntity::columnName<"description">(), query.nullIf(query.value(input.description), query.value("")))
            .set(service::alert::entities::AlertRuleTemplateEntity::columnName<"severity">(), query.value(input.severity))
            .set(service::alert::entities::AlertRuleTemplateEntity::columnName<"conditions">(), query.cast(query.value(input.conditions), Type::kJsonb))
            .set(service::alert::entities::AlertRuleTemplateEntity::columnName<"logic">(), query.value(input.logic))
            .set(service::alert::entities::AlertRuleTemplateEntity::columnName<"silence_duration">(), query.cast(query.value(input.silenceDuration), Type::kInteger))
            .set(service::alert::entities::AlertRuleTemplateEntity::columnName<"recovery_condition">(), query.value(input.recoveryCondition))
            .set(service::alert::entities::AlertRuleTemplateEntity::columnName<"recovery_wait_seconds">(), query.cast(query.value(input.recoveryWaitSeconds), Type::kInteger))
            .set(service::alert::entities::AlertRuleTemplateEntity::columnName<"applicable_protocols">(), query.cast(query.value(input.applicableProtocols), Type::kJsonb))
            .set(service::alert::entities::AlertRuleTemplateEntity::columnName<"protocol_config_id">(), query.cast(query.nullIf(query.value(input.protocolConfigId), query.value("")), Type::kUuid))
            .set(service::alert::entities::AlertRuleTemplateEntity::columnName<"updated_at">(), query.call("now"))
            .andWhere(query.binary(query.column(service::alert::entities::AlertRuleTemplateEntity::columnName<"id">()), Op::kEqual, query.cast(query.value(id), Type::kUuid)))
            .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull, query.column(service::alert::entities::AlertRuleTemplateEntity::columnName<"deleted_at">())));
        (void)co_await c.db().execute(query);
    }

    template <typename Context>
    ruvia::Task<void> removeTemplate(Context& c, std::string_view id) {
        service::common::requireUuid(19002, id, "告警模板 ID 无效");
        co_await requireTemplate(c, id);
        Query removal(c.pool());
        removal.update(service::alert::entities::AlertRuleTemplateEntity::tableName()).set(service::alert::entities::AlertRuleTemplateEntity::columnName<"deleted_at">(), removal.call("now")).set(service::alert::entities::AlertRuleTemplateEntity::columnName<"updated_at">(), removal.call("now")).andWhere(removal.binary(removal.column(service::alert::entities::AlertRuleTemplateEntity::columnName<"id">()), Op::kEqual, removal.cast(removal.value(id), Type::kUuid))).andWhere(removal.unary(ruvia::DbUnaryOperator::kIsNull, removal.column(service::alert::entities::AlertRuleTemplateEntity::columnName<"deleted_at">())));
        (void)co_await c.db().execute(removal);
    }

    template <typename Context>
    ruvia::Task<std::string> applyTemplate(Context& c, const ApplyTemplateInput& input) {
        const auto& templateId = input.templateId;
        const auto& deviceIds = input.deviceIds;
        co_await requireTemplate(c, templateId);
        Query selected(c.pool());
        selected.select(selected.star()).from(service::alert::entities::AlertRuleTemplateEntity::tableName()).andWhere(selected.binary(selected.column(service::alert::entities::AlertRuleTemplateEntity::columnName<"id">()), Op::kEqual, selected.cast(selected.value(templateId), Type::kUuid))).andWhere(selected.unary(ruvia::DbUnaryOperator::kIsNull, selected.column(service::alert::entities::AlertRuleTemplateEntity::columnName<"deleted_at">())));
        Query requested(c.pool());
        for (const auto& id : deviceIds) {
            requested.values({ requested.cast(requested.value(id), Type::kUuid) });
        }
        Query existing(c.pool());
        const auto existingName = existing.binary(existing.binary(existing.column("name", "selected"), Op::kConcat, existing.value(" - ")), Op::kConcat, existing.column(service::alert::entities::DeviceEntity::columnName<"name">(), "device"));
        existing.select(existing.cast(existing.value(1), Type::kInteger)).from(service::alert::entities::AlertRuleEntity::tableName(), "existing").andWhere(existing.binary(existing.column(service::alert::entities::AlertRuleEntity::columnName<"device_id">(), "existing"), Op::kEqual, existing.column(service::alert::entities::DeviceEntity::columnName<"id">(), "device"))).andWhere(existing.binary(existing.column(service::alert::entities::AlertRuleEntity::columnName<"name">(), "existing"), Op::kEqual, existingName)).andWhere(existing.unary(ruvia::DbUnaryOperator::kIsNull, existing.column(service::alert::entities::AlertRuleEntity::columnName<"deleted_at">(), "existing")));
        Query source(c.pool());
        const auto name = source.binary(source.binary(source.column("name", "selected"), Op::kConcat, source.value(" - ")), Op::kConcat, source.column(service::alert::entities::DeviceEntity::columnName<"name">(), "device"));
        source.select({ source.call("gen_random_uuid"), name, source.column(service::alert::entities::DeviceEntity::columnName<"id">(), "device"), source.column("severity", "selected"), source.column("conditions", "selected"), source.column("logic", "selected"), source.column("silence_duration", "selected"), source.column("recovery_condition", "selected"), source.column("recovery_wait_seconds", "selected"), source.cast(source.value("enabled"), { .customName = "status_enum" }), source.column("description", "selected"), source.cast(source.value(c.userId), Type::kUuid) })
            .from("selected")
            .join(ruvia::DbJoinType::kCross, "requested")
            .join(ruvia::DbJoinType::kInner, service::alert::entities::DeviceEntity::tableName(), source.binary(source.column(service::alert::entities::DeviceEntity::columnName<"id">(), "device"), Op::kEqual, source.column("device_id", "requested")))
            .andWhere(source.unary(ruvia::DbUnaryOperator::kIsNull, source.column(service::alert::entities::DeviceEntity::columnName<"deleted_at">(), "device")))
            .andWhere(source.unary(ruvia::DbUnaryOperator::kNot, source.exists(existing)));
        Query created(c.pool());
        created.insertInto(service::alert::entities::AlertRuleEntity::tableName(), { "id", "name", "device_id", "severity", "conditions", "logic", "silence_duration", "recovery_condition", "recovery_wait_seconds", "status", "remark", "created_by" })
            .insertFrom(source)
            .returning({ created.column(service::alert::entities::AlertRuleEntity::columnName<"id">()) });
        Query count(c.pool());
        count.select(count.aggregate("count", { count.star() })).from("created");
        Query ids(c.pool());
        ids.select(ids.aggregate("jsonb_agg", { ids.column("id") })).from("created");
        Query response(c.pool());
        response.with("selected", selected).with("requested", requested, { .columns = { "device_id" } }).with("created", created).select(response.cast(response.call("jsonb_build_object", { response.cast(response.value("success"), Type::kText), response.subquery(count), response.cast(response.value("total"), Type::kText), response.cast(response.value(static_cast<std::int64_t>(deviceIds.size())), Type::kInteger), response.cast(response.value("createdIds"), Type::kText), response.coalesce({ response.subquery(ids), response.cast(response.value("[]"), Type::kJsonb) }) }), Type::kText));
        const auto result = co_await c.db().query(response);
        (void)co_await service::rpc::call(c, "alert", "refresh", "{}");
        co_return firstJson(result);
    }

    template <typename Context>
    ruvia::Task<std::string> listRecords(Context& c, const AlertListQuery& parameters) {
        Query filtered(c.pool());
        filtered.select(filtered.star("record")).from(service::alert::entities::OpenAlertRecordEntity::tableName(), "record");
        appendUuidFilter(parameters.get<"deviceId">(), "deviceId", filtered, filtered.column(service::alert::entities::OpenAlertRecordEntity::columnName<"device_id">(), "record"));
        appendUuidFilter(parameters.get<"ruleId">(), "ruleId", filtered, filtered.column(service::alert::entities::OpenAlertRecordEntity::columnName<"rule_id">(), "record"));
        appendTextFilter(parameters.get<"status">(), "status", filtered, filtered.column(service::alert::entities::OpenAlertRecordEntity::columnName<"status">(), "record"));
        appendTextFilter(parameters.get<"severity">(), "severity", filtered, filtered.column(service::alert::entities::OpenAlertRecordEntity::columnName<"severity">(), "record"));
        Query listed(c.pool());
        listed.select({ listed.star("page_rows"), listed.alias(listed.column(service::alert::entities::AlertRuleEntity::columnName<"name">(), "rule"), "rule_name"), listed.alias(listed.column(service::alert::entities::DeviceEntity::columnName<"name">(), "device"), "device_name") })
            .from("page_rows")
            .join(ruvia::DbJoinType::kLeft, service::alert::entities::AlertRuleEntity::tableName(), listed.binary(listed.column(service::alert::entities::AlertRuleEntity::columnName<"id">(), "rule"), Op::kEqual, listed.column("rule_id", "page_rows")), "rule")
            .join(ruvia::DbJoinType::kInner, service::alert::entities::DeviceEntity::tableName(), listed.binary(listed.column(service::alert::entities::DeviceEntity::columnName<"id">(), "device"), Op::kEqual, listed.column("device_id", "page_rows")));
        co_return co_await pageResult(c, parameters, filtered, listed, { "id", "rule_id", "rule_name", "device_id", "device_name", "severity", "status", "message", "detail", "triggered_at", "acknowledged_at", "acknowledged_by", "resolved_at" }, "triggered_at");
    }

    template <typename Context>
    ruvia::Task<void> acknowledge(Context& c, std::string_view id) {
        service::common::requireUuid(19002, id, "告警记录 ID 无效");
        Query acknowledgement(c.pool());
        acknowledgement.update(service::alert::entities::OpenAlertRecordEntity::tableName()).set(service::alert::entities::OpenAlertRecordEntity::columnName<"status">(), acknowledgement.value("acknowledged")).set(service::alert::entities::OpenAlertRecordEntity::columnName<"acknowledged_at">(), acknowledgement.call("now")).set(service::alert::entities::OpenAlertRecordEntity::columnName<"acknowledged_by">(), acknowledgement.cast(acknowledgement.value(c.userId), Type::kUuid)).set(service::alert::entities::OpenAlertRecordEntity::columnName<"updated_at">(), acknowledgement.call("now")).andWhere(acknowledgement.binary(acknowledgement.column(service::alert::entities::OpenAlertRecordEntity::columnName<"id">()), Op::kEqual, acknowledgement.cast(acknowledgement.value(id), Type::kUuid))).andWhere(acknowledgement.binary(acknowledgement.column(service::alert::entities::OpenAlertRecordEntity::columnName<"status">()), Op::kEqual, acknowledgement.value("active")));
        const auto result = co_await c.db().execute(acknowledgement);
        if (result.affectedRows() == 0) {
            service::common::fail(17003, "告警记录不存在或已处理", 404);
        }
    }

    template <typename Context>
    ruvia::Task<void> batchAcknowledge(Context& c, const std::vector<std::string>& ids) {
        Query acknowledgement(c.pool());
        acknowledgement.update(service::alert::entities::OpenAlertRecordEntity::tableName()).set(service::alert::entities::OpenAlertRecordEntity::columnName<"status">(), acknowledgement.value("acknowledged")).set(service::alert::entities::OpenAlertRecordEntity::columnName<"acknowledged_at">(), acknowledgement.call("now")).set(service::alert::entities::OpenAlertRecordEntity::columnName<"acknowledged_by">(), acknowledgement.cast(acknowledgement.value(c.userId), Type::kUuid)).set(service::alert::entities::OpenAlertRecordEntity::columnName<"updated_at">(), acknowledgement.call("now")).andWhere(acknowledgement.binary(acknowledgement.column(service::alert::entities::OpenAlertRecordEntity::columnName<"id">()), Op::kIn, uuidList(acknowledgement, ids))).andWhere(acknowledgement.binary(acknowledgement.column(service::alert::entities::OpenAlertRecordEntity::columnName<"status">()), Op::kEqual, acknowledgement.value("active")));
        (void)co_await c.db().execute(acknowledgement);
    }

    template <typename Context>
    ruvia::Task<std::string> stats(Context& c) {
        Query unresolved(c.pool());
        const auto count = unresolved.aggregate("count", { unresolved.star() });
        unresolved.select(unresolved.alias(count, "total"));
        for (const auto severity : { "critical", "warning", "info" }) {
            unresolved.addSelect(unresolved.alias(unresolved.filter(count, unresolved.binary(unresolved.column(service::alert::entities::OpenAlertRecordEntity::columnName<"severity">()), Op::kEqual, unresolved.value(severity))), severity));
        }
        unresolved.addSelect(unresolved.alias(unresolved.aggregate("count", { unresolved.column(service::alert::entities::OpenAlertRecordEntity::columnName<"device_id">()) }, true), "affected_devices"))
            .from(service::alert::entities::OpenAlertRecordEntity::tableName())
            .andWhere(unresolved.binary(unresolved.column(service::alert::entities::OpenAlertRecordEntity::columnName<"status">()), Op::kIn, unresolved.list({ unresolved.value("active"), unresolved.value("acknowledged") })));
        Query todayNew(c.pool());
        todayNew.select(todayNew.alias(todayNew.aggregate("count", { todayNew.star() }), "total")).from(service::alert::entities::OpenAlertRecordEntity::tableName()).andWhere(todayNew.binary(todayNew.column(service::alert::entities::OpenAlertRecordEntity::columnName<"triggered_at">()), Op::kGreaterEqual, todayNew.cast(todayNew.call("now"), Type::kDate)));
        Query acknowledged(c.pool());
        acknowledged.select(acknowledged.alias(acknowledged.aggregate("count", { acknowledged.star() }), "total")).from(service::alert::entities::OpenAlertRecordEntity::tableName()).andWhere(acknowledged.binary(acknowledged.column(service::alert::entities::OpenAlertRecordEntity::columnName<"status">()), Op::kEqual, acknowledged.value("acknowledged")));
        Query todayResolved(c.pool());
        todayResolved.select(todayResolved.alias(todayResolved.aggregate("count", { todayResolved.star() }), "total")).from(service::alert::entities::OpenAlertRecordEntity::tableName()).andWhere(todayResolved.binary(todayResolved.column(service::alert::entities::OpenAlertRecordEntity::columnName<"status">()), Op::kEqual, todayResolved.value("resolved"))).andWhere(todayResolved.binary(todayResolved.column(service::alert::entities::OpenAlertRecordEntity::columnName<"resolved_at">()), Op::kGreaterEqual, todayResolved.cast(todayResolved.call("now"), Type::kDate)));
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
        result.with("unresolved", unresolved).with("today_new", todayNew).with("acknowledged_summary", acknowledged).with("today_resolved", todayResolved).select(result.cast(result.call("jsonb_build_object", values), Type::kText)).from("unresolved").join(ruvia::DbJoinType::kCross, "today_new").join(ruvia::DbJoinType::kCross, "acknowledged_summary").join(ruvia::DbJoinType::kCross, "today_resolved");
        co_return firstJson(co_await c.db().query(result));
    }

    template <typename Context>
    ruvia::Task<std::string> grouped(Context& c, std::int64_t days) {
        Query groups(c.pool());
        const auto count = groups.aggregate("count", { groups.star() });
        groups.select({ groups.column(service::alert::entities::OpenAlertRecordEntity::columnName<"rule_id">(), "record"), groups.alias(groups.coalesce({ groups.column(service::alert::entities::AlertRuleEntity::columnName<"name">(), "rule"), groups.value("已删除规则") }), "rule_name"), groups.column(service::alert::entities::OpenAlertRecordEntity::columnName<"device_id">(), "record"), groups.alias(groups.column(service::alert::entities::DeviceEntity::columnName<"name">(), "device"), "device_name"), groups.column(service::alert::entities::OpenAlertRecordEntity::columnName<"severity">(), "record"), groups.alias(count, "total_count") });
        for (const auto& [status, alias] : { std::pair{ "active", "active_count" }, std::pair{ "acknowledged", "acked_count" }, std::pair{ "resolved", "resolved_count" } }) {
            groups.addSelect(groups.alias(groups.filter(count, groups.binary(groups.column(service::alert::entities::OpenAlertRecordEntity::columnName<"status">(), "record"), Op::kEqual, groups.value(status))), alias));
        }
        groups.addSelect(groups.alias(groups.aggregate("max", { groups.column(service::alert::entities::OpenAlertRecordEntity::columnName<"triggered_at">(), "record") }), "latest_trigger_time"))
            .from(service::alert::entities::OpenAlertRecordEntity::tableName(), "record")
            .join(ruvia::DbJoinType::kLeft, service::alert::entities::AlertRuleEntity::tableName(), groups.binary(groups.column(service::alert::entities::AlertRuleEntity::columnName<"id">(), "rule"), Op::kEqual, groups.column(service::alert::entities::OpenAlertRecordEntity::columnName<"rule_id">(), "record")), "rule")
            .join(ruvia::DbJoinType::kInner, service::alert::entities::DeviceEntity::tableName(), groups.binary(groups.column(service::alert::entities::DeviceEntity::columnName<"id">(), "device"), Op::kEqual, groups.column(service::alert::entities::OpenAlertRecordEntity::columnName<"device_id">(), "record")))
            .andWhere(groups.binary(groups.column(service::alert::entities::OpenAlertRecordEntity::columnName<"triggered_at">(), "record"), Op::kGreaterEqual, groups.binary(groups.call("now"), Op::kSubtract, groups.binary(groups.cast(groups.value(days), Type::kBigInt), Op::kMultiply, groups.cast(groups.value("1 day"), Type::kInterval)))))
            .groupBy({ groups.column(service::alert::entities::OpenAlertRecordEntity::columnName<"rule_id">(), "record"), groups.column(service::alert::entities::AlertRuleEntity::columnName<"name">(), "rule"), groups.column(service::alert::entities::OpenAlertRecordEntity::columnName<"device_id">(), "record"), groups.column(service::alert::entities::DeviceEntity::columnName<"name">(), "device"), groups.column(service::alert::entities::OpenAlertRecordEntity::columnName<"severity">(), "record") });
        Query result(c.pool());
        const std::vector<ruvia::DbOrderTerm> order{ { result.column("latest_trigger_time"), ruvia::DbOrderDirection::kDesc } };
        result.select(result.cast(result.coalesce({ result.aggregate("jsonb_agg", { fieldJson(result, { "rule_id", "rule_name", "device_id", "device_name", "severity", "total_count", "active_count", "acked_count", "resolved_count", "latest_trigger_time" }) }, false, order), result.cast(result.value("[]"), Type::kJsonb) }), Type::kText)).from(groups, "grouped");
        co_return firstJson(co_await c.db().query(result));
    }

  private:
    static ruvia::DbExpression uuidList(Query& query, const std::vector<std::string>& ids) {
        std::vector<ruvia::DbExpression> values;
        values.reserve(ids.size());
        for (const auto& id : ids) {
            values.push_back(query.cast(query.value(id), Type::kUuid));
        }
        return query.list(values);
    }

    template <typename Rows>
    static std::string firstJson(const Rows& rows) {
        if (rows.empty() || rows.front().empty() || !rows.front()[0].value().has_value()) {
            return "{}";
        }
        return std::string(rows.front()[0].value().value_or(std::string_view{}));
    }

    template <typename Rows>
    static std::string firstObject(const Rows& rows, std::string_view notFound) {
        if (rows.empty()) {
            service::common::fail(17003, std::string(notFound), 404);
        }
        return std::string(rows.front()[0].value().value_or(std::string_view{}));
    }

    static ruvia::DbExpression fieldJson(Query& query, const std::vector<std::string_view>& fields) {
        std::vector<ruvia::DbExpression> values;
        values.reserve(fields.size() * 2);
        for (const auto field : fields) {
            values.push_back(query.cast(query.value(field), Type::kText));
            auto value = query.column(field);
            if (field.ends_with("_at") || field == "latest_trigger_time") {
                value = query.call("iot_utc_timestamp", { value });
            }
            values.push_back(value);
        }
        return query.call("jsonb_build_object", values);
    }

    template <typename Context>
    static ruvia::Task<std::string> pageResult(Context& c, const AlertListQuery& parameters, const Query& filtered, const Query& listed, std::vector<std::string_view> fields, std::string_view timeColumn) {
        service::common::Page pagination;
        pagination.page = static_cast<std::int64_t>(*parameters.get<"page">());
        pagination.pageSize = static_cast<std::int64_t>(*parameters.get<"pageSize">());
        if (pagination.page > INT64_MAX / pagination.pageSize) {
            service::common::fail(17002, "分页超出范围", 400);
        }
        pagination.offset = (pagination.page - 1) * pagination.pageSize;
        Query counted(c.pool());
        counted.select(counted.alias(counted.aggregate("count", { counted.star() }), "total")).from("filtered");
        Query page(c.pool());
        page.select(page.star()).from("filtered").addOrderBy(page.column(timeColumn), ruvia::DbOrderDirection::kDesc).addOrderBy(page.column("id"), ruvia::DbOrderDirection::kDesc).limit(static_cast<std::uint64_t>(pagination.pageSize)).offset(static_cast<std::uint64_t>(pagination.offset));
        Query rows(c.pool());
        const std::vector<ruvia::DbOrderTerm> order{ { rows.column(timeColumn), ruvia::DbOrderDirection::kDesc }, { rows.column("id"), ruvia::DbOrderDirection::kDesc } };
        rows.select(rows.aggregate("jsonb_agg", { fieldJson(rows, fields) }, false, order)).from("listed");
        Query count(c.pool());
        count.select(count.column("total")).from("counted");
        Query result(c.pool());
        const auto total = result.coalesce({ result.subquery(count), result.value(0) });
        const auto pageSize = result.cast(result.value(pagination.pageSize), Type::kBigInt);
        const auto pages = result.caseWhen({ { result.binary(pageSize, Op::kEqual, result.value(0)), result.value(0) } }, result.cast(result.call("ceil", { result.binary(result.cast(total, Type::kNumeric), Op::kDivide, pageSize) }), Type::kBigInt));
        result.with("filtered", filtered).with("counted", counted).with("page_rows", page).with("listed", listed).select(result.cast(result.call("jsonb_build_object", { result.cast(result.value("list"), Type::kText), result.coalesce({ result.subquery(rows), result.cast(result.value("[]"), Type::kJsonb) }), result.cast(result.value("total"), Type::kText), total, result.cast(result.value("page"), Type::kText), result.cast(result.value(pagination.page), Type::kBigInt), result.cast(result.value("pageSize"), Type::kText), pageSize, result.cast(result.value("totalPages"), Type::kText), pages }), Type::kText));
        co_return firstJson(co_await c.db().query(result));
    }

    static void appendTextFilter(const std::optional<ruvia::String>& value, std::string_view, Query& query, ruvia::DbExpression column, bool contains = false) {
        if (!value || value->view().empty()) {
            return;
        }
        query.andWhere(query.binary(column, contains ? Op::kILike : Op::kEqual, query.value(contains ? "%" + std::string(value->view()) + "%" : std::string(value->view()))));
    }

    static void appendUuidFilter(const std::optional<ruvia::String>& value, std::string_view parameter, Query& query, ruvia::DbExpression column) {
        if (!value || value->view().empty()) {
            return;
        }
        service::common::requireUuid(19002, value->view(), std::string(parameter) + " 无效");
        query.andWhere(query.binary(column, Op::kEqual, query.cast(query.value(value->view()), Type::kUuid)));
    }

    template <typename Context>
    static ruvia::Task<void> ensureDevice(Context& c, std::string_view id) {
        Query query(c.pool());
        query.select(query.cast(query.value(1), Type::kInteger)).from(service::alert::entities::DeviceEntity::tableName()).andWhere(query.binary(query.column(service::alert::entities::DeviceEntity::columnName<"id">()), Op::kEqual, query.cast(query.value(id), Type::kUuid))).andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull, query.column(service::alert::entities::DeviceEntity::columnName<"deleted_at">())));
        const auto rows = co_await c.db().query(query);
        if (rows.empty()) {
            service::common::fail(17003, "关联设备不存在", 404);
        }
    }

    template <typename Context>
    static ruvia::Task<void> ensureProtocolConfig(Context& c, std::string_view id) {
        Query query(c.pool());
        query.select(query.cast(query.value(1), Type::kInteger)).from(service::alert::entities::ProtocolConfigEntity::tableName()).andWhere(query.binary(query.column(service::alert::entities::ProtocolConfigEntity::columnName<"id">()), Op::kEqual, query.cast(query.value(id), Type::kUuid))).andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull, query.column(service::alert::entities::ProtocolConfigEntity::columnName<"deleted_at">())));
        const auto rows = co_await c.db().query(query);
        if (rows.empty()) {
            service::common::fail(17003, "协议配置不存在", 404);
        }
    }

    template <typename Context>
    static ruvia::Task<void> requireRule(Context& c, std::string_view id) {
        Query query(c.pool());
        query.select(query.cast(query.value(1), Type::kInteger)).from(service::alert::entities::AlertRuleEntity::tableName()).andWhere(query.binary(query.column(service::alert::entities::AlertRuleEntity::columnName<"id">()), Op::kEqual, query.cast(query.value(id), Type::kUuid))).andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull, query.column(service::alert::entities::AlertRuleEntity::columnName<"deleted_at">())));
        const auto rows = co_await c.db().query(query);
        if (rows.empty()) {
            service::common::fail(17003, "告警规则不存在", 404);
        }
    }

    template <typename Context>
    static ruvia::Task<void> requireTemplate(Context& c, std::string_view id) {
        Query query(c.pool());
        query.select(query.cast(query.value(1), Type::kInteger)).from(service::alert::entities::AlertRuleTemplateEntity::tableName()).andWhere(query.binary(query.column(service::alert::entities::AlertRuleTemplateEntity::columnName<"id">()), Op::kEqual, query.cast(query.value(id), Type::kUuid))).andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull, query.column(service::alert::entities::AlertRuleTemplateEntity::columnName<"deleted_at">())));
        const auto rows = co_await c.db().query(query);
        if (rows.empty()) {
            service::common::fail(17003, "告警模板不存在", 404);
        }
    }

    template <typename Context>
    static ruvia::Task<void> ensureRuleName(Context& c, std::string_view name, std::string_view deviceId, const std::optional<std::string>& excluded) {
        Query query(c.pool());
        query.select(query.cast(query.value(1), Type::kInteger)).from(service::alert::entities::AlertRuleEntity::tableName()).andWhere(query.binary(query.column(service::alert::entities::AlertRuleEntity::columnName<"name">()), Op::kEqual, query.value(name))).andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull, query.column(service::alert::entities::AlertRuleEntity::columnName<"deleted_at">())));
        query.andWhere(query.binary(query.column(service::alert::entities::AlertRuleEntity::columnName<"device_id">()), Op::kEqual, query.cast(query.value(deviceId), Type::kUuid)));
        if (excluded && !excluded->empty()) {
            query.andWhere(query.binary(query.column(service::alert::entities::AlertRuleEntity::columnName<"id">()), Op::kNotEqual, query.cast(query.value(*excluded), Type::kUuid)));
        }
        const auto rows = co_await c.db().query(query);
        if (!rows.empty()) {
            service::common::fail(17009, "该设备已存在同名告警规则", 409);
        }
    }

    template <typename Context>
    static ruvia::Task<void> ensureTemplateName(Context& c, std::string_view name, const std::optional<std::string>& excluded) {
        Query query(c.pool());
        query.select(query.cast(query.value(1), Type::kInteger)).from(service::alert::entities::AlertRuleTemplateEntity::tableName()).andWhere(query.binary(query.column(service::alert::entities::AlertRuleTemplateEntity::columnName<"name">()), Op::kEqual, query.value(name))).andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull, query.column(service::alert::entities::AlertRuleTemplateEntity::columnName<"deleted_at">())));
        if (excluded && !excluded->empty()) {
            query.andWhere(query.binary(query.column(service::alert::entities::AlertRuleTemplateEntity::columnName<"id">()), Op::kNotEqual, query.cast(query.value(*excluded), Type::kUuid)));
        }
        const auto rows = co_await c.db().query(query);
        if (!rows.empty()) {
            service::common::fail(17009, "告警模板名称已存在", 409);
        }
    }
};

inline AlertService& alertService() {
    return AlertService::instance();
}

} // namespace service::alert
