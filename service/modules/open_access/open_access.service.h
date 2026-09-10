#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/web/ModelObject.h>
#include <ruvia/web/db/Db.h>
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
        co_return firstJson(co_await c.db().query(R"sql(
SELECT COALESCE(jsonb_agg(item ORDER BY created_at DESC), '[]'::jsonb)::text
FROM (
  SELECT jsonb_build_object(
    'id', key.id, 'name', key.name, 'accessKeyPrefix', key.access_key_prefix,
    'status', key.status, 'scopes', key.scopes,
    'expiresAt', iot_utc_timestamp(key.expires_at),
    'lastUsedAt', iot_utc_timestamp(key.last_used_at), 'lastUsedIp', key.last_used_ip,
    'remark', key.remark, 'createdAt', iot_utc_timestamp(key.created_at),
    'updatedAt', iot_utc_timestamp(key.updated_at),
    'webhookCount', COUNT(DISTINCT webhook.id),
    'deviceIds', COALESCE(jsonb_agg(DISTINCT binding.device_id)
      FILTER (WHERE binding.device_id IS NOT NULL), '[]'::jsonb)
  ) AS item, key.created_at
  FROM open_access_key key
  LEFT JOIN open_access_key_device binding ON binding.access_key_id = key.id
  LEFT JOIN open_webhook webhook ON webhook.access_key_id = key.id
    AND webhook.deleted_at IS NULL
  WHERE key.deleted_at IS NULL
  GROUP BY key.id
) listed)sql"));
    }

    ruvia::Task<std::string> deviceOptions(ruvia::Context& c) {
        const auto actor = co_await service::device::deviceAccessService().actor(c);
        co_return firstJson(
            co_await c.db().query(service::device::DeviceAccessService::scopedDevicesCte() + R"sql(
SELECT COALESCE(jsonb_agg(jsonb_build_object(
  'id', id, 'name', name, 'deviceCode', protocol_params->>'device_code',
  'canCommand', access_rank >= 2 AND
    CASE
      WHEN protocol_params IS NULL OR NOT (protocol_params ? 'remote_control') THEN TRUE
      WHEN jsonb_typeof(protocol_params->'remote_control') = 'boolean'
        THEN protocol_params->>'remote_control' = 'true'
      WHEN jsonb_typeof(protocol_params->'remote_control') = 'string'
        THEN lower(btrim(protocol_params->>'remote_control')) = 'true'
      ELSE FALSE
    END)
  ORDER BY name, id), '[]'::jsonb)::text
FROM scoped_device WHERE access_rank > 0)sql",
                                  service::common::dbParams(actor.userId, actor.departmentId,
                                                            actor.superadmin ? "true" : "false")));
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
        (void)co_await transaction.execute(
            R"sql(
INSERT INTO open_access_key(
  id, name, access_key_prefix, access_key_hash, status, scopes,
  expires_at, remark, created_by)
VALUES ($1::uuid, $2, $3, $4, $5, $6::jsonb,
        NULLIF($7, '')::timestamptz, NULLIF($8, ''), $9::uuid))sql",
            service::common::dbParams(id, name, prefix, keyHash, status, scopeJson, expiresAtValue,
                                      remarkValue, principal.userId));
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
        (void)co_await transaction.execute(
            R"sql(
UPDATE open_access_key
SET name = $2, status = $3, scopes = $4::jsonb,
    expires_at = NULLIF($5, '')::timestamptz, remark = NULLIF($6, ''), updated_at = NOW()
WHERE id = $1::uuid AND deleted_at IS NULL)sql",
            service::common::dbParams(id, name, status, scopeJson, expiresAtValue, remarkValue));
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
        (void)co_await transaction.execute(R"sql(
UPDATE open_access_key SET access_key_hash = $2, access_key_prefix = $3, updated_at = NOW()
WHERE id = $1::uuid AND deleted_at IS NULL)sql",
                                        service::common::dbParams(id, keyHash, prefix));
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
        (void)co_await transaction.execute(
            "UPDATE open_webhook SET deleted_at = NOW(), updated_at = NOW() "
            "WHERE access_key_id = $1::uuid AND deleted_at IS NULL",
            service::common::dbParams(id));
        (void)co_await transaction.execute(
            "UPDATE open_access_key SET deleted_at = NOW(), updated_at = NOW() "
            "WHERE id = $1::uuid AND deleted_at IS NULL",
            service::common::dbParams(id));
        co_await service::system::OutboxService::enqueueConfigEvent(transaction, "access_key", "deleted", id);
        co_await transaction.commit();
        co_await refreshProjection(c);
    }

    ruvia::Task<std::string> listWebhooks(ruvia::Context& c) {
        std::string where = " WHERE webhook.deleted_at IS NULL";
        std::vector<ruvia::DbValue> params;
        if (const auto key = c.req().query("accessKeyId"); key && !key->empty()) {
            service::common::requireUuid(19002, *key, "调用配置 ID 无效");
            params.emplace_back(*key);
            where += " AND webhook.access_key_id = $1::uuid";
        }
        co_return firstJson(co_await c.db().query(R"sql(
SELECT COALESCE(jsonb_agg(item ORDER BY created_at DESC), '[]'::jsonb)::text
FROM (
  SELECT jsonb_build_object(
    'id', webhook.id, 'accessKeyId', webhook.access_key_id,
    'accessKeyName', key.name, 'name', webhook.name, 'url', webhook.url,
    'status', webhook.status, 'timeoutSeconds', webhook.timeout_seconds,
    'skipTlsVerify', webhook.skip_tls_verify,
    'headers', webhook.headers, 'eventTypes', webhook.event_types,
    'deviceIds', COALESCE(jsonb_agg(binding.device_id)
      FILTER (WHERE binding.device_id IS NOT NULL), '[]'::jsonb),
    'hasSecret', webhook.secret IS NOT NULL,
    'lastTriggeredAt', iot_utc_timestamp(webhook.last_triggered_at),
    'lastSuccessAt', iot_utc_timestamp(webhook.last_success_at),
    'lastFailureAt', iot_utc_timestamp(webhook.last_failure_at),
    'lastHttpStatus', webhook.last_http_status, 'lastError', webhook.last_error,
    'createdAt', iot_utc_timestamp(webhook.created_at),
    'updatedAt', iot_utc_timestamp(webhook.updated_at)
  ) AS item, webhook.created_at
  FROM open_webhook webhook
  JOIN open_access_key key ON key.id = webhook.access_key_id AND key.deleted_at IS NULL
  LEFT JOIN open_access_key_device binding ON binding.access_key_id = key.id
)sql" + where + " GROUP BY webhook.id, key.id) listed",
                                                  params));
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
        (void)co_await transaction.execute(R"sql(
INSERT INTO open_webhook(
  id, access_key_id, name, url, status, timeout_seconds, skip_tls_verify,
  headers, event_types, secret)
VALUES ($1::uuid, $2::uuid, $3, $4, $5, $6, $7::boolean, $8::jsonb, $9::jsonb,
        NULLIF($10, ''))
)sql",
                                      service::common::dbParams(id, accessKeyId, name, url, status,
                                                                 timeout,
                                                                 skipTlsVerify ? "true" : "false",
                                                                 headers, eventJson, secretValue));
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
        (void)co_await transaction.execute(R"sql(
UPDATE open_webhook
SET access_key_id = $2::uuid, name = $3, url = $4, status = $5,
    timeout_seconds = $6, skip_tls_verify = $7::boolean, headers = $8::jsonb,
    event_types = $9::jsonb, secret = NULLIF($10, ''), updated_at = NOW()
WHERE id = $1::uuid AND deleted_at IS NULL)sql",
                                      service::common::dbParams(id, accessKeyId, name, url, status,
                                                                 timeout,
                                                                 skipTlsVerify ? "true" : "false",
                                                                 headers, eventJson, secretValue));
        co_await service::system::OutboxService::enqueueConfigEvent(transaction, "webhook", "updated", id);
        co_await transaction.commit();
    }

    ruvia::Task<void> removeWebhook(ruvia::Context& c, std::string_view id) {
        service::common::requireUuid(19002, id, "Webhook ID 无效");
        (void)co_await requireWebhook(c, id);
        auto transaction = co_await c.db().beginTransaction();
        (void)co_await transaction.execute(
            "UPDATE open_webhook SET deleted_at = NOW(), updated_at = NOW() "
            "WHERE id = $1::uuid AND deleted_at IS NULL",
            service::common::dbParams(id));
        co_await service::system::OutboxService::enqueueConfigEvent(transaction, "webhook", "deleted", id);
        co_await transaction.commit();
    }

    ruvia::Task<std::string> listLogs(ruvia::Context& c) {
        const auto pagination = service::common::page(c.req());
        std::string where = " WHERE TRUE";
        std::vector<ruvia::DbValue> params;
        const auto uuidFilter = [&](std::string_view query, std::string_view column) {
            if (const auto value = c.req().query(query); value && !value->empty()) {
                service::common::requireUuid(19002, *value, std::string(query) + " 无效");
                params.emplace_back(*value);
                where += " AND " + std::string(column) + " = $" + std::to_string(params.size()) +
                         "::uuid";
            }
        };
        const auto textFilter = [&](std::string_view query, std::string_view column) {
            if (const auto value = c.req().query(query); value && !value->empty()) {
                params.emplace_back(*value);
                where += " AND " + std::string(column) + " = $" + std::to_string(params.size());
            }
        };
        uuidFilter("accessKeyId", "log.access_key_id");
        uuidFilter("webhookId", "log.webhook_id");
        uuidFilter("deviceId", "log.device_id");
        textFilter("direction", "log.direction");
        textFilter("action", "log.action");
        textFilter("status", "log.status");
        textFilter("eventType", "log.event_type");
        params.emplace_back(pagination.pageSize);
        const auto limitParam = params.size();
        params.emplace_back(pagination.offset);
        const auto offsetParam = params.size();
        params.emplace_back(pagination.page);
        const auto pageParam = params.size();
        params.emplace_back(pagination.pageSize);
        const auto pageSizeParam = params.size();
        try {
            const auto rows = co_await c.db().query(
                R"sql(
WITH counted AS (
  SELECT COUNT(*) AS total FROM open_access_log log
)sql" + where + R"sql(
), page_rows AS (
  SELECT log.* FROM open_access_log log
)sql" + where + R"sql(
  ORDER BY log.created_at DESC, log.id DESC
  LIMIT $)sql" + std::to_string(limitParam) +
                     "::bigint OFFSET $" + std::to_string(offsetParam) + R"sql(::bigint
), page AS (
  SELECT page_rows.*, key.name AS access_key_name, webhook.name AS webhook_name
  FROM page_rows
  LEFT JOIN open_access_key key ON key.id = page_rows.access_key_id
  LEFT JOIN open_webhook webhook ON webhook.id = page_rows.webhook_id
)
SELECT jsonb_build_object(
  'list', COALESCE((SELECT jsonb_agg(jsonb_build_object(
    'id', id, 'accessKeyId', access_key_id, 'accessKeyName', access_key_name,
    'webhookId', webhook_id, 'webhookName', webhook_name,
    'direction', direction, 'action', action, 'eventType', event_type,
    'status', status, 'httpMethod', http_method, 'target', target,
    'requestIp', request_ip, 'httpStatus', http_status, 'deviceId', device_id,
    'deviceCode', device_code, 'message', message,
    'requestPayload', request_payload, 'responsePayload', response_payload,
    'createdAt', iot_utc_timestamp(created_at))
    ORDER BY created_at DESC, id DESC) FROM page), '[]'::jsonb),
  'total', (SELECT total FROM counted), 'page', $)sql" +
                    std::to_string(pageParam) + "::bigint" + ", 'pageSize', $" +
                    std::to_string(pageSizeParam) + "::bigint" +
                    ", 'totalPages', CEIL((SELECT total FROM counted)::numeric / $" +
                    std::to_string(pageSizeParam) + "::numeric" + ")::bigint)::text",
                params);
            co_return firstJson(rows);
        } catch (const std::exception& error) {
            std::cerr << "open access log query failed: " << error.what() << '\n';
            throw;
        }
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
        const auto rows = co_await c.db().query(
            R"sql(
WITH visible AS (
  SELECT device.id, device.name, device.protocol_params->>'device_code' AS code
  FROM device
  JOIN open_access_key_device binding ON binding.device_id = device.id
  WHERE binding.access_key_id = $1::uuid AND device.deleted_at IS NULL
    AND ($2 = '' OR device.id::text ILIKE '%' || $2 || '%'
      OR device.name ILIKE '%' || $2 || '%'
      OR COALESCE(device.protocol_params->>'device_code', '') ILIKE '%' || $2 || '%')
), counted AS (SELECT COUNT(*) AS total FROM visible), page AS (
  SELECT * FROM visible ORDER BY name, id LIMIT $3::bigint OFFSET $4::bigint
)
SELECT jsonb_build_object(
  'list', COALESCE((SELECT jsonb_agg(jsonb_build_object(
    'id', id, 'code', code, 'name', name) ORDER BY name, id) FROM page), '[]'::jsonb),
  'total', (SELECT total FROM counted), 'page', $5::bigint, 'pageSize', $3::bigint,
  'totalPages', CEIL((SELECT total FROM counted)::numeric / $3::numeric)::bigint)::text)sql",
            service::common::dbParams(session.id, keyword, pagination.pageSize, pagination.offset,
                                      pagination.page));
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
            const auto rows = co_await c.db().query(
                historySql(), service::common::dbParams(deviceId, start, end, pagination.pageSize,
                                                        pagination.offset, pagination.page));
            co_return firstJson(rows);
        } catch (const std::exception&) {
            service::common::fail(19002, "时间范围格式错误", 400);
        }
    }

    ruvia::Task<std::string> publicAlerts(ruvia::Context& c, const AccessSession& session) {
        const auto pagination = service::common::page(c.req());
        std::string where = " WHERE binding.access_key_id = $1::uuid";
        std::vector<ruvia::DbValue> params{ruvia::DbValue{std::string_view(session.id)}};
        if (const auto device = c.req().query("deviceId"); device && !device->empty()) {
            service::common::requireUuid(19002, *device, "设备 ID 无效");
            if (!session.allowsDevice(*device))
                service::common::fail(19011, "AccessKey 无权访问该设备", 403);
            params.emplace_back(*device);
            where += " AND record.device_id = $2::uuid";
        }
        const auto filter = [&](std::string_view name, std::string_view column) {
            if (const auto value = c.req().query(name); value && !value->empty()) {
                params.emplace_back(*value);
                where += " AND " + std::string(column) + " = $" + std::to_string(params.size());
            }
        };
        filter("status", "record.status");
        filter("severity", "record.severity");
        params.emplace_back(pagination.pageSize);
        const auto limit = params.size();
        params.emplace_back(pagination.offset);
        const auto offset = params.size();
        params.emplace_back(pagination.page);
        const auto pageParam = params.size();
        const auto rows = co_await c.db().query(
            R"sql(
WITH counted AS (
  SELECT COUNT(*) AS total
  FROM open_alert_record record
  JOIN open_access_key_device binding ON binding.device_id = record.device_id
)sql" + where + R"sql(
), page_rows AS (
  SELECT record.*
  FROM open_alert_record record
  JOIN open_access_key_device binding ON binding.device_id = record.device_id
)sql" + where + R"sql(
  ORDER BY record.triggered_at DESC, record.id DESC
  LIMIT $)sql" + std::to_string(limit) +
                 "::bigint OFFSET $" + std::to_string(offset) + R"sql(::bigint
), page AS (
  SELECT page_rows.*, device.name AS device_name,
         device.protocol_params->>'device_code' AS device_code
  FROM page_rows
  JOIN device ON device.id = page_rows.device_id
)
SELECT jsonb_build_object(
  'list', COALESCE((SELECT jsonb_agg(jsonb_build_object(
    'id', id, 'device', jsonb_build_object('id', device_id, 'code', device_code,
      'name', device_name), 'ruleId', rule_id, 'severity', severity,
    'status', status, 'message', message, 'time', iot_utc_timestamp(triggered_at))
    ORDER BY triggered_at DESC, id DESC) FROM page), '[]'::jsonb),
  'total', (SELECT total FROM counted), 'page', $)sql" +
                std::to_string(pageParam) + "::bigint, 'pageSize', $" + std::to_string(limit) +
                "::bigint" + ", 'totalPages', CEIL((SELECT total FROM counted)::numeric / $" +
                std::to_string(limit) + "::numeric)::bigint)::text",
            params);
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
        const auto rows = co_await c.db().query(R"sql(
SELECT NOT EXISTS (
  SELECT 1 FROM jsonb_each($1::jsonb)
  WHERE key !~ '^[!#$%&''*+.^_`|~0-9A-Za-z-]+$'
     OR jsonb_typeof(value) <> 'string'
     OR value #>> '{}' ~ E'[\\r\\n]'
     OR lower(key) IN ('host', 'content-length', 'connection', 'x-iot-event',
                       'x-iot-timestamp', 'x-iot-delivery', 'x-iot-signature',
                       'content-type', 'user-agent', 'transfer-encoding', 'trailer',
                       'te', 'upgrade', 'expect', 'proxy-connection')
))sql",
                                                service::common::dbParams(headers));
        if (rows.front()[0].value().value_or(std::string_view{}) != "t")
            service::common::fail(19002, "自定义 Header 包含非法或保留字段", 400);
    }

    ruvia::Task<void> ensureDevicesAccessible(ruvia::Context& c,
                                              const std::vector<std::string>& ids,
                                              bool requireOperate) {
        const auto actor = co_await service::device::deviceAccessService().actor(c);
        std::vector<ruvia::DbValue> params = service::common::dbParams(
            actor.userId, actor.departmentId, actor.superadmin ? "true" : "false");
        std::string in;
        for (const auto& id : ids) {
            params.emplace_back(std::string_view(id));
            if (!in.empty())
                in.push_back(',');
            in += "$" + std::to_string(params.size()) + "::uuid";
        }
        const auto rows =
            co_await c.db().query(service::device::DeviceAccessService::scopedDevicesCte() +
                                      " SELECT COUNT(*) FROM scoped_device WHERE id IN (" + in +
                                      ") AND access_rank >= " + (requireOperate ? "2" : "1"),
                                  params);
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
        (void)co_await transaction.execute(
            "DELETE FROM open_access_key_device WHERE access_key_id = $1::uuid",
            service::common::dbParams(keyId));
        for (const auto& device : devices)
            (void)co_await transaction.execute(
                "INSERT INTO open_access_key_device(access_key_id, device_id) "
                "VALUES ($1::uuid, $2::uuid)",
                service::common::dbParams(keyId, device));
    }

    ruvia::Task<void> ensureKeyNameAvailable(ruvia::Context& c, std::string_view name,
                                             std::optional<std::string> except) {
        const auto exceptValue = except.value_or("");
        const auto rows = co_await c.db().query(R"sql(
SELECT EXISTS(SELECT 1 FROM open_access_key
WHERE name = $1 AND deleted_at IS NULL AND ($2 = '' OR id <> $2::uuid))
)sql",
                                                service::common::dbParams(name, exceptValue));
        if (rows.front()[0].value().value_or(std::string_view{}) == "t")
            service::common::fail(19003, "调用配置名称已存在", 409);
    }

    ruvia::Task<void> ensureWebhookNameAvailable(ruvia::Context& c, std::string_view accessKeyId,
                                                 std::string_view name,
                                                 std::optional<std::string> except) {
        const auto exceptValue = except.value_or("");
        const auto rows =
            co_await c.db().query(R"sql(
SELECT EXISTS(SELECT 1 FROM open_webhook
WHERE access_key_id = $1::uuid AND name = $2 AND deleted_at IS NULL
  AND ($3 = '' OR id <> $3::uuid))
)sql",
                                  service::common::dbParams(accessKeyId, name, exceptValue));
        if (rows.front()[0].value().value_or(std::string_view{}) == "t")
            service::common::fail(19003, "同一调用配置下的 Webhook 名称已存在", 409);
    }

    ruvia::Task<KeyState> requireKey(ruvia::Context& c, std::string_view id) {
        const auto rows = co_await c.db().query(R"sql(
SELECT name, status, scopes::text, iot_utc_timestamp(expires_at), remark
FROM open_access_key WHERE id = $1::uuid AND deleted_at IS NULL LIMIT 1)sql",
                                                service::common::dbParams(id));
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
        const auto devices =
            co_await c.db().query("SELECT device_id::text FROM open_access_key_device "
                                  "WHERE access_key_id = $1::uuid ORDER BY device_id",
                                  service::common::dbParams(id));
        for (const auto& device : devices)
            state.deviceIds.emplace_back(device[0].value().value_or(std::string_view{}));
        co_return state;
    }

    ruvia::Task<WebhookState> requireWebhook(ruvia::Context& c, std::string_view id) {
        const auto rows = co_await c.db().query(R"sql(
SELECT access_key_id::text, name, url, status, timeout_seconds,
       CASE WHEN skip_tls_verify THEN '1' ELSE '0' END,
       headers::text, event_types::text, secret
FROM open_webhook WHERE id = $1::uuid AND deleted_at IS NULL LIMIT 1)sql",
                                                service::common::dbParams(id));
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
        const auto rows = co_await c.db().query(
            "SELECT EXISTS(SELECT 1 FROM device WHERE id = $1::uuid AND deleted_at IS NULL)",
            service::common::dbParams(deviceId));
        if (rows.front()[0].value().value_or(std::string_view{}) != "t")
            service::common::fail(19001, "设备不存在", 404);
    }

    static std::string historySql() {
        return R"sql(
WITH device_ref AS (
  SELECT d.id, d.name, d.protocol_params->>'device_code' AS code
  FROM device d WHERE d.id = $1::uuid AND d.deleted_at IS NULL
), counted AS (
  SELECT COUNT(*) AS total
  FROM device_data data
  WHERE data.device_id = $1::uuid
    AND data.report_time >= $2::timestamptz AND data.report_time <= $3::timestamptz
    AND jsonb_typeof(data.data->'values') = 'object'
), filtered AS (
  SELECT data.id, data.report_time, data.data
  FROM device_data data
  WHERE data.device_id = $1::uuid
    AND data.report_time >= $2::timestamptz AND data.report_time <= $3::timestamptz
    AND jsonb_typeof(data.data->'values') = 'object'
  ORDER BY data.report_time DESC, data.id DESC LIMIT $4::bigint OFFSET $5::bigint
), items AS (
  SELECT filtered.report_time, filtered.id,
    jsonb_build_object(
      'device', jsonb_build_object('id', device_ref.id, 'code', device_ref.code,
        'name', device_ref.name),
      'points', COALESCE(jsonb_agg(jsonb_build_object(
        'id', point.key, 'name', COALESCE(point.value->>'name', point.key),
        'value', CASE
          WHEN jsonb_typeof(point.value->'value') = 'boolean'
          THEN to_jsonb(CASE WHEN (point.value->>'value')::boolean
                             THEN 1 ELSE 0 END)
          ELSE point.value->'value' END,
        'unit', COALESCE(point.value->>'unit', ''),
        'time', iot_utc_timestamp(filtered.report_time))
        ORDER BY point.key) FILTER (WHERE point.key IS NOT NULL), '[]'::jsonb)
    ) AS item
  FROM filtered CROSS JOIN device_ref
  LEFT JOIN LATERAL jsonb_each(filtered.data->'values') point
    ON COALESCE(point.value->>'type','') <> 'JPEG'
  GROUP BY filtered.report_time, filtered.id,
            device_ref.id, device_ref.code, device_ref.name
)
SELECT jsonb_build_object(
  'list', COALESCE(jsonb_agg(item ORDER BY report_time DESC, id DESC), '[]'::jsonb),
  'total', COALESCE((SELECT total FROM counted), 0),
  'page', $6::bigint, 'pageSize', $4::bigint,
  'totalPages',
    CEIL(COALESCE((SELECT total FROM counted), 0)::numeric / $4::numeric)::bigint)::text
FROM items)sql";
    }
};

inline AccessService& accessService() { return AccessService::instance(); }

} // namespace service::access
