#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include <ruvia/core/Task.h>
#include <ruvia/core/Timer.h>
#include <ruvia/web/WebWorker.h>

#include "service/common/bridge/message.contract.h"
#include "service/modules/northbridge/config/runtime_config.projector.h"
#include "service/modules/southbridge/queue/redis_stream_async.h"

namespace service::northbridge::config {

inline std::atomic_bool runtimeConfigProjectionRequested{false};

inline void requestRuntimeConfigProjection() noexcept {
    runtimeConfigProjectionRequested.store(true, std::memory_order_release);
}

// PostgreSQL is the configuration source of truth. One Northbridge coroutine periodically
// reconciles it to Redis so a transient Redis failure after a committed CRUD request heals
// automatically without allowing Southbridge to read PostgreSQL.
class RuntimeConfigReconciler final {
  public:
    RuntimeConfigReconciler() = default;
    RuntimeConfigReconciler(const RuntimeConfigReconciler&) = delete;
    RuntimeConfigReconciler& operator=(const RuntimeConfigReconciler&) = delete;
    ~RuntimeConfigReconciler() { stop(); }

    void start(ruvia::WebWorkerHandle worker, std::size_t southWorkerCount) {
        if (running_.exchange(true))
            return;
        worker_ = std::move(worker);
        southWorkerCount_ = southWorkerCount;
        if (southWorkerCount_ == 0) {
            running_.store(false);
            throw std::runtime_error("runtime config reconciler requires south workers");
        }
        auto ready = std::make_shared<std::promise<void>>();
        auto stopped = std::make_shared<std::promise<void>>();
        auto readiness = ready->get_future();
        stopped_ = stopped->get_future().share();
        const auto posted = worker_.post([this, ready, stopped](ruvia::WebWorkerContext& context) {
            return run(context, ready, stopped);
        });
        if (posted != ruvia::PostResult::kAccepted) {
            running_.store(false);
            throw std::runtime_error("north worker rejected runtime config reconciler");
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
    ruvia::Task<void> run(ruvia::WebWorkerContext& context,
                          std::shared_ptr<std::promise<void>> ready,
                          std::shared_ptr<std::promise<void>> stopped) {
        try {
            // Startup already projected the first snapshot. Signal readiness immediately so
            // the application lifecycle never waits on an unavailable Redis a second time.
            ready->set_value();
            auto nextPeriodicProjection =
                std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (running_.load() && !context.stopToken().stopRequested()) {
                auto delay = std::chrono::milliseconds(100);
                const auto now = std::chrono::steady_clock::now();
                const auto requested =
                    runtimeConfigProjectionRequested.exchange(false, std::memory_order_acq_rel);
                if (requested || now >= nextPeriodicProjection) {
                    try {
                        const auto version = co_await projectRuntimeConfig(context);
                        if (version != lastNotifiedVersion_) {
                            co_await publishWorkerNotifications(context, version);
                            lastNotifiedVersion_ = version;
                        }
                        nextPeriodicProjection =
                            std::chrono::steady_clock::now() + std::chrono::seconds(5);
                    } catch (const std::exception& error) {
                        std::cerr << "runtime config reconciliation failed: " << error.what()
                                  << '\n';
                        requestRuntimeConfigProjection();
                        delay = std::chrono::milliseconds(500);
                    }
                }
                // Sleep in short interruptible slices. ruvia timers are coroutine based, but a
                // single five-second sleep would still make Ctrl-C wait for that timer before the
                // reconciler can release its worker handle.
                constexpr auto stopPollInterval = std::chrono::milliseconds(100);
                for (auto remaining = delay; remaining.count() > 0 && running_.load() &&
                                             !context.stopToken().stopRequested();) {
                    const auto slice = std::min(remaining, stopPollInterval);
                    (void)co_await ruvia::sleepFor(context.worker(), slice);
                    remaining -= slice;
                }
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

    static constexpr std::size_t kConfigStreamCapacity = 10000;

    ruvia::Task<void> publishWorkerNotifications(ruvia::WebWorkerContext& context,
                                                 std::string_view version) {
        const auto createdAt = std::to_string(service::bridge::utcNowMilliseconds());
        for (std::size_t workerIndex = 0; workerIndex < southWorkerCount_; ++workerIndex) {
            (void)co_await service::bridge::redis_async::publish(
                context.redis(), service::bridge::configStream(workerIndex),
                {{"message_id", service::bridge::nextMessageId()},
                 {"version", std::string(version)},
                 {"worker_id", std::to_string(workerIndex)},
                 {"created_at_ms", createdAt}},
                kConfigStreamCapacity);
        }
    }

    ruvia::WebWorkerHandle worker_;
    std::shared_future<void> stopped_;
    std::size_t southWorkerCount_ = 0;
    std::string lastNotifiedVersion_;
    std::atomic_bool running_{false};
};

} // namespace service::northbridge::config
