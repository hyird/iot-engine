#pragma once
#include "service/common/http.h"


#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/web/ModelObject.h>

#include "service/common/uuid.h"
#include "service/features/event/event.transport.h"

namespace service::access::session {

template <typename Context>
ruvia::Task<void> refresh(Context& context, bool onlyIfMissing = false) {
    // Serialize the database snapshot and Redis pointer swap across service instances.
    // The lock is held only by cold-path configuration projection, never by API reads.
    auto transaction = co_await context.db().beginTransaction();
    (void)co_await transaction.query(
        "SELECT pg_advisory_xact_lock(734622::bigint)");
    if (onlyIfMissing) {
        const auto active = co_await service::message::redis::command(
            context.redis(), {"GET", std::string(kActiveVersionKey)});
        if (!active.null()) {
            if (active.kind() != ruvia::RedisValue::Kind::kString)
                service::message::redis::throwValue(
                    "read active access-session projection", active);
            co_await transaction.commit();
            co_return;
        }
    }
    const auto rows = co_await transaction.query(R"sql(
SELECT key.access_key_hash, key.id::text, key.name, key.status::text,
       COALESCE((EXTRACT(EPOCH FROM key.expires_at) * 1000)::bigint, 0)::text,
       key.scopes::text,
       COALESCE(jsonb_agg(binding.device_id::text ORDER BY binding.device_id)
         FILTER (WHERE binding.device_id IS NOT NULL), '[]'::jsonb)::text
FROM open_access_key key
LEFT JOIN open_access_key_device binding ON binding.access_key_id = key.id
WHERE key.deleted_at IS NULL
GROUP BY key.id
ORDER BY key.id)sql");

    const auto version = service::common::nextUuidV7();
    const auto versionKey = std::string(kVersionPrefix) + version;
    constexpr std::size_t chunkSize = 128;
    for (std::size_t offset = 0; offset < rows.size(); offset += chunkSize) {
        const auto end = std::min(rows.size(), offset + chunkSize);
        std::vector<std::string> command{"HSET", versionKey};
        command.reserve(2 + (end - offset) * 2);
        for (std::size_t index = offset; index < end; ++index) {
            const auto& row = rows[index];
            command.emplace_back(row[0].value().value_or(std::string_view{}));
            command.push_back(encode(row[1].value().value_or(std::string_view{}), row[2].value().value_or(std::string_view{}), row[3].value().value_or(std::string_view{}),
                                     row[4].value().value_or(std::string_view{}), row[5].value().value_or(std::string_view{}), row[6].value().value_or(std::string_view{})));
        }
        (void)co_await service::message::redis::command(context.redis(), command);
    }
    if (rows.empty())
        (void)co_await service::message::redis::command(
            context.redis(), {"HSET", versionKey, "__ready", "1"});
    (void)co_await service::message::redis::command(
        context.redis(), {"EXPIRE", versionKey, "600"});

    static constexpr std::string_view swapScript = R"lua(
local previous = redis.call('GET', KEYS[1])
redis.call('SET', KEYS[1], ARGV[1])
redis.call('PERSIST', KEYS[2])
if previous and previous ~= ARGV[1] then
  redis.call('EXPIRE', ARGV[2] .. previous, ARGV[3])
end
return previous or ''
)lua";
    const std::string activeKey(kActiveVersionKey);
    const std::string_view keys[]{activeKey, versionKey};
    const std::string prefix(kVersionPrefix);
    const std::string grace = "120";
    const std::string_view args[]{version, prefix, grace};
    const auto reply = co_await context.redis().eval(swapScript, keys, args);
    if (reply.kind() != ruvia::RedisValue::Kind::kString)
        service::message::redis::throwValue("activate access-session projection", reply);
    co_await transaction.commit();
}

template <typename Context> ruvia::Task<void> ensure(Context& context) {
    const auto active = co_await service::message::redis::command(
        context.redis(), {"GET", std::string(kActiveVersionKey)});
    if (!active.null()) {
        if (active.kind() != ruvia::RedisValue::Kind::kString)
            service::message::redis::throwValue(
                "read active access-session projection", active);
        co_return;
    }
    co_await refresh(context, true);
}

} // namespace service::access::session

#include <ruvia/web/WebWorker.h>
#include "service/common/message.h"
#include "service/features/access/access.types.h"

namespace service::access::webhook {

inline ruvia::Task<Catalog> loadCatalog(ruvia::WebWorkerContext& context) {
        const auto rows = co_await context.db().query(R"sql(
SELECT binding.device_id::text, device.name,
       COALESCE(device.protocol_params->>'device_code', ''),
       webhook.id::text, webhook.access_key_id::text, webhook.url,
       COALESCE(webhook.secret, ''), webhook.headers::text,
       webhook.timeout_seconds::text,
       CASE WHEN webhook.skip_tls_verify THEN '1' ELSE '0' END,
       COALESCE((EXTRACT(EPOCH FROM key.expires_at) * 1000)::bigint, 0)::text,
       event_type.value
FROM open_webhook webhook
JOIN open_access_key key ON key.id = webhook.access_key_id
JOIN open_access_key_device binding ON binding.access_key_id = key.id
JOIN device ON device.id = binding.device_id
CROSS JOIN LATERAL jsonb_array_elements_text(
  CASE WHEN jsonb_typeof(webhook.event_types) = 'array'
       THEN webhook.event_types ELSE '[]'::jsonb END) event_type(value)
WHERE webhook.deleted_at IS NULL AND webhook.status = 'enabled'
  AND key.deleted_at IS NULL AND key.status = 'enabled'
  AND (key.expires_at IS NULL OR key.expires_at > NOW())
  AND device.deleted_at IS NULL
ORDER BY binding.device_id, event_type.value, webhook.id)sql");
        Catalog result;
        for (const auto& row : rows) {
            const std::string deviceId(row[0].value().value_or(std::string_view{}));
            auto& device = result[deviceId];
            device.name.assign(row[1].value().value_or(std::string_view{}));
            device.code.assign(row[2].value().value_or(std::string_view{}));
            const auto timeout =
                service::common::parseInt64(std::optional<std::string_view>{row[8].value().value_or(std::string_view{})})
                    .value_or(5);
            const auto expiresAtMs =
                service::common::parseInt64(std::optional<std::string_view>{row[10].value().value_or(std::string_view{})})
                    .value_or(0);
            device.targets[std::string(row[11].value().value_or(std::string_view{}))].push_back(
                {std::string(row[3].value().value_or(std::string_view{})), std::string(row[4].value().value_or(std::string_view{})),
                 std::string(row[5].value().value_or(std::string_view{})), std::string(row[6].value().value_or(std::string_view{})),
                 std::string(row[7].value().value_or(std::string_view{})), timeout > 0 ? timeout : std::int64_t{5},
                 row[9].value().value_or(std::string_view{}) == "1", expiresAtMs});
        }
        co_return result;
    }

inline ruvia::Task<void> persistResults(
        ruvia::WebWorkerContext& context,
        const std::vector<message::StreamMessage>& messages) {
        if (messages.empty())
            co_return;
        std::string sql = R"sql(
WITH incoming(
  sequence, log_id, access_key_id, webhook_id, event_type, status, target,
  http_status, device_id, device_code, message, request_payload,
  response_payload, completed_at_ms) AS (VALUES )sql";
        std::vector<ruvia::DbValue> params;
        params.reserve(messages.size() * 14);
        for (std::size_t index = 0; index < messages.size(); ++index) {
            if (index != 0)
                sql.push_back(',');
            const auto base = params.size() + 1;
            sql += "($" + std::to_string(base) + "::bigint,$" +
                   std::to_string(base + 1) + "::uuid,$" +
                   std::to_string(base + 2) + "::uuid,$" +
                   std::to_string(base + 3) + "::uuid,$" +
                   std::to_string(base + 4) + "::text,$" +
                   std::to_string(base + 5) + "::text,$" +
                   std::to_string(base + 6) + "::text,$" +
                   std::to_string(base + 7) + "::bigint,$" +
                   std::to_string(base + 8) + "::uuid,$" +
                   std::to_string(base + 9) + "::text,$" +
                   std::to_string(base + 10) + "::text,$" +
                   std::to_string(base + 11) + "::jsonb,$" +
                   std::to_string(base + 12) + "::jsonb,$" +
                   std::to_string(base + 13) + "::bigint)";
            const auto httpStatus = service::common::parseInt64(
                std::optional<std::string_view>(messages[index].get("http_status")));
            const auto completedAt = service::common::parseInt64(
                std::optional<std::string_view>(messages[index].get("completed_at_ms")));
            params.emplace_back(static_cast<std::int64_t>(index));
            params.emplace_back(messages[index].get("log_id"));
            params.emplace_back(messages[index].get("access_key_id"));
            params.emplace_back(messages[index].get("webhook_id"));
            params.emplace_back(messages[index].get("event_type"));
            params.emplace_back(messages[index].get("status"));
            params.emplace_back(messages[index].get("target"));
            params.emplace_back(httpStatus.value_or(0));
            params.emplace_back(messages[index].get("device_id"));
            params.emplace_back(messages[index].get("device_code"));
            params.emplace_back(messages[index].get("message"));
            params.emplace_back(messages[index].get("request_payload"));
            params.emplace_back(messages[index].get("response_payload"));
            params.emplace_back(completedAt.value_or(service::message::utcNowMilliseconds()));
        }
        sql += R"sql(), latest AS (
  SELECT DISTINCT ON (webhook_id) *
  FROM incoming
  ORDER BY webhook_id, completed_at_ms DESC, sequence DESC
), summary AS (
  SELECT webhook_id,
         MAX(completed_at_ms) FILTER (WHERE status = 'success') AS last_success_ms,
         MAX(completed_at_ms) FILTER (WHERE status = 'failed') AS last_failure_ms
  FROM incoming GROUP BY webhook_id
), updated AS (
  UPDATE open_webhook webhook
  SET last_triggered_at = GREATEST(
        COALESCE(webhook.last_triggered_at, to_timestamp(0)),
        to_timestamp(latest.completed_at_ms::double precision / 1000.0)),
      last_success_at = CASE WHEN summary.last_success_ms IS NULL
        THEN webhook.last_success_at ELSE GREATEST(
          COALESCE(webhook.last_success_at, to_timestamp(0)),
          to_timestamp(summary.last_success_ms::double precision / 1000.0)) END,
      last_failure_at = CASE WHEN summary.last_failure_ms IS NULL
        THEN webhook.last_failure_at ELSE GREATEST(
          COALESCE(webhook.last_failure_at, to_timestamp(0)),
          to_timestamp(summary.last_failure_ms::double precision / 1000.0)) END,
      last_http_status = CASE
        WHEN webhook.last_triggered_at IS NULL OR webhook.last_triggered_at <=
             to_timestamp(latest.completed_at_ms::double precision / 1000.0)
        THEN NULLIF(latest.http_status, 0) ELSE webhook.last_http_status END,
      last_error = CASE
        WHEN webhook.last_triggered_at IS NULL OR webhook.last_triggered_at <=
             to_timestamp(latest.completed_at_ms::double precision / 1000.0)
        THEN NULLIF(latest.message, '') ELSE webhook.last_error END,
      updated_at = NOW()
  FROM summary JOIN latest USING (webhook_id)
  WHERE webhook.id = summary.webhook_id AND webhook.deleted_at IS NULL
  RETURNING webhook.id
)
INSERT INTO open_access_log(
  id, access_key_id, webhook_id, direction, action, event_type, status,
  http_method, target, http_status, device_id, device_code, message,
  request_payload, response_payload)
SELECT log_id, access_key_id, webhook_id, 'push', 'webhook', event_type, status,
       'POST', target, NULLIF(http_status, 0), device_id, NULLIF(device_code, ''),
       NULLIF(message, ''), request_payload, response_payload
FROM incoming
ON CONFLICT (id) DO NOTHING)sql";
        (void)co_await context.db().execute(sql, params);
    }

inline ruvia::Task<void> persistAudits(
        ruvia::WebWorkerContext& context,
        const std::vector<message::StreamMessage>& messages) {
        if (messages.empty())
            co_return;
        std::string sql = R"sql(
WITH incoming(
  sequence, log_id, access_key_id, action, http_method, target, request_ip,
  http_status, device_id, request_payload, response_payload, used_at_ms) AS (VALUES )sql";
        std::vector<ruvia::DbValue> params;
        params.reserve(messages.size() * 12);
        for (std::size_t index = 0; index < messages.size(); ++index) {
            if (index != 0)
                sql.push_back(',');
            const auto base = params.size() + 1;
            sql += "($" + std::to_string(base) + "::bigint,$" +
                   std::to_string(base + 1) + "::uuid,$" +
                   std::to_string(base + 2) + "::uuid,$" +
                   std::to_string(base + 3) + "::text,$" +
                   std::to_string(base + 4) + "::text,$" +
                   std::to_string(base + 5) + "::text,$" +
                   std::to_string(base + 6) + "::text,$" +
                   std::to_string(base + 7) + "::integer,NULLIF($" +
                   std::to_string(base + 8) + ", '')::uuid,$" +
                   std::to_string(base + 9) + "::jsonb,$" +
                   std::to_string(base + 10) + "::jsonb,$" +
                   std::to_string(base + 11) + "::bigint)";
            const auto httpStatus = service::common::parseInt64(
                std::optional<std::string_view>(messages[index].get("http_status")));
            const auto usedAt = service::common::parseInt64(
                std::optional<std::string_view>(messages[index].get("used_at_ms")));
            params.emplace_back(static_cast<std::int64_t>(index));
            params.emplace_back(messages[index].get("log_id"));
            params.emplace_back(messages[index].get("access_key_id"));
            params.emplace_back(messages[index].get("action"));
            params.emplace_back(messages[index].get("http_method"));
            params.emplace_back(messages[index].get("target"));
            params.emplace_back(messages[index].get("request_ip"));
            params.emplace_back(httpStatus.value_or(0));
            params.emplace_back(messages[index].get("device_id"));
            params.emplace_back(messages[index].get("request_payload"));
            params.emplace_back(messages[index].get("response_payload"));
            params.emplace_back(usedAt.value_or(service::message::utcNowMilliseconds()));
        }
        sql += R"sql(), latest_usage AS MATERIALIZED (
  SELECT DISTINCT ON (access_key_id)
         access_key_id, request_ip, used_at_ms
  FROM incoming
  ORDER BY access_key_id, used_at_ms DESC, sequence DESC
), usage_updated AS (
  UPDATE open_access_key key
  SET last_used_at = to_timestamp(latest.used_at_ms::double precision / 1000.0),
      last_used_ip = NULLIF(latest.request_ip, '')
  FROM latest_usage latest
  WHERE key.id = latest.access_key_id
    AND (key.last_used_at IS NULL OR key.last_used_at <=
         to_timestamp(latest.used_at_ms::double precision / 1000.0))
  RETURNING key.id
)
INSERT INTO open_access_log(
  id, access_key_id, direction, action, status, http_method, target,
  request_ip, http_status, device_id, request_payload, response_payload)
SELECT incoming.log_id, incoming.access_key_id, 'pull', incoming.action, 'success',
       NULLIF(incoming.http_method, ''), NULLIF(incoming.target, ''),
       NULLIF(incoming.request_ip, ''), NULLIF(incoming.http_status, 0),
       incoming.device_id, incoming.request_payload, incoming.response_payload
FROM incoming
CROSS JOIN (SELECT count(*) AS updated_count FROM usage_updated) update_barrier
WHERE update_barrier.updated_count >= 0
ON CONFLICT (id) DO NOTHING)sql";
        (void)co_await context.db().execute(sql, params);
    }

} // namespace service::access::webhook
