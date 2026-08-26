#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/core/Timer.h>
#include <ruvia/web/WebWorker.h>

#include "service/common/message/contract.h"
#include "service/features/alert/metadata.h"
#include "service/features/event/config.h"
#include "service/features/event/idempotency.h"
#include "service/features/runtime/projector.h"
#include "service/features/collector/stream.h"

namespace service::runtime {

// PostgreSQL is the configuration source of truth. CRUD commits enqueue durable change events;
// this single Service coroutine blocks on that stream and projects a versioned Redis snapshot.
class Reconciler final {
  public:
    Reconciler() = default;
    Reconciler(const Reconciler&) = delete;
    Reconciler& operator=(const Reconciler&) = delete;
    ~Reconciler() { stop(); }

    void start(ruvia::WebWorkerHandle worker, std::size_t collectorWorkerCount) {
        if (running_.exchange(true))
            return;
        worker_ = std::move(worker);
        collectorWorkerCount_ = collectorWorkerCount;
        if (collectorWorkerCount_ == 0) {
            running_.store(false);
            throw std::runtime_error("runtime reconciler requires collector workers");
        }
        auto ready = std::make_shared<std::promise<void>>();
        auto stopped = std::make_shared<std::promise<void>>();
        auto readiness = ready->get_future();
        stopped_ = stopped->get_future().share();
        const auto posted = worker_.post([this, ready, stopped](ruvia::WebWorkerContext& context) {
            return run(context, ready, stopped);
        });
        if (!posted.accepted()) {
            running_.store(false);
            throw std::runtime_error("service worker rejected runtime reconciler");
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
    static constexpr std::string_view kGroup{"iot-engine:runtime-reconciler"};
    static constexpr std::size_t kBatchSize = 256;
    static constexpr std::size_t kCoalesceLimit = 4096;
    static constexpr auto kCoalesceWindow = std::chrono::milliseconds(25);

    static bool requiresProjection(const service::message::StreamMessage& message) {
        const auto aggregate = message.get("aggregate");
        return aggregate == "link" || aggregate == "device" || aggregate == "protocol";
    }

    ruvia::Task<void> run(ruvia::WebWorkerContext& context,
                          std::shared_ptr<std::promise<void>> ready,
                          std::shared_ptr<std::promise<void>> stopped) {
        try {
            const auto redis = context.redis();
            co_await service::message::redis::ensureGroup(
                redis, service::message::kRuntimeConfigChangesStream, kGroup);
            ready->set_value();
            bool recovering = true;
            std::optional<std::chrono::steady_clock::time_point> cleanupDeadline =
                std::chrono::steady_clock::now();
            while (running_.load() && !context.stopToken().stopRequested()) {
                if (!recovering && cleanupDeadline &&
                    std::chrono::steady_clock::now() >= *cleanupDeadline) {
                    try {
                        co_await service::collector::config::cleanupExpiredSnapshots(
                            redis, service::message::utcNowMilliseconds());
                        cleanupDeadline.reset();
                    } catch (const std::exception& error) {
                        std::cerr << "runtime config cleanup failed: " << error.what() << '\n';
                        cleanupDeadline = std::chrono::steady_clock::now() +
                                          std::chrono::milliseconds(250);
                    }
                    continue;
                }
                std::vector<service::message::StreamMessage> messages;
                bool readFailed = false;
                try {
                    if (recovering) {
                        messages = co_await service::message::redis::readGroup(
                            redis, service::message::kRuntimeConfigChangesStream, kGroup,
                            "service-0", "0", std::chrono::milliseconds(0), kBatchSize);
                    } else {
                        std::optional<std::chrono::milliseconds> timeout;
                        if (cleanupDeadline) {
                            timeout = std::max(
                                std::chrono::milliseconds(1),
                                std::chrono::duration_cast<std::chrono::milliseconds>(
                                    *cleanupDeadline - std::chrono::steady_clock::now()));
                        }
                        messages = co_await service::message::redis::readGroupBlockingUntil(
                            redis, service::message::kRuntimeConfigChangesStream, kGroup,
                            "service-0", context.stopToken(), timeout, kBatchSize);
                    }
                } catch (const std::exception& error) {
                    if (context.stopToken().stopRequested())
                        break;
                    std::cerr << "runtime config stream read failed: " << error.what() << '\n';
                    recovering = true;
                    readFailed = true;
                }
                if (readFailed) {
                    (void)co_await ruvia::sleepFor(context.worker(),
                                                   std::chrono::milliseconds(250));
                    continue;
                }
                if (recovering && messages.empty()) {
                    recovering = false;
                    continue;
                }
                if (messages.empty())
                    continue;
                std::vector<std::vector<service::message::StreamMessage>> batches;
                batches.push_back(std::move(messages));
                bool failed = false;
                try {
                    if (!recovering) {
                        // Absorb the rest of a CRUD burst before taking the global projection
                        // lock. Recovery already has a durable backlog and needs no delay.
                        (void)co_await ruvia::sleepFor(context.worker(), kCoalesceWindow);
                    }
                    std::size_t messageCount = batches.front().size();
                    std::string recoveryCursor = batches.front().back().id;
                    while (messageCount < kCoalesceLimit) {
                        const auto remaining = kCoalesceLimit - messageCount;
                        auto next = co_await service::message::redis::readGroup(
                            redis, service::message::kRuntimeConfigChangesStream, kGroup,
                            "service-0", recovering ? std::string_view(recoveryCursor)
                                                     : std::string_view(">"),
                            std::chrono::milliseconds(0), std::min(kBatchSize, remaining));
                        if (next.empty())
                            break;
                        messageCount += next.size();
                        recoveryCursor = next.back().id;
                        batches.push_back(std::move(next));
                    }
                    std::vector<service::message::StreamMessage> received;
                    received.reserve(messageCount);
                    for (const auto& batch : batches)
                        received.insert(received.end(), batch.begin(), batch.end());
                    const auto eventIds = service::message::idempotency::eventIds(received);
                    const auto pendingIds = co_await service::message::idempotency::pending(
                        context, kGroup, eventIds);
                    const auto projectionRequired = std::ranges::any_of(
                        batches, [&pendingIds](const auto& batch) {
                            return std::ranges::any_of(batch, [&pendingIds](const auto& message) {
                                return service::message::idempotency::shouldProcess(
                                           message, pendingIds) &&
                                       requiresProjection(message);
                            });
                        });
                    const auto alertRefreshRequired = std::ranges::any_of(
                        batches, [&pendingIds](const auto& batch) {
                            return std::ranges::any_of(batch, [&pendingIds](const auto& message) {
                                return service::message::idempotency::shouldProcess(
                                           message, pendingIds) &&
                                       message.get("aggregate") == "device";
                            });
                        });
                    if (projectionRequired) {
                        const auto version = co_await project(context);
                        cleanupDeadline = std::chrono::steady_clock::now() +
                                          std::chrono::milliseconds(
                                              service::collector::config::
                                                  kSnapshotGraceMilliseconds);
                        if (version != lastNotifiedVersion_) {
                            co_await publishWorkerNotifications(context, version);
                            lastNotifiedVersion_ = version;
                        }
                    }
                    if (alertRefreshRequired)
                        co_await service::alert::metadata::refresh(context);
                    co_await service::message::idempotency::markProcessed(
                        context, kGroup, eventIds);
                    for (const auto& batch : batches)
                        co_await service::message::redis::acknowledgeAndDeleteMany(
                            redis, service::message::kRuntimeConfigChangesStream, kGroup,
                            batch);
                } catch (const std::exception& error) {
                    if (context.stopToken().stopRequested())
                        break;
                    std::cerr << "runtime reconciliation failed: " << error.what() << '\n';
                    recovering = true;
                    failed = true;
                }
                if (failed)
                    (void)co_await ruvia::sleepFor(context.worker(),
                                                   std::chrono::milliseconds(250));
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
        const auto createdAt = std::to_string(service::message::utcNowMilliseconds());
        for (std::size_t workerIndex = 0; workerIndex < collectorWorkerCount_; ++workerIndex) {
            (void)co_await service::message::redis::publish(
                context.redis(), service::message::configStream(workerIndex),
                {{"message_id", service::message::nextMessageId()},
                 {"version", std::string(version)},
                 {"worker_id", std::to_string(workerIndex)},
                 {"created_at_ms", createdAt}},
                kConfigStreamCapacity);
        }
    }

    ruvia::WebWorkerHandle worker_;
    std::shared_future<void> stopped_;
    std::size_t collectorWorkerCount_ = 0;
    std::string lastNotifiedVersion_;
    std::atomic_bool running_{false};
};

} // namespace service::runtime
