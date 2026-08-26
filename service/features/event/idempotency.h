#pragma once

#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/web/WebWorker.h>

#include "service/common/http.h"
#include "service/common/uuid.h"
#include "service/features/collector/stream.h"

namespace service::message::idempotency {

inline std::vector<std::string>
eventIds(const std::vector<service::message::StreamMessage> &messages) {
  std::set<std::string, std::less<>> unique;
  for (const auto &message : messages) {
    const auto id = message.get("event_id");
    if (service::common::isUuid(id))
      unique.emplace(id);
  }
  return {unique.begin(), unique.end()};
}

inline ruvia::Task<std::set<std::string, std::less<>>>
pending(ruvia::WebWorkerContext &context, std::string_view consumer,
        const std::vector<std::string> &eventIds) {
  std::set<std::string, std::less<>> result(eventIds.begin(), eventIds.end());
  if (eventIds.empty())
    co_return result;

  std::string sql = "SELECT event_id::text FROM outbox_consumer_receipt WHERE "
                    "consumer_name = $1 AND "
                    "event_id IN (";
  std::vector<ruvia::DbValue> params;
  params.reserve(eventIds.size() + 1);
  params.emplace_back(consumer);
  for (std::size_t index = 0; index < eventIds.size(); ++index) {
    if (index != 0)
      sql.push_back(',');
    sql += "$" + std::to_string(index + 2) + "::uuid";
    params.emplace_back(eventIds[index]);
  }
  sql.push_back(')');
  const auto rows = co_await context.db().query(sql, params);
  for (const auto &row : rows)
    result.erase(std::string(row[0].value().value_or(std::string_view{})));
  co_return result;
}

inline bool
shouldProcess(const service::message::StreamMessage &message,
              const std::set<std::string, std::less<>> &pendingIds) {
  const auto id = message.get("event_id");
  return !service::common::isUuid(id) || pendingIds.contains(id);
}

inline ruvia::Task<void>
markProcessed(ruvia::WebWorkerContext &context, std::string_view consumer,
              const std::vector<std::string> &eventIds) {
  if (eventIds.empty())
    co_return;
  std::string sql =
      "INSERT INTO outbox_consumer_receipt(consumer_name, event_id) VALUES ";
  std::vector<ruvia::DbValue> params;
  params.reserve(eventIds.size() * 2);
  for (std::size_t index = 0; index < eventIds.size(); ++index) {
    if (index != 0)
      sql.push_back(',');
    const auto base = index * 2 + 1;
    sql += "($" + std::to_string(base) + ",$" + std::to_string(base + 1) +
           "::uuid)";
    params.emplace_back(consumer);
    params.emplace_back(eventIds[index]);
  }
  sql += " ON CONFLICT (consumer_name, event_id) DO NOTHING";
  (void)co_await context.db().execute(sql, params);
}

} // namespace service::message::idempotency
