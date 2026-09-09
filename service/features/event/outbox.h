#pragma once

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/core/Timer.h>
#include <ruvia/core/Channel.h>
#include <ruvia/core/StopToken.h>
#include <ruvia/web/WebWorker.h>

#include "service/common/http.h"
#include "service/features/event/config.h"
#include "service/features/event/postgres-notifier.h"
#include "service/features/event/stream-multiplexer.h"
#include "service/features/access/event.h"
#include "service/features/live/runtime.h"
#include "service/observability/registry.h"

namespace service::message::outbox {

struct Policy final {
    std::int64_t pendingAlertThreshold{1000};
    std::int64_t oldestAgeAlertMs{300000};
    std::int64_t deadLetterAlertThreshold{1};
    std::int64_t receiptRetentionDays{30};
};

class Runtime final {
  public:
    Runtime(observability::Registry& observability, std::size_t collectorWorkerCount,
            std::size_t serviceWorkerCount,
            ruvia::DbConfig database,
            Policy policy = {})
        : observability_(observability), collectorWorkerCount_(collectorWorkerCount),
          serviceWorkerCount_(serviceWorkerCount), policy_(policy),
          notifier_(std::move(database), [this] { wake(); }) {}
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    ~Runtime() { stop(); }

    void start(std::vector<ruvia::WebWorkerHandle> workers) {
        if (running_.exchange(true))
            return;
        workers_ = std::move(workers);
        if (workers_.empty()) {
            running_.store(false);
            throw std::runtime_error("outbox dispatcher requires Service Workers");
        }
        stopSource_ = std::make_unique<ruvia::StopSource>();
        std::vector<std::future<void>> readiness;
        readiness.reserve(workers_.size());
        stopped_.reserve(workers_.size());
        try {
        for (auto& worker : workers_) {
            auto ready = std::make_shared<std::promise<void>>();
            auto stopped = std::make_shared<std::promise<void>>();
            readiness.push_back(ready->get_future());
            auto completion = stopped->get_future().share();
            const auto posted = worker.post(
                [this, ready, stopped](ruvia::WebWorkerContext& context) {
                    return run(context, ready, stopped);
                });
            if (!posted.accepted()) {
                throw std::runtime_error("service worker rejected outbox dispatcher");
            }
            stopped_.push_back(std::move(completion));
        }
            for (auto& ready : readiness)
                ready.get();
            // Every worker has installed its bounded wake channel first. LISTEN
            // completion also wakes all workers to close the startup/reconnect gap.
            notifier_.start();
        } catch (...) {
            stop();
            throw;
        }
    }

    void stop() noexcept {
        if (!running_.exchange(false))
            return;
        notifier_.stop();
        if (stopSource_) stopSource_->requestStop();
        for (const auto& stopped : stopped_)
            if (stopped.valid())
                stopped.wait();
        stopped_.clear();
        workers_.clear();
        wakeChannels_.clear();
        stopSource_.reset();
    }

  private:
    using Clock = std::chrono::steady_clock;
    struct WakeChannel {
        ruvia::ChannelSender<int> sender;
        ruvia::ChannelReceiver<int> receiver;
    };

    void wake() {
        std::lock_guard lock(wakeMutex_);
        for (const auto& channel : wakeChannels_)
            (void)channel->sender.send(1);
        // Commands use the same committed-work hint, including startup and
        // reconnect catchup; their separate timer tracks actual attempt deadlines.
        service::message::workerStreamMultiplexer().signal(
            service::message::WorkerStreamTask::CommandResult);
    }

    struct Event {
        std::string id;
        std::string type;
        std::string aggregate;
        std::string aggregateId;
        std::string action;
        std::string schemaVersion;
        std::string occurredAtMs;
        std::string deviceCode;
        std::string data;
    };

    ruvia::Task<void> run(ruvia::WebWorkerContext& context,
                          std::shared_ptr<std::promise<void>> ready,
                          std::shared_ptr<std::promise<void>> stopped) {
        try {
            auto [sender, receiver] = ruvia::makeChannel<int>(context.worker(), {.capacity = 1});
            auto wakeChannel = std::make_shared<WakeChannel>(
                WakeChannel{std::move(sender), std::move(receiver)});
            {
                std::lock_guard lock(wakeMutex_);
                wakeChannels_.push_back(wakeChannel);
            }
            const auto stop = ruvia::combineStopTokens(context.stopToken(), stopSource_->token());
            ready->set_value();
            auto nextMetrics = std::chrono::steady_clock::now();
            auto nextReceiptCleanup = std::chrono::steady_clock::now();
            std::optional<Clock::time_point> nextDispatch = Clock::now();
            while (!stop.stopRequested()) {
                if (policy_.receiptRetentionDays > 0 &&
                    std::chrono::steady_clock::now() >= nextReceiptCleanup) {
                    try {
                        co_await cleanupReceipts(context);
                    } catch (const std::exception& error) {
                        observability_.increment(
                            "iot_engine_outbox_receipt_cleanup_failures_total");
                        std::cerr << "outbox receipt cleanup failed: " << error.what() << '\n';
                    }
                    nextReceiptCleanup =
                        std::chrono::steady_clock::now() + std::chrono::hours(1);
                }
                if (std::chrono::steady_clock::now() >= nextMetrics) {
                    observability_.gauge("iot_engine_outbox_listener_connected", notifier_.connected() ? 1 : 0);
                    try {
                        co_await collectMetrics(context);
                    } catch (const std::exception& error) {
                        observability_.increment("iot_engine_metrics_collection_failures_total");
                        std::cerr << "metrics collection failed: " << error.what() << '\n';
                    }
                    nextMetrics = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                }
                if (nextDispatch && Clock::now() >= *nextDispatch) {
                    try {
                        observability_.increment("iot_engine_outbox_dispatch_checks_total");
                        if (co_await dispatch(context)) {
                            nextDispatch = Clock::now();
                            continue;
                        }
                        nextDispatch = co_await nextAvailable(context);
                    } catch (const std::exception& error) {
                        observability_.increment("iot_engine_outbox_dispatch_failures_total");
                        std::cerr << "outbox dispatch failed: " << error.what() << '\n';
                        nextDispatch = Clock::now() + std::chrono::milliseconds(250);
                    }
                }
                auto deadline = nextMetrics;
                if (policy_.receiptRetentionDays > 0)
                    deadline = std::min(deadline, nextReceiptCleanup);
                if (nextDispatch) deadline = std::min(deadline, *nextDispatch);
                const auto delay = std::max(std::chrono::milliseconds(1),
                    std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()));
                const auto notification = co_await wakeChannel->receiver.receiveFor(delay, stop);
                if (notification.hasValue()) nextDispatch = Clock::now();
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

    ruvia::Task<std::optional<Clock::time_point>> nextAvailable(ruvia::WebWorkerContext& context) {
        // No periodic empty-queue scan. A timer is armed only for a durable
        // retry, or for eligible rows currently locked by another dispatcher.
        const auto rows = co_await context.db().query(R"sql(
SELECT ceil(extract(epoch FROM (min(available_at) - clock_timestamp())) * 1000)::bigint::text
FROM outbox_event WHERE published_at IS NULL AND dead_lettered_at IS NULL)sql");
        if (rows.empty() || !rows.front()[0].value()) co_return std::nullopt;
        const auto delay = std::chrono::milliseconds(std::max<std::int64_t>(
            25, integer(*rows.front()[0].value())));
        co_return Clock::now() + delay;
    }

    ruvia::Task<bool> dispatch(ruvia::WebWorkerContext& context) {
        auto transaction = co_await context.db().beginTransaction();
        const auto rows = co_await transaction.query(R"sql(
SELECT id::text, event_type, aggregate_type, aggregate_id, action,
       schema_version::text,
       floor(extract(epoch FROM occurred_at) * 1000)::bigint::text,
       COALESCE(payload->>'device_code',''), COALESCE(payload->'data','{}'::jsonb)::text
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
            event.deviceCode = row[7].value().value_or(std::string_view{});
            event.data = row[8].value().value_or(std::string_view{});
            events.push_back(std::move(event));
        }

        for (const auto& event : events) {
            std::string publishError;
            try {
                if (event.type == "query.changed") {
                    co_await service::live::publish(context.redis(), event.aggregate);
                } else if (event.aggregate == "command") {
                    co_await service::access::event::publish(context.redis(),event.id,event.type,
                        event.aggregateId,event.deviceCode,integer(event.occurredAtMs),event.data);
                } else co_await publishConfigEnvelope(context.redis(), event.id, event.type,
                                               event.aggregate, event.aggregateId, event.action,
                                               event.schemaVersion, event.occurredAtMs,
                                               serviceWorkerCount_);
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
            const auto pending =
                integer(rows.front()[0].value().value_or(std::string_view{}));
            const auto oldestAge =
                integer(rows.front()[1].value().value_or(std::string_view{}));
            const auto deadLettered =
                integer(rows.front()[2].value().value_or(std::string_view{}));
            observability_.gauge("iot_engine_outbox_pending", pending);
            observability_.gauge("iot_engine_outbox_oldest_age_ms", oldestAge);
            observability_.gauge("iot_engine_outbox_dead_lettered", deadLettered);
            updateAlert("outbox_pending", pending, policy_.pendingAlertThreshold);
            updateAlert("outbox_oldest_age", oldestAge, policy_.oldestAgeAlertMs);
            updateAlert("outbox_dead_lettered", deadLettered,
                        policy_.deadLetterAlertThreshold);
        }
        observability_.gauge("iot_engine_outbox_receipt_retention_days",
                             policy_.receiptRetentionDays);

        for (std::size_t shardIndex = 0;
             shardIndex < service::message::shard::kCount; ++shardIndex)
            co_await collectStream(context,
                                   "runtime_config_" + std::to_string(shardIndex),
                                   runtimeConfigChangesStream(shardIndex),
                                   "iot-engine:runtime-reconciler");
        for (std::size_t index = 0; index < serviceWorkerCount_; ++index)
            co_await collectStream(context, "webhook_config_" + std::to_string(index),
                                   webhookCatalogChangesStream(index),
                                   "iot-engine:open-webhook");
        for (std::size_t shardIndex = 0;
             shardIndex < service::message::shard::kCount; ++shardIndex)
            co_await collectStream(
                context, "access_session_" + std::to_string(shardIndex),
                service::access::stream::sessionChanges(shardIndex),
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

    ruvia::Task<void> cleanupReceipts(ruvia::WebWorkerContext& context) {
        (void)co_await context.db().execute(R"sql(
DELETE FROM outbox_consumer_receipt
WHERE processed_at < NOW() - make_interval(days => $1::integer))sql",
                                            service::common::dbParams(
                                                policy_.receiptRetentionDays));
    }

    void updateAlert(std::string name, std::int64_t value, std::int64_t threshold) {
        const bool active = threshold > 0 && value >= threshold;
        const auto detail = "value=" + std::to_string(value) +
                            ", threshold=" + std::to_string(threshold);
        if (observability_.alert(name, active, detail))
            std::cerr << "operational alert " << (active ? "active" : "cleared")
                      << ": " << name << " (" << detail << ")\n";
    }

    observability::Registry& observability_;
    std::size_t collectorWorkerCount_{};
    std::size_t serviceWorkerCount_{};
    Policy policy_;
    PostgresNotifier notifier_;
    std::unique_ptr<ruvia::StopSource> stopSource_;
    std::mutex wakeMutex_;
    std::vector<std::shared_ptr<WakeChannel>> wakeChannels_;
    std::vector<ruvia::WebWorkerHandle> workers_;
    std::vector<std::shared_future<void>> stopped_;
    std::atomic_bool running_{false};
};

} // namespace service::message::outbox
