#pragma once

#include <memory>
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory_resource>
#include <openssl/rand.h>
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

#include "service/common/http.h"
#include "service/common/message.h"
#include "service/common/timestamp.h"
#include "service/common/uuid.h"
#include "service/middleware/rpc.h"
#include "service/modules/device/device.service.h"
#include "service/modules/open_access/open_access.entity.h"
#include "service/modules/open_access/open_access.types.h"
#include "service/modules/system/outbox/outbox.service.h"
#include "service/utils/crypto.h"
#include "service/utils/json.h"
#include "service/utils/number.h"
#include "service/utils/text.h"

namespace service::access {

inline std::string generateAccessKey() {
    std::array<unsigned char, 24> random{};
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1) {
        throw std::runtime_error("access-key random generation failed");
    }
    return "ak_" + service::utils::hexEncode(random.data(), random.size());
}

class AccessService final {
  public:
    static AccessService& instance() {
        static thread_local AccessService service;
        return service;
    }

    template <typename Context>
    ruvia::Task<std::string> listKeys(Context& c) {
        ruvia::DbQuery listed(c.pool());
        const auto keyId = listed.column(service::open_access::entities::OpenAccessKeyEntity::columnName<"id">(), "key");
        const auto bindingId = listed.column(service::open_access::entities::OpenAccessKeyDeviceEntity::columnName<"device_id">(), "binding");
        const auto item = listed.call(
            "jsonb_build_object",
            { textKey(listed, "id"), text(listed, keyId), textKey(listed, "name"), listed.column(service::open_access::entities::OpenAccessKeyEntity::columnName<"name">(), "key"), textKey(listed, "accessKeyPrefix"), listed.column(service::open_access::entities::OpenAccessKeyEntity::columnName<"access_key_prefix">(), "key"), textKey(listed, "status"), listed.column(service::open_access::entities::OpenAccessKeyEntity::columnName<"status">(), "key"), textKey(listed, "scopes"), listed.column(service::open_access::entities::OpenAccessKeyEntity::columnName<"scopes">(), "key"), textKey(listed, "expiresAt"), listed.call("iot_utc_timestamp", { listed.column(service::open_access::entities::OpenAccessKeyEntity::columnName<"expires_at">(), "key") }), textKey(listed, "lastUsedAt"), listed.call("iot_utc_timestamp", { listed.column(service::open_access::entities::OpenAccessKeyEntity::columnName<"last_used_at">(), "key") }), textKey(listed, "lastUsedIp"), listed.column(service::open_access::entities::OpenAccessKeyEntity::columnName<"last_used_ip">(), "key"), textKey(listed, "remark"), listed.column(service::open_access::entities::OpenAccessKeyEntity::columnName<"remark">(), "key"), textKey(listed, "createdAt"), listed.call("iot_utc_timestamp", { listed.column(service::open_access::entities::OpenAccessKeyEntity::columnName<"created_at">(), "key") }), textKey(listed, "updatedAt"), listed.call("iot_utc_timestamp", { listed.column(service::open_access::entities::OpenAccessKeyEntity::columnName<"updated_at">(), "key") }), textKey(listed, "webhookCount"), listed.aggregate("count", { listed.column(service::open_access::entities::OpenWebhookEntity::columnName<"id">(), "webhook") }, true), textKey(listed, "deviceIds"), listed.coalesce({ listed.filter(listed.aggregate("jsonb_agg", { bindingId }, true), listed.unary(ruvia::DbUnaryOperator::kIsNotNull, bindingId)), emptyJson(listed) }) }
        );
        listed.select({ listed.alias(item, "item"), listed.column(service::open_access::entities::OpenAccessKeyEntity::columnName<"created_at">(), "key") })
            .from(service::open_access::entities::OpenAccessKeyEntity::tableName(), "key")
            .join(ruvia::DbJoinType::kLeft, service::open_access::entities::OpenAccessKeyDeviceEntity::tableName(), listed.binary(listed.column(service::open_access::entities::OpenAccessKeyDeviceEntity::columnName<"access_key_id">(), "binding"), ruvia::DbBinaryOperator::kEqual, keyId), "binding")
            .join(ruvia::DbJoinType::kLeft, service::open_access::entities::OpenWebhookEntity::tableName(), andAll(listed, listed.binary(listed.column(service::open_access::entities::OpenWebhookEntity::columnName<"access_key_id">(), "webhook"), ruvia::DbBinaryOperator::kEqual, keyId), listed.unary(ruvia::DbUnaryOperator::kIsNull, listed.column(service::open_access::entities::OpenWebhookEntity::columnName<"deleted_at">(), "webhook"))), "webhook")
            .where(listed.unary(ruvia::DbUnaryOperator::kIsNull, listed.column(service::open_access::entities::OpenAccessKeyEntity::columnName<"deleted_at">(), "key")))
            .groupBy({ keyId });
        ruvia::DbQuery query(c.pool());
        const std::array<ruvia::DbOrderTerm, 1> keyOrder{ { ruvia::DbOrderTerm{ query.column("created_at", "listed"),
                                                                                ruvia::DbOrderDirection::kDesc,
                                                                                ruvia::DbNullsOrder::kDefault } } };
        query.select(text(query, query.coalesce({ query.aggregate("jsonb_agg", { query.column("item", "listed") }, false, keyOrder), emptyJson(query) })))
            .from(listed, "listed");
        co_return firstJson(co_await c.db().query(query));
    }

    template <typename Context>
    ruvia::Task<std::string> deviceOptions(Context& c) {
        const auto actor = co_await service::device::deviceAccessService().actor(c, c.userId);
        ruvia::DbQuery query(c.pool());
        service::device::DeviceAccessService::addScopedDevicesCtes(query, actor);
        const auto params = query.column("protocol_params");
        const auto key = service::device::DeviceAccessService::textKey(query, "remote_control");
        const auto hasKey = query.binary(params, ruvia::DbBinaryOperator::kJsonHasKey, key);
        const auto type = query.call(
            "jsonb_typeof",
            { service::device::DeviceAccessService::jsonValue(query, params, "remote_control") }
        );
        const auto remoteText = service::device::DeviceAccessService::jsonText(
            query,
            params,
            "remote_control"
        );
        const auto booleanRemote = query.binary(remoteText, ruvia::DbBinaryOperator::kEqual, query.value("true"));
        const auto stringRemote = query.binary(
            query.call("lower", { query.call("btrim", { remoteText }) }),
            ruvia::DbBinaryOperator::kEqual,
            query.value("true")
        );
        const auto safeRemote = query.caseWhen(
            { { query.unary(ruvia::DbUnaryOperator::kIsNull, params),
                service::device::DeviceAccessService::boolean(query, true) },
              { query.unary(ruvia::DbUnaryOperator::kNot, hasKey),
                service::device::DeviceAccessService::boolean(query, true) },
              { query.binary(type, ruvia::DbBinaryOperator::kEqual, query.value("boolean")),
                booleanRemote },
              { query.binary(type, ruvia::DbBinaryOperator::kEqual, query.value("string")),
                stringRemote } },
            service::device::DeviceAccessService::boolean(query, false)
        );
        const auto canCommand = query.binary(
            query.binary(query.column("access_rank"), ruvia::DbBinaryOperator::kGreaterEqual, service::device::DeviceAccessService::integer(query, 2)),
            ruvia::DbBinaryOperator::kAnd,
            safeRemote
        );
        const auto item = query.call(
            "jsonb_build_object",
            { textKey(query, "id"), service::device::DeviceAccessService::text(query, query.column("id")), textKey(query, "name"), query.column("name"), textKey(query, "deviceCode"), service::device::DeviceAccessService::jsonText(query, params, "device_code"), textKey(query, "canCommand"), canCommand }
        );
        const std::array<ruvia::DbOrderTerm, 2> deviceOrder{ { ruvia::DbOrderTerm{ query.column("name"), ruvia::DbOrderDirection::kAsc, ruvia::DbNullsOrder::kDefault },
                                                               ruvia::DbOrderTerm{ query.column("id"), ruvia::DbOrderDirection::kAsc, ruvia::DbNullsOrder::kDefault } } };
        query.select(text(query, query.coalesce({ query.aggregate("jsonb_agg", { item }, false, deviceOrder), emptyJson(query) })))
            .from("scoped_device")
            .where(query.binary(query.column("access_rank"), ruvia::DbBinaryOperator::kGreater, service::device::DeviceAccessService::integer(query, 0)));
        co_return firstJson(co_await c.db().query(query));
    }

    template <typename Context>
    ruvia::Task<std::string> createKey(Context& c, const AccessKeyInput& payload) {
        const auto& name = *payload.name;
        const auto& status = *payload.status;
        const auto& scopes = *payload.scopes;
        const auto& devices = *payload.deviceIds;
        co_await ensureDevicesAccessible(c, devices, scopes.contains(std::string(kScopeCommand)));
        const auto& expiresAt = payload.expiresAt;
        const auto& remark = payload.remark;
        co_await ensureKeyNameAvailable(c, name, std::nullopt);

        const auto id = c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next();
        const auto rawKey = service::access::generateAccessKey();
        const auto prefix = rawKey.substr(0, 14);
        const auto keyHash = service::utils::sha256(rawKey);
        const auto scopeJson = stringArrayJson(scopes);
        const auto expiresAtValue = expiresAt.value_or("");
        const auto remarkValue = remark.value_or("");
        auto transaction = co_await c.db().beginTransaction();
        ruvia::DbQuery insert(c.pool());
        insert.insertInto(service::open_access::entities::OpenAccessKeyEntity::tableName(), { "id", "name", "access_key_prefix", "access_key_hash", "status", "scopes", "expires_at", "remark", "created_by" })
            .values({ uuid(insert, id), insert.value(name), insert.value(prefix), insert.value(keyHash), insert.value(status), insert.cast(insert.value(scopeJson), ruvia::DbDataType::kJsonb), insert.cast(insert.nullIf(insert.value(expiresAtValue), insert.value("")), ruvia::DbDataType::kTimestampTz), insert.nullIf(insert.value(remarkValue), insert.value("")), uuid(insert, c.userId) });
        (void)co_await transaction.execute(insert);
        co_await replaceDevices(transaction, id, devices);
        co_await service::system::OutboxService::enqueueConfigEvent(transaction, "access_key", "created", id, c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next());
        co_await transaction.commit();
        co_await refreshProjection(c);

        co_return "{\"id\":" + service::utils::jsonQuoted(id) + ",\"name\":" + service::utils::jsonQuoted(name) +
            ",\"status\":" + service::utils::jsonQuoted(status) + ",\"scopes\":" + scopeJson +
            ",\"expiresAt\":" + (expiresAt ? service::utils::jsonQuoted(*expiresAt) : std::string("null")) +
            ",\"deviceIds\":" + stringArrayJson(devices) + ",\"accessKey\":" + service::utils::jsonQuoted(rawKey) +
            ",\"accessKeyPrefix\":" + service::utils::jsonQuoted(prefix) + "}";
    }

    template <typename Context>
    ruvia::Task<void> updateKey(Context& c, std::string_view id, const AccessKeyInput& payload) {
        service::common::requireUuid(19002, id, "调用配置 ID 无效");
        const auto existing = co_await requireKey(c, id);
        const auto name = payload.name.value_or(existing.name);
        const auto status = payload.status.value_or(existing.status);
        const auto scopes = payload.scopes.value_or(existing.scopes);
        const auto devices = payload.deviceIds.value_or(existing.deviceIds);
        co_await ensureDevicesAccessible(c, devices, scopes.contains(std::string(kScopeCommand)));
        co_await ensureKeyNameAvailable(c, name, std::string(id));
        const auto expiresAt = payload.expiresAtPresent ? payload.expiresAt : existing.expiresAt;
        const auto remark = payload.remarkPresent ? payload.remark : existing.remark;
        const auto scopeJson = stringArrayJson(scopes);
        const auto expiresAtValue = expiresAt.value_or("");
        const auto remarkValue = remark.value_or("");
        auto transaction = co_await c.db().beginTransaction();
        ruvia::DbQuery update(c.pool());
        update.update(service::open_access::entities::OpenAccessKeyEntity::tableName())
            .set(service::open_access::entities::OpenAccessKeyEntity::columnName<"name">(), update.value(name))
            .set(service::open_access::entities::OpenAccessKeyEntity::columnName<"status">(), update.value(status))
            .set(service::open_access::entities::OpenAccessKeyEntity::columnName<"scopes">(), update.cast(update.value(scopeJson), ruvia::DbDataType::kJsonb))
            .set(service::open_access::entities::OpenAccessKeyEntity::columnName<"expires_at">(), update.cast(update.nullIf(update.value(expiresAtValue), update.value("")), ruvia::DbDataType::kTimestampTz))
            .set(service::open_access::entities::OpenAccessKeyEntity::columnName<"remark">(), update.nullIf(update.value(remarkValue), update.value("")))
            .set(service::open_access::entities::OpenAccessKeyEntity::columnName<"updated_at">(), update.call("now"))
            .where(andAll(update, update.binary(update.column(service::open_access::entities::OpenAccessKeyEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual, uuid(update, id)), update.unary(ruvia::DbUnaryOperator::kIsNull, update.column(service::open_access::entities::OpenAccessKeyEntity::columnName<"deleted_at">()))));
        (void)co_await transaction.execute(update);
        if (payload.deviceIds) {
            co_await replaceDevices(transaction, id, devices);
        }
        co_await service::system::OutboxService::enqueueConfigEvent(transaction, "access_key", "updated", id, c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next());
        co_await transaction.commit();
        co_await refreshProjection(c);
    }

    template <typename Context>
    ruvia::Task<std::string> rotateKey(Context& c, std::string_view id) {
        service::common::requireUuid(19002, id, "调用配置 ID 无效");
        const auto existing = co_await requireKey(c, id);
        const auto rawKey = service::access::generateAccessKey();
        const auto prefix = rawKey.substr(0, 14);
        const auto keyHash = service::utils::sha256(rawKey);
        auto transaction = co_await c.db().beginTransaction();
        ruvia::DbQuery update(c.pool());
        update.update(service::open_access::entities::OpenAccessKeyEntity::tableName())
            .set(service::open_access::entities::OpenAccessKeyEntity::columnName<"access_key_hash">(), update.value(keyHash))
            .set(service::open_access::entities::OpenAccessKeyEntity::columnName<"access_key_prefix">(), update.value(prefix))
            .set(service::open_access::entities::OpenAccessKeyEntity::columnName<"updated_at">(), update.call("now"))
            .where(andAll(update, update.binary(update.column(service::open_access::entities::OpenAccessKeyEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual, uuid(update, id)), update.unary(ruvia::DbUnaryOperator::kIsNull, update.column(service::open_access::entities::OpenAccessKeyEntity::columnName<"deleted_at">()))));
        (void)co_await transaction.execute(update);
        co_await service::system::OutboxService::enqueueConfigEvent(transaction, "access_key", "rotated", id, c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next());
        co_await transaction.commit();
        co_await refreshProjection(c);
        co_return "{\"id\":" + service::utils::jsonQuoted(id) + ",\"name\":" + service::utils::jsonQuoted(existing.name) +
            ",\"accessKey\":" + service::utils::jsonQuoted(rawKey) + ",\"accessKeyPrefix\":" + service::utils::jsonQuoted(prefix) +
            "}";
    }

    template <typename Context>
    ruvia::Task<void> removeKey(Context& c, std::string_view id) {
        service::common::requireUuid(19002, id, "调用配置 ID 无效");
        (void)co_await requireKey(c, id);
        auto transaction = co_await c.db().beginTransaction();
        ruvia::DbQuery webhookUpdate(c.pool());
        webhookUpdate.update(service::open_access::entities::OpenWebhookEntity::tableName())
            .set(service::open_access::entities::OpenWebhookEntity::columnName<"deleted_at">(), webhookUpdate.call("now"))
            .set(service::open_access::entities::OpenWebhookEntity::columnName<"updated_at">(), webhookUpdate.call("now"))
            .where(andAll(webhookUpdate, webhookUpdate.binary(webhookUpdate.column(service::open_access::entities::OpenWebhookEntity::columnName<"access_key_id">()), ruvia::DbBinaryOperator::kEqual, uuid(webhookUpdate, id)), webhookUpdate.unary(ruvia::DbUnaryOperator::kIsNull, webhookUpdate.column(service::open_access::entities::OpenWebhookEntity::columnName<"deleted_at">()))));
        (void)co_await transaction.execute(webhookUpdate);
        ruvia::DbQuery keyUpdate(c.pool());
        keyUpdate.update(service::open_access::entities::OpenAccessKeyEntity::tableName())
            .set(service::open_access::entities::OpenAccessKeyEntity::columnName<"deleted_at">(), keyUpdate.call("now"))
            .set(service::open_access::entities::OpenAccessKeyEntity::columnName<"updated_at">(), keyUpdate.call("now"))
            .where(andAll(keyUpdate, keyUpdate.binary(keyUpdate.column(service::open_access::entities::OpenAccessKeyEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual, uuid(keyUpdate, id)), keyUpdate.unary(ruvia::DbUnaryOperator::kIsNull, keyUpdate.column(service::open_access::entities::OpenAccessKeyEntity::columnName<"deleted_at">()))));
        (void)co_await transaction.execute(keyUpdate);
        co_await service::system::OutboxService::enqueueConfigEvent(transaction, "access_key", "deleted", id, c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next());
        co_await transaction.commit();
        co_await refreshProjection(c);
    }

    template <typename Context>
    ruvia::Task<std::string> listWebhooks(Context& c, const WebhookQuery& filters) {
        ruvia::DbQuery listed(c.pool());
        const auto webhookId = listed.column(service::open_access::entities::OpenWebhookEntity::columnName<"id">(), "webhook");
        const auto bindingDevice = listed.column(service::open_access::entities::OpenAccessKeyDeviceEntity::columnName<"device_id">(), "binding");
        const auto item = listed.call(
            "jsonb_build_object",
            { textKey(listed, "id"), text(listed, webhookId), textKey(listed, "accessKeyId"), text(listed, listed.column(service::open_access::entities::OpenWebhookEntity::columnName<"access_key_id">(), "webhook")), textKey(listed, "accessKeyName"), listed.column(service::open_access::entities::OpenAccessKeyEntity::columnName<"name">(), "key"), textKey(listed, "name"), listed.column(service::open_access::entities::OpenWebhookEntity::columnName<"name">(), "webhook"), textKey(listed, "url"), listed.column(service::open_access::entities::OpenWebhookEntity::columnName<"url">(), "webhook"), textKey(listed, "status"), listed.column(service::open_access::entities::OpenWebhookEntity::columnName<"status">(), "webhook"), textKey(listed, "timeoutSeconds"), listed.column(service::open_access::entities::OpenWebhookEntity::columnName<"timeout_seconds">(), "webhook"), textKey(listed, "skipTlsVerify"), listed.column(service::open_access::entities::OpenWebhookEntity::columnName<"skip_tls_verify">(), "webhook"), textKey(listed, "headers"), listed.column(service::open_access::entities::OpenWebhookEntity::columnName<"headers">(), "webhook"), textKey(listed, "eventTypes"), listed.column(service::open_access::entities::OpenWebhookEntity::columnName<"event_types">(), "webhook"), textKey(listed, "deviceIds"), listed.coalesce({ listed.filter(listed.aggregate("jsonb_agg", { bindingDevice }), listed.unary(ruvia::DbUnaryOperator::kIsNotNull, bindingDevice)), emptyJson(listed) }), textKey(listed, "hasSecret"), listed.unary(ruvia::DbUnaryOperator::kIsNotNull, listed.column(service::open_access::entities::OpenWebhookEntity::columnName<"secret">(), "webhook")), textKey(listed, "lastTriggeredAt"), listed.call("iot_utc_timestamp", { listed.column(service::open_access::entities::OpenWebhookEntity::columnName<"last_triggered_at">(), "webhook") }), textKey(listed, "lastSuccessAt"), listed.call("iot_utc_timestamp", { listed.column(service::open_access::entities::OpenWebhookEntity::columnName<"last_success_at">(), "webhook") }), textKey(listed, "lastFailureAt"), listed.call("iot_utc_timestamp", { listed.column(service::open_access::entities::OpenWebhookEntity::columnName<"last_failure_at">(), "webhook") }), textKey(listed, "lastHttpStatus"), listed.column(service::open_access::entities::OpenWebhookEntity::columnName<"last_http_status">(), "webhook"), textKey(listed, "lastError"), listed.column(service::open_access::entities::OpenWebhookEntity::columnName<"last_error">(), "webhook"), textKey(listed, "createdAt"), listed.call("iot_utc_timestamp", { listed.column(service::open_access::entities::OpenWebhookEntity::columnName<"created_at">(), "webhook") }), textKey(listed, "updatedAt"), listed.call("iot_utc_timestamp", { listed.column(service::open_access::entities::OpenWebhookEntity::columnName<"updated_at">(), "webhook") }) }
        );
        listed.select({ listed.alias(item, "item"), listed.column(service::open_access::entities::OpenWebhookEntity::columnName<"created_at">(), "webhook") })
            .from(service::open_access::entities::OpenWebhookEntity::tableName(), "webhook")
            .join(ruvia::DbJoinType::kInner, service::open_access::entities::OpenAccessKeyEntity::tableName(), andAll(listed, listed.binary(listed.column(service::open_access::entities::OpenAccessKeyEntity::columnName<"id">(), "key"), ruvia::DbBinaryOperator::kEqual, listed.column(service::open_access::entities::OpenWebhookEntity::columnName<"access_key_id">(), "webhook")), listed.unary(ruvia::DbUnaryOperator::kIsNull, listed.column(service::open_access::entities::OpenAccessKeyEntity::columnName<"deleted_at">(), "key"))), "key")
            .join(ruvia::DbJoinType::kLeft, service::open_access::entities::OpenAccessKeyDeviceEntity::tableName(), listed.binary(listed.column(service::open_access::entities::OpenAccessKeyDeviceEntity::columnName<"access_key_id">(), "binding"), ruvia::DbBinaryOperator::kEqual, listed.column(service::open_access::entities::OpenAccessKeyEntity::columnName<"id">(), "key")), "binding")
            .where(listed.unary(ruvia::DbUnaryOperator::kIsNull, listed.column(service::open_access::entities::OpenWebhookEntity::columnName<"deleted_at">(), "webhook")))
            .groupBy({ webhookId, listed.column(service::open_access::entities::OpenAccessKeyEntity::columnName<"id">(), "key") });
        if (const auto& key = filters.accessKeyId; key && !key->empty()) {
            service::common::requireUuid(19002, *key, "调用配置 ID 无效");
            listed.andWhere(listed.binary(listed.column(service::open_access::entities::OpenWebhookEntity::columnName<"access_key_id">(), "webhook"), ruvia::DbBinaryOperator::kEqual, uuid(listed, *key)));
        }
        ruvia::DbQuery query(c.pool());
        const std::array<ruvia::DbOrderTerm, 1> webhookOrder{ { ruvia::DbOrderTerm{ query.column("created_at", "listed"),
                                                                                    ruvia::DbOrderDirection::kDesc,
                                                                                    ruvia::DbNullsOrder::kDefault } } };
        query.select(text(query, query.coalesce({ query.aggregate("jsonb_agg", { query.column("item", "listed") }, false, webhookOrder), emptyJson(query) })))
            .from(listed, "listed");
        co_return firstJson(co_await c.db().query(query));
    }

    template <typename Context>
    ruvia::Task<std::string> createWebhook(Context& c, const WebhookInput& payload) {
        const auto& accessKeyId = *payload.accessKeyId;
        (void)co_await requireKey(c, accessKeyId);
        const auto& name = *payload.name;
        const auto& url = *payload.url;
        const auto& status = *payload.status;
        const auto timeout = *payload.timeoutSeconds;
        const auto skipTlsVerify = *payload.skipTlsVerify;
        const auto& headers = *payload.headers;
        const auto& events = *payload.eventTypes;
        const auto& secret = payload.secret;
        co_await ensureWebhookNameAvailable(c, accessKeyId, name, std::nullopt);
        const auto id = c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next();
        const auto eventJson = stringArrayJson(events);
        const auto secretValue = secret.value_or("");
        auto transaction = co_await c.db().beginTransaction();
        ruvia::DbQuery insert(c.pool());
        insert.insertInto(service::open_access::entities::OpenWebhookEntity::tableName(), { "id", "access_key_id", "name", "url", "status", "timeout_seconds", "skip_tls_verify", "headers", "event_types", "secret" })
            .values({ uuid(insert, id), uuid(insert, accessKeyId), insert.value(name), insert.value(url), insert.value(status), insert.value(timeout), insert.value(skipTlsVerify), insert.cast(insert.value(headers), ruvia::DbDataType::kJsonb), insert.cast(insert.value(eventJson), ruvia::DbDataType::kJsonb), insert.nullIf(insert.value(secretValue), insert.value("")) });
        (void)co_await transaction.execute(insert);
        co_await service::system::OutboxService::enqueueConfigEvent(transaction, "webhook", "created", id, c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next());
        co_await transaction.commit();
        co_return "{\"id\":" + service::utils::jsonQuoted(id) + ",\"accessKeyId\":" + service::utils::jsonQuoted(accessKeyId) +
            ",\"name\":" + service::utils::jsonQuoted(name) + ",\"url\":" + service::utils::jsonQuoted(url) +
            ",\"status\":" + service::utils::jsonQuoted(status) + ",\"timeoutSeconds\":" + std::to_string(timeout) +
            ",\"skipTlsVerify\":" + (skipTlsVerify ? "true" : "false") +
            ",\"headers\":" + headers + ",\"eventTypes\":" + stringArrayJson(events) +
            ",\"hasSecret\":" + (secret && !secret->empty() ? "true" : "false") + "}";
    }

    template <typename Context>
    ruvia::Task<void> updateWebhook(Context& c, std::string_view id, const WebhookInput& payload) {
        service::common::requireUuid(19002, id, "Webhook ID 无效");
        const auto existing = co_await requireWebhook(c, id);
        const auto accessKeyId = payload.accessKeyId.value_or(existing.accessKeyId);
        (void)co_await requireKey(c, accessKeyId);
        const auto name = payload.name.value_or(existing.name);
        const auto url = payload.url.value_or(existing.url);
        const auto status = payload.status.value_or(existing.status);
        const auto timeout = payload.timeoutSeconds.value_or(existing.timeout);
        const auto skipTlsVerify = payload.skipTlsVerify.value_or(existing.skipTlsVerify);
        const auto headers = payload.headers.value_or(existing.headers);
        const auto events = payload.eventTypes.value_or(existing.events);
        const auto secret = payload.secretPresent ? payload.secret : existing.secret;
        co_await ensureWebhookNameAvailable(c, accessKeyId, name, std::string(id));
        const auto eventJson = stringArrayJson(events);
        const auto secretValue = secret.value_or("");
        auto transaction = co_await c.db().beginTransaction();
        ruvia::DbQuery update(c.pool());
        update.update(service::open_access::entities::OpenWebhookEntity::tableName())
            .set(service::open_access::entities::OpenWebhookEntity::columnName<"access_key_id">(), uuid(update, accessKeyId))
            .set(service::open_access::entities::OpenWebhookEntity::columnName<"name">(), update.value(name))
            .set(service::open_access::entities::OpenWebhookEntity::columnName<"url">(), update.value(url))
            .set(service::open_access::entities::OpenWebhookEntity::columnName<"status">(), update.value(status))
            .set(service::open_access::entities::OpenWebhookEntity::columnName<"timeout_seconds">(), update.value(timeout))
            .set(service::open_access::entities::OpenWebhookEntity::columnName<"skip_tls_verify">(), update.value(skipTlsVerify))
            .set(service::open_access::entities::OpenWebhookEntity::columnName<"headers">(), update.cast(update.value(headers), ruvia::DbDataType::kJsonb))
            .set(service::open_access::entities::OpenWebhookEntity::columnName<"event_types">(), update.cast(update.value(eventJson), ruvia::DbDataType::kJsonb))
            .set(service::open_access::entities::OpenWebhookEntity::columnName<"secret">(), update.nullIf(update.value(secretValue), update.value("")))
            .set(service::open_access::entities::OpenWebhookEntity::columnName<"updated_at">(), update.call("now"))
            .where(andAll(update, update.binary(update.column(service::open_access::entities::OpenWebhookEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual, uuid(update, id)), update.unary(ruvia::DbUnaryOperator::kIsNull, update.column(service::open_access::entities::OpenWebhookEntity::columnName<"deleted_at">()))));
        (void)co_await transaction.execute(update);
        co_await service::system::OutboxService::enqueueConfigEvent(transaction, "webhook", "updated", id, c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next());
        co_await transaction.commit();
    }

    template <typename Context>
    ruvia::Task<void> removeWebhook(Context& c, std::string_view id) {
        service::common::requireUuid(19002, id, "Webhook ID 无效");
        (void)co_await requireWebhook(c, id);
        auto transaction = co_await c.db().beginTransaction();
        ruvia::DbQuery update(c.pool());
        update.update(service::open_access::entities::OpenWebhookEntity::tableName())
            .set(service::open_access::entities::OpenWebhookEntity::columnName<"deleted_at">(), update.call("now"))
            .set(service::open_access::entities::OpenWebhookEntity::columnName<"updated_at">(), update.call("now"))
            .where(andAll(update, update.binary(update.column(service::open_access::entities::OpenWebhookEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual, uuid(update, id)), update.unary(ruvia::DbUnaryOperator::kIsNull, update.column(service::open_access::entities::OpenWebhookEntity::columnName<"deleted_at">()))));
        (void)co_await transaction.execute(update);
        co_await service::system::OutboxService::enqueueConfigEvent(transaction, "webhook", "deleted", id, c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next());
        co_await transaction.commit();
    }

    template <typename Context>
    ruvia::Task<std::string> listLogs(Context& c, const AccessLogQuery& filters) {
        const service::common::Page pagination{ filters.page, filters.pageSize, (filters.page - 1) * filters.pageSize };
        const auto& accessKeyId = filters.accessKeyId;
        const auto& webhookId = filters.webhookId;
        const auto& deviceId = filters.deviceId;
        const auto& direction = filters.direction;
        const auto& action = filters.action;
        const auto& status = filters.status;
        const auto& eventType = filters.eventType;
        const auto applyFilters = [&](ruvia::DbQuery& query, std::string_view alias) {
            auto predicate = service::device::DeviceAccessService::boolean(query, true);
            const auto addUuid = [&](std::string_view value, std::string_view column) {
                if (!value.empty()) {
                    predicate = query.binary(
                        predicate,
                        ruvia::DbBinaryOperator::kAnd,
                        query.binary(query.column(column, alias), ruvia::DbBinaryOperator::kEqual, uuid(query, value))
                    );
                }
            };
            const auto addText = [&](const std::optional<std::string_view>& value,
                                     std::string_view column) {
                if (value && !value->empty()) {
                    predicate = query.binary(
                        predicate,
                        ruvia::DbBinaryOperator::kAnd,
                        query.binary(query.column(column, alias), ruvia::DbBinaryOperator::kEqual, query.value(*value))
                    );
                }
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
        counted.select(counted.aggregate("count", { counted.star() }))
            .from(service::open_access::entities::OpenAccessLogEntity::tableName(), "log");
        applyFilters(counted, "log");
        ruvia::DbQuery pageRows(c.pool());
        pageRows.select(pageRows.star("log")).from(service::open_access::entities::OpenAccessLogEntity::tableName(), "log");
        applyFilters(pageRows, "log");
        pageRows.orderBy(pageRows.column(service::open_access::entities::OpenAccessLogEntity::columnName<"created_at">(), "log"), ruvia::DbOrderDirection::kDesc)
            .addOrderBy(pageRows.column(service::open_access::entities::OpenAccessLogEntity::columnName<"id">(), "log"), ruvia::DbOrderDirection::kDesc)
            .limit(static_cast<std::uint64_t>(pagination.pageSize))
            .offset(static_cast<std::uint64_t>(pagination.offset));
        ruvia::DbQuery page(c.pool());
        page.select({ page.star("page_rows"), page.alias(page.column(service::open_access::entities::OpenAccessKeyEntity::columnName<"name">(), "key"), "access_key_name"), page.alias(page.column(service::open_access::entities::OpenWebhookEntity::columnName<"name">(), "webhook"), "webhook_name") })
            .from(pageRows, "page_rows")
            .join(ruvia::DbJoinType::kLeft, service::open_access::entities::OpenAccessKeyEntity::tableName(), page.binary(page.column(service::open_access::entities::OpenAccessKeyEntity::columnName<"id">(), "key"), ruvia::DbBinaryOperator::kEqual, page.column("access_key_id", "page_rows")), "key")
            .join(ruvia::DbJoinType::kLeft, service::open_access::entities::OpenWebhookEntity::tableName(), page.binary(page.column(service::open_access::entities::OpenWebhookEntity::columnName<"id">(), "webhook"), ruvia::DbBinaryOperator::kEqual, page.column("webhook_id", "page_rows")), "webhook");
        ruvia::DbQuery query(c.pool());
        query.with("counted", counted).with("page_rows", pageRows).with("page", page);
        const auto total = query.subquery(counted);
        const auto item = query.call(
            "jsonb_build_object",
            { textKey(query, "id"), text(query, query.column("id", "page")), textKey(query, "accessKeyId"), text(query, query.column("access_key_id", "page")), textKey(query, "accessKeyName"), query.column("access_key_name", "page"), textKey(query, "webhookId"), text(query, query.column("webhook_id", "page")), textKey(query, "webhookName"), query.column("webhook_name", "page"), textKey(query, "direction"), query.column("direction", "page"), textKey(query, "action"), query.column("action", "page"), textKey(query, "eventType"), query.column("event_type", "page"), textKey(query, "status"), query.column("status", "page"), textKey(query, "httpMethod"), query.column("http_method", "page"), textKey(query, "target"), query.column("target", "page"), textKey(query, "requestIp"), query.column("request_ip", "page"), textKey(query, "httpStatus"), query.column("http_status", "page"), textKey(query, "deviceId"), text(query, query.column("device_id", "page")), textKey(query, "deviceCode"), query.column("device_code", "page"), textKey(query, "message"), query.column("message", "page"), textKey(query, "requestPayload"), query.column("request_payload", "page"), textKey(query, "responsePayload"), query.column("response_payload", "page"), textKey(query, "createdAt"), query.call("iot_utc_timestamp", { query.column("created_at", "page") }) }
        );
        const std::array<ruvia::DbOrderTerm, 2> logOrder{ { ruvia::DbOrderTerm{ query.column("created_at", "page"),
                                                                                ruvia::DbOrderDirection::kDesc,
                                                                                ruvia::DbNullsOrder::kDefault },
                                                            ruvia::DbOrderTerm{ query.column("id", "page"), ruvia::DbOrderDirection::kDesc, ruvia::DbNullsOrder::kDefault } } };
        const auto list = query.coalesce(
            { query.aggregate("jsonb_agg", { item }, false, logOrder),
              emptyJson(query) }
        );
        const auto totalPages = query.cast(
            query.call("ceil", { query.binary(query.cast(total, ruvia::DbDataType::kNumeric), ruvia::DbBinaryOperator::kDivide, query.cast(query.value(pagination.pageSize), ruvia::DbDataType::kNumeric)) }),
            ruvia::DbDataType::kBigInt
        );
        query.select(query.cast(
                         query.call("jsonb_build_object", { textKey(query, "list"), list, textKey(query, "total"), total, textKey(query, "page"), query.cast(query.value(pagination.page), ruvia::DbDataType::kBigInt), textKey(query, "pageSize"), query.cast(query.value(pagination.pageSize), ruvia::DbDataType::kBigInt), textKey(query, "totalPages"), totalPages }),
                         ruvia::DbDataType::kText
                     ))
            .from("page");
        co_return firstJson(co_await c.db().query(query));
    }

    ruvia::Task<AccessSession> authenticate(ruvia::Context& c, std::string_view requiredScope) {
        const auto raw = requestAccessKey(c.req());
        if (raw.empty()) {
            service::common::fail(19010, "缺少 X-Access-Key", 401);
        }
        const auto projected = co_await loadSession(c, raw);
        if (!projected) {
            service::common::fail(19010, "AccessKey 无效", 401);
        }
        if (projected->status != "enabled") {
            service::common::fail(19011, "AccessKey 已被禁用", 403);
        }
        if (session::expired(*projected, service::message::utcNowMilliseconds())) {
            service::common::fail(19010, "AccessKey 已过期", 401);
        }
        AccessSession session;
        session.id = projected->id;
        session.name = projected->name;
        session.scopes = projected->scopes;
        if (!requiredScope.empty() && !session.allows(requiredScope)) {
            service::common::fail(19011, "AccessKey 未开通所需权限", 403);
        }
        session.deviceIds = projected->deviceIds;
        if (session.deviceIds.empty()) {
            service::common::fail(19011, "AccessKey 未配置可访问设备", 403);
        }
        co_return session;
    }

    ruvia::Task<void> audit(ruvia::Context& c, std::string_view action, const AccessSession& session, std::string_view deviceId = {}, std::string_view requestPayload = "{}", std::string_view responsePayload = "{}") {
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
        const auto deviceId = visible.column(service::open_access::entities::DeviceEntity::columnName<"id">(), "device");
        const auto code = service::device::DeviceAccessService::jsonText(
            visible,
            visible.column(service::open_access::entities::DeviceEntity::columnName<"protocol_params">(), "device"),
            "device_code"
        );
        const auto pattern = visible.binary(
            visible.binary(service::device::DeviceAccessService::text(visible, "%"), ruvia::DbBinaryOperator::kConcat, service::device::DeviceAccessService::text(visible, keyword)),
            ruvia::DbBinaryOperator::kConcat,
            service::device::DeviceAccessService::text(visible, "%")
        );
        const auto keywordFilter = visible.binary(
            visible.binary(service::device::DeviceAccessService::text(visible, keyword), ruvia::DbBinaryOperator::kEqual, service::device::DeviceAccessService::text(visible, "")),
            ruvia::DbBinaryOperator::kOr,
            visible.binary(
                visible.binary(
                    visible.binary(service::device::DeviceAccessService::text(visible, deviceId), ruvia::DbBinaryOperator::kILike, pattern),
                    ruvia::DbBinaryOperator::kOr,
                    visible.binary(visible.column(service::open_access::entities::DeviceEntity::columnName<"name">(), "device"), ruvia::DbBinaryOperator::kILike, pattern)
                ),
                ruvia::DbBinaryOperator::kOr,
                visible.binary(visible.coalesce({ code, visible.value("") }), ruvia::DbBinaryOperator::kILike, pattern)
            )
        );
        visible.select({ deviceId, visible.column(service::open_access::entities::DeviceEntity::columnName<"name">(), "device"), visible.alias(code, "code") })
            .from(service::open_access::entities::DeviceEntity::tableName(), "device")
            .join(ruvia::DbJoinType::kInner, service::open_access::entities::OpenAccessKeyDeviceEntity::tableName(), visible.binary(visible.column(service::open_access::entities::OpenAccessKeyDeviceEntity::columnName<"device_id">(), "binding"), ruvia::DbBinaryOperator::kEqual, deviceId), "binding")
            .where(andAll(visible, visible.binary(visible.column(service::open_access::entities::OpenAccessKeyDeviceEntity::columnName<"access_key_id">(), "binding"), ruvia::DbBinaryOperator::kEqual, uuid(visible, session.id)), visible.unary(ruvia::DbUnaryOperator::kIsNull, visible.column(service::open_access::entities::DeviceEntity::columnName<"deleted_at">(), "device")), keywordFilter));
        ruvia::DbQuery counted(c.pool());
        counted.select(counted.aggregate("count", { counted.star() })).from(visible, "visible");
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
            { textKey(query, "id"), text(query, query.column("id", "page")), textKey(query, "code"), query.column("code", "page"), textKey(query, "name"), query.column("name", "page") }
        );
        const auto total = query.subquery(counted);
        const std::array<ruvia::DbOrderTerm, 2> deviceListOrder{ { ruvia::DbOrderTerm{ query.column("name", "page"), ruvia::DbOrderDirection::kAsc, ruvia::DbNullsOrder::kDefault },
                                                                   ruvia::DbOrderTerm{ query.column("id", "page"), ruvia::DbOrderDirection::kAsc, ruvia::DbNullsOrder::kDefault } } };
        const auto list = query.coalesce(
            { query.aggregate("jsonb_agg", { item }, false, deviceListOrder),
              emptyJson(query) }
        );
        const auto totalPages = query.cast(
            query.call("ceil", { query.binary(query.cast(total, ruvia::DbDataType::kNumeric), ruvia::DbBinaryOperator::kDivide, query.cast(query.value(pagination.pageSize), ruvia::DbDataType::kNumeric)) }),
            ruvia::DbDataType::kBigInt
        );
        query.select(query.cast(
                         query.call("jsonb_build_object", { textKey(query, "list"), list, textKey(query, "total"), total, textKey(query, "page"), query.cast(query.value(pagination.page), ruvia::DbDataType::kBigInt), textKey(query, "pageSize"), query.cast(query.value(pagination.pageSize), ruvia::DbDataType::kBigInt), textKey(query, "totalPages"), totalPages }),
                         ruvia::DbDataType::kText
                     ))
            .from("page");
        const auto rows = co_await c.db().query(query);
        co_return firstJson(rows);
    }

    ruvia::Task<std::string> publicRealtime(ruvia::Context& c, const AccessSession& session, std::string_view deviceId) {
        service::common::requireUuid(19002, deviceId, "设备 ID 无效");
        if (!session.allowsDevice(deviceId)) {
            service::common::fail(19011, "AccessKey 无权访问该设备", 403);
        }
        co_return co_await realtimeData(c, deviceId);
    }

    ruvia::Task<std::string> publicHistory(ruvia::Context& c, const AccessSession& session, std::string_view deviceId) {
        co_await requireSessionDevice(c, session, deviceId);
        const auto start = service::utils::trim(c.req().query("startTime").value_or(""));
        const auto end = service::utils::trim(c.req().query("endTime").value_or(""));
        if (start.empty() || end.empty()) {
            service::common::fail(19002, "startTime 和 endTime 不能为空", 400);
        }
        const auto pagination = service::common::page(c.req());
        try {
            const auto query = historyQuery(c.pool(), deviceId, start, end, pagination.pageSize, pagination.offset, pagination.page);
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
            if (!session.allowsDevice(*device)) {
                service::common::fail(19011, "AccessKey 无权访问该设备", 403);
            }
            deviceFilter = std::string(*device);
        }
        const auto status = c.req().query("status");
        const auto severity = c.req().query("severity");
        const auto applyFilters = [&](ruvia::DbQuery& query, std::string_view recordAlias, std::string_view bindingAlias) {
            auto predicate = query.binary(query.column("access_key_id", bindingAlias), ruvia::DbBinaryOperator::kEqual, uuid(query, session.id));
            if (deviceFilter) {
                predicate = query.binary(
                    predicate,
                    ruvia::DbBinaryOperator::kAnd,
                    query.binary(query.column("device_id", recordAlias), ruvia::DbBinaryOperator::kEqual, uuid(query, *deviceFilter))
                );
            }
            if (status && !status->empty()) {
                predicate = query.binary(
                    predicate,
                    ruvia::DbBinaryOperator::kAnd,
                    query.binary(query.column("status", recordAlias), ruvia::DbBinaryOperator::kEqual, query.value(*status))
                );
            }
            if (severity && !severity->empty()) {
                predicate = query.binary(
                    predicate,
                    ruvia::DbBinaryOperator::kAnd,
                    query.binary(query.column("severity", recordAlias), ruvia::DbBinaryOperator::kEqual, query.value(*severity))
                );
            }
            query.where(predicate);
        };
        ruvia::DbQuery counted(c.pool());
        counted.select(counted.aggregate("count", { counted.star() }))
            .from(service::open_access::entities::OpenAlertRecordEntity::tableName(), "record")
            .join(ruvia::DbJoinType::kInner, service::open_access::entities::OpenAccessKeyDeviceEntity::tableName(), counted.binary(counted.column(service::open_access::entities::OpenAccessKeyDeviceEntity::columnName<"device_id">(), "binding"), ruvia::DbBinaryOperator::kEqual, counted.column(service::open_access::entities::OpenAlertRecordEntity::columnName<"device_id">(), "record")), "binding");
        applyFilters(counted, "record", "binding");
        ruvia::DbQuery pageRows(c.pool());
        pageRows.select(pageRows.star("record"))
            .from(service::open_access::entities::OpenAlertRecordEntity::tableName(), "record")
            .join(ruvia::DbJoinType::kInner, service::open_access::entities::OpenAccessKeyDeviceEntity::tableName(), pageRows.binary(pageRows.column(service::open_access::entities::OpenAccessKeyDeviceEntity::columnName<"device_id">(), "binding"), ruvia::DbBinaryOperator::kEqual, pageRows.column(service::open_access::entities::OpenAlertRecordEntity::columnName<"device_id">(), "record")), "binding");
        applyFilters(pageRows, "record", "binding");
        pageRows.orderBy(pageRows.column(service::open_access::entities::OpenAlertRecordEntity::columnName<"triggered_at">(), "record"), ruvia::DbOrderDirection::kDesc)
            .addOrderBy(pageRows.column(service::open_access::entities::OpenAlertRecordEntity::columnName<"id">(), "record"), ruvia::DbOrderDirection::kDesc)
            .limit(static_cast<std::uint64_t>(pagination.pageSize))
            .offset(static_cast<std::uint64_t>(pagination.offset));
        ruvia::DbQuery page(c.pool());
        page.select({ page.star("page_rows"), page.alias(page.column(service::open_access::entities::DeviceEntity::columnName<"name">(), "device"), "device_name"), page.alias(service::device::DeviceAccessService::jsonText(page, page.column(service::open_access::entities::DeviceEntity::columnName<"protocol_params">(), "device"), "device_code"), "device_code") })
            .from(pageRows, "page_rows")
            .join(ruvia::DbJoinType::kInner, service::open_access::entities::DeviceEntity::tableName(), page.binary(page.column(service::open_access::entities::DeviceEntity::columnName<"id">(), "device"), ruvia::DbBinaryOperator::kEqual, page.column("device_id", "page_rows")), "device");
        ruvia::DbQuery query(c.pool());
        query.with("counted", counted).with("page_rows", pageRows).with("page", page);
        const auto item = query.call(
            "jsonb_build_object",
            { textKey(query, "id"), text(query, query.column("id", "page")), textKey(query, "device"), query.call("jsonb_build_object", { textKey(query, "id"), text(query, query.column("device_id", "page")), textKey(query, "code"), query.column("device_code", "page"), textKey(query, "name"), query.column("device_name", "page") }), textKey(query, "ruleId"), text(query, query.column("rule_id", "page")), textKey(query, "severity"), query.column("severity", "page"), textKey(query, "status"), query.column("status", "page"), textKey(query, "message"), query.column("message", "page"), textKey(query, "time"), query.call("iot_utc_timestamp", { query.column("triggered_at", "page") }) }
        );
        const auto total = query.subquery(counted);
        const std::array<ruvia::DbOrderTerm, 2> alertOrder{ { ruvia::DbOrderTerm{ query.column("triggered_at", "page"),
                                                                                  ruvia::DbOrderDirection::kDesc,
                                                                                  ruvia::DbNullsOrder::kDefault },
                                                              ruvia::DbOrderTerm{ query.column("id", "page"), ruvia::DbOrderDirection::kDesc, ruvia::DbNullsOrder::kDefault } } };
        const auto list = query.coalesce(
            { query.aggregate("jsonb_agg", { item }, false, alertOrder),
              emptyJson(query) }
        );
        const auto totalPages = query.cast(
            query.call("ceil", { query.binary(query.cast(total, ruvia::DbDataType::kNumeric), ruvia::DbBinaryOperator::kDivide, query.cast(query.value(pagination.pageSize), ruvia::DbDataType::kNumeric)) }),
            ruvia::DbDataType::kBigInt
        );
        query.select(query.cast(
                         query.call("jsonb_build_object", { textKey(query, "list"), list, textKey(query, "total"), total, textKey(query, "page"), query.cast(query.value(pagination.page), ruvia::DbDataType::kBigInt), textKey(query, "pageSize"), query.cast(query.value(pagination.pageSize), ruvia::DbDataType::kBigInt), textKey(query, "totalPages"), totalPages }),
                         ruvia::DbDataType::kText
                     ))
            .from("page");
        const auto rows = co_await c.db().query(query);
        co_return firstJson(rows);
    }

    template <typename Context>
    ruvia::Task<std::string> realtimeData(Context& c, std::string_view deviceId) {
        const auto device = co_await loadRealtimeDevice(c, deviceId);
        if (!device) {
            service::common::fail(19001, "设备不存在", 404);
        }
        const auto latestKey = service::telemetry::latest::latestKey(device->id);
        const auto latest = co_await service::message::redis::command(
            c.redis(),
            std::vector<std::string>{ "HGETALL", latestKey }
        );
        std::map<std::string_view, std::string_view, std::less<>> latestFields;
        if (latest.kind() == ruvia::RedisValue::Kind::kArray) {
            const auto values = latest.array();
            for (std::size_t index = 0; index + 1 < values.size(); index += 2) {
                if (values[index].kind() == ruvia::RedisValue::Kind::kString &&
                    values[index + 1].kind() == ruvia::RedisValue::Kind::kString) {
                    latestFields.insert_or_assign(values[index].string(), values[index + 1].string());
                }
            }
        } else {
            service::message::redis::throwValue("read realtime device values", latest);
        }
        std::string body = "{\"device\":{\"id\":" + service::utils::jsonQuoted(deviceId) +
            ",\"code\":" + service::utils::jsonQuoted(device->code) +
            ",\"name\":" + service::utils::jsonQuoted(device->name) + "},\"points\":[";
        for (std::size_t index = 0; index < device->points.size(); ++index) {
            if (index != 0) {
                body.push_back(',');
            }
            std::string value = "null";
            std::string time = "null";
            if (const auto data = latestFields.find(device->points[index].id);
                data != latestFields.end()) {
                if (const auto json = ruvia::JsonValue::parse(data->second)) {
                    const auto dataType = json->template get<ruvia::String>("dataType");
                    if (const auto current = service::utils::jsonField(*json, "value")) {
                        value = service::telemetry::latest::canonicalPointJson(
                            current->view(),
                            dataType ? dataType->view() : std::string_view{}
                        );
                    }
                    if (const auto observed =
                            json->template get<ruvia::Int64>("observedAt")) {
                        time = service::utils::jsonQuoted(service::common::utcTimestampFromMilliseconds(static_cast<std::int64_t>(*observed)));
                    }
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

    static ruvia::DbQuery::Expr andAll(ruvia::DbQuery& query, ruvia::DbQuery::Expr first, ruvia::DbQuery::Expr second) {
        return service::device::andAll(query, first, second);
    }

    template <typename... Expressions>
    static ruvia::DbQuery::Expr andAll(ruvia::DbQuery& query, ruvia::DbQuery::Expr first, ruvia::DbQuery::Expr second, Expressions... rest) {
        return service::device::andAll(query, first, second, rest...);
    }

    template <typename Context>
    static ruvia::Task<void> refreshProjection(Context& c) {
        (void)co_await service::rpc::call(c, "access", "refresh", "{}");
    }

    static ruvia::Task<std::optional<session::Entry>> loadSession(
        ruvia::Context& c,
        std::string_view rawKey
    ) {
        static constexpr std::string_view script = R"lua(
local version = redis.call('GET', KEYS[1])
if not version then return nil end
return redis.call('HGET', ARGV[2] .. version, ARGV[1])
)lua";
        const auto keyHash = service::utils::sha256(rawKey);
        const std::string activeKey(session::kActiveVersionKey);
        const std::string_view keys[]{ activeKey };
        const std::string prefix(session::kVersionPrefix);
        const std::string_view args[]{ keyHash, prefix };
        const auto reply = co_await c.redis().eval(script, keys, args);
        if (reply.null()) {
            co_return std::nullopt;
        }
        if (reply.kind() != ruvia::RedisValue::Kind::kString) {
            service::message::redis::throwValue("load projected access session", reply);
        }
        co_return session::decode(reply.string());
    }

    static std::string_view projectionField(
        const ruvia::RedisValue& value,
        std::string_view operation,
        std::string_view field
    ) {
        if (value.kind() != ruvia::RedisValue::Kind::kArray) {
            service::message::redis::throwValue(operation, value);
        }
        const auto& values = value.array();
        if (values.size() % 2 != 0) {
            service::message::redis::throwValue(operation, value);
        }
        for (std::size_t index = 0; index < values.size(); index += 2) {
            if (values[index].kind() != ruvia::RedisValue::Kind::kString ||
                values[index + 1].kind() != ruvia::RedisValue::Kind::kString) {
                service::message::redis::throwValue(operation, value);
            }
            if (values[index].string() == field) {
                return values[index + 1].string();
            }
        }
        return {};
    }

    static ruvia::Task<std::optional<service::message::realtime::Device>> loadRealtimeDevice(
        ruvia::Context& c,
        std::string_view deviceId
    ) {
        const auto versionReply = co_await service::message::redis::command(
            c.redis(),
            { "GET", std::string(service::message::projection::kRuntimeActiveVersionKey) }
        );
        if (versionReply.null()) {
            throw std::runtime_error("runtime is not ready");
        }
        if (versionReply.kind() != ruvia::RedisValue::Kind::kString) {
            service::message::redis::throwValue("GET active runtime", versionReply);
        }
        const auto version = versionReply.string();
        const std::vector<std::vector<std::string>> commands{
            { "HGETALL", service::message::projection::realtimeDeviceKey(version, deviceId) },
            { "LRANGE", service::message::projection::realtimeDevicePointsKey(version, deviceId), "0", "-1" }
        };
        auto pipeline = c.redis().pipeline();
        for (const auto& command : commands) {
            const std::vector<std::string_view> views(command.begin(), command.end());
            pipeline.command(views);
        }
        const auto replies = co_await std::move(pipeline).exec();
        if (replies.size() != 2) {
            throw std::runtime_error("incomplete realtime device projection reply");
        }
        if (replies[0].kind() != ruvia::RedisValue::Kind::kArray ||
            replies[0].array().empty()) {
            co_return std::nullopt;
        }

        service::message::realtime::Device result;
        result.id.assign(projectionField(replies[0], "read realtime device metadata", "id"));
        result.code.assign(projectionField(replies[0], "read realtime device metadata", "code"));
        result.name.assign(projectionField(replies[0], "read realtime device metadata", "name"));
        if (result.id.empty()) {
            co_return std::nullopt;
        }
        if (replies[1].kind() != ruvia::RedisValue::Kind::kArray) {
            service::message::redis::throwValue("read realtime device points", replies[1]);
        }
        const auto& points = replies[1].array();
        result.points.reserve(points.size());
        for (const auto& point : points) {
            if (point.kind() != ruvia::RedisValue::Kind::kString) {
                service::message::redis::throwValue("read realtime device points", replies[1]);
            }
            const auto decoded = service::message::realtime::decodePoint(point.string());
            if (!decoded) {
                throw std::runtime_error("invalid realtime point projection");
            }
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
        std::int64_t timeout{ 5 };
        bool skipTlsVerify{ false };
        std::string headers{ "{}" };
        std::set<std::string, std::less<>> events;
        std::optional<std::string> secret;
    };

    static std::string requestAccessKey(const ruvia::ContextRequest& request) {
        return service::utils::trim(request.header("X-Access-Key").value_or(""));
    }

    template <typename Rows>
    static std::string firstJson(const Rows& rows) {
        if (rows.empty() || rows.front().empty() || !rows.front()[0].value().has_value()) {
            return "[]";
        }
        return std::string(rows.front()[0].value().value_or(std::string_view{}));
    }

    template <typename Range>
    static std::string stringArrayJson(const Range& values) {
        std::string result{ "[" };
        bool first = true;
        for (const auto& value : values) {
            if (!first) {
                result.push_back(',');
            }
            first = false;
            result += service::utils::jsonQuoted(value);
        }
        result.push_back(']');
        return result;
    }

    static std::set<std::string, std::less<>> parseStringArray(std::string_view json) {
        std::set<std::string, std::less<>> result;
        const auto value = ruvia::JsonValue::parse(json);
        if (!value || !value->isArray()) {
            return result;
        }
        auto remaining = json;
        const auto parsed = ruvia::detail::parseJsonValue<ruvia::Array<ruvia::String>>(
            remaining,
            std::pmr::get_default_resource()
        );
        if (!parsed) {
            return result;
        }
        for (const auto& item : *parsed) {
            result.emplace(item.view());
        }
        return result;
    }

    template <typename Context>
    ruvia::Task<void> ensureDevicesAccessible(Context& c, const std::vector<std::string>& ids, bool requireOperate) {
        const auto actor = co_await service::device::deviceAccessService().actor(c, c.userId);
        ruvia::DbQuery query(c.pool());
        service::device::DeviceAccessService::addScopedDevicesCtes(query, actor);
        std::vector<ruvia::DbQuery::Expr> deviceIds;
        deviceIds.reserve(ids.size());
        for (const auto& id : ids) {
            deviceIds.push_back(uuid(query, id));
        }
        query.select(query.aggregate("count", { query.star() }))
            .from("scoped_device")
            .where(andAll(query, query.binary(query.column("id"), ruvia::DbBinaryOperator::kIn, query.list(deviceIds)), query.binary(query.column("access_rank"), ruvia::DbBinaryOperator::kGreaterEqual, service::device::DeviceAccessService::integer(query, requireOperate ? 2 : 1))));
        const auto rows = co_await c.db().query(query);
        const auto visible =
            service::utils::parseInt64(
                std::optional<std::string_view>{ rows.front()[0].value().value_or(std::string_view{}) }
            )
                .value_or(-1);
        if (visible != static_cast<std::int64_t>(ids.size())) {
            service::common::fail(19011, requireOperate ? "所选设备中包含无控制权限的设备" : "所选设备中包含无访问权限的设备", 403);
        }
    }

    static ruvia::Task<void> replaceDevices(ruvia::DbTransaction& transaction, std::string_view keyId, const std::vector<std::string>& devices) {
        ruvia::DbQuery remove;
        remove.deleteFrom(service::open_access::entities::OpenAccessKeyDeviceEntity::tableName())
            .where(remove.binary(remove.column(service::open_access::entities::OpenAccessKeyDeviceEntity::columnName<"access_key_id">()), ruvia::DbBinaryOperator::kEqual, uuid(remove, keyId)));
        (void)co_await transaction.execute(remove);
        for (const auto& device : devices) {
            ruvia::DbQuery insert;
            insert.insertInto(service::open_access::entities::OpenAccessKeyDeviceEntity::tableName(), { "access_key_id", "device_id" })
                .values({ uuid(insert, keyId), uuid(insert, device) });
            (void)co_await transaction.execute(insert);
        }
    }

    template <typename Context>
    ruvia::Task<void> ensureKeyNameAvailable(Context& c, std::string_view name, std::optional<std::string> except) {
        ruvia::DbQuery query(c.pool());
        auto predicate = andAll(
            query,
            query.binary(query.column(service::open_access::entities::OpenAccessKeyEntity::columnName<"name">()), ruvia::DbBinaryOperator::kEqual, query.value(name)),
            query.unary(ruvia::DbUnaryOperator::kIsNull, query.column(service::open_access::entities::OpenAccessKeyEntity::columnName<"deleted_at">()))
        );
        if (except) {
            predicate = query.binary(
                predicate,
                ruvia::DbBinaryOperator::kAnd,
                query.binary(query.column(service::open_access::entities::OpenAccessKeyEntity::columnName<"id">()), ruvia::DbBinaryOperator::kNotEqual, uuid(query, *except))
            );
        }
        query.select(service::device::DeviceAccessService::integer(query, 1))
            .from(service::open_access::entities::OpenAccessKeyEntity::tableName())
            .where(predicate)
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (!rows.empty()) {
            service::common::fail(19003, "调用配置名称已存在", 409);
        }
    }

    template <typename Context>
    ruvia::Task<void> ensureWebhookNameAvailable(Context& c, std::string_view accessKeyId, std::string_view name, std::optional<std::string> except) {
        ruvia::DbQuery query(c.pool());
        auto predicate = andAll(
            query,
            query.binary(query.column(service::open_access::entities::OpenWebhookEntity::columnName<"access_key_id">()), ruvia::DbBinaryOperator::kEqual, uuid(query, accessKeyId)),
            query.binary(query.column(service::open_access::entities::OpenWebhookEntity::columnName<"name">()), ruvia::DbBinaryOperator::kEqual, query.value(name)),
            query.unary(ruvia::DbUnaryOperator::kIsNull, query.column(service::open_access::entities::OpenWebhookEntity::columnName<"deleted_at">()))
        );
        if (except) {
            predicate = query.binary(
                predicate,
                ruvia::DbBinaryOperator::kAnd,
                query.binary(query.column(service::open_access::entities::OpenWebhookEntity::columnName<"id">()), ruvia::DbBinaryOperator::kNotEqual, uuid(query, *except))
            );
        }
        query.select(service::device::DeviceAccessService::integer(query, 1))
            .from(service::open_access::entities::OpenWebhookEntity::tableName())
            .where(predicate)
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (!rows.empty()) {
            service::common::fail(19003, "同一调用配置下的 Webhook 名称已存在", 409);
        }
    }

    template <typename Context>
    ruvia::Task<KeyState> requireKey(Context& c, std::string_view id) {
        ruvia::DbQuery query(c.pool());
        query.select({ query.column(service::open_access::entities::OpenAccessKeyEntity::columnName<"name">()), text(query, query.column(service::open_access::entities::OpenAccessKeyEntity::columnName<"status">())), text(query, query.column(service::open_access::entities::OpenAccessKeyEntity::columnName<"scopes">())), query.call("iot_utc_timestamp", { query.column(service::open_access::entities::OpenAccessKeyEntity::columnName<"expires_at">()) }), query.column(service::open_access::entities::OpenAccessKeyEntity::columnName<"remark">()) })
            .from(service::open_access::entities::OpenAccessKeyEntity::tableName())
            .where(andAll(query, query.binary(query.column(service::open_access::entities::OpenAccessKeyEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual, uuid(query, id)), query.unary(ruvia::DbUnaryOperator::kIsNull, query.column(service::open_access::entities::OpenAccessKeyEntity::columnName<"deleted_at">()))))
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty()) {
            service::common::fail(19001, "调用配置不存在", 404);
        }
        const auto& row = rows.front();
        KeyState state;
        state.name = std::string(row[0].value().value_or(std::string_view{}));
        state.status = std::string(row[1].value().value_or(std::string_view{}));
        state.scopes = parseStringArray(row[2].value().value_or(std::string_view{}));
        if (row[3].value().has_value()) {
            state.expiresAt = std::string(row[3].value().value_or(std::string_view{}));
        }
        if (row[4].value().has_value()) {
            state.remark = std::string(row[4].value().value_or(std::string_view{}));
        }
        ruvia::DbQuery devicesQuery(c.pool());
        devicesQuery
            .select(text(devicesQuery, devicesQuery.column(service::open_access::entities::OpenAccessKeyDeviceEntity::columnName<"device_id">())))
            .from(service::open_access::entities::OpenAccessKeyDeviceEntity::tableName())
            .where(devicesQuery.binary(devicesQuery.column(service::open_access::entities::OpenAccessKeyDeviceEntity::columnName<"access_key_id">()), ruvia::DbBinaryOperator::kEqual, uuid(devicesQuery, id)))
            .orderBy(devicesQuery.column(service::open_access::entities::OpenAccessKeyDeviceEntity::columnName<"device_id">()));
        const auto devices = co_await c.db().query(devicesQuery);
        for (const auto& device : devices) {
            state.deviceIds.emplace_back(device[0].value().value_or(std::string_view{}));
        }
        co_return state;
    }

    template <typename Context>
    ruvia::Task<WebhookState> requireWebhook(Context& c, std::string_view id) {
        ruvia::DbQuery query(c.pool());
        query.select({ text(query, query.column(service::open_access::entities::OpenWebhookEntity::columnName<"access_key_id">())), query.column(service::open_access::entities::OpenWebhookEntity::columnName<"name">()), query.column(service::open_access::entities::OpenWebhookEntity::columnName<"url">()), text(query, query.column(service::open_access::entities::OpenWebhookEntity::columnName<"status">())), query.column(service::open_access::entities::OpenWebhookEntity::columnName<"timeout_seconds">()), query.caseWhen({ { query.binary(query.column(service::open_access::entities::OpenWebhookEntity::columnName<"skip_tls_verify">()), ruvia::DbBinaryOperator::kEqual, service::device::DeviceAccessService::boolean(query, true)), text(query, "1") } }, text(query, "0")), text(query, query.column(service::open_access::entities::OpenWebhookEntity::columnName<"headers">())), text(query, query.column(service::open_access::entities::OpenWebhookEntity::columnName<"event_types">())), query.column(service::open_access::entities::OpenWebhookEntity::columnName<"secret">()) })
            .from(service::open_access::entities::OpenWebhookEntity::tableName())
            .where(andAll(query, query.binary(query.column(service::open_access::entities::OpenWebhookEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual, uuid(query, id)), query.unary(ruvia::DbUnaryOperator::kIsNull, query.column(service::open_access::entities::OpenWebhookEntity::columnName<"deleted_at">()))))
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty()) {
            service::common::fail(19001, "Webhook 不存在", 404);
        }
        const auto& row = rows.front();
        WebhookState state;
        state.accessKeyId = std::string(row[0].value().value_or(std::string_view{}));
        state.name = std::string(row[1].value().value_or(std::string_view{}));
        state.url = std::string(row[2].value().value_or(std::string_view{}));
        state.status = std::string(row[3].value().value_or(std::string_view{}));
        state.timeout =
            service::utils::parseInt64(std::optional<std::string_view>{ row[4].value().value_or(std::string_view{}) })
                .value_or(state.timeout);
        state.skipTlsVerify = row[5].value().value_or(std::string_view{}) == "1";
        state.headers = std::string(row[6].value().value_or(std::string_view{}));
        state.events = parseStringArray(row[7].value().value_or(std::string_view{}));
        if (row[8].value().has_value()) {
            state.secret = std::string(row[8].value().value_or(std::string_view{}));
        }
        co_return state;
    }

    static ruvia::Task<void> requireSessionDevice(ruvia::Context& c, const AccessSession& session, std::string_view deviceId) {
        service::common::requireUuid(19002, deviceId, "设备 ID 无效");
        if (!session.allowsDevice(deviceId)) {
            service::common::fail(19011, "AccessKey 无权访问该设备", 403);
        }
        ruvia::DbQuery query(c.pool());
        query.select(service::device::DeviceAccessService::integer(query, 1))
            .from(service::open_access::entities::DeviceEntity::tableName())
            .where(andAll(query, query.binary(query.column(service::open_access::entities::DeviceEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual, uuid(query, deviceId)), query.unary(ruvia::DbUnaryOperator::kIsNull, query.column(service::open_access::entities::DeviceEntity::columnName<"deleted_at">()))))
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty()) {
            service::common::fail(19001, "设备不存在", 404);
        }
    }

    static ruvia::DbQuery historyQuery(std::pmr::memory_resource* resource, std::string_view deviceId, std::string_view start, std::string_view end, std::int64_t pageSize, std::int64_t offset, std::int64_t page) {
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
        const auto deviceParams = deviceRef.column(service::open_access::entities::DeviceEntity::columnName<"protocol_params">(), "device");
        deviceRef
            .select({ deviceRef.column(service::open_access::entities::DeviceEntity::columnName<"id">(), "device"), deviceRef.alias(service::device::DeviceAccessService::jsonText(deviceRef, deviceParams, "device_code"), "code"), deviceRef.column(service::open_access::entities::DeviceEntity::columnName<"name">(), "device") })
            .from(service::open_access::entities::DeviceEntity::tableName(), "device")
            .where(andAll(deviceRef, deviceRef.binary(deviceRef.column(service::open_access::entities::DeviceEntity::columnName<"id">(), "device"), ruvia::DbBinaryOperator::kEqual, uuid(deviceRef, deviceId)), deviceRef.unary(ruvia::DbUnaryOperator::kIsNull, deviceRef.column(service::open_access::entities::DeviceEntity::columnName<"deleted_at">(), "device"))));

        ruvia::DbQuery counted(resource);
        const auto countedData = counted.column(service::open_access::entities::DeviceDataEntity::columnName<"data">(), "record");
        counted
            .select(counted.aggregate("count", { counted.star() }))
            .from(service::open_access::entities::DeviceDataEntity::tableName(), "record")
            .where(andAll(counted, counted.binary(counted.column(service::open_access::entities::DeviceDataEntity::columnName<"device_id">(), "record"), ruvia::DbBinaryOperator::kEqual, uuid(counted, deviceId)), counted.binary(counted.column(service::open_access::entities::DeviceDataEntity::columnName<"report_time">(), "record"), ruvia::DbBinaryOperator::kGreaterEqual, timestamp(counted, start)), counted.binary(counted.column(service::open_access::entities::DeviceDataEntity::columnName<"report_time">(), "record"), ruvia::DbBinaryOperator::kLessEqual, timestamp(counted, end)), counted.binary(counted.call("jsonb_typeof", { service::device::DeviceAccessService::jsonValue(counted, countedData, "values") }), ruvia::DbBinaryOperator::kEqual, text(counted, "object"))));

        ruvia::DbQuery filtered(resource);
        const auto filteredData = filtered.column(service::open_access::entities::DeviceDataEntity::columnName<"data">(), "record");
        filtered
            .select({ filtered.column(service::open_access::entities::DeviceDataEntity::columnName<"id">(), "record"), filtered.column(service::open_access::entities::DeviceDataEntity::columnName<"report_time">(), "record"), filteredData })
            .from(service::open_access::entities::DeviceDataEntity::tableName(), "record")
            .where(andAll(filtered, filtered.binary(filtered.column(service::open_access::entities::DeviceDataEntity::columnName<"device_id">(), "record"), ruvia::DbBinaryOperator::kEqual, uuid(filtered, deviceId)), filtered.binary(filtered.column(service::open_access::entities::DeviceDataEntity::columnName<"report_time">(), "record"), ruvia::DbBinaryOperator::kGreaterEqual, timestamp(filtered, start)), filtered.binary(filtered.column(service::open_access::entities::DeviceDataEntity::columnName<"report_time">(), "record"), ruvia::DbBinaryOperator::kLessEqual, timestamp(filtered, end)), filtered.binary(filtered.call("jsonb_typeof", { service::device::DeviceAccessService::jsonValue(filtered, filteredData, "values") }), ruvia::DbBinaryOperator::kEqual, text(filtered, "object"))))
            .orderBy(filtered.column(service::open_access::entities::DeviceDataEntity::columnName<"report_time">(), "record"), ruvia::DbOrderDirection::kDesc)
            .addOrderBy(filtered.column(service::open_access::entities::DeviceDataEntity::columnName<"id">(), "record"), ruvia::DbOrderDirection::kDesc)
            .limit(static_cast<std::uint64_t>(pageSize))
            .offset(static_cast<std::uint64_t>(offset));

        ruvia::DbQuery normalizedValues(resource);
        const auto normalizedPoint = normalizedValues.column("value", "point");
        const auto normalizedPointValue = service::device::DeviceAccessService::jsonValue(
            normalizedValues,
            normalizedPoint,
            "value"
        );
        const auto pointObject = andAll(
            normalizedValues,
            normalizedValues.binary(normalizedValues.call("jsonb_typeof", { normalizedPoint }), ruvia::DbBinaryOperator::kEqual, text(normalizedValues, "object")),
            normalizedValues.binary(normalizedValues.call("jsonb_typeof", { normalizedPointValue }), ruvia::DbBinaryOperator::kEqual, text(normalizedValues, "boolean"))
        );
        const auto pointBoolean = normalizedValues.cast(
            service::device::DeviceAccessService::jsonText(normalizedValues, normalizedPoint, "value"),
            ruvia::DbDataType::kBoolean
        );
        const auto pointNumber = normalizedValues.caseWhen(
            { { pointBoolean,
                normalizedValues.cast(normalizedValues.value(1), ruvia::DbDataType::kInteger) } },
            normalizedValues.cast(normalizedValues.value(0), ruvia::DbDataType::kInteger)
        );
        const auto pointPath = normalizedValues.cast(
            normalizedValues.array({ textKey(normalizedValues, "value") }),
            ruvia::DbTypeDefinition{ .dataType = ruvia::DbDataType::kText, .array = true }
        );
        const auto normalizedPointValueExpr = normalizedValues.caseWhen(
            { { pointObject,
                normalizedValues.call(
                    "jsonb_set",
                    { normalizedPoint, pointPath, normalizedValues.call("to_jsonb", { pointNumber }), normalizedValues.cast(normalizedValues.value(false), ruvia::DbDataType::kBoolean) }
                ) } },
            normalizedPoint
        );
        normalizedValues
            .select(normalizedValues.aggregate(
                "jsonb_object_agg",
                { normalizedValues.column("key", "point"), normalizedPointValueExpr }
            ))
            .fromFunction(
                normalizedValues.call(
                    "jsonb_each",
                    { normalizedValues.coalesce(
                        { service::device::DeviceAccessService::jsonValue(
                              normalizedValues,
                              normalizedValues.column("data", "filtered"),
                              "values"
                          ),
                          emptyObject(normalizedValues) }
                    ) }
                ),
                "point",
                { .lateral = true,
                  .columns = { { .name = "key" }, { .name = "value" } } }
            );
        ruvia::DbQuery normalized(resource);
        normalized
            .select({ normalized.star("filtered"), normalized.alias(normalized.coalesce({ normalized.subquery(normalizedValues), emptyObject(normalized) }), "normalized_values") })
            .from(filtered, "filtered");

        ruvia::DbQuery items(resource);
        const auto point = items.column("value", "point");
        const auto pointValue = service::device::DeviceAccessService::jsonValue(
            items,
            point,
            "value"
        );
        const auto normalizedData = items.column("data", "normalized");
        const auto itemPointBoolean = items.cast(
            service::device::DeviceAccessService::jsonText(items, point, "value"),
            ruvia::DbDataType::kBoolean
        );
        const auto itemPointNumber = items.caseWhen(
            { { itemPointBoolean, items.cast(items.value(1), ruvia::DbDataType::kInteger) } },
            items.cast(items.value(0), ruvia::DbDataType::kInteger)
        );
        const auto itemPoint = items.call(
            "jsonb_build_object",
            { textKey(items, "id"), items.column("key", "point"), textKey(items, "name"), items.coalesce({ service::device::DeviceAccessService::jsonText(items, point, "name"), items.column("key", "point") }), textKey(items, "value"), items.caseWhen({ { items.binary(items.call("jsonb_typeof", { pointValue }), ruvia::DbBinaryOperator::kEqual, text(items, "boolean")), items.call("to_jsonb", { itemPointNumber }) } }, pointValue), textKey(items, "unit"), items.coalesce({ service::device::DeviceAccessService::jsonText(items, point, "unit"), text(items, "") }), textKey(items, "time"), items.call("iot_utc_timestamp", { items.column("report_time", "normalized") }) }
        );
        const std::array<ruvia::DbOrderTerm, 1> pointOrder{ { ruvia::DbOrderTerm{ items.column("key", "point"), ruvia::DbOrderDirection::kAsc, ruvia::DbNullsOrder::kDefault } } };
        const auto points = items.coalesce(
            { items.filter(
                  items.aggregate("jsonb_agg", { itemPoint }, false, pointOrder),
                  items.unary(ruvia::DbUnaryOperator::kIsNotNull, items.column("key", "point"))
              ),
              emptyArray(items) }
        );
        const auto item = items.call(
            "jsonb_build_object",
            { textKey(items, "device"),
              items.call("jsonb_build_object", { textKey(items, "id"), items.cast(items.column("id", "device_ref"), ruvia::DbDataType::kUuid), textKey(items, "code"), items.column("code", "device_ref"), textKey(items, "name"), items.column("name", "device_ref") }),
              textKey(items, "points"),
              points }
        );
        items
            .select({ items.column("report_time", "normalized"), items.column("id", "normalized"), items.alias(item, "item") })
            .from(normalized, "normalized")
            .join(ruvia::DbJoinType::kCross, "device_ref", {}, "device_ref")
            .joinFunction(
                ruvia::DbJoinType::kLeft,
                items.call("jsonb_each", { service::device::DeviceAccessService::jsonValue(items, normalizedData, "values") }),
                items.binary(
                    items.coalesce({ service::device::DeviceAccessService::jsonText(items, point, "type"), text(items, "") }),
                    ruvia::DbBinaryOperator::kNotEqual,
                    text(items, "JPEG")
                ),
                "point",
                { .lateral = true,
                  .columns = { { .name = "key" }, { .name = "value" } } }
            )
            .groupBy({ items.column("report_time", "normalized"), items.column("id", "normalized"), items.column("id", "device_ref"), items.column("code", "device_ref"), items.column("name", "device_ref") });

        ruvia::DbQuery query(resource);
        query.with("device_ref", deviceRef)
            .with("counted", counted)
            .with("filtered", filtered)
            .with("normalized", normalized)
            .with("items", items);
        const auto total = query.coalesce(
            { query.subquery(counted), query.cast(query.value(0), ruvia::DbDataType::kBigInt) }
        );
        const std::array<ruvia::DbOrderTerm, 2> historyOrder{ { ruvia::DbOrderTerm{ query.column("report_time", "items"),
                                                                                    ruvia::DbOrderDirection::kDesc,
                                                                                    ruvia::DbNullsOrder::kDefault },
                                                                ruvia::DbOrderTerm{ query.column("id", "items"), ruvia::DbOrderDirection::kDesc, ruvia::DbNullsOrder::kDefault } } };
        const auto list = query.coalesce(
            { query.aggregate(
                  "jsonb_agg",
                  { query.column("item", "items") },
                  false,
                  historyOrder
              ),
              emptyArray(query) }
        );
        const auto totalPages = query.cast(
            query.call("ceil", { query.binary(query.cast(total, ruvia::DbDataType::kNumeric), ruvia::DbBinaryOperator::kDivide, query.cast(query.value(pageSize), ruvia::DbDataType::kNumeric)) }),
            ruvia::DbDataType::kBigInt
        );
        query.select(query.cast(
                         query.call("jsonb_build_object", { textKey(query, "list"), list, textKey(query, "total"), total, textKey(query, "page"), query.cast(query.value(page), ruvia::DbDataType::kBigInt), textKey(query, "pageSize"), query.cast(query.value(pageSize), ruvia::DbDataType::kBigInt), textKey(query, "totalPages"), totalPages }),
                         ruvia::DbDataType::kText
                     ))
            .from("items");
        return query;
    }
};

inline AccessService& accessService() {
    return AccessService::instance();
}

} // namespace service::access
