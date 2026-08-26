#pragma once

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/core/Timer.h>
#include <ruvia/web/WebWorker.h>

#include "service/common/http.h"
#include "service/features/event/config.h"
#include "service/observability/registry.h"

namespace service::message::outbox {

class Runtime final {
  public:
    Runtime(observability::Registry& observability, std::size_t collectorWorkerCount)
        : observability_(observability), collectorWorkerCount_(collectorWorkerCount) {}
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    ~Runtime() { stop(); }

    void start(ruvia::WebWorkerHandle worker) {
        if (running_.exchange(true))
            return;
        worker_ = std::move(worker);
        auto ready = std::make_shared<std::promise<void>>();
        auto stopped = std::make_shared<std::promise<void>>();
        auto readiness = ready->get_future();
        stopped_ = stopped->get_future().share();
        const auto posted = worker_.post(
            [this, ready, stopped](ruvia::WebWorkerContext& context) {
                return run(context, ready, stopped);
            });
        if (!posted.accepted()) {
            running_.store(false);
            throw std::runtime_error("service worker rejected outbox dispatcher");
        }
        readiness.get();
    }

    void stop() noexcept {
        if (!running_.exchange(false))
            return;
        if (stopped_.valid())
            (void)stopped_.wait_for(std::chrono::seconds(3));
        stopped_ = {};
        worker_ = {};
    }

  private:
    struct Event {
        std::string id;
        std::string type;
        std::string aggregate;
        std::string aggregateId;
        std::string action;
        std::string schemaVersion;
        std::string occurredAtMs;
    };

    ruvia::Task<void> run(ruvia::WebWorkerContext& context,
                          std::shared_ptr<std::promise<void>> ready,
                          std::shared_ptr<std::promise<void>> stopped) {
        try {
            ready->set_value();
            auto nextMetrics = std::chrono::steady_clock::now();
            while (running_.load() && !context.stopToken().stopRequested()) {
                if (std::chrono::steady_clock::now() >= nextMetrics) {
                    try {
                        co_await collectMetrics(context);
                    } catch (const std::exception& error) {
                        observability_.increment("iot_engine_metrics_collection_failures_total");
                        std::cerr << "metrics collection failed: " << error.what() << '\n';
                    }
                    nextMetrics = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                }
                bool dispatched = false;
                bool failed = false;
                try {
                    dispatched = co_await dispatch(context);
                } catch (const std::exception& error) {
                    observability_.increment("iot_engine_outbox_dispatch_failures_total");
                    std::cerr << "outbox dispatch failed: " << error.what() << '\n';
                    failed = true;
                }
                if (failed)
                    (void)co_await ruvia::sleepFor(context.worker(),
                                                   std::chrono::milliseconds(250));
                else if (!dispatched)
                    (void)co_await ruvia::sleepFor(context.worker(),
                                                   std::chrono::milliseconds(100));
            }
        } catch (...) {
            try {
                ready->set_exception(std::current_exception());
            } catch (...) {
            }
        }
        try {
            stopped->set_value();
        } catch (...) {
        }
    }

    ruvia::Task<bool> dispatch(ruvia::WebWorkerContext& context) {
        auto transaction = co_await context.db().beginTransaction();
        const auto rows = co_await transaction.query(R"sql(
SELECT id::text, event_type, aggregate_type, aggregate_id, action,
       schema_version::text,
       floor(extract(epoch FROM occurred_at) * 1000)::bigint::text
FROM outbox_event
WHERE published_at IS NULL AND dead_lettered_at IS NULL AND available_at <= NOW()
ORDER BY occurred_at, id
FOR UPDATE SKIP LOCKED
LIMIT 100)sql");
        if (rows.empty()) {
            co_await transaction.commit();
            observability_.gauge("iot_engine_outbox_last_batch_size", 0);
            co_return false;
        }

        std::vector<Event> events;
        events.reserve(rows.size());
        for (const auto& row : rows) {
            Event event;
            event.id = std::string(row[0].value().value_or(std::string_view{}));
            event.type = std::string(row[1].value().value_or(std::string_view{}));
            event.aggregate = std::string(row[2].value().value_or(std::string_view{}));
            event.aggregateId = std::string(row[3].value().value_or(std::string_view{}));
            event.action = std::string(row[4].value().value_or(std::string_view{}));
            event.schemaVersion = std::string(row[5].value().value_or(std::string_view{"1"}));
            event.occurredAtMs = std::string(row[6].value().value_or(std::string_view{"0"}));
            events.push_back(std::move(event));
        }

        for (const auto& event : events) {
            std::string publishError;
            try {
                co_await publishConfigEnvelope(context.redis(), event.id, event.type,
                                               event.aggregate, event.aggregateId, event.action,
                                               event.schemaVersion, event.occurredAtMs);
            } catch (const std::exception& error) {
                publishError = error.what();
            } catch (...) {
                publishError = "unknown publish failure";
            }
            if (!publishError.empty()) {
                if (publishError.size() > 2000)
                    publishError.resize(2000);
                (void)co_await transaction.execute(R"sql(
UPDATE outbox_event
SET attempts = attempts + 1,
    last_error = $2,
    available_at = NOW() + make_interval(
      secs => LEAST(300, (1::bigint << LEAST(attempts, 8))::integer)),
    dead_lettered_at = CASE WHEN attempts + 1 >= 20 THEN NOW()
                            ELSE dead_lettered_at END
WHERE id = $1::uuid)sql",
                    service::common::dbParams(event.id, publishError));
                co_await transaction.commit();
                observability_.increment("iot_engine_outbox_publish_retries_total");
                co_return true;
            }
            (void)co_await transaction.execute(
                "UPDATE outbox_event SET published_at = NOW(), attempts = attempts + 1, "
                "last_error = NULL WHERE id = $1::uuid",
                service::common::dbParams(event.id));
        }
        co_await transaction.commit();
        observability_.increment("iot_engine_outbox_published_total", events.size());
        observability_.gauge("iot_engine_outbox_last_batch_size",
                             static_cast<std::int64_t>(events.size()));
        co_return true;
    }

    ruvia::Task<void> collectMetrics(ruvia::WebWorkerContext& context) {
        const auto rows = co_await context.db().query(R"sql(
SELECT count(*) FILTER (WHERE dead_lettered_at IS NULL)::text,
       COALESCE(floor(extract(epoch FROM (
         NOW() - min(occurred_at) FILTER (WHERE dead_lettered_at IS NULL))) * 1000), 0)::bigint::text,
       count(*) FILTER (WHERE dead_lettered_at IS NOT NULL)::text
FROM outbox_event WHERE published_at IS NULL)sql");
        if (!rows.empty()) {
            observability_.gauge(
                "iot_engine_outbox_pending",
                integer(rows.front()[0].value().value_or(std::string_view{})));
            observability_.gauge(
                "iot_engine_outbox_oldest_age_ms",
                integer(rows.front()[1].value().value_or(std::string_view{})));
            observability_.gauge(
                "iot_engine_outbox_dead_lettered",
                integer(rows.front()[2].value().value_or(std::string_view{})));
        }

        co_await collectStream(context, "runtime_config",
                               std::string(kRuntimeConfigChangesStream),
                               "iot-engine:runtime-reconciler");
        co_await collectStream(context, "webhook_config",
                               std::string(kWebhookCatalogChangesStream),
                               "iot-engine:open-webhook");
        for (std::size_t index = 0; index < collectorWorkerCount_; ++index) {
            const auto suffix = std::to_string(index);
            co_await collectStream(context, "ingress_" + suffix,
                                   message::ingressStream(index), "iot-engine:collector");
            co_await collectStream(context, "telemetry_" + suffix,
                                   message::parsedStream(index),
                                   "iot-engine:telemetry-persistence");
            co_await collectStream(context, "command_result_" + suffix,
                                   message::commandResultStream(index),
                                   "iot-engine:command-result");
            co_await collectStream(context, "dead_letter_" + suffix,
                                   message::deadLetterStream(index), {});
        }
    }

    ruvia::Task<void> collectStream(ruvia::WebWorkerContext& context,
                                    std::string metricSuffix, std::string stream,
                                    std::string_view group) {
        const auto length = co_await service::message::redis::command(
            context.redis(), {"XLEN", stream});
        if (length.kind() == ruvia::RedisValue::Kind::kInteger)
            observability_.gauge("iot_engine_stream_" + metricSuffix + "_entries",
                                 length.integer());
        if (group.empty())
            co_return;
        const auto pending = co_await service::message::redis::command(
            context.redis(), {"XPENDING", stream, std::string(group)});
        if (pending.kind() == ruvia::RedisValue::Kind::kArray &&
            !pending.array().empty() &&
            pending.array().front().kind() == ruvia::RedisValue::Kind::kInteger)
            observability_.gauge("iot_engine_stream_" + metricSuffix + "_pending",
                                 pending.array().front().integer());
    }

    static std::int64_t integer(std::string_view value) {
        std::int64_t result{};
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
        return error == std::errc{} && end == value.data() + value.size() ? result : 0;
    }

    observability::Registry& observability_;
    std::size_t collectorWorkerCount_{};
    ruvia::WebWorkerHandle worker_;
    std::shared_future<void> stopped_;
    std::atomic_bool running_{false};
};

} // namespace service::message::outbox
