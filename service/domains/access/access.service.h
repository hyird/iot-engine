#pragma once

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/web/ModelObject.h>
#include <ruvia/web/db/Db.h>

#include "service/common/http.h"
#include "service/common/uuid.h"
#include "service/middleware/auth.h"
#include "service/domains/device/device.service.h"
#include "service/domains/access/access.schema.h"
#include "service/domains/access/access.types.h"
#include "service/features/telemetry/latest.h"

namespace service::access {

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
    'status', key.status, 'scopes', key.scopes, 'expiresAt', key.expires_at,
    'lastUsedAt', key.last_used_at, 'lastUsedIp', key.last_used_ip,
    'remark', key.remark, 'createdAt', key.created_at, 'updatedAt', key.updated_at,
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
    COALESCE((protocol_params->>'remote_control')::boolean, TRUE))
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
        const auto rawKey = generateAccessKey();
        const auto prefix = rawKey.substr(0, 14);
        const auto keyHash = sha256(rawKey);
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
        co_await transaction.commit();

        co_return "{\"id\":" + jsonQuoted(id) + ",\"name\":" + jsonQuoted(name) +
            ",\"status\":" + jsonQuoted(status) + ",\"scopes\":" + scopeJson +
            ",\"expiresAt\":" + (expiresAt ? jsonQuoted(*expiresAt) : std::string("null")) +
            ",\"deviceIds\":" + stringArrayJson(devices) + ",\"accessKey\":" + jsonQuoted(rawKey) +
            ",\"accessKeyPrefix\":" + jsonQuoted(prefix) + "}";
    }

    ruvia::Task<void> updateKey(ruvia::Context& c, std::string_view id,
                                const ruvia::JsonValue& payload) {
        requireUuid(id, "调用配置 ID 无效");
        const auto existing = co_await requireKey(c, id);
        const auto name = optionalString(payload, "name", 64).value_or(existing.name);
        if (name.empty())
            service::common::fail(19002, "调用配置名称不能为空", 400);
        const auto status = optionalStatus(payload, existing.status);
        const auto scopes = payload.get<ruvia::Array<ruvia::String>>("scopes")
                                ? requiredScopes(payload)
                                : existing.scopes;
        const auto deviceField = jsonField(payload, "deviceIds");
        const auto devices = deviceField
                                 ? requiredUuids(payload, "deviceIds", "至少选择一个设备", 10000)
                                 : existing.deviceIds;
        co_await ensureDevicesAccessible(c, devices, scopes.contains(std::string(kScopeCommand)));
        co_await ensureKeyNameAvailable(c, name, std::string(id));

        const auto expiresAt = jsonField(payload, "expiresAt")
                                   ? optionalNullableString(payload, "expiresAt", 64)
                                   : existing.expiresAt;
        const auto remark = jsonField(payload, "remark")
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
        co_await transaction.commit();
    }

    ruvia::Task<std::string> rotateKey(ruvia::Context& c, std::string_view id) {
        requireUuid(id, "调用配置 ID 无效");
        const auto existing = co_await requireKey(c, id);
        const auto rawKey = generateAccessKey();
        const auto prefix = rawKey.substr(0, 14);
        const auto keyHash = sha256(rawKey);
        (void)co_await c.db().execute(R"sql(
UPDATE open_access_key SET access_key_hash = $2, access_key_prefix = $3, updated_at = NOW()
WHERE id = $1::uuid AND deleted_at IS NULL)sql",
                                      service::common::dbParams(id, keyHash, prefix));
        co_return "{\"id\":" + jsonQuoted(id) + ",\"name\":" + jsonQuoted(existing.name) +
            ",\"accessKey\":" + jsonQuoted(rawKey) + ",\"accessKeyPrefix\":" + jsonQuoted(prefix) +
            "}";
    }

    ruvia::Task<void> removeKey(ruvia::Context& c, std::string_view id) {
        requireUuid(id, "调用配置 ID 无效");
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
        co_await transaction.commit();
    }

    ruvia::Task<std::string> listWebhooks(ruvia::Context& c) {
        std::string where = " WHERE webhook.deleted_at IS NULL";
        std::vector<ruvia::DbValue> params;
        if (const auto key = c.req().query("accessKeyId"); key && !key->empty()) {
            requireUuid(*key, "调用配置 ID 无效");
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
    'headers', webhook.headers, 'eventTypes', webhook.event_types,
    'deviceIds', COALESCE(jsonb_agg(binding.device_id)
      FILTER (WHERE binding.device_id IS NOT NULL), '[]'::jsonb),
    'hasSecret', webhook.secret IS NOT NULL,
    'lastTriggeredAt', webhook.last_triggered_at,
    'lastSuccessAt', webhook.last_success_at,
    'lastFailureAt', webhook.last_failure_at,
    'lastHttpStatus', webhook.last_http_status, 'lastError', webhook.last_error,
    'createdAt', webhook.created_at, 'updatedAt', webhook.updated_at
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
        const auto headers = objectJson(payload, "headers", "{}");
        co_await validateHeaders(c, headers);
        const auto events = eventTypes(payload);
        const auto secret = optionalNullableString(payload, "secret", 255);
        co_await ensureWebhookNameAvailable(c, accessKeyId, name, std::nullopt);
        const auto id = service::common::nextUuidV7();
        const auto eventJson = stringArrayJson(events);
        const auto secretValue = secret.value_or("");
        (void)co_await c.db().execute(R"sql(
INSERT INTO open_webhook(
  id, access_key_id, name, url, status, timeout_seconds, headers, event_types, secret)
VALUES ($1::uuid, $2::uuid, $3, $4, $5, $6, $7::jsonb, $8::jsonb, NULLIF($9, ''))
)sql",
                                      service::common::dbParams(id, accessKeyId, name, url, status,
                                                                timeout, headers, eventJson,
                                                                secretValue));
        co_return "{\"id\":" + jsonQuoted(id) + ",\"accessKeyId\":" + jsonQuoted(accessKeyId) +
            ",\"name\":" + jsonQuoted(name) + ",\"url\":" + jsonQuoted(url) +
            ",\"status\":" + jsonQuoted(status) + ",\"timeoutSeconds\":" + std::to_string(timeout) +
            ",\"headers\":" + headers + ",\"eventTypes\":" + stringArrayJson(events) +
            ",\"hasSecret\":" + (secret && !secret->empty() ? "true" : "false") + "}";
    }

    ruvia::Task<void> updateWebhook(ruvia::Context& c, std::string_view id,
                                    const ruvia::JsonValue& payload) {
        requireUuid(id, "Webhook ID 无效");
        const auto existing = co_await requireWebhook(c, id);
        const auto accessKeyId = payload.get<ruvia::String>("accessKeyId")
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
            payload.get<ruvia::Int64>("timeoutSeconds")
                ? optionalInteger(payload, "timeoutSeconds", existing.timeout, 1, 30)
                : existing.timeout;
        const auto headers =
            jsonField(payload, "headers") ? objectJson(payload, "headers", "{}") : existing.headers;
        co_await validateHeaders(c, headers);
        const auto events = payload.get<ruvia::Array<ruvia::String>>("eventTypes")
                                ? eventTypes(payload)
                                : existing.events;
        const auto secret = jsonField(payload, "secret")
                                ? optionalNullableString(payload, "secret", 255)
                                : existing.secret;
        co_await ensureWebhookNameAvailable(c, accessKeyId, name, std::string(id));
        const auto eventJson = stringArrayJson(events);
        const auto secretValue = secret.value_or("");
        (void)co_await c.db().execute(R"sql(
UPDATE open_webhook
SET access_key_id = $2::uuid, name = $3, url = $4, status = $5,
    timeout_seconds = $6, headers = $7::jsonb, event_types = $8::jsonb,
    secret = NULLIF($9, ''), updated_at = NOW()
WHERE id = $1::uuid AND deleted_at IS NULL)sql",
                                      service::common::dbParams(id, accessKeyId, name, url, status,
                                                                timeout, headers, eventJson,
                                                                secretValue));
    }

    ruvia::Task<void> removeWebhook(ruvia::Context& c, std::string_view id) {
        requireUuid(id, "Webhook ID 无效");
        (void)co_await requireWebhook(c, id);
        (void)co_await c.db().execute(
            "UPDATE open_webhook SET deleted_at = NOW(), updated_at = NOW() "
            "WHERE id = $1::uuid AND deleted_at IS NULL",
            service::common::dbParams(id));
    }

    ruvia::Task<std::string> listLogs(ruvia::Context& c) {
        const auto pagination = page(c.req());
        std::string where = " WHERE TRUE";
        std::vector<ruvia::DbValue> params;
        const auto uuidFilter = [&](std::string_view query, std::string_view column) {
            if (const auto value = c.req().query(query); value && !value->empty()) {
                requireUuid(*value, std::string(query) + " 无效");
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
WITH filtered AS (
  SELECT log.*, key.name AS access_key_name, webhook.name AS webhook_name
  FROM open_access_log log
  LEFT JOIN open_access_key key ON key.id = log.access_key_id
  LEFT JOIN open_webhook webhook ON webhook.id = log.webhook_id
)sql" + where + R"sql(
), counted AS (SELECT COUNT(*) AS total FROM filtered), page AS (
  SELECT * FROM filtered ORDER BY created_at DESC, id DESC
  LIMIT $)sql" + std::to_string(limitParam) +
                    "::bigint OFFSET $" + std::to_string(offsetParam) + R"sql(::bigint
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
    'createdAt', created_at) ORDER BY created_at DESC, id DESC) FROM page), '[]'::jsonb),
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
        const auto raw = accessKey(c.req());
        if (raw.empty())
            service::common::fail(19010, "缺少 X-Access-Key", 401);
        const auto keyHash = sha256(raw);
        const auto rows = co_await c.db().query(R"sql(
SELECT id::text, name, status, scopes::text,
       expires_at IS NOT NULL AND expires_at <= NOW()
FROM open_access_key
WHERE access_key_hash = $1 AND deleted_at IS NULL LIMIT 1)sql",
                                                service::common::dbParams(keyHash));
        if (rows.rows().empty())
            service::common::fail(19010, "AccessKey 无效", 401);
        const auto& row = rows.rows().front();
        if (row[2].text() != "enabled")
            service::common::fail(19011, "AccessKey 已被禁用", 403);
        if (row[4].text() == "t")
            service::common::fail(19010, "AccessKey 已过期", 401);
        AccessSession session;
        session.id = std::string(row[0].text());
        session.name = std::string(row[1].text());
        session.scopes = parseStringArray(row[3].text());
        if (!requiredScope.empty() && !session.allows(requiredScope))
            service::common::fail(19011, "AccessKey 未开通所需权限", 403);
        const auto bindings =
            co_await c.db().query("SELECT device_id::text FROM open_access_key_device "
                                  "WHERE access_key_id = $1::uuid ORDER BY device_id",
                                  service::common::dbParams(session.id));
        for (const auto& binding : bindings.rows())
            session.deviceIds.emplace(binding[0].text());
        if (session.deviceIds.empty())
            service::common::fail(19011, "AccessKey 未配置可访问设备", 403);
        const auto remoteIp = clientIp(c);
        (void)co_await c.db().execute(
            "UPDATE open_access_key SET last_used_at = NOW(), last_used_ip = NULLIF($2, '') "
            "WHERE id = $1::uuid",
            service::common::dbParams(session.id, remoteIp));
        co_return session;
    }

    ruvia::Task<std::string> publicDevices(ruvia::Context& c, const AccessSession& session) {
        const auto pagination = page(c.req());
        const auto keyword = trim(c.req().query("keyword").value_or(""));
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
        co_await requireSessionDevice(c, session, deviceId);
        co_return co_await realtimeData(c, deviceId);
    }

    ruvia::Task<std::string> publicHistory(ruvia::Context& c, const AccessSession& session,
                                           std::string_view deviceId) {
        co_await requireSessionDevice(c, session, deviceId);
        const auto start = trim(c.req().query("startTime").value_or(""));
        const auto end = trim(c.req().query("endTime").value_or(""));
        if (start.empty() || end.empty())
            service::common::fail(19002, "startTime 和 endTime 不能为空", 400);
        const auto pagination = page(c.req());
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
        const auto pagination = page(c.req());
        std::string where = " WHERE binding.access_key_id = $1::uuid";
        std::vector<ruvia::DbValue> params{ruvia::DbValue{std::string_view(session.id)}};
        if (const auto device = c.req().query("deviceId"); device && !device->empty()) {
            requireUuid(*device, "设备 ID 无效");
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
WITH filtered AS (
  SELECT record.*, device.name AS device_name,
         device.protocol_params->>'device_code' AS device_code
  FROM open_alert_record record
  JOIN open_access_key_device binding ON binding.device_id = record.device_id
  JOIN device ON device.id = record.device_id
)sql" + where + R"sql(
), counted AS (SELECT COUNT(*) AS total FROM filtered), page AS (
  SELECT * FROM filtered ORDER BY triggered_at DESC, id DESC
  LIMIT $)sql" + std::to_string(limit) +
                "::bigint OFFSET $" + std::to_string(offset) + R"sql(::bigint
)
SELECT jsonb_build_object(
  'list', COALESCE((SELECT jsonb_agg(jsonb_build_object(
    'id', id, 'device', jsonb_build_object('id', device_id, 'code', device_code,
      'name', device_name), 'ruleId', rule_id, 'severity', severity,
    'status', status, 'message', message, 'time', triggered_at)
    ORDER BY triggered_at DESC, id DESC) FROM page), '[]'::jsonb),
  'total', (SELECT total FROM counted), 'page', $)sql" +
                std::to_string(pageParam) + "::bigint, 'pageSize', $" + std::to_string(limit) +
                "::bigint" + ", 'totalPages', CEIL((SELECT total FROM counted)::numeric / $" +
                std::to_string(limit) + "::numeric)::bigint)::text",
            params);
        co_return firstJson(rows);
    }

    ruvia::Task<void> writeLog(ruvia::Context& c, std::string_view direction,
                               std::string_view action, std::string_view status,
                               std::string_view accessKeyId = {}, std::string_view webhookId = {},
                               std::string_view eventType = {}, std::string_view method = {},
                               std::string_view target = {}, std::string_view requestIp = {},
                               std::int64_t httpStatus = 0, std::string_view deviceId = {},
                               std::string_view deviceCode = {}, std::string_view message = {},
                               std::string_view requestPayload = "{}",
                               std::string_view responsePayload = "{}") {
        const auto id = service::common::nextUuidV7();
        const auto safeMessage = sanitize(message);
        (void)co_await c.db().execute(
            R"sql(
INSERT INTO open_access_log(
  id, access_key_id, webhook_id, direction, action, event_type, status,
  http_method, target, request_ip, http_status, device_id, device_code,
  message, request_payload, response_payload)
VALUES ($1::uuid, NULLIF($2, '')::uuid, NULLIF($3, '')::uuid, $4, $5,
  NULLIF($6, ''), $7, NULLIF($8, ''), NULLIF($9, ''), NULLIF($10, ''),
  NULLIF($11, '0')::integer, NULLIF($12, '')::uuid, NULLIF($13, ''),
  NULLIF($14, ''), $15::jsonb, $16::jsonb))sql",
            service::common::dbParams(id, accessKeyId, webhookId, direction, action, eventType,
                                      status, method, target, requestIp, httpStatus, deviceId,
                                      deviceCode, safeMessage, requestPayload, responsePayload));
    }

    template <typename Context>
    ruvia::Task<std::string> realtimeData(Context& c, std::string_view deviceId) {
        const auto deviceRows = co_await c.db().query(R"sql(
SELECT device.name, device.protocol_params->>'device_code'
FROM device WHERE id = $1::uuid AND deleted_at IS NULL LIMIT 1)sql",
                                                      service::common::dbParams(deviceId));
        if (deviceRows.rows().empty())
            service::common::fail(19001, "设备不存在", 404);
        const auto& device = deviceRows.rows().front();
        const auto points = co_await configuredPoints(c, deviceId);
        auto pipeline = c.redis().pipeline();
        for (const auto& point : points)
            pipeline.hgetAll(
                service::telemetry::latest::elementKey(device[1].text(), point.id));
        const auto replies = co_await std::move(pipeline).exec();
        std::string body = "{\"device\":{\"id\":" + jsonQuoted(deviceId) +
                           ",\"code\":" + jsonQuoted(device[1].text()) +
                           ",\"name\":" + jsonQuoted(device[0].text()) + "},\"points\":[";
        for (std::size_t index = 0; index < points.size(); ++index) {
            if (index != 0)
                body.push_back(',');
            const auto data = redisHashField(replies[index], "data");
            std::string value = "null";
            std::string time = "null";
            if (!data.empty()) {
                if (const auto json = ruvia::JsonValue::parse(data)) {
                    if (const auto current = jsonField(*json, "value"))
                        value.assign(current->view());
                    if (const auto observed =
                            json->template get<ruvia::Int64>("observed_at_ms"))
                        time = jsonQuoted(iso8601(static_cast<std::int64_t>(*observed)));
                }
            }
            body += "{\"id\":" + jsonQuoted(points[index].id) +
                    ",\"name\":" + jsonQuoted(points[index].name) + ",\"value\":" + value +
                    ",\"unit\":" + jsonQuoted(points[index].unit) + ",\"time\":" + time + "}";
        }
        body += "]}";
        co_return body;
    }

  private:
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
        std::string headers{"{}"};
        std::set<std::string, std::less<>> events;
        std::optional<std::string> secret;
    };
    struct Point final {
        std::string id;
        std::string name;
        std::string unit;
    };

    template <typename Rows> static std::string firstJson(const Rows& rows) {
        if (rows.rows().empty() || rows.rows().front().empty() || rows.rows().front()[0].isNull())
            return "[]";
        return std::string(rows.rows().front()[0].text());
    }

    static std::string requiredString(const ruvia::JsonValue& payload, std::string_view field,
                                      std::string_view message, std::size_t maximum) {
        const auto value = payload.get<ruvia::String>(field);
        if (!value)
            service::common::fail(19002, std::string(message), 400);
        auto result = trim(value->view());
        if (result.empty() || result.size() > maximum)
            service::common::fail(19002, std::string(message), 400);
        return result;
    }

    static std::optional<std::string> optionalString(const ruvia::JsonValue& payload,
                                                     std::string_view field, std::size_t maximum) {
        const auto value = payload.get<ruvia::String>(field);
        if (!value)
            return std::nullopt;
        auto result = trim(value->view());
        if (result.size() > maximum)
            service::common::fail(19002, std::string(field) + " 长度超出限制", 400);
        return result;
    }

    static std::optional<std::string> optionalNullableString(const ruvia::JsonValue& payload,
                                                             std::string_view field,
                                                             std::size_t maximum) {
        const auto raw = jsonField(payload, field);
        if (!raw || raw->isNull())
            return std::nullopt;
        const auto value = payload.get<ruvia::String>(field);
        if (!value)
            service::common::fail(19002, std::string(field) + " 必须是字符串或 null", 400);
        auto result = trim(value->view());
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
        const auto value = payload.get<ruvia::Int64>(field);
        const auto result = value ? static_cast<std::int64_t>(*value) : fallback;
        if (result < minimum || result > maximum)
            service::common::fail(19002, std::string(field) + " 超出允许范围", 400);
        return result;
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
            requireUuid(value.view(), std::string(field) + " 包含无效 UUID");
            unique.emplace(value.view());
        }
        return {unique.begin(), unique.end()};
    }

    static std::string requiredUuid(const ruvia::JsonValue& payload, std::string_view field,
                                    std::string_view message) {
        const auto value = payload.get<ruvia::String>(field);
        if (!value)
            service::common::fail(19002, std::string(message), 400);
        requireUuid(value->view(), message);
        return std::string(value->view());
    }

    template <typename Range> static std::string stringArrayJson(const Range& values) {
        std::string result{"["};
        bool first = true;
        for (const auto& value : values) {
            if (!first)
                result.push_back(',');
            first = false;
            result += jsonQuoted(value);
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
        const auto value = jsonField(payload, field);
        if (!value)
            return std::string(fallback);
        if (!value->isObject())
            service::common::fail(19002, std::string(field) + " 必须是对象", 400);
        return std::string(value->view());
    }

    static std::set<std::string, std::less<>> eventTypes(const ruvia::JsonValue& payload) {
        const auto values = payload.get<ruvia::Array<ruvia::String>>("eventTypes");
        if (!values || values->empty())
            return {"device.data.reported"};
        std::set<std::string, std::less<>> result;
        for (const auto& value : *values) {
            if (!supportedEvent(value.view()))
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
                       'content-type', 'user-agent')
))sql",
                                                service::common::dbParams(headers));
        if (rows.rows().front()[0].text() != "t")
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
        if (std::stoll(std::string(rows.rows().front()[0].text())) !=
            static_cast<std::int64_t>(ids.size()))
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
        if (rows.rows().front()[0].text() == "t")
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
        if (rows.rows().front()[0].text() == "t")
            service::common::fail(19003, "同一调用配置下的 Webhook 名称已存在", 409);
    }

    ruvia::Task<KeyState> requireKey(ruvia::Context& c, std::string_view id) {
        const auto rows = co_await c.db().query(R"sql(
SELECT name, status, scopes::text, expires_at::text, remark
FROM open_access_key WHERE id = $1::uuid AND deleted_at IS NULL LIMIT 1)sql",
                                                service::common::dbParams(id));
        if (rows.rows().empty())
            service::common::fail(19001, "调用配置不存在", 404);
        const auto& row = rows.rows().front();
        KeyState state;
        state.name = std::string(row[0].text());
        state.status = std::string(row[1].text());
        state.scopes = parseStringArray(row[2].text());
        if (!row[3].isNull())
            state.expiresAt = std::string(row[3].text());
        if (!row[4].isNull())
            state.remark = std::string(row[4].text());
        const auto devices =
            co_await c.db().query("SELECT device_id::text FROM open_access_key_device "
                                  "WHERE access_key_id = $1::uuid ORDER BY device_id",
                                  service::common::dbParams(id));
        for (const auto& device : devices.rows())
            state.deviceIds.emplace_back(device[0].text());
        co_return state;
    }

    ruvia::Task<WebhookState> requireWebhook(ruvia::Context& c, std::string_view id) {
        const auto rows = co_await c.db().query(R"sql(
SELECT access_key_id::text, name, url, status, timeout_seconds,
       headers::text, event_types::text, secret
FROM open_webhook WHERE id = $1::uuid AND deleted_at IS NULL LIMIT 1)sql",
                                                service::common::dbParams(id));
        if (rows.rows().empty())
            service::common::fail(19001, "Webhook 不存在", 404);
        const auto& row = rows.rows().front();
        WebhookState state;
        state.accessKeyId = std::string(row[0].text());
        state.name = std::string(row[1].text());
        state.url = std::string(row[2].text());
        state.status = std::string(row[3].text());
        state.timeout = std::stoll(std::string(row[4].text()));
        state.headers = std::string(row[5].text());
        state.events = parseStringArray(row[6].text());
        if (!row[7].isNull())
            state.secret = std::string(row[7].text());
        co_return state;
    }

    static ruvia::Task<void> requireSessionDevice(ruvia::Context& c, const AccessSession& session,
                                                  std::string_view deviceId) {
        requireUuid(deviceId, "设备 ID 无效");
        if (!session.allowsDevice(deviceId))
            service::common::fail(19011, "AccessKey 无权访问该设备", 403);
        const auto rows = co_await c.db().query(
            "SELECT EXISTS(SELECT 1 FROM device WHERE id = $1::uuid AND deleted_at IS NULL)",
            service::common::dbParams(deviceId));
        if (rows.rows().front()[0].text() != "t")
            service::common::fail(19001, "设备不存在", 404);
    }

    static std::string redisHashField(const ruvia::RedisValue& value, std::string_view name) {
        if (value.kind() != ruvia::RedisValue::Kind::kArray)
            return {};
        const auto values = value.array();
        for (std::size_t index = 0; index + 1 < values.size(); index += 2)
            if (values[index].kind() == ruvia::RedisValue::Kind::kString &&
                values[index + 1].kind() == ruvia::RedisValue::Kind::kString &&
                values[index].string() == name)
                return std::string(values[index + 1].string());
        return {};
    }

    template <typename Context>
    static ruvia::Task<std::vector<Point>> configuredPoints(Context& c,
                                                            std::string_view deviceId) {
        const auto rows = co_await c.db().query(R"sql(
WITH configured AS (
  SELECT element, 1 AS protocol_order, position AS function_order, 0::bigint AS element_order
  FROM device d JOIN protocol_config p ON p.id = d.protocol_config_id AND p.protocol = 'Modbus'
  CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'registers', '[]'::jsonb))
    WITH ORDINALITY AS entry(element, position)
  WHERE d.id = $1::uuid AND d.deleted_at IS NULL
  UNION ALL
  SELECT element, 2, position, 0::bigint
  FROM device d JOIN protocol_config p ON p.id = d.protocol_config_id AND p.protocol = 'S7'
  CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'areas', '[]'::jsonb))
    WITH ORDINALITY AS entry(element, position)
  WHERE d.id = $1::uuid AND d.deleted_at IS NULL
  UNION ALL
  SELECT element, 3, function_position, element_position
  FROM device d JOIN protocol_config p ON p.id = d.protocol_config_id AND p.protocol = 'SL651'
  CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'funcs', '[]'::jsonb))
    WITH ORDINALITY AS functions(function, function_position)
  CROSS JOIN LATERAL jsonb_array_elements(COALESCE(function->'elements', '[]'::jsonb))
    WITH ORDINALITY AS elements(element, element_position)
  WHERE d.id = $1::uuid AND d.deleted_at IS NULL AND function->>'dir' = 'UP'
)
SELECT element->>'id', COALESCE(element->>'name', element->>'id'),
       COALESCE(element->>'unit', '')
FROM configured WHERE COALESCE(element->>'encode', '') <> 'JPEG'
ORDER BY protocol_order, function_order, element_order)sql",
                                                service::common::dbParams(deviceId));
        std::vector<Point> result;
        result.reserve(rows.rows().size());
        for (const auto& row : rows.rows())
            result.push_back({std::string(row[0].text()), std::string(row[1].text()),
                              std::string(row[2].text())});
        co_return result;
    }

    static std::string historySql() {
        return R"sql(
WITH device_ref AS (
  SELECT d.id, d.name, d.protocol_params->>'device_code' AS code
  FROM device d WHERE d.id = $1::uuid AND d.deleted_at IS NULL
), configured AS (
  SELECT element, 1 AS protocol_order, position AS function_order, 0::bigint AS element_order
  FROM device d JOIN protocol_config p ON p.id = d.protocol_config_id AND p.protocol = 'Modbus'
  CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'registers', '[]'::jsonb))
    WITH ORDINALITY AS entry(element, position) WHERE d.id = $1::uuid
  UNION ALL
  SELECT element, 2, position, 0::bigint
  FROM device d JOIN protocol_config p ON p.id = d.protocol_config_id AND p.protocol = 'S7'
  CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'areas', '[]'::jsonb))
    WITH ORDINALITY AS entry(element, position) WHERE d.id = $1::uuid
  UNION ALL
  SELECT element, 3, function_position, element_position
  FROM device d JOIN protocol_config p ON p.id = d.protocol_config_id AND p.protocol = 'SL651'
  CROSS JOIN LATERAL jsonb_array_elements(COALESCE(p.config->'funcs', '[]'::jsonb))
    WITH ORDINALITY AS functions(function, function_position)
  CROSS JOIN LATERAL jsonb_array_elements(COALESCE(function->'elements', '[]'::jsonb))
    WITH ORDINALITY AS elements(element, element_position)
  WHERE d.id = $1::uuid AND function->>'dir' = 'UP'
), point AS (
  SELECT element->>'id' AS id, COALESCE(element->>'name', element->>'id') AS name,
         COALESCE(element->>'unit', '') AS unit, protocol_order, function_order, element_order
  FROM configured WHERE COALESCE(element->>'encode', '') <> 'JPEG'
), filtered AS (
  SELECT data.*, COUNT(*) OVER () AS total
  FROM device_data data
  WHERE data.device_id = $1::uuid
    AND data.report_time >= $2::timestamptz AND data.report_time <= $3::timestamptz
    AND jsonb_typeof(data.data->'values') = 'object'
  ORDER BY data.report_time DESC, data.id DESC LIMIT $4::bigint OFFSET $5::bigint
), items AS (
  SELECT filtered.report_time, filtered.id, filtered.total,
    jsonb_build_object(
      'device', jsonb_build_object('id', device_ref.id, 'code', device_ref.code,
        'name', device_ref.name),
      'points', COALESCE(jsonb_agg(jsonb_build_object(
        'id', point.id, 'name', point.name,
        'value', filtered.data->'values'->point.id->'value', 'unit', point.unit,
        'time', filtered.report_time)
        ORDER BY point.protocol_order, point.function_order, point.element_order), '[]'::jsonb)
    ) AS item
  FROM filtered CROSS JOIN device_ref CROSS JOIN point
  GROUP BY filtered.report_time, filtered.id, filtered.total,
           device_ref.id, device_ref.code, device_ref.name
)
SELECT jsonb_build_object(
  'list', COALESCE(jsonb_agg(item ORDER BY report_time DESC, id DESC), '[]'::jsonb),
  'total', COALESCE(MAX(total), 0), 'page', $6::bigint, 'pageSize', $4::bigint,
  'totalPages', CEIL(COALESCE(MAX(total), 0)::numeric / $4::numeric)::bigint)::text
FROM items)sql";
    }
};

inline AccessService& accessService() { return AccessService::instance(); }

} // namespace service::access
