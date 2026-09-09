#pragma once

#include <future>
#include <iostream>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <ruvia/web/WebWorker.h>
#include "service/features/collector/stream.h"
#include "service/features/live/bus.h"
#include "service/features/edge/session.h"

namespace service::live {

inline constexpr std::string_view kChanges = "iot:live:changes";

template <typename Redis>
ruvia::Task<void> publish(const Redis& redis, std::string_view topic) {
    const std::vector<service::message::StreamField> fields{{"topic", std::string(topic)}};
    co_await service::message::redis::add(redis, kChanges, fields, 100000);
}

// One Redis blocking reader per process, never per HTTP subscription. Separate
// consumer groups broadcast each change across process instances. This stream
// contains invalidations only; reconnect always resets to an authorized snapshot.
class Runtime final {
  public:
    explicit Runtime(std::size_t collectorWorkerCount)
        : collectorWorkerCount_(collectorWorkerCount) {
        if (collectorWorkerCount_ == 0)
            throw std::invalid_argument("live runtime requires collector workers");
    }

    void start(ruvia::WebWorkerHandle worker) {
        stop_ = std::make_unique<ruvia::StopSource>();
        auto ready = std::make_shared<std::promise<void>>();
        auto done = std::make_shared<std::promise<void>>();
        auto started = ready->get_future();
        stopped_ = done->get_future();
        if (!worker.post([this, ready, done](ruvia::WebWorkerContext& context) {
                return run(context, ready, done);
            }).accepted())
            throw std::runtime_error("live query worker rejected startup");
        started.get();
        auto leasesDone = std::make_shared<std::promise<void>>();
        leasesStopped_ = leasesDone->get_future();
        if (!worker.post([this, leasesDone](ruvia::WebWorkerContext& context) {
                return expireSessions(context, leasesDone);
            }).accepted()) {
            leasesDone->set_value();
            stop();
            throw std::runtime_error("live session deadline worker rejected startup");
        }
    }

    void stop() {
        if (!stop_) return;
        stop_->requestStop();
        if (stopped_.valid()) stopped_.wait();
        if (leasesStopped_.valid()) leasesStopped_.wait();
        stop_.reset();
    }

    ~Runtime() { stop(); }

  private:
    ruvia::Task<void> relayConfigNotifications(ruvia::WebWorkerContext& context) {
        const auto createdAt = std::to_string(service::message::utcNowMilliseconds());
        for (std::size_t workerIndex = 0; workerIndex < collectorWorkerCount_; ++workerIndex)
            (void)co_await service::message::redis::publish(
                context.redis(), service::message::configStream(workerIndex),
                {{"message_id", service::message::nextMessageId()},
                 {"worker_id", std::to_string(workerIndex)},
                 {"created_at_ms", createdAt}},
                10000);
    }

    ruvia::Task<void> expireSessions(ruvia::WebWorkerContext& context,
                                    std::shared_ptr<std::promise<void>> done) {
        const auto stop = ruvia::combineStopTokens(context.stopToken(), stop_->token());
        const std::string_view keys[]{service::edge::session_state::kDeadlines, kChanges};
        while (!stop.stopRequested()) {
            try {
                (void)co_await context.redis().eval(service::edge::session_state::kExpireScript,
                    keys, std::span<const std::string_view>{});
            } catch (const std::exception& error) {
                if (!stop.stopRequested())
                    std::cerr << "edge session deadline: " << error.what() << '\n';
            }
            (void)co_await ruvia::sleepFor(context.worker(), std::chrono::seconds(1), stop);
        }
        done->set_value();
    }

    ruvia::Task<void> run(ruvia::WebWorkerContext& context,
                         std::shared_ptr<std::promise<void>> ready,
                         std::shared_ptr<std::promise<void>> done) {
        const auto group = "live:" + service::runtime::instanceId();
        const auto redis = context.redis();
        const auto stop = ruvia::combineStopTokens(context.stopToken(), stop_->token());
        bool initialized = false;
        try {
            co_await service::message::redis::ensureGroup(redis, kChanges, group);
            ready->set_value();
            initialized = true;
            bool recovering = true;
            while (!stop.stopRequested()) {
                bool failed = false;
                try {
                    if (recovering) {
                        co_await service::message::redis::ensureGroup(redis, kChanges, group);
                        bus().publish("*");
                        // A reconnect can miss the invalidation while the consumer is down.
                        // Replaying the local config shards repairs every Collector worker.
                        co_await relayConfigNotifications(context);
                        // Pending notifications contain no business payload;
                        // a full invalidation repairs their effects safely.
                        const auto pending = co_await service::message::redis::readGroup(
                            redis, kChanges, group, "fanout", "0", std::chrono::milliseconds(0), 256);
                        for (const auto& change : pending)
                            co_await service::message::redis::acknowledge(redis, kChanges, group, change.id);
                        if (!pending.empty()) continue;
                        recovering = false;
                    }
                    const auto changes = co_await service::message::redis::readGroupBlocking(
                        redis, kChanges, group, "fanout", stop, 256);
                    for (const auto& change : changes) {
                        const auto topic = change.get("topic");
                        if (topic == "runtime-config")
                            co_await relayConfigNotifications(context);
                        else
                            bus().publish(topic);
                        // Shared stream: acknowledge but never delete entries
                        // needed by the other process consumer groups.
                        co_await service::message::redis::acknowledge(
                            redis, kChanges, group, change.id);
                    }
                } catch (const std::exception& error) {
                    if (stop.stopRequested()) break;
                    std::cerr << "live query fanout: " << error.what() << '\n';
                    failed = true;
                    recovering = true;
                }
                if (failed) {
                    (void)co_await ruvia::sleepFor(context.worker(), std::chrono::seconds(1));
                }
            }
        } catch (...) {
            if (!initialized) ready->set_exception(std::current_exception());
        }
        try {
            (void)co_await service::message::redis::command(
                redis, {"XGROUP", "DESTROY", std::string(kChanges), group});
        } catch (...) {}
        done->set_value();
    }

    std::unique_ptr<ruvia::StopSource> stop_;
    std::future<void> stopped_;
    std::future<void> leasesStopped_;
    std::size_t collectorWorkerCount_ = 0;
};

} // namespace service::live
