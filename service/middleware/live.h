#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/core/Channel.h>
#include <ruvia/web/Controller.h>
#include <ruvia/web/ModelJson.h>
#include <ruvia/web/WebWorker.h>

#include "service/common/http.h"
#include "service/common/message.h"
#include "service/common/uuid.h"
#include "service/middleware/auth.h"
#include "service/utils/redis.h"

namespace service::live {

// Notifications carry no data or authorization decisions. A subscriber always
// rebuilds its own authorized snapshot. Capacity one intentionally coalesces
// intermediate changes while a slow subscriber is writing its current snapshot.
class Bus final {
  public:
    struct Subscription {
        std::string topic;
        ruvia::ChannelSender<int> sender;
        ruvia::ChannelReceiver<int> receiver;
    };

    void setWorkerIndex(std::size_t workerIndex) noexcept {
        workerIndex_ = workerIndex;
    }

    [[nodiscard]] std::size_t workerIndex() const {
        if (!workerIndex_) {
            throw std::logic_error("live bus worker index is not initialized");
        }
        return *workerIndex_;
    }

    std::shared_ptr<Subscription> subscribe(const ruvia::WorkerHandle& worker, std::string_view topic) {
        if (!worker.isCurrent()) {
            throw std::logic_error("live subscriptions must belong to the current worker");
        }
        auto [sender, receiver] = ruvia::makeChannel<int>(worker, { .capacity = 1 });
        auto subscription = std::make_shared<Subscription>(
            Subscription{ std::string(topic), std::move(sender), std::move(receiver) }
        );
        std::lock_guard lock(mutex_);
        std::erase_if(subscriptions_, [](const auto& entry) {
            return entry.expired();
        });
        subscriptions_.push_back(subscription);
        return subscription;
    }

    void publish(std::string_view topic) {
        std::lock_guard lock(mutex_);
        std::erase_if(subscriptions_, [&](const auto& entry) {
            auto subscription = entry.lock();
            if (!subscription) {
                return true;
            }
            const auto& target = subscription->topic;
            const bool related =
                (target == "device" && (topic == "protocol" || topic == "link" || topic == "edge" || topic == "command")) ||
                (target == "access" &&
                 (topic == "device" || topic == "protocol" || topic == "alert")) ||
                (target == "edge" &&
                 (topic == "device" || topic == "link" || topic == "protocol")) ||
                (target == "alert" && (topic == "protocol" || topic == "device")) ||
                (target == "vpn" && topic == "edge");
            if (topic == "*" || topic == "auth" || target == topic || related) {
                (void)subscription->sender.send(1);
            }
            return false;
        });
    }

  private:
    std::mutex mutex_;
    std::vector<std::weak_ptr<Subscription>> subscriptions_;
    std::optional<std::size_t> workerIndex_;
};

inline Bus& bus() {
    // Every WebWorker owns one event-loop thread. Keeping the bus in thread
    // local storage prevents subscriptions and wakeups from crossing workers;
    // QueryRuntime initializes the worker index before requests are served.
    thread_local Bus value;
    return value;
}

// Each Service Worker owns one API reader. Every worker gets a separate group,
// so each receives every invalidation while the shared stream remains available
// to the feature runtime and other workers/instances.
class QueryRuntime final {
  public:
    void start(ruvia::WebWorkerHandle worker, std::size_t workerIndex) {
        stop_ = std::make_unique<ruvia::StopSource>();
        auto ready = std::make_shared<std::promise<void>>();
        auto done = std::make_shared<std::promise<void>>();
        auto started = ready->get_future();
        stopped_ = done->get_future();
        if (!worker.post([this, workerIndex, ready, done](ruvia::WebWorkerContext& context) {
                       return run(context, workerIndex, ready, done);
                   })
                 .accepted()) {
            stop_.reset();
            throw std::runtime_error("live query worker rejected startup");
        }
        try {
            started.get();
        } catch (...) {
            stop();
            throw;
        }
    }

    void stop() {
        if (!stop_) {
            return;
        }
        stop_->requestStop();
        if (stopped_.valid()) {
            stopped_.wait();
        }
        stop_.reset();
    }

    ~QueryRuntime() { stop(); }

  private:
    ruvia::Task<void> run(ruvia::WebWorkerContext& context, std::size_t workerIndex, std::shared_ptr<std::promise<void>> ready, std::shared_ptr<std::promise<void>> done) {
        bus().setWorkerIndex(workerIndex);
        const auto group = "api-live:" + service::runtime::instanceId() + ":" +
            std::to_string(workerIndex);
        const auto consumer = "api-live:" + service::runtime::instanceId() + ":" +
            std::to_string(workerIndex);
        const auto redis = context.redis();
        const auto stop = ruvia::combineStopTokens(context.stopToken(), stop_->token());
        bool initialized = false;
        try {
            co_await service::message::redis::ensureGroup(
                redis,
                service::message::live::kChanges,
                group
            );
            ready->set_value();
            initialized = true;
            bool recovering = true;
            while (!stop.stopRequested()) {
                bool failed = false;
                try {
                    if (recovering) {
                        co_await service::message::redis::ensureGroup(
                            redis,
                            service::message::live::kChanges,
                            group
                        );
                        // A reconnect can miss an invalidation while the reader is
                        // disconnected. Rebuild every local subscription first.
                        bus().publish("*");
                        const auto pending = co_await service::message::redis::readGroup(
                            redis,
                            service::message::live::kChanges,
                            group,
                            consumer,
                            "0",
                            std::chrono::milliseconds(0),
                            256
                        );
                        for (const auto& change : pending) {
                            co_await service::message::redis::acknowledge(
                                redis,
                                service::message::live::kChanges,
                                group,
                                change.id
                            );
                        }
                        if (!pending.empty()) {
                            continue;
                        }
                        recovering = false;
                    }
                    const auto changes = co_await service::message::redis::readGroupBlocking(
                        redis,
                        service::message::live::kChanges,
                        group,
                        consumer,
                        stop,
                        256
                    );
                    for (const auto& change : changes) {
                        bus().publish(change.get(service::message::live::kTopicField));
                        // ACK advances only this process's group. Never remove a
                        // shared entry needed by another group.
                        co_await service::message::redis::acknowledge(
                            redis,
                            service::message::live::kChanges,
                            group,
                            change.id
                        );
                    }
                } catch (const std::exception& error) {
                    if (stop.stopRequested()) {
                        break;
                    }
                    std::cerr << "live API fanout: " << error.what() << '\n';
                    failed = true;
                    recovering = true;
                }
                if (failed) {
                    (void)co_await ruvia::sleepFor(context.worker(), std::chrono::seconds(1));
                }
            }
        } catch (...) {
            if (!initialized) {
                ready->set_exception(std::current_exception());
            }
        }
        try {
            (void)co_await service::message::redis::command(
                redis,
                { "XGROUP", "DESTROY", std::string(service::message::live::kChanges), group }
            );
        } catch (...) {
        }
        done->set_value();
    }

    std::unique_ptr<ruvia::StopSource> stop_;
    std::future<void> stopped_;
};

template <typename Model>
std::string json(const Model& model) {
    return std::string(ruvia::toJson(model));
}

inline std::string data(ruvia::Context&, std::string_view value) {
    return "{\"code\":0,\"message\":\"ok\",\"data\":" + std::string(value) + "}";
}

// The query callback includes authentication and authorization. It runs before
// opening the stream and before every snapshot; the bus carries invalidations,
// never response data.
template <typename Query>
ruvia::Task<void> serve(ruvia::Context& context, std::string_view topic, Query query, std::function<ruvia::Task<void>()> authorize = {}) {
    using namespace std::chrono_literals;
    if (context.req().header("Accept").value_or("").find("text/event-stream") ==
        std::string_view::npos) {
        service::common::fail(10002, "This query requires text/event-stream", 406);
    }
    auto subscription = bus().subscribe(context.worker(), topic);
    auto snapshot = co_await query();
    context.header("X-Accel-Buffering", "no");
    context.header("Cache-Control", "no-store");
    auto stream = context.streamSse();
    std::uint64_t revision = 1;
    auto id = std::to_string(revision);
    co_await stream.write({ .data = snapshot, .event = "snapshot", .id = id, .retry = 1s });
    // Bound request-arena retention. Reconnection creates a new authorized
    // snapshot; event IDs are connection-local, never misleading replay cursors.
    const auto expires = std::chrono::steady_clock::now() + 5min;
    while (!stream.aborted() && std::chrono::steady_clock::now() < expires) {
        const auto notification =
            co_await subscription->receiver.receiveFor(15s, context.stopToken());
        if (stream.aborted() || context.stopToken().stopRequested()) {
            co_return;
        }
        std::string error;
        std::string next;
        try {
            // Heartbeats only check token expiration. Permission changes publish
            // an auth event, which reexecutes the authorized query immediately.
            if (authorize) {
                co_await authorize();
            } else {
                (void)service::middleware::requireAuth(context);
            }
            if (notification.hasValue()) {
                next = co_await query();
            }
        } catch (const ruvia::HttpError& failure) {
            const auto info = failure.info();
            error = json(service::common::error(context, service::common::errorCode(info.code(), info.status().value()), info.message()));
        } catch (const std::exception&) {
            error = "{\"code\":10004,\"message\":\"Subscription interrupted\"}";
        }
        if (!error.empty()) {
            co_await stream.write({ .data = error, .event = "error" });
            co_return;
        }
        if (notification.hasValue() && next != snapshot) {
            snapshot = std::move(next);
            id = std::to_string(++revision);
            co_await stream.write({ .data = snapshot, .event = "snapshot", .id = id });
        } else {
            co_await stream.write({ .data = "{}", .event = "heartbeat" });
        }
    }
}

} // namespace service::live
