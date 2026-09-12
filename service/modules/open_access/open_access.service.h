#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory_resource>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/web/ModelObject.h>
#include <ruvia/web/db/Db.h>
#include <ruvia/web/db/DbQuery.h>
#include <openssl/rand.h>

#include "service/common/http.h"
#include "service/common/message.h"
#include "service/common/timestamp.h"
#include "service/common/uuid.h"
#include "service/middleware/auth.h"
#include "service/middleware/rpc.h"
#include "service/modules/device/device.service.h"
#include "service/modules/open_access/open_access.types.h"
#include "service/modules/system/outbox/outbox.service.h"
#include "service/utils/crypto.h"
#include "service/utils/json.h"
#include "service/utils/text.h"

namespace service::access {

inline std::string generateAccessKey() {
    std::array<unsigned char, 24> random{};
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1)
        throw std::runtime_error("access-key random generation failed");
    return "ak_" + service::utils::hexEncode(random.data(), random.size());
}

class AccessService final {
  public:
    static AccessService& instance() {
        static AccessService service;
        return service;
    }

    ruvia::Task<std::string> listKeys(ruvia::Context& c) {
        ruvia::DbQuery listed(c.pool());
        const auto keyId = listed.column("id", "key");
        const auto bindingId = listed.column("device_id", "binding");
        const auto item = listed.call(
            "jsonb_build_object",
            {textKey(listed, "id"), text(listed, keyId), textKey(listed, "name"),
             listed.column("name", "key"), textKey(listed, "accessKeyPrefix"),
             listed.column("access_key_prefix", "key"), textKey(listed, "status"),
             listed.column("status", "key"), textKey(listed, "scopes"),
             listed.column("scopes", "key"), textKey(listed, "expiresAt"),
             listed.call("iot_utc_timestamp", {listed.column("expires_at", "key")}),
             textKey(listed, "lastUsedAt"),
             listed.call("iot_utc_timestamp", {listed.column("last_used_at", "key")}),
             textKey(listed, "lastUsedIp"), listed.column("last_used_ip", "key"),
             textKey(listed, "remark"), listed.column("remark", "key"),
             textKey(listed, "createdAt"),
             listed.call("iot_utc_timestamp", {listed.column("created_at", "key")}),
             textKey(listed, "updatedAt"),
             listed.call("iot_utc_timestamp", {listed.column("updated_at", "key")}),
             textKey(listed, "webhookCount"),
             listed.aggregate("count", {listed.column("id", "webhook")}, true),
             textKey(listed, "deviceIds"),
             listed.coalesce({listed.filter(
                                  listed.aggregate("jsonb_agg", {bindingId}, true),
                                  listed.unary(ruvia::DbUnaryOperator::kIsNotNull, bindingId)),
                              emptyJson(listed)})});
         listed.select({listed.alias(item, "item"), listed.column("created_at", "key")})
            .from("open_access_key", "key")
            .join(ruvia::DbJoinType::kLeft, "open_access_key_device",
                  listed.binary(listed.column("access_key_id", "binding"),
                                ruvia::DbBinaryOperator::kEqual, keyId),
                  "binding")
            .join(ruvia::DbJoinType::kLeft, "open_webhook",
                  andAll(listed,
                         listed.binary(listed.column("access_key_id", "webhook"),
                                       ruvia::DbBinaryOperator::kEqual, keyId),
                         listed.unary(ruvia::DbUnaryOperator::kIsNull,
                                      listed.column("deleted_at", "webhook"))),
                  "webhook")
            .where(listed.unary(ruvia::DbUnaryOperator::kIsNull,
                                listed.column("deleted_at", "key")))
            .groupBy({keyId});
        ruvia::DbQuery query(c.pool());
        const std::array<ruvia::DbOrderTerm, 1> keyOrder{{
            ruvia::DbOrderTerm{query.column("created_at", "listed"),
                               ruvia::DbOrderDirection::kDesc, ruvia::DbNullsOrder::kDefault}}};
        query.select(text(query, query.coalesce(
                                   {query.aggregate("jsonb_agg", {query.column("item", "listed")},
                                                    false, keyOrder),
                                    emptyJson(query)})))
            .from(listed, "listed");
        co_return firstJson(co_await c.db().query(query));
    }

    ruvia::Task<std::string> deviceOptions(ruvia::Context& c) {
        const auto actor = co_await service::device::deviceAccessService().actor(c);
        ruvia::DbQuery query(c.pool());
        service::device::DeviceAccessService::addScopedDevicesCtes(query, actor);
        const auto params = query.column("protocol_params");
        const auto key = service::device::DeviceAccessService::textKey(query, "remote_control");
        const auto hasKey = query.binary(params, ruvia::DbBinaryOperator::kJsonHasKey, key);
        const auto type = query.call(
            "jsonb_typeof",
            {service::device::DeviceAccessService::jsonValue(query, params, "remote_control")});
        const auto remoteText = service::device::DeviceAccessService::jsonText(
            query, params, "remote_control");
        const auto booleanRemote = query.binary(remoteText, ruvia::DbBinaryOperator::kEqual,
                                                query.value("true"));
        const auto stringRemote = query.binary(
            query.call("lower", {query.call("btrim", {remoteText})}),
            ruvia::DbBinaryOperator::kEqual, query.value("true"));
        const auto safeRemote = query.caseWhen(
            {{query.unary(ruvia::DbUnaryOperator::kIsNull, params),
              service::device::DeviceAccessService::boolean(query, true)},
             {query.unary(ruvia::DbUnaryOperator::kNot, hasKey),
              service::device::DeviceAccessService::boolean(query, true)},
             {query.binary(type, ruvia::DbBinaryOperator::kEqual, query.value("boolean")),
              booleanRemote},
             {query.binary(type, ruvia::DbBinaryOperator::kEqual, query.value("string")),
              stringRemote}},
            service::device::DeviceAccessService::boolean(query, false));
        const auto canCommand = query.binary(
            query.binary(query.column("access_rank"), ruvia::DbBinaryOperator::kGreaterEqual,
                         service::device::DeviceAccessService::integer(query, 2)),
            ruvia::DbBinaryOperator::kAnd, safeRemote);
        const auto item = query.call(
            "jsonb_build_object",
            {textKey(query, "id"), service::device::DeviceAccessService::text(
                                         query, query.column("id")),
             textKey(query, "name"), query.column("name"), textKey(query, "deviceCode"),
             service::device::DeviceAccessService::jsonText(query, params, "device_code"),
             textKey(query, "canCommand"), canCommand});
        const std::array<ruvia::DbOrderTerm, 2> deviceOrder{{
            ruvia::DbOrderTerm{query.column("name"), ruvia::DbOrderDirection::kAsc,
                               ruvia::DbNullsOrder::kDefault},
            ruvia::DbOrderTerm{query.column("id"), ruvia::DbOrderDirection::kAsc,
                               ruvia::DbNullsOrder::kDefault}}};
        query.select(text(query, query.coalesce(
                                  {query.aggregate("jsonb_agg", {item}, false, deviceOrder),
                                   emptyJson(query)})))
            .from("scoped_device")
            .where(query.binary(query.column("access_rank"), ruvia::DbBinaryOperator::kGreater,
                                service::device::DeviceAccessService::integer(query, 0)));
        co_return firstJson(co_await c.db().query(query));
    }

    ruvia::Task<std::string> createKey(ruvia::Context& c, const ruvia::JsonValue& payload) {
        const auto principal = service::middleware::requireAuth(c);
        const auto name = requiredString(payload, "name", "调用配置名称不能为空", 64);
        const auto status = optionalStatus(payload, "enabled");
        const auto scopes = requiredScopes(payload);
        const auto devices = requiredUuids(payload, "deviceIds", "至少选择一个设备", 10000);
        co_await ensureDevicesAccessible(c, devices, scopes.contains(std::string(kScopeCommand)));
        const auto expiresAt = optionalNullableString(payload, "expiresAt", 64);
        const auto remark = optionalNullableString(payload, "remark", 200);
        co_await ensureKeyNameAvailable(c, name, std::nullopt);

        const auto id = service::common::nextUuidV7();
        const auto rawKey = service::access::generateAccessKey();
        const auto prefix = rawKey.substr(0, 14);
        const auto keyHash = service::utils::sha256(rawKey);
        const auto scopeJson = stringArrayJson(scopes);
        const auto expiresAtValue = expiresAt.value_or("");
        const auto remarkValue = remark.value_or("");
        auto transaction = co_await c.db().beginTransaction();
        ruvia::DbQuery insert(c.pool());
        insert.insertInto("open_access_key",
                          {"id", "name", "access_key_prefix", "access_key_hash", "status",
                           "scopes", "expires_at", "remark", "created_by"})
            .values({uuid(insert, id), insert.value(name), insert.value(prefix),
                     insert.value(keyHash), insert.value(status),
                     insert.cast(insert.value(scopeJson), ruvia::DbDataType::kJsonb),
                     insert.cast(insert.nullIf(insert.value(expiresAtValue), insert.value("")),
                                 ruvia::DbDataType::kTimestampTz),
                     insert.nullIf(insert.value(remarkValue), insert.value("")),
                     uuid(insert, principal.userId)});
        (void)co_await transaction.execute(insert);
        co_await replaceDevices(transaction, id, devices);
        co_await service::system::OutboxService::enqueueConfigEvent(transaction, "access_key", "created", id);
        co_await transaction.commit();
        co_await refreshProjection(c);

        co_return "{\"id\":" + service::utils::jsonQuoted(id) + ",\"name\":" + service::utils::jsonQuoted(name) +
            ",\"status\":" + service::utils::jsonQuoted(status) + ",\"scopes\":" + scopeJson +
            ",\"expiresAt\":" + (expiresAt ? service::utils::jsonQuoted(*expiresAt) : std::string("null")) +
            ",\"deviceIds\":" + stringArrayJson(devices) + ",\"accessKey\":" + service::utils::jsonQuoted(rawKey) +
            ",\"accessKeyPrefix\":" + service::utils::jsonQuoted(prefix) + "}";
    }

    ruvia::Task<void> updateKey(ruvia::Context& c, std::string_view id,
                                const ruvia::JsonValue& payload) {
        service::common::requireUuid(19002, id, "调用配置 ID 无效");
        const auto existing = co_await requireKey(c, id);
        const auto name = optionalString(payload, "name", 64).value_or(existing.name);
        if (name.empty())
            service::common::fail(19002, "调用配置名称不能为空", 400);
        const auto status = optionalStatus(payload, existing.status);
        const auto scopes = service::utils::jsonField(payload, "scopes") ? requiredScopes(payload)
                                                         : existing.scopes;
        const auto deviceField = service::utils::jsonField(payload, "deviceIds");
        const auto devices = deviceField
                                 ? requiredUuids(payload, "deviceIds", "至少选择一个设备", 10000)
                                 : existing.deviceIds;
        co_await ensureDevicesAccessible(c, devices, scopes.contains(std::string(kScopeCommand)));
        co_await ensureKeyNameAvailable(c, name, std::string(id));

        const auto expiresAt = service::utils::jsonField(payload, "expiresAt")
                                   ? optionalNullableString(payload, "expiresAt", 64)
                                   : existing.expiresAt;
        const auto remark = service::utils::jsonField(payload, "remark")
                                ? optionalNullableString(payload, "remark", 200)
                                : existing.remark;
        const auto scopeJson = stringArrayJson(scopes);
        const auto expiresAtValue = expiresAt.value_or("");
        const auto remarkValue = remark.value_or("");
        auto transaction = co_await c.db().beginTransaction();
        ruvia::DbQuery update(c.pool());
        update.update("open_access_key")
            .set("name", update.value(name))
            .set("status", update.value(status))
            .set("scopes", update.cast(update.value(scopeJson), ruvia::DbDataType::kJsonb))
            .set("expires_at",
                 update.cast(update.nullIf(update.value(expiresAtValue), update.value("")),
                             ruvia::DbDataType::kTimestampTz))
            .set("remark", update.nullIf(update.value(remarkValue), update.value("")))
            .set("updated_at", update.call("now"))
            .where(andAll(update,
                          update.binary(update.column("id"), ruvia::DbBinaryOperator::kEqual,
                                        uuid(update, id)),
                          update.unary(ruvia::DbUnaryOperator::kIsNull,
                                       update.column("deleted_at"))));
        (void)co_await transaction.execute(update);
        if (deviceField)
            co_await replaceDevices(transaction, id, devices);
        co_await service::system::OutboxService::enqueueConfigEvent(transaction, "access_key", "updated", id);
        co_await transaction.commit();
        co_await refreshProjection(c);
    }

    ruvia::Task<std::string> rotateKey(ruvia::Context& c, std::string_view id) {
        service::common::requireUuid(19002, id, "调用配置 ID 无效");
        const auto existing = co_await requireKey(c, id);
        const auto rawKey = service::access::generateAccessKey();
        const auto prefix = rawKey.substr(0, 14);
        const auto keyHash = service::utils::sha256(rawKey);
        auto transaction = co_await c.db().beginTransaction();
        ruvia::DbQuery update(c.pool());
        update.update("open_access_key")
            .set("access_key_hash", update.value(keyHash))
            .set("access_key_prefix", update.value(prefix))
            .set("updated_at", update.call("now"))
            .where(andAll(update,
                          update.binary(update.column("id"), ruvia::DbBinaryOperator::kEqual,
                                        uuid(update, id)),
                          update.unary(ruvia::DbUnaryOperator::kIsNull,
                                       update.column("deleted_at"))));
        (void)co_await transaction.execute(update);
        co_await service::system::OutboxService::enqueueConfigEvent(transaction, "access_key", "rotated", id);
        co_await transaction.commit();
        co_await refreshProjection(c);
        co_return "{\"id\":" + service::utils::jsonQuoted(id) + ",\"name\":" + service::utils::jsonQuoted(existing.name) +
            ",\"accessKey\":" + service::utils::jsonQuoted(rawKey) + ",\"accessKeyPrefix\":" + service::utils::jsonQuoted(prefix) +
            "}";
    }

    ruvia::Task<void> removeKey(ruvia::Context& c, std::string_view id) {
        service::common::requireUuid(19002, id, "调用配置 ID 无效");
        (void)co_await requireKey(c, id);
        auto transaction = co_await c.db().beginTransaction();
        ruvia::DbQuery webhookUpdate(c.pool());
        webhookUpdate.update("open_webhook")
            .set("deleted_at", webhookUpdate.call("now"))
            .set("updated_at", webhookUpdate.call("now"))
            .where(andAll(webhookUpdate,
                          webhookUpdate.binary(webhookUpdate.column("access_key_id"),
                                               ruvia::DbBinaryOperator::kEqual,
                                               uuid(webhookUpdate, id)),
                          webhookUpdate.unary(ruvia::DbUnaryOperator::kIsNull,
                                              webhookUpdate.column("deleted_at"))));
        (void)co_await transaction.execute(webhookUpdate);
        ruvia::DbQuery keyUpdate(c.pool());
        keyUpdate.update("open_access_key")
            .set("deleted_at", keyUpdate.call("now"))
            .set("updated_at", keyUpdate.call("now"))
            .where(andAll(keyUpdate,
                          keyUpdate.binary(keyUpdate.column("id"),
                                           ruvia::DbBinaryOperator::kEqual,
                                           uuid(keyUpdate, id)),
                          keyUpdate.unary(ruvia::DbUnaryOperator::kIsNull,
                                          keyUpdate.column("deleted_at"))));
        (void)co_await transaction.execute(keyUpdate);
        co_await service::system::OutboxService::enqueueConfigEvent(transaction, "access_key", "deleted", id);
        co_await transaction.commit();
        co_await refreshProjection(c);
    }

    ruvia::Task<std::string> listWebhooks(ruvia::Context& c) {
        ruvia::DbQuery listed(c.pool());
        const auto webhookId = listed.column("id", "webhook");
        const auto bindingDevice = listed.column("device_id", "binding");
        const auto item = listed.call(
            "jsonb_build_object",
            {textKey(listed, "id"), text(listed, webhookId), textKey(listed, "accessKeyId"),
             text(listed, listed.column("access_key_id", "webhook")),
             textKey(listed, "accessKeyName"), listed.column("name", "key"),
             textKey(listed, "name"), listed.column("name", "webhook"),
             textKey(listed, "url"), listed.column("url", "webhook"),
             textKey(listed, "status"), listed.column("status", "webhook"),
             textKey(listed, "timeoutSeconds"), listed.column("timeout_seconds", "webhook"),
             textKey(listed, "skipTlsVerify"), listed.column("skip_tls_verify", "webhook"),
             textKey(listed, "headers"), listed.column("headers", "webhook"),
             textKey(listed, "eventTypes"), listed.column("event_types", "webhook"),
             textKey(listed, "deviceIds"),
             listed.coalesce({listed.filter(
                                  listed.aggregate("jsonb_agg", {bindingDevice}),
                                  listed.unary(ruvia::DbUnaryOperator::kIsNotNull,
                                               bindingDevice)),
                              emptyJson(listed)}),
             textKey(listed, "hasSecret"),
             listed.unary(ruvia::DbUnaryOperator::kIsNotNull,
                         listed.column("secret", "webhook")),
             textKey(listed, "lastTriggeredAt"),
             listed.call("iot_utc_timestamp", {listed.column("last_triggered_at", "webhook")}),
             textKey(listed, "lastSuccessAt"),
             listed.call("iot_utc_timestamp", {listed.column("last_success_at", "webhook")}),
             textKey(listed, "lastFailureAt"),
             listed.call("iot_utc_timestamp", {listed.column("last_failure_at", "webhook")}),
             textKey(listed, "lastHttpStatus"),
             listed.column("last_http_status", "webhook"), textKey(listed, "lastError"),
             listed.column("last_error", "webhook"), textKey(listed, "createdAt"),
             listed.call("iot_utc_timestamp", {listed.column("created_at", "webhook")}),
             textKey(listed, "updatedAt"),
             listed.call("iot_utc_timestamp", {listed.column("updated_at", "webhook")})});
         listed.select({listed.alias(item, "item"), listed.column("created_at", "webhook")})
            .from("open_webhook", "webhook")
            .join(ruvia::DbJoinType::kInner, "open_access_key",
                  andAll(listed,
                         listed.binary(listed.column("id", "key"),
                                       ruvia::DbBinaryOperator::kEqual,
                                       listed.column("access_key_id", "webhook")),
                         listed.unary(ruvia::DbUnaryOperator::kIsNull,
                                      listed.column("deleted_at", "key"))),
                  "key")
            .join(ruvia::DbJoinType::kLeft, "open_access_key_device",
                  listed.binary(listed.column("access_key_id", "binding"),
                                ruvia::DbBinaryOperator::kEqual,
                                listed.column("id", "key")),
                  "binding")
            .where(listed.unary(ruvia::DbUnaryOperator::kIsNull,
                                listed.column("deleted_at", "webhook")))
            .groupBy({webhookId, listed.column("id", "key")});
        if (const auto key = c.req().query("accessKeyId"); key && !key->empty()) {
            service::common::requireUuid(19002, *key, "调用配置 ID 无效");
            listed.andWhere(listed.binary(listed.column("access_key_id", "webhook"),
                                          ruvia::DbBinaryOperator::kEqual,
                                          uuid(listed, *key)));
        }
        ruvia::DbQuery query(c.pool());
        const std::array<ruvia::DbOrderTerm, 1> webhookOrder{{
            ruvia::DbOrderTerm{query.column("created_at", "listed"),
                               ruvia::DbOrderDirection::kDesc, ruvia::DbNullsOrder::kDefault}}};
        query.select(text(query, query.coalesce(
                                  {query.aggregate("jsonb_agg", {query.column("item", "listed")},
                                                   false, webhookOrder),
                                   emptyJson(query)})))
            .from(listed, "listed");
        co_return firstJson(co_await c.db().query(query));
    }

    ruvia::Task<std::string> createWebhook(ruvia::Context& c, const ruvia::JsonValue& payload) {
        const auto accessKeyId = requiredUuid(payload, "accessKeyId", "请选择调用配置");
        (void)co_await requireKey(c, accessKeyId);
        const auto name = requiredString(payload, "name", "Webhook 名称不能为空", 64);
        const auto url = requiredString(payload, "url", "Webhook 地址不能为空", 2048);
        validateWebhookUrl(url);
        const auto status = optionalStatus(payload, "enabled");
        const auto timeout = optionalInteger(payload, "timeoutSeconds", 5, 1, 30);
        const auto skipTlsVerify = optionalBoolean(payload, "skipTlsVerify", false);
        const auto headers = objectJson(payload, "headers", "{}");
        co_await validateHeaders(c, headers);
        const auto events = eventTypes(payload);
        const auto secret = optionalNullableString(payload, "secret", 255);
        co_await ensureWebhookNameAvailable(c, accessKeyId, name, std::nullopt);
        const auto id = service::common::nextUuidV7();
        const auto eventJson = stringArrayJson(events);
        const auto secretValue = secret.value_or("");
        auto transaction = co_await c.db().beginTransaction();
        ruvia::DbQuery insert(c.pool());
        insert.insertInto("open_webhook",
                          {"id", "access_key_id", "name", "url", "status", "timeout_seconds",
                           "skip_tls_verify", "headers", "event_types", "secret"})
            .values({uuid(insert, id), uuid(insert, accessKeyId), insert.value(name),
                     insert.value(url), insert.value(status), insert.value(timeout),
                     insert.value(skipTlsVerify),
                     insert.cast(insert.value(headers), ruvia::DbDataType::kJsonb),
                     insert.cast(insert.value(eventJson), ruvia::DbDataType::kJsonb),
                     insert.nullIf(insert.value(secretValue), insert.value(""))});
        (void)co_await transaction.execute(insert);
        co_await service::system::OutboxService::enqueueConfigEvent(transaction, "webhook", "created", id);
        co_await transaction.commit();
        co_return "{\"id\":" + service::utils::jsonQuoted(id) + ",\"accessKeyId\":" + service::utils::jsonQuoted(accessKeyId) +
            ",\"name\":" + service::utils::jsonQuoted(name) + ",\"url\":" + service::utils::jsonQuoted(url) +
            ",\"status\":" + service::utils::jsonQuoted(status) + ",\"timeoutSeconds\":" + std::to_string(timeout) +
            ",\"skipTlsVerify\":" + (skipTlsVerify ? "true" : "false") +
            ",\"headers\":" + headers + ",\"eventTypes\":" + stringArrayJson(events) +
            ",\"hasSecret\":" + (secret && !secret->empty() ? "true" : "false") + "}";
    }

    ruvia::Task<void> updateWebhook(ruvia::Context& c, std::string_view id,
                                    const ruvia::JsonValue& payload) {
        service::common::requireUuid(19002, id, "Webhook ID 无效");
        const auto existing = co_await requireWebhook(c, id);
        const auto accessKeyId = service::utils::jsonField(payload, "accessKeyId")
                                     ? requiredUuid(payload, "accessKeyId", "请选择调用配置")
                                     : existing.accessKeyId;
        (void)co_await requireKey(c, accessKeyId);
        const auto name = optionalString(payload, "name", 64).value_or(existing.name);
        const auto url = optionalString(payload, "url", 2048).value_or(existing.url);
        if (name.empty())
            service::common::fail(19002, "Webhook 名称不能为空", 400);
        validateWebhookUrl(url);
        const auto status = optionalStatus(payload, existing.status);
        const auto timeout =
            service::utils::jsonField(payload, "timeoutSeconds")
                ? optionalInteger(payload, "timeoutSeconds", existing.timeout, 1, 30)
                : existing.timeout;
        const auto skipTlsVerify =
            service::utils::jsonField(payload, "skipTlsVerify")
                ? optionalBoolean(payload, "skipTlsVerify", existing.skipTlsVerify)
                : existing.skipTlsVerify;
        const auto headers =
            service::utils::jsonField(payload, "headers") ? objectJson(payload, "headers", "{}") : existing.headers;
        co_await validateHeaders(c, headers);
        const auto events = service::utils::jsonField(payload, "eventTypes") ? eventTypes(payload)
                                                             : existing.events;
        const auto secret = service::utils::jsonField(payload, "secret")
                                ? optionalNullableString(payload, "secret", 255)
                                : existing.secret;
        co_await ensureWebhookNameAvailable(c, accessKeyId, name, std::string(id));
        const auto eventJson = stringArrayJson(events);
        const auto secretValue = secret.value_or("");
        auto transaction = co_await c.db().beginTransaction();
        ruvia::DbQuery update(c.pool());
        update.update("open_webhook")
            .set("access_key_id", uuid(update, accessKeyId))
            .set("name", update.value(name))
            .set("url", update.value(url))
            .set("status", update.value(status))
            .set("timeout_seconds", update.value(timeout))
            .set("skip_tls_verify", update.value(skipTlsVerify))
            .set("headers", update.cast(update.value(headers), ruvia::DbDataType::kJsonb))
            .set("event_types", update.cast(update.value(eventJson), ruvia::DbDataType::kJsonb))
            .set("secret", update.nullIf(update.value(secretValue), update.value("")))
            .set("updated_at", update.call("now"))
            .where(andAll(update,
                          update.binary(update.column("id"), ruvia::DbBinaryOperator::kEqual,
                                        uuid(update, id)),
                          update.unary(ruvia::DbUnaryOperator::kIsNull,
                                       update.column("deleted_at"))));
        (void)co_await transaction.execute(update);
        co_await service::system::OutboxService::enqueueConfigEvent(transaction, "webhook", "updated", id);
        co_await transaction.commit();
    }

    ruvia::Task<void> removeWebhook(ruvia::Context& c, std::string_view id) {
        service::common::requireUuid(19002, id, "Webhook ID 无效");
        (void)co_await requireWebhook(c, id);
        auto transaction = co_await c.db().beginTransaction();
        ruvia::DbQuery update(c.pool());
        update.update("open_webhook")
            .set("deleted_at", update.call("now"))
            .set("updated_at", update.call("now"))
            .where(andAll(update,
                          update.binary(update.column("id"), ruvia::DbBinaryOperator::kEqual,
                                        uuid(update, id)),
                          update.unary(ruvia::DbUnaryOperator::kIsNull,
                                       update.column("deleted_at"))));
        (void)co_await transaction.execute(update);
        co_await service::system::OutboxService::enqueueConfigEvent(transaction, "webhook", "deleted", id);
        co_await transaction.commit();
    }

    ruvia::Task<std::string> listLogs(ruvia::Context& c) {
        const auto pagination = service::common::page(c.req());
        const auto readUuid = [&](std::string_view name) -> std::optional<std::string> {
            if (const auto value = c.req().query(name); value && !value->empty()) {
                service::common::requireUuid(19002, *value, std::string(name) + " 无效");
                return std::string(*value);
            }
            return std::nullopt;
        };
        const auto accessKeyId = readUuid("accessKeyId");
        const auto webhookId = readUuid("webhookId");
        const auto deviceId = readUuid("deviceId");
        const auto direction = c.req().query("direction");
        const auto action = c.req().query("action");
        const auto status = c.req().query("status");
        const auto eventType = c.req().query("eventType");
        const auto applyFilters = [&](ruvia::DbQuery& query, std::string_view alias) {
            auto predicate = service::device::DeviceAccessService::boolean(query, true);
            const auto addUuid = [&](std::string_view value, std::string_view column) {
                if (!value.empty())
                    predicate = query.binary(
                        predicate, ruvia::DbBinaryOperator::kAnd,
                        query.binary(query.column(column, alias),
                                     ruvia::DbBinaryOperator::kEqual, uuid(query, value)));
            };
            const auto addText = [&](const std::optional<std::string_view>& value,
                                     std::string_view column) {
                if (value && !value->empty())
                    predicate = query.binary(
                        predicate, ruvia::DbBinaryOperator::kAnd,
                        query.binary(query.column(column, alias),
                                     ruvia::DbBinaryOperator::kEqual,
                                     query.value(*value)));
            };
            addUuid(accessKeyId.value_or(""), "access_key_id");
            addUuid(webhookId.value_or(""), "webhook_id");
            addUuid(deviceId.value_or(""), "device_id");
            addText(direction, "direction");
            addText(action, "action");
            addText(status, "status");
            addText(eventType, "event_type");
            query.where(predicate);
        };
        ruvia::DbQuery counted(c.pool());
        counted.select(counted.aggregate("count", {counted.star()}))
            .from("open_access_log", "log");
        applyFilters(counted, "log");
        ruvia::DbQuery pageRows(c.pool());
        pageRows.select(pageRows.star("log")).from("open_access_log", "log");
        applyFilters(pageRows, "log");
        pageRows.orderBy(pageRows.column("created_at", "log"), ruvia::DbOrderDirection::kDesc)
            .addOrderBy(pageRows.column("id", "log"), ruvia::DbOrderDirection::kDesc)
            .limit(static_cast<std::uint64_t>(pagination.pageSize))
            .offset(static_cast<std::uint64_t>(pagination.offset));
        ruvia::DbQuery page(c.pool());
        page.select({page.star("page_rows"), page.alias(page.column("name", "key"),
                                                         "access_key_name"),
                     page.alias(page.column("name", "webhook"), "webhook_name")})
            .from(pageRows, "page_rows")
            .join(ruvia::DbJoinType::kLeft, "open_access_key",
                  page.binary(page.column("id", "key"), ruvia::DbBinaryOperator::kEqual,
                              page.column("access_key_id", "page_rows")),
                  "key")
            .join(ruvia::DbJoinType::kLeft, "open_webhook",
                  page.binary(page.column("id", "webhook"), ruvia::DbBinaryOperator::kEqual,
                              page.column("webhook_id", "page_rows")),
                  "webhook");
        ruvia::DbQuery query(c.pool());
        query.with("counted", counted).with("page_rows", pageRows).with("page", page);
        const auto total = query.subquery(counted);
        const auto item = query.call(
            "jsonb_build_object",
            {textKey(query, "id"), text(query, query.column("id", "page")),
             textKey(query, "accessKeyId"), text(query, query.column("access_key_id", "page")),
             textKey(query, "accessKeyName"), query.column("access_key_name", "page"),
             textKey(query, "webhookId"), text(query, query.column("webhook_id", "page")),
             textKey(query, "webhookName"), query.column("webhook_name", "page"),
             textKey(query, "direction"), query.column("direction", "page"),
             textKey(query, "action"), query.column("action", "page"),
             textKey(query, "eventType"), query.column("event_type", "page"),
             textKey(query, "status"), query.column("status", "page"),
             textKey(query, "httpMethod"), query.column("http_method", "page"),
             textKey(query, "target"), query.column("target", "page"),
             textKey(query, "requestIp"), query.column("request_ip", "page"),
             textKey(query, "httpStatus"), query.column("http_status", "page"),
             textKey(query, "deviceId"), text(query, query.column("device_id", "page")),
             textKey(query, "deviceCode"), query.column("device_code", "page"),
             textKey(query, "message"), query.column("message", "page"),
             textKey(query, "requestPayload"), query.column("request_payload", "page"),
             textKey(query, "responsePayload"), query.column("response_payload", "page"),
             textKey(query, "createdAt"),
             query.call("iot_utc_timestamp", {query.column("created_at", "page")})});
        const std::array<ruvia::DbOrderTerm, 2> logOrder{{
            ruvia::DbOrderTerm{query.column("created_at", "page"),
                               ruvia::DbOrderDirection::kDesc, ruvia::DbNullsOrder::kDefault},
            ruvia::DbOrderTerm{query.column("id", "page"), ruvia::DbOrderDirection::kDesc,
                               ruvia::DbNullsOrder::kDefault}}};
        const auto list = query.coalesce(
            {query.aggregate("jsonb_agg", {item}, false, logOrder),
             emptyJson(query)});
        const auto totalPages = query.cast(
            query.call("ceil",
                       {query.binary(query.cast(total, ruvia::DbDataType::kNumeric),
                                     ruvia::DbBinaryOperator::kDivide,
                                     query.cast(query.value(pagination.pageSize),
                                                ruvia::DbDataType::kNumeric))}),
            ruvia::DbDataType::kBigInt);
        query.select(query.cast(
            query.call("jsonb_build_object",
                       {textKey(query, "list"), list, textKey(query, "total"), total,
                        textKey(query, "page"), query.cast(query.value(pagination.page),
                                                            ruvia::DbDataType::kBigInt),
                        textKey(query, "pageSize"),
                        query.cast(query.value(pagination.pageSize), ruvia::DbDataType::kBigInt),
                        textKey(query, "totalPages"), totalPages}),
            ruvia::DbDataType::kText))
            .from("page");
        co_return firstJson(co_await c.db().query(query));
    }

    ruvia::Task<AccessSession> authenticate(ruvia::Context& c, std::string_view requiredScope) {
        const auto raw = requestAccessKey(c.req());
        if (raw.empty())
            service::common::fail(19010, "缺少 X-Access-Key", 401);
        const auto projected = co_await loadSession(c, raw);
        if (!projected)
            service::common::fail(19010, "AccessKey 无效", 401);
        if (projected->status != "enabled")
            service::common::fail(19011, "AccessKey 已被禁用", 403);
        if (session::expired(*projected, service::message::utcNowMilliseconds()))
            service::common::fail(19010, "AccessKey 已过期", 401);
        AccessSession session;
        session.id = projected->id;
        session.name = projected->name;
        session.scopes = projected->scopes;
        if (!requiredScope.empty() && !session.allows(requiredScope))
            service::common::fail(19011, "AccessKey 未开通所需权限", 403);
        session.deviceIds = projected->deviceIds;
        if (session.deviceIds.empty())
            service::common::fail(19011, "AccessKey 未配置可访问设备", 403);
        co_return session;
    }

    ruvia::Task<void> audit(ruvia::Context& c, std::string_view action,
                            const AccessSession& session, std::string_view deviceId = {},
                            std::string_view requestPayload = "{}",
                            std::string_view responsePayload = "{}") {
        try {
            std::string payload =
                "{\"action\":" + service::utils::jsonQuoted(action) +
                ",\"accessKeyId\":" + service::utils::jsonQuoted(session.id) +
                ",\"method\":" + service::utils::jsonQuoted(c.req().method()) +
                ",\"target\":" + service::utils::jsonQuoted(c.req().path()) +
                ",\"requestIp\":" +
                service::utils::jsonQuoted(service::common::clientIp(c)) +
                ",\"httpStatus\":200,\"deviceId\":" +
                service::utils::jsonQuoted(deviceId) +
                ",\"requestPayload\":" + std::string(requestPayload) +
                ",\"responsePayload\":" + std::string(responsePayload) + "}";
            (void)co_await service::rpc::call(c, "access", "audit", std::move(payload));
        } catch (...) {
            // Open-access audit is best effort, matching the previous stream enqueue path.
        }
    }

    ruvia::Task<std::string> publicDevices(ruvia::Context& c, const AccessSession& session) {
        const auto pagination = service::common::page(c.req());
        const auto keyword = service::utils::trim(c.req().query("keyword").value_or(""));
        ruvia::DbQuery visible(c.pool());
        const auto deviceId = visible.column("id", "device");
        const auto code = service::device::DeviceAccessService::jsonText(
            visible, visible.column("protocol_params", "device"), "device_code");
        const auto pattern = visible.binary(
            visible.binary(service::device::DeviceAccessService::text(visible, "%"),
                           ruvia::DbBinaryOperator::kConcat,
                           service::device::DeviceAccessService::text(visible, keyword)),
            ruvia::DbBinaryOperator::kConcat,
            service::device::DeviceAccessService::text(visible, "%"));
        const auto keywordFilter = visible.binary(
            visible.binary(service::device::DeviceAccessService::text(visible, keyword),
                           ruvia::DbBinaryOperator::kEqual,
                           service::device::DeviceAccessService::text(visible, "")),
            ruvia::DbBinaryOperator::kOr,
            visible.binary(
                visible.binary(
                    visible.binary(service::device::DeviceAccessService::text(
                                       visible, deviceId),
                                   ruvia::DbBinaryOperator::kILike, pattern),
                    ruvia::DbBinaryOperator::kOr,
                    visible.binary(visible.column("name", "device"),
                                   ruvia::DbBinaryOperator::kILike, pattern)),
                ruvia::DbBinaryOperator::kOr,
                visible.binary(visible.coalesce({code, visible.value("")}),
                               ruvia::DbBinaryOperator::kILike, pattern)));
        visible.select({deviceId, visible.column("name", "device"),
                       visible.alias(code, "code")})
            .from("device", "device")
            .join(ruvia::DbJoinType::kInner, "open_access_key_device",
                  visible.binary(visible.column("device_id", "binding"),
                                ruvia::DbBinaryOperator::kEqual, deviceId),
                  "binding")
            .where(andAll(visible,
                          visible.binary(visible.column("access_key_id", "binding"),
                                        ruvia::DbBinaryOperator::kEqual,
                                        uuid(visible, session.id)),
                          visible.unary(ruvia::DbUnaryOperator::kIsNull,
                                        visible.column("deleted_at", "device")),
                          keywordFilter));
        ruvia::DbQuery counted(c.pool());
        counted.select(counted.aggregate("count", {counted.star()})).from(visible, "visible");
        ruvia::DbQuery page(c.pool());
        page.select(page.star("visible"))
            .from(visible, "visible")
            .orderBy(page.column("name", "visible"))
            .addOrderBy(page.column("id", "visible"))
            .limit(static_cast<std::uint64_t>(pagination.pageSize))
            .offset(static_cast<std::uint64_t>(pagination.offset));
        ruvia::DbQuery query(c.pool());
        query.with("visible", visible).with("counted", counted).with("page", page);
        const auto item = query.call(
            "jsonb_build_object",
            {textKey(query, "id"), text(query, query.column("id", "page")),
             textKey(query, "code"), query.column("code", "page"),
             textKey(query, "name"), query.column("name", "page")});
        const auto total = query.subquery(counted);
        const std::array<ruvia::DbOrderTerm, 2> deviceListOrder{{
            ruvia::DbOrderTerm{query.column("name", "page"), ruvia::DbOrderDirection::kAsc,
                               ruvia::DbNullsOrder::kDefault},
            ruvia::DbOrderTerm{query.column("id", "page"), ruvia::DbOrderDirection::kAsc,
                               ruvia::DbNullsOrder::kDefault}}};
        const auto list = query.coalesce(
            {query.aggregate("jsonb_agg", {item}, false, deviceListOrder),
             emptyJson(query)});
        const auto totalPages = query.cast(
            query.call("ceil",
                       {query.binary(query.cast(total, ruvia::DbDataType::kNumeric),
                                     ruvia::DbBinaryOperator::kDivide,
                                     query.cast(query.value(pagination.pageSize),
                                                ruvia::DbDataType::kNumeric))}),
            ruvia::DbDataType::kBigInt);
        query.select(query.cast(
            query.call("jsonb_build_object",
                       {textKey(query, "list"), list, textKey(query, "total"), total,
                        textKey(query, "page"), query.cast(query.value(pagination.page),
                                                            ruvia::DbDataType::kBigInt),
                        textKey(query, "pageSize"),
                        query.cast(query.value(pagination.pageSize), ruvia::DbDataType::kBigInt),
                        textKey(query, "totalPages"), totalPages}),
            ruvia::DbDataType::kText))
            .from("page");
        const auto rows = co_await c.db().query(query);
        co_return firstJson(rows);
    }

    ruvia::Task<std::string> publicRealtime(ruvia::Context& c, const AccessSession& session,
                                            std::string_view deviceId) {
        service::common::requireUuid(19002, deviceId, "设备 ID 无效");
        if (!session.allowsDevice(deviceId))
            service::common::fail(19011, "AccessKey 无权访问该设备", 403);
        co_return co_await realtimeData(c, deviceId);
    }

    ruvia::Task<std::string> publicHistory(ruvia::Context& c, const AccessSession& session,
                                           std::string_view deviceId) {
        co_await requireSessionDevice(c, session, deviceId);
        const auto start = service::utils::trim(c.req().query("startTime").value_or(""));
        const auto end = service::utils::trim(c.req().query("endTime").value_or(""));
        if (start.empty() || end.empty())
            service::common::fail(19002, "startTime 和 endTime 不能为空", 400);
        const auto pagination = service::common::page(c.req());
        try {
            const auto query = historyQuery(c.pool(), deviceId, start, end,
                                            pagination.pageSize, pagination.offset,
                                            pagination.page);
            const auto rows = co_await c.db().query(query);
            co_return firstJson(rows);
        } catch (const std::exception&) {
            service::common::fail(19002, "时间范围格式错误", 400);
        }
    }

    ruvia::Task<std::string> publicAlerts(ruvia::Context& c, const AccessSession& session) {
        const auto pagination = service::common::page(c.req());
        std::optional<std::string> deviceFilter;
        if (const auto device = c.req().query("deviceId"); device && !device->empty()) {
            service::common::requireUuid(19002, *device, "设备 ID 无效");
            if (!session.allowsDevice(*device))
                service::common::fail(19011, "AccessKey 无权访问该设备", 403);
            deviceFilter = std::string(*device);
        }
        const auto status = c.req().query("status");
        const auto severity = c.req().query("severity");
        const auto applyFilters = [&](ruvia::DbQuery& query, std::string_view recordAlias,
                                      std::string_view bindingAlias) {
            auto predicate = query.binary(query.column("access_key_id", bindingAlias),
                                          ruvia::DbBinaryOperator::kEqual,
                                          uuid(query, session.id));
            if (deviceFilter)
                predicate = query.binary(
                    predicate, ruvia::DbBinaryOperator::kAnd,
                    query.binary(query.column("device_id", recordAlias),
                                 ruvia::DbBinaryOperator::kEqual,
                                 uuid(query, *deviceFilter)));
            if (status && !status->empty())
                predicate = query.binary(
                    predicate, ruvia::DbBinaryOperator::kAnd,
                    query.binary(query.column("status", recordAlias),
                                 ruvia::DbBinaryOperator::kEqual, query.value(*status)));
            if (severity && !severity->empty())
                predicate = query.binary(
                    predicate, ruvia::DbBinaryOperator::kAnd,
                    query.binary(query.column("severity", recordAlias),
                                 ruvia::DbBinaryOperator::kEqual, query.value(*severity)));
            query.where(predicate);
        };
        ruvia::DbQuery counted(c.pool());
        counted.select(counted.aggregate("count", {counted.star()}))
            .from("open_alert_record", "record")
            .join(ruvia::DbJoinType::kInner, "open_access_key_device",
                  counted.binary(counted.column("device_id", "binding"),
                                 ruvia::DbBinaryOperator::kEqual,
                                 counted.column("device_id", "record")),
                  "binding");
        applyFilters(counted, "record", "binding");
        ruvia::DbQuery pageRows(c.pool());
        pageRows.select(pageRows.star("record"))
            .from("open_alert_record", "record")
            .join(ruvia::DbJoinType::kInner, "open_access_key_device",
                  pageRows.binary(pageRows.column("device_id", "binding"),
                                  ruvia::DbBinaryOperator::kEqual,
                                  pageRows.column("device_id", "record")),
                  "binding");
        applyFilters(pageRows, "record", "binding");
        pageRows.orderBy(pageRows.column("triggered_at", "record"),
                         ruvia::DbOrderDirection::kDesc)
            .addOrderBy(pageRows.column("id", "record"), ruvia::DbOrderDirection::kDesc)
            .limit(static_cast<std::uint64_t>(pagination.pageSize))
            .offset(static_cast<std::uint64_t>(pagination.offset));
        ruvia::DbQuery page(c.pool());
        page.select({page.star("page_rows"), page.alias(page.column("name", "device"),
                                                          "device_name"),
                     page.alias(service::device::DeviceAccessService::jsonText(
                                    page, page.column("protocol_params", "device"),
                                    "device_code"),
                                 "device_code")})
            .from(pageRows, "page_rows")
            .join(ruvia::DbJoinType::kInner, "device",
                  page.binary(page.column("id", "device"), ruvia::DbBinaryOperator::kEqual,
                              page.column("device_id", "page_rows")),
                  "device");
        ruvia::DbQuery query(c.pool());
        query.with("counted", counted).with("page_rows", pageRows).with("page", page);
        const auto item = query.call(
            "jsonb_build_object",
            {textKey(query, "id"), text(query, query.column("id", "page")),
             textKey(query, "device"),
             query.call("jsonb_build_object",
                        {textKey(query, "id"),
                         text(query, query.column("device_id", "page")),
                         textKey(query, "code"), query.column("device_code", "page"),
                         textKey(query, "name"), query.column("device_name", "page")}),
             textKey(query, "ruleId"), text(query, query.column("rule_id", "page")),
             textKey(query, "severity"), query.column("severity", "page"),
             textKey(query, "status"), query.column("status", "page"),
             textKey(query, "message"), query.column("message", "page"),
             textKey(query, "time"),
             query.call("iot_utc_timestamp", {query.column("triggered_at", "page")})});
        const auto total = query.subquery(counted);
        const std::array<ruvia::DbOrderTerm, 2> alertOrder{{
            ruvia::DbOrderTerm{query.column("triggered_at", "page"),
                               ruvia::DbOrderDirection::kDesc, ruvia::DbNullsOrder::kDefault},
            ruvia::DbOrderTerm{query.column("id", "page"), ruvia::DbOrderDirection::kDesc,
                               ruvia::DbNullsOrder::kDefault}}};
        const auto list = query.coalesce(
            {query.aggregate("jsonb_agg", {item}, false, alertOrder),
             emptyJson(query)});
        const auto totalPages = query.cast(
            query.call("ceil",
                       {query.binary(query.cast(total, ruvia::DbDataType::kNumeric),
                                     ruvia::DbBinaryOperator::kDivide,
                                     query.cast(query.value(pagination.pageSize),
                                                ruvia::DbDataType::kNumeric))}),
            ruvia::DbDataType::kBigInt);
        query.select(query.cast(
            query.call("jsonb_build_object",
                       {textKey(query, "list"), list, textKey(query, "total"), total,
                        textKey(query, "page"), query.cast(query.value(pagination.page),
                                                            ruvia::DbDataType::kBigInt),
                        textKey(query, "pageSize"),
                        query.cast(query.value(pagination.pageSize), ruvia::DbDataType::kBigInt),
                        textKey(query, "totalPages"), totalPages}),
            ruvia::DbDataType::kText))
            .from("page");
        const auto rows = co_await c.db().query(query);
        co_return firstJson(rows);
    }

    template <typename Context>
    ruvia::Task<std::string> realtimeData(Context& c, std::string_view deviceId) {
        const auto device = co_await loadRealtimeDevice(c, deviceId);
        if (!device)
            service::common::fail(19001, "设备不存在", 404);
        const auto latestKey = service::telemetry::latest::latestKey(device->id);
        const auto latest = co_await service::message::redis::command(
            c.redis(), std::vector<std::string>{"HGETALL", latestKey});
        std::map<std::string_view, std::string_view, std::less<>> latestFields;
        if (latest.kind() == ruvia::RedisValue::Kind::kArray) {
            const auto values = latest.array();
            for (std::size_t index = 0; index + 1 < values.size(); index += 2)
                if (values[index].kind() == ruvia::RedisValue::Kind::kString &&
                    values[index + 1].kind() == ruvia::RedisValue::Kind::kString)
                    latestFields.insert_or_assign(values[index].string(),
                                                  values[index + 1].string());
        } else {
            service::message::redis::throwValue("read realtime device values", latest);
        }
        std::string body = "{\"device\":{\"id\":" + service::utils::jsonQuoted(deviceId) +
                           ",\"code\":" + service::utils::jsonQuoted(device->code) +
                           ",\"name\":" + service::utils::jsonQuoted(device->name) + "},\"points\":[";
        for (std::size_t index = 0; index < device->points.size(); ++index) {
            if (index != 0)
                body.push_back(',');
            std::string value = "null";
            std::string time = "null";
            if (const auto data = latestFields.find(device->points[index].id);
                data != latestFields.end()) {
                if (const auto json = ruvia::JsonValue::parse(data->second)) {
                    const auto dataType = json->template get<ruvia::String>("dataType");
                    if (const auto current = service::utils::jsonField(*json, "value"))
                        value = service::telemetry::latest::canonicalPointJson(
                            current->view(), dataType ? dataType->view() : std::string_view{});
                    if (const auto observed =
                            json->template get<ruvia::Int64>("observedAt"))
                        time = service::utils::jsonQuoted(service::common::utcTimestampFromMilliseconds(static_cast<std::int64_t>(*observed)));
                }
            }
            body += "{\"id\":" + service::utils::jsonQuoted(device->points[index].id) +
                    ",\"name\":" + service::utils::jsonQuoted(device->points[index].name) +
                    ",\"value\":" + value +
                    ",\"unit\":" + service::utils::jsonQuoted(device->points[index].unit) +
                    ",\"time\":" + time + "}";
        }
        body += "]}";
        co_return body;
    }

  private:
    static ruvia::DbQuery::Expr text(ruvia::DbQuery& query, ruvia::DbQuery::Expr value) {
        return service::device::DeviceAccessService::text(query, value);
    }

    static ruvia::DbQuery::Expr text(ruvia::DbQuery& query, std::string_view value) {
        return service::device::DeviceAccessService::text(query, value);
    }

    static ruvia::DbQuery::Expr textKey(ruvia::DbQuery& query, std::string_view value) {
        return service::device::DeviceAccessService::textKey(query, value);
    }

    static ruvia::DbQuery::Expr uuid(ruvia::DbQuery& query, std::string_view value) {
        return service::device::DeviceAccessService::uuid(query, value);
    }

    static ruvia::DbQuery::Expr emptyJson(ruvia::DbQuery& query) {
        return query.cast(query.value("[]"), ruvia::DbDataType::kJsonb);
    }

    static ruvia::DbQuery::Expr andAll(ruvia::DbQuery& query,
                                       ruvia::DbQuery::Expr first,
                                       ruvia::DbQuery::Expr second) {
        return service::device::andAll(query, first, second);
    }

    template <typename... Expressions>
    static ruvia::DbQuery::Expr andAll(ruvia::DbQuery& query,
                                       ruvia::DbQuery::Expr first,
                                       ruvia::DbQuery::Expr second, Expressions... rest) {
        return service::device::andAll(query, first, second, rest...);
    }

    static ruvia::Task<void> refreshProjection(ruvia::Context& c) {
        (void)co_await service::rpc::call(c, "access", "refresh", "{}");
    }

    static ruvia::Task<std::optional<session::Entry>> loadSession(
        ruvia::Context& c, std::string_view rawKey) {
        static constexpr std::string_view script = R"lua(
local version = redis.call('GET', KEYS[1])
if not version then return nil end
return redis.call('HGET', ARGV[2] .. version, ARGV[1])
)lua";
        const auto keyHash = service::utils::sha256(rawKey);
        const std::string activeKey(session::kActiveVersionKey);
        const std::string_view keys[]{activeKey};
        const std::string prefix(session::kVersionPrefix);
        const std::string_view args[]{keyHash, prefix};
        const auto reply = co_await c.redis().eval(script, keys, args);
        if (reply.null())
            co_return std::nullopt;
        if (reply.kind() != ruvia::RedisValue::Kind::kString)
            service::message::redis::throwValue("load projected access session", reply);
        co_return session::decode(reply.string());
    }

    static std::string_view projectionField(
        const ruvia::RedisValue& value, std::string_view operation,
        std::string_view field) {
        if (value.kind() != ruvia::RedisValue::Kind::kArray)
            service::message::redis::throwValue(operation, value);
        const auto& values = value.array();
        if (values.size() % 2 != 0)
            service::message::redis::throwValue(operation, value);
        for (std::size_t index = 0; index < values.size(); index += 2) {
            if (values[index].kind() != ruvia::RedisValue::Kind::kString ||
                values[index + 1].kind() != ruvia::RedisValue::Kind::kString)
                service::message::redis::throwValue(operation, value);
            if (values[index].string() == field)
                return values[index + 1].string();
        }
        return {};
    }

    static ruvia::Task<std::optional<service::message::realtime::Device>> loadRealtimeDevice(
        ruvia::Context& c, std::string_view deviceId) {
        const auto versionReply = co_await service::message::redis::command(
            c.redis(), {"GET", std::string(service::message::projection::kRuntimeActiveVersionKey)});
        if (versionReply.null())
            throw std::runtime_error("runtime is not ready");
        if (versionReply.kind() != ruvia::RedisValue::Kind::kString)
            service::message::redis::throwValue("GET active runtime", versionReply);
        const auto version = versionReply.string();
        const std::vector<std::vector<std::string>> commands{
            {"HGETALL", service::message::projection::realtimeDeviceKey(version, deviceId)},
            {"LRANGE", service::message::projection::realtimeDevicePointsKey(version, deviceId), "0", "-1"}};
        auto pipeline = c.redis().pipeline();
        for (const auto& command : commands) {
            const std::vector<std::string_view> views(command.begin(), command.end());
            pipeline.command(views);
        }
        const auto replies = co_await std::move(pipeline).exec();
        if (replies.size() != 2)
            throw std::runtime_error("incomplete realtime device projection reply");
        if (replies[0].kind() != ruvia::RedisValue::Kind::kArray ||
            replies[0].array().empty())
            co_return std::nullopt;

        service::message::realtime::Device result;
        result.id.assign(projectionField(replies[0], "read realtime device metadata", "id"));
        result.code.assign(projectionField(replies[0], "read realtime device metadata", "code"));
        result.name.assign(projectionField(replies[0], "read realtime device metadata", "name"));
        if (result.id.empty())
            co_return std::nullopt;
        if (replies[1].kind() != ruvia::RedisValue::Kind::kArray)
            service::message::redis::throwValue("read realtime device points", replies[1]);
        const auto& points = replies[1].array();
        result.points.reserve(points.size());
        for (const auto& point : points) {
            if (point.kind() != ruvia::RedisValue::Kind::kString)
                service::message::redis::throwValue("read realtime device points", replies[1]);
            const auto decoded = service::message::realtime::decodePoint(point.string());
            if (!decoded)
                throw std::runtime_error("invalid realtime point projection");
            result.points.push_back(*decoded);
        }
        co_return result;
    }

    struct KeyState final {
        std::string name;
        std::string status;
        std::set<std::string, std::less<>> scopes;
        std::vector<std::string> deviceIds;
        std::optional<std::string> expiresAt;
        std::optional<std::string> remark;
    };
    struct WebhookState final {
        std::string accessKeyId;
        std::string name;
        std::string url;
        std::string status;
        std::int64_t timeout{5};
        bool skipTlsVerify{false};
        std::string headers{"{}"};
        std::set<std::string, std::less<>> events;
        std::optional<std::string> secret;
    };

    static std::string requestAccessKey(const ruvia::ContextRequest& request) {
        return service::utils::trim(request.header("X-Access-Key").value_or(""));
    }

    template <typename Rows> static std::string firstJson(const Rows& rows) {
        if (rows.empty() || rows.front().empty() || !rows.front()[0].value().has_value())
            return "[]";
        return std::string(rows.front()[0].value().value_or(std::string_view{}));
    }

    static std::string requiredString(const ruvia::JsonValue& payload, std::string_view field,
                                      std::string_view message, std::size_t maximum) {
        const auto value = payload.get<ruvia::String>(field);
        if (!value)
            service::common::fail(19002, std::string(message), 400);
        auto result = service::utils::trim(value->view());
        if (result.empty() || result.size() > maximum)
            service::common::fail(19002, std::string(message), 400);
        return result;
    }

    static std::optional<std::string> optionalString(const ruvia::JsonValue& payload,
                                                     std::string_view field, std::size_t maximum) {
        const auto raw = service::utils::jsonField(payload, field);
        if (!raw)
            return std::nullopt;
        const auto value = payload.get<ruvia::String>(field);
        if (!value)
            service::common::fail(19002, std::string(field) + " 必须是字符串", 400);
        auto result = service::utils::trim(value->view());
        if (result.size() > maximum)
            service::common::fail(19002, std::string(field) + " 长度超出限制", 400);
        return result;
    }

    static std::optional<std::string> optionalNullableString(const ruvia::JsonValue& payload,
                                                             std::string_view field,
                                                             std::size_t maximum) {
        const auto raw = service::utils::jsonField(payload, field);
        if (!raw || raw->isNull())
            return std::nullopt;
        const auto value = payload.get<ruvia::String>(field);
        if (!value)
            service::common::fail(19002, std::string(field) + " 必须是字符串或 null", 400);
        auto result = service::utils::trim(value->view());
        if (result.size() > maximum)
            service::common::fail(19002, std::string(field) + " 长度超出限制", 400);
        return result.empty() ? std::nullopt : std::optional<std::string>(std::move(result));
    }

    static std::string optionalStatus(const ruvia::JsonValue& payload, std::string_view fallback) {
        const auto status = optionalString(payload, "status", 16).value_or(std::string(fallback));
        if (status != "enabled" && status != "disabled")
            service::common::fail(19002, "status 只能为 enabled 或 disabled", 400);
        return status;
    }

    static std::int64_t optionalInteger(const ruvia::JsonValue& payload, std::string_view field,
                                        std::int64_t fallback, std::int64_t minimum,
                                        std::int64_t maximum) {
        const auto raw = service::utils::jsonField(payload, field);
        if (!raw)
            return fallback;
        const auto value = payload.get<ruvia::Int64>(field);
        if (!value)
            service::common::fail(19002, std::string(field) + " 必须是整数", 400);
        const auto result = static_cast<std::int64_t>(*value);
        if (result < minimum || result > maximum)
            service::common::fail(19002, std::string(field) + " 超出允许范围", 400);
        return result;
    }

    static bool optionalBoolean(const ruvia::JsonValue& payload, std::string_view field,
                                bool fallback) {
        const auto raw = service::utils::jsonField(payload, field);
        if (!raw)
            return fallback;
        const auto value = payload.get<ruvia::Bool>(field);
        if (!value)
            service::common::fail(19002, std::string(field) + " 必须是布尔值", 400);
        return static_cast<bool>(*value);
    }

    static std::set<std::string, std::less<>> requiredScopes(const ruvia::JsonValue& payload) {
        const auto values = payload.get<ruvia::Array<ruvia::String>>("scopes");
        if (!values || values->empty())
            service::common::fail(19002, "至少选择一个开放权限", 400);
        std::set<std::string, std::less<>> result;
        for (const auto& value : *values) {
            if (!supportedScope(value.view()))
                service::common::fail(19002, "包含不支持的开放权限", 400);
            result.emplace(value.view());
        }
        return result;
    }

    static std::vector<std::string> requiredUuids(const ruvia::JsonValue& payload,
                                                  std::string_view field, std::string_view message,
                                                  std::size_t maximum) {
        const auto values = payload.get<ruvia::Array<ruvia::String>>(field);
        if (!values || values->empty() || values->size() > maximum)
            service::common::fail(19002, std::string(message), 400);
        std::set<std::string, std::less<>> unique;
        for (const auto& value : *values) {
            service::common::requireUuid(19002, value.view(), std::string(field) + " 包含无效 UUID");
            unique.emplace(value.view());
        }
        return {unique.begin(), unique.end()};
    }

    static std::string requiredUuid(const ruvia::JsonValue& payload, std::string_view field,
                                    std::string_view message) {
        const auto value = payload.get<ruvia::String>(field);
        if (!value)
            service::common::fail(19002, std::string(message), 400);
        service::common::requireUuid(19002, value->view(), message);
        return std::string(value->view());
    }

    template <typename Range> static std::string stringArrayJson(const Range& values) {
        std::string result{"["};
        bool first = true;
        for (const auto& value : values) {
            if (!first)
                result.push_back(',');
            first = false;
            result += service::utils::jsonQuoted(value);
        }
        result.push_back(']');
        return result;
    }

    static std::set<std::string, std::less<>> parseStringArray(std::string_view json) {
        std::set<std::string, std::less<>> result;
        const auto value = ruvia::JsonValue::parse(json);
        if (!value || !value->isArray())
            return result;
        auto remaining = json;
        const auto parsed = ruvia::detail::parseJsonValue<ruvia::Array<ruvia::String>>(
            remaining, std::pmr::get_default_resource());
        if (!parsed)
            return result;
        for (const auto& item : *parsed)
            result.emplace(item.view());
        return result;
    }

    static std::string objectJson(const ruvia::JsonValue& payload, std::string_view field,
                                  std::string_view fallback) {
        const auto value = service::utils::jsonField(payload, field);
        if (!value)
            return std::string(fallback);
        if (!value->isObject())
            service::common::fail(19002, std::string(field) + " 必须是对象", 400);
        return std::string(value->view());
    }

    static std::set<std::string, std::less<>> eventTypes(const ruvia::JsonValue& payload) {
        const auto raw = service::utils::jsonField(payload, "eventTypes");
        if (!raw)
            return {"device.data.reported"};
        const auto values = payload.get<ruvia::Array<ruvia::String>>("eventTypes");
        if (!values || values->empty())
            service::common::fail(19002, "eventTypes 必须是非空字符串数组", 400);
        std::set<std::string, std::less<>> result;
        for (const auto& value : *values) {
            if (!service::message::supportedEvent(value.view()))
                service::common::fail(19002, "包含不支持的 Webhook 事件", 400);
            result.emplace(value.view());
        }
        return result;
    }

    static void validateWebhookUrl(std::string_view url) {
        if ((!url.starts_with("http://") && !url.starts_with("https://")) ||
            url.find('\r') != std::string_view::npos || url.find('\n') != std::string_view::npos ||
            url.find('@') != std::string_view::npos || url.find('#') != std::string_view::npos)
            service::common::fail(19002, "Webhook 地址必须是有效的 HTTP(S) URL", 400);
        const auto authority = url.find("://") + 3;
        if (authority >= url.size() || url[authority] == '/' || url[authority] == '?' ||
            url[authority] == '#')
            service::common::fail(19002, "Webhook 地址主机无效", 400);
    }

    static ruvia::Task<void> validateHeaders(ruvia::Context& c, std::string_view headers) {
        ruvia::DbQuery invalid(c.pool());
        const auto key = invalid.column("key", "header");
        const auto value = invalid.column("value", "header");
        const auto path = invalid.cast(
            invalid.value("{}"),
            ruvia::DbTypeDefinition{.dataType = ruvia::DbDataType::kText, .array = true});
        auto predicate = invalid.unary(
            ruvia::DbUnaryOperator::kNot,
            invalid.binary(key, ruvia::DbBinaryOperator::kRegex,
                          invalid.value("^[!#$%&'*+.^_`|~0-9A-Za-z-]+$")));
        predicate = invalid.binary(
            predicate, ruvia::DbBinaryOperator::kOr,
            invalid.binary(invalid.call("jsonb_typeof", {value}),
                           ruvia::DbBinaryOperator::kNotEqual,
                           invalid.value("string")));
        predicate = invalid.binary(
            predicate, ruvia::DbBinaryOperator::kOr,
            invalid.binary(invalid.binary(value, ruvia::DbBinaryOperator::kJsonPathText, path),
                           ruvia::DbBinaryOperator::kRegex, invalid.value("[\\r\\n]")));
        const auto reserved = invalid.call(
            "lower", {key});
        predicate = invalid.binary(
            predicate, ruvia::DbBinaryOperator::kOr,
            invalid.binary(
                reserved, ruvia::DbBinaryOperator::kIn,
                invalid.list({textKey(invalid, "host"), textKey(invalid, "content-length"),
                              textKey(invalid, "connection"), textKey(invalid, "x-iot-event"),
                              textKey(invalid, "x-iot-timestamp"),
                              textKey(invalid, "x-iot-delivery"),
                              textKey(invalid, "x-iot-signature"),
                              textKey(invalid, "content-type"), textKey(invalid, "user-agent"),
                              textKey(invalid, "transfer-encoding"), textKey(invalid, "trailer"),
                              textKey(invalid, "te"), textKey(invalid, "upgrade"),
                              textKey(invalid, "expect"),
                              textKey(invalid, "proxy-connection")})));
        invalid.select(service::device::DeviceAccessService::integer(invalid, 1))
            .fromFunction(
                invalid.call("jsonb_each",
                             {invalid.cast(invalid.value(headers), ruvia::DbDataType::kJsonb)}),
                "header", {.lateral = true,
                            .columns = {{.name = "key"}, {.name = "value"}}})
            .where(predicate)
            .limit(1);
        ruvia::DbQuery query(c.pool());
        query.select(query.unary(ruvia::DbUnaryOperator::kNot, query.exists(invalid)));
        const auto rows = co_await c.db().query(query);
        if (rows.front()[0].value().value_or(std::string_view{}) != "t")
            service::common::fail(19002, "自定义 Header 包含非法或保留字段", 400);
    }

    ruvia::Task<void> ensureDevicesAccessible(ruvia::Context& c,
                                              const std::vector<std::string>& ids,
                                              bool requireOperate) {
        const auto actor = co_await service::device::deviceAccessService().actor(c);
        ruvia::DbQuery query(c.pool());
        service::device::DeviceAccessService::addScopedDevicesCtes(query, actor);
        std::vector<ruvia::DbQuery::Expr> deviceIds;
        deviceIds.reserve(ids.size());
        for (const auto& id : ids) {
            deviceIds.push_back(uuid(query, id));
        }
        query.select(query.aggregate("count", {query.star()}))
            .from("scoped_device")
            .where(andAll(query,
                          query.binary(query.column("id"), ruvia::DbBinaryOperator::kIn,
                                       query.list(deviceIds)),
                          query.binary(query.column("access_rank"),
                                       ruvia::DbBinaryOperator::kGreaterEqual,
                                        service::device::DeviceAccessService::integer(
                                            query, requireOperate ? 2 : 1))));
        const auto rows = co_await c.db().query(query);
        const auto visible =
            service::common::parseInt64(
                std::optional<std::string_view>{rows.front()[0].value().value_or(std::string_view{})})
                .value_or(-1);
        if (visible != static_cast<std::int64_t>(ids.size()))
            service::common::fail(19011,
                                  requireOperate ? "所选设备中包含无控制权限的设备"
                                                 : "所选设备中包含无访问权限的设备",
                                  403);
    }

    static ruvia::Task<void> replaceDevices(ruvia::DbTransaction& transaction,
                                            std::string_view keyId,
                                            const std::vector<std::string>& devices) {
        ruvia::DbQuery remove;
        remove.deleteFrom("open_access_key_device")
            .where(remove.binary(remove.column("access_key_id"),
                                 ruvia::DbBinaryOperator::kEqual, uuid(remove, keyId)));
        (void)co_await transaction.execute(remove);
        for (const auto& device : devices) {
            ruvia::DbQuery insert;
            insert.insertInto("open_access_key_device", {"access_key_id", "device_id"})
                .values({uuid(insert, keyId), uuid(insert, device)});
            (void)co_await transaction.execute(insert);
        }
    }

    ruvia::Task<void> ensureKeyNameAvailable(ruvia::Context& c, std::string_view name,
                                             std::optional<std::string> except) {
        ruvia::DbQuery query(c.pool());
        auto predicate = andAll(
            query, query.binary(query.column("name"), ruvia::DbBinaryOperator::kEqual,
                                query.value(name)),
            query.unary(ruvia::DbUnaryOperator::kIsNull, query.column("deleted_at")));
        if (except)
            predicate = query.binary(
                predicate, ruvia::DbBinaryOperator::kAnd,
                query.binary(query.column("id"), ruvia::DbBinaryOperator::kNotEqual,
                             uuid(query, *except)));
        query.select(service::device::DeviceAccessService::integer(query, 1))
            .from("open_access_key")
            .where(predicate)
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (!rows.empty())
            service::common::fail(19003, "调用配置名称已存在", 409);
    }

    ruvia::Task<void> ensureWebhookNameAvailable(ruvia::Context& c, std::string_view accessKeyId,
                                                 std::string_view name,
                                                 std::optional<std::string> except) {
        ruvia::DbQuery query(c.pool());
        auto predicate = andAll(
            query,
            query.binary(query.column("access_key_id"), ruvia::DbBinaryOperator::kEqual,
                         uuid(query, accessKeyId)),
            query.binary(query.column("name"), ruvia::DbBinaryOperator::kEqual,
                         query.value(name)),
            query.unary(ruvia::DbUnaryOperator::kIsNull, query.column("deleted_at")));
        if (except)
            predicate = query.binary(
                predicate, ruvia::DbBinaryOperator::kAnd,
                query.binary(query.column("id"), ruvia::DbBinaryOperator::kNotEqual,
                             uuid(query, *except)));
        query.select(service::device::DeviceAccessService::integer(query, 1))
            .from("open_webhook")
            .where(predicate)
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (!rows.empty())
            service::common::fail(19003, "同一调用配置下的 Webhook 名称已存在", 409);
    }

    ruvia::Task<KeyState> requireKey(ruvia::Context& c, std::string_view id) {
        ruvia::DbQuery query(c.pool());
        query.select({query.column("name"), text(query, query.column("status")),
                      text(query, query.column("scopes")),
                      query.call("iot_utc_timestamp", {query.column("expires_at")} ),
                      query.column("remark")})
            .from("open_access_key")
            .where(andAll(query,
                          query.binary(query.column("id"), ruvia::DbBinaryOperator::kEqual,
                                       uuid(query, id)),
                          query.unary(ruvia::DbUnaryOperator::kIsNull,
                                      query.column("deleted_at"))))
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty())
            service::common::fail(19001, "调用配置不存在", 404);
        const auto& row = rows.front();
        KeyState state;
        state.name = std::string(row[0].value().value_or(std::string_view{}));
        state.status = std::string(row[1].value().value_or(std::string_view{}));
        state.scopes = parseStringArray(row[2].value().value_or(std::string_view{}));
        if (row[3].value().has_value())
            state.expiresAt = std::string(row[3].value().value_or(std::string_view{}));
        if (row[4].value().has_value())
            state.remark = std::string(row[4].value().value_or(std::string_view{}));
        ruvia::DbQuery devicesQuery(c.pool());
        devicesQuery
            .select(text(devicesQuery, devicesQuery.column("device_id")))
            .from("open_access_key_device")
            .where(devicesQuery.binary(devicesQuery.column("access_key_id"),
                                       ruvia::DbBinaryOperator::kEqual,
                                       uuid(devicesQuery, id)))
            .orderBy(devicesQuery.column("device_id"));
        const auto devices = co_await c.db().query(devicesQuery);
        for (const auto& device : devices)
            state.deviceIds.emplace_back(device[0].value().value_or(std::string_view{}));
        co_return state;
    }

    ruvia::Task<WebhookState> requireWebhook(ruvia::Context& c, std::string_view id) {
        ruvia::DbQuery query(c.pool());
        query.select({text(query, query.column("access_key_id")), query.column("name"),
                      query.column("url"), text(query, query.column("status")),
                      query.column("timeout_seconds"),
                       query.caseWhen(
                           {{query.binary(query.column("skip_tls_verify"),
                                          ruvia::DbBinaryOperator::kEqual,
                                          service::device::DeviceAccessService::boolean(query, true)),
                            text(query, "1")}},
                          text(query, "0")),
                      text(query, query.column("headers")),
                      text(query, query.column("event_types")), query.column("secret")})
            .from("open_webhook")
            .where(andAll(query,
                          query.binary(query.column("id"), ruvia::DbBinaryOperator::kEqual,
                                       uuid(query, id)),
                          query.unary(ruvia::DbUnaryOperator::kIsNull,
                                      query.column("deleted_at"))))
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty())
            service::common::fail(19001, "Webhook 不存在", 404);
        const auto& row = rows.front();
        WebhookState state;
        state.accessKeyId = std::string(row[0].value().value_or(std::string_view{}));
        state.name = std::string(row[1].value().value_or(std::string_view{}));
        state.url = std::string(row[2].value().value_or(std::string_view{}));
        state.status = std::string(row[3].value().value_or(std::string_view{}));
        state.timeout =
            service::common::parseInt64(std::optional<std::string_view>{row[4].value().value_or(std::string_view{})})
                .value_or(state.timeout);
        state.skipTlsVerify = row[5].value().value_or(std::string_view{}) == "1";
        state.headers = std::string(row[6].value().value_or(std::string_view{}));
        state.events = parseStringArray(row[7].value().value_or(std::string_view{}));
        if (row[8].value().has_value())
            state.secret = std::string(row[8].value().value_or(std::string_view{}));
        co_return state;
    }

    static ruvia::Task<void> requireSessionDevice(ruvia::Context& c, const AccessSession& session,
                                                  std::string_view deviceId) {
        service::common::requireUuid(19002, deviceId, "设备 ID 无效");
        if (!session.allowsDevice(deviceId))
            service::common::fail(19011, "AccessKey 无权访问该设备", 403);
        ruvia::DbQuery query(c.pool());
        query.select(service::device::DeviceAccessService::integer(query, 1))
            .from("device")
            .where(andAll(query,
                          query.binary(query.column("id"), ruvia::DbBinaryOperator::kEqual,
                                       uuid(query, deviceId)),
                          query.unary(ruvia::DbUnaryOperator::kIsNull,
                                      query.column("deleted_at"))))
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty())
            service::common::fail(19001, "设备不存在", 404);
    }

    static ruvia::DbQuery historyQuery(std::pmr::memory_resource* resource,
                                       std::string_view deviceId, std::string_view start,
                                       std::string_view end, std::int64_t pageSize,
                                       std::int64_t offset, std::int64_t page) {
        const auto timestamp = [](ruvia::DbQuery& query, std::string_view value) {
            return query.cast(query.value(value), ruvia::DbDataType::kTimestampTz);
        };
        const auto emptyObject = [](ruvia::DbQuery& query) {
            return query.cast(query.value("{}"), ruvia::DbDataType::kJsonb);
        };
        const auto emptyArray = [](ruvia::DbQuery& query) {
            return query.cast(query.value("[]"), ruvia::DbDataType::kJsonb);
        };

        ruvia::DbQuery deviceRef(resource);
        const auto deviceParams = deviceRef.column("protocol_params", "device");
        deviceRef
            .select({deviceRef.column("id", "device"),
                     deviceRef.alias(service::device::DeviceAccessService::jsonText(
                                         deviceRef, deviceParams, "device_code"),
                                     "code"),
                     deviceRef.column("name", "device")})
            .from("device", "device")
            .where(andAll(deviceRef,
                          deviceRef.binary(deviceRef.column("id", "device"),
                                          ruvia::DbBinaryOperator::kEqual,
                                          uuid(deviceRef, deviceId)),
                          deviceRef.unary(ruvia::DbUnaryOperator::kIsNull,
                                          deviceRef.column("deleted_at", "device"))));

        ruvia::DbQuery counted(resource);
        const auto countedData = counted.column("data", "record");
        counted
            .select(counted.aggregate("count", {counted.star()}))
            .from("device_data", "record")
            .where(andAll(counted,
                          counted.binary(counted.column("device_id", "record"),
                                         ruvia::DbBinaryOperator::kEqual,
                                         uuid(counted, deviceId)),
                          counted.binary(counted.column("report_time", "record"),
                                         ruvia::DbBinaryOperator::kGreaterEqual,
                                         timestamp(counted, start)),
                          counted.binary(counted.column("report_time", "record"),
                                         ruvia::DbBinaryOperator::kLessEqual,
                                         timestamp(counted, end)),
                          counted.binary(
                              counted.call("jsonb_typeof",
                                           {service::device::DeviceAccessService::jsonValue(
                                               counted, countedData, "values")}),
                              ruvia::DbBinaryOperator::kEqual,
                              text(counted, "object"))));

        ruvia::DbQuery filtered(resource);
        const auto filteredData = filtered.column("data", "record");
        filtered
            .select({filtered.column("id", "record"),
                     filtered.column("report_time", "record"), filteredData})
            .from("device_data", "record")
            .where(andAll(filtered,
                          filtered.binary(filtered.column("device_id", "record"),
                                          ruvia::DbBinaryOperator::kEqual,
                                          uuid(filtered, deviceId)),
                          filtered.binary(filtered.column("report_time", "record"),
                                          ruvia::DbBinaryOperator::kGreaterEqual,
                                          timestamp(filtered, start)),
                          filtered.binary(filtered.column("report_time", "record"),
                                          ruvia::DbBinaryOperator::kLessEqual,
                                          timestamp(filtered, end)),
                          filtered.binary(
                              filtered.call("jsonb_typeof",
                                            {service::device::DeviceAccessService::jsonValue(
                                                filtered, filteredData, "values")}),
                              ruvia::DbBinaryOperator::kEqual,
                              text(filtered, "object"))))
            .orderBy(filtered.column("report_time", "record"),
                     ruvia::DbOrderDirection::kDesc)
            .addOrderBy(filtered.column("id", "record"), ruvia::DbOrderDirection::kDesc)
            .limit(static_cast<std::uint64_t>(pageSize))
            .offset(static_cast<std::uint64_t>(offset));

        ruvia::DbQuery normalizedValues(resource);
        const auto normalizedPoint = normalizedValues.column("value", "point");
        const auto normalizedPointValue = service::device::DeviceAccessService::jsonValue(
            normalizedValues, normalizedPoint, "value");
        const auto pointObject = andAll(
            normalizedValues,
            normalizedValues.binary(normalizedValues.call("jsonb_typeof", {normalizedPoint}),
                                     ruvia::DbBinaryOperator::kEqual,
                                     text(normalizedValues, "object")),
            normalizedValues.binary(normalizedValues.call("jsonb_typeof", {normalizedPointValue}),
                                     ruvia::DbBinaryOperator::kEqual,
                                     text(normalizedValues, "boolean")));
        const auto pointBoolean = normalizedValues.cast(
            service::device::DeviceAccessService::jsonText(normalizedValues, normalizedPoint,
                                                            "value"),
            ruvia::DbDataType::kBoolean);
        const auto pointNumber = normalizedValues.caseWhen(
            {{pointBoolean,
              normalizedValues.cast(normalizedValues.value(1), ruvia::DbDataType::kInteger)}},
            normalizedValues.cast(normalizedValues.value(0), ruvia::DbDataType::kInteger));
        const auto pointPath = normalizedValues.cast(
            normalizedValues.array({textKey(normalizedValues, "value")} ),
            ruvia::DbTypeDefinition{.dataType = ruvia::DbDataType::kText, .array = true});
        const auto normalizedPointValueExpr = normalizedValues.caseWhen(
            {{pointObject,
              normalizedValues.call(
                  "jsonb_set",
                  {normalizedPoint, pointPath,
                   normalizedValues.call("to_jsonb", {pointNumber}),
                   normalizedValues.cast(normalizedValues.value(false),
                                         ruvia::DbDataType::kBoolean)})}},
            normalizedPoint);
        normalizedValues
            .select(normalizedValues.aggregate(
                "jsonb_object_agg",
                {normalizedValues.column("key", "point"), normalizedPointValueExpr}))
            .fromFunction(
                normalizedValues.call(
                    "jsonb_each",
                    {normalizedValues.coalesce(
                        {service::device::DeviceAccessService::jsonValue(
                             normalizedValues, normalizedValues.column("data", "filtered"),
                             "values"),
                         emptyObject(normalizedValues)})}),
                "point", {.lateral = true,
                           .columns = {{.name = "key"}, {.name = "value"}}});
        ruvia::DbQuery normalized(resource);
        normalized
            .select({normalized.star("filtered"),
                     normalized.alias(
                         normalized.coalesce({normalized.subquery(normalizedValues),
                        emptyObject(normalized)}),
                         "normalized_values")})
            .from(filtered, "filtered");

        ruvia::DbQuery items(resource);
        const auto point = items.column("value", "point");
        const auto pointValue = service::device::DeviceAccessService::jsonValue(
            items, point, "value");
        const auto normalizedData = items.column("data", "normalized");
        const auto itemPointBoolean = items.cast(
            service::device::DeviceAccessService::jsonText(items, point, "value"),
            ruvia::DbDataType::kBoolean);
        const auto itemPointNumber = items.caseWhen(
            {{itemPointBoolean, items.cast(items.value(1), ruvia::DbDataType::kInteger)}},
            items.cast(items.value(0), ruvia::DbDataType::kInteger));
        const auto itemPoint = items.call(
            "jsonb_build_object",
            {textKey(items, "id"), items.column("key", "point"),
             textKey(items, "name"),
             items.coalesce({service::device::DeviceAccessService::jsonText(
                                 items, point, "name"),
                             items.column("key", "point")}),
             textKey(items, "value"),
             items.caseWhen(
                 {{items.binary(items.call("jsonb_typeof", {pointValue}),
                                ruvia::DbBinaryOperator::kEqual, text(items, "boolean")),
                   items.call("to_jsonb", {itemPointNumber})}},
                 pointValue),
             textKey(items, "unit"),
             items.coalesce({service::device::DeviceAccessService::jsonText(
                                 items, point, "unit"),
                             text(items, "")}),
             textKey(items, "time"),
             items.call("iot_utc_timestamp", {items.column("report_time", "normalized")})});
        const std::array<ruvia::DbOrderTerm, 1> pointOrder{{
            ruvia::DbOrderTerm{items.column("key", "point"), ruvia::DbOrderDirection::kAsc,
                               ruvia::DbNullsOrder::kDefault}}};
        const auto points = items.coalesce(
            {items.filter(
                 items.aggregate("jsonb_agg", {itemPoint}, false, pointOrder),
                 items.unary(ruvia::DbUnaryOperator::kIsNotNull,
                             items.column("key", "point"))),
             emptyArray(items)});
        const auto item = items.call(
            "jsonb_build_object",
            {textKey(items, "device"),
             items.call("jsonb_build_object",
                        {textKey(items, "id"),
                         items.cast(items.column("id", "device_ref"),
                                    ruvia::DbDataType::kUuid),
                         textKey(items, "code"), items.column("code", "device_ref"),
                         textKey(items, "name"), items.column("name", "device_ref")} ),
             textKey(items, "points"), points});
        items
            .select({items.column("report_time", "normalized"),
                     items.column("id", "normalized"), items.alias(item, "item")})
            .from(normalized, "normalized")
            .join(ruvia::DbJoinType::kCross, "device_ref", {}, "device_ref")
            .joinFunction(
                ruvia::DbJoinType::kLeft,
                items.call("jsonb_each",
                           {service::device::DeviceAccessService::jsonValue(
                               items, normalizedData, "values")}),
                items.binary(
                    items.coalesce({service::device::DeviceAccessService::jsonText(
                                        items, point, "type"),
                                    text(items, "")}),
                    ruvia::DbBinaryOperator::kNotEqual, text(items, "JPEG")),
                "point", {.lateral = true,
                           .columns = {{.name = "key"}, {.name = "value"}}})
            .groupBy({items.column("report_time", "normalized"),
                      items.column("id", "normalized"),
                      items.column("id", "device_ref"),
                      items.column("code", "device_ref"),
                      items.column("name", "device_ref")});

        ruvia::DbQuery query(resource);
        query.with("device_ref", deviceRef)
            .with("counted", counted)
            .with("filtered", filtered)
            .with("normalized", normalized)
            .with("items", items);
        const auto total = query.coalesce(
            {query.subquery(counted), query.cast(query.value(0), ruvia::DbDataType::kBigInt)});
        const std::array<ruvia::DbOrderTerm, 2> historyOrder{{
            ruvia::DbOrderTerm{query.column("report_time", "items"),
                               ruvia::DbOrderDirection::kDesc, ruvia::DbNullsOrder::kDefault},
            ruvia::DbOrderTerm{query.column("id", "items"), ruvia::DbOrderDirection::kDesc,
                               ruvia::DbNullsOrder::kDefault}}};
        const auto list = query.coalesce(
            {query.aggregate(
                 "jsonb_agg", {query.column("item", "items")}, false,
                 historyOrder),
             emptyArray(query)});
        const auto totalPages = query.cast(
            query.call("ceil",
                       {query.binary(query.cast(total, ruvia::DbDataType::kNumeric),
                                     ruvia::DbBinaryOperator::kDivide,
                                     query.cast(query.value(pageSize),
                                                ruvia::DbDataType::kNumeric))}),
            ruvia::DbDataType::kBigInt);
        query.select(query.cast(
            query.call("jsonb_build_object",
                       {textKey(query, "list"), list, textKey(query, "total"), total,
                        textKey(query, "page"),
                        query.cast(query.value(page), ruvia::DbDataType::kBigInt),
                        textKey(query, "pageSize"),
                        query.cast(query.value(pageSize), ruvia::DbDataType::kBigInt),
                        textKey(query, "totalPages"), totalPages}),
            ruvia::DbDataType::kText))
            .from("items");
        return query;
    }
};

inline AccessService& accessService() { return AccessService::instance(); }

} // namespace service::access
