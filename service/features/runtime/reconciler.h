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
#include "service/features/runtime/repository.h"
#include "service/features/collector/stream.h"

namespace service::runtime {

// PostgreSQL is the configuration source of truth. Every Service Worker owns a disjoint set
// of durable config shards and independently projects the versioned Redis snapshot.
class Reconciler final {
  public:
    Reconciler() = default;
    Reconciler(const Reconciler&) = delete;
    Reconciler& operator=(const Reconciler&) = delete;
    ~Reconciler() { stop(); }

    void start(std::vector<ruvia::WebWorkerHandle> workers,
               std::size_t collectorWorkerCount) {
        if (running_.exchange(true))
            return;
        workers_ = std::move(workers);
        collectorWorkerCount_ = collectorWorkerCount;
        if (workers_.empty() || workers_.size() > service::message::shard::kCount ||
            collectorWorkerCount_ == 0) {
            running_.store(false);
            throw std::runtime_error(
                "runtime reconciler requires valid Service and Collector Workers");
        }
        std::vector<std::future<void>> readiness;
        readiness.reserve(workers_.size());
        stopped_.reserve(workers_.size());
        for (std::size_t index = 0; index < workers_.size(); ++index) {
            auto ready = std::make_shared<std::promise<void>>();
            auto stopped = std::make_shared<std::promise<void>>();
            readiness.push_back(ready->get_future());
            stopped_.push_back(stopped->get_future().share());
            const auto posted = workers_[index].post(
                [this, index, ready, stopped](ruvia::WebWorkerContext& context) {
                    return run(context, index, ready, stopped);
                });
            if (!posted.accepted()) {
                running_.store(false);
                throw std::runtime_error("service worker rejected runtime reconciler");
            }
        }
        for (auto& ready : readiness)
            ready.get();
    }

    void stop() noexcept {
        if (!running_.exchange(false))
            return;
        for (const auto& stopped : stopped_)
            if (stopped.valid())
                (void)stopped.wait_for(std::chrono::seconds(3));
        stopped_.clear();
        workers_.clear();
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

    ruvia::Task<void> run(ruvia::WebWorkerContext& context, std::size_t index,
                           std::shared_ptr<std::promise<void>> ready,
                           std::shared_ptr<std::promise<void>> stopped) {
        try {
            const auto redis = context.redis();
            std::vector<std::string> streams;
            for (auto shardIndex = index;
                 shardIndex < service::message::shard::kCount;
                 shardIndex += workers_.size())
                streams.push_back(
                    service::message::runtimeConfigChangesStream(shardIndex));
            // Worker 0 drains entries produced before config Streams were sharded.
            if (index == 0)
                streams.emplace_back(service::message::kRuntimeConfigChangesStream);
            for (const auto& stream : streams)
                co_await service::message::redis::ensureGroup(redis, stream, kGroup);
            ready->set_value();
            bool recovering = true;
            const auto consumer = "service-" + std::to_string(index);
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
                std::vector<service::message::redis::StreamBatch> batches;
                bool readFailed = false;
                try {
                    if (recovering) {
                        batches = co_await service::message::redis::claimGroupMany(
                            redis, streams, kGroup, consumer, kBatchSize);
                    } else {
                        std::optional<std::chrono::milliseconds> timeout;
                        if (cleanupDeadline) {
                            timeout = std::max(
                                std::chrono::milliseconds(1),
                                std::chrono::duration_cast<std::chrono::milliseconds>(
                                    *cleanupDeadline - std::chrono::steady_clock::now()));
                        }
                        batches = co_await service::message::redis::readGroupManyBlockingUntil(
                            redis, streams, kGroup, consumer, context.stopToken(), timeout,
                            kBatchSize);
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
                if (recovering && batches.empty()) {
                    recovering = false;
                    continue;
                }
                if (batches.empty())
                    continue;
                bool failed = false;
                try {
                    if (!recovering) {
                        // Absorb the rest of a CRUD burst before taking the global projection
                        // lock. Recovery already has a durable backlog and needs no delay.
                        (void)co_await ruvia::sleepFor(context.worker(), kCoalesceWindow);
                    }
                    std::size_t messageCount = 0;
                    for (const auto& batch : batches)
                        messageCount += batch.messages.size();
                    while (!recovering && messageCount < kCoalesceLimit) {
                        const auto remaining = kCoalesceLimit - messageCount;
                        auto next = co_await service::message::redis::readGroupMany(
                            redis, streams, kGroup, consumer, ">",
                            std::min(kBatchSize, remaining));
                        if (next.empty())
                            break;
                        for (auto& batch : next) {
                            messageCount += batch.messages.size();
                            batches.push_back(std::move(batch));
                        }
                    }
                    std::vector<service::message::StreamMessage> received;
                    received.reserve(messageCount);
                    for (const auto& batch : batches)
                        received.insert(received.end(), batch.messages.begin(),
                                        batch.messages.end());
                    const auto eventIds = service::message::idempotency::eventIds(received);
                    const auto pendingIds = co_await service::message::idempotency::pending(
                        context, kGroup, eventIds);
                    const auto projectionRequired = std::ranges::any_of(
                        batches, [&pendingIds](const auto& batch) {
                            return std::ranges::any_of(batch.messages, [&pendingIds](const auto& message) {
                                return service::message::idempotency::shouldProcess(
                                           message, pendingIds) &&
                                       requiresProjection(message);
                            });
                        });
                    const auto alertRefreshRequired = std::ranges::any_of(
                        batches, [&pendingIds](const auto& batch) {
                            return std::ranges::any_of(batch.messages, [&pendingIds](const auto& message) {
                                return service::message::idempotency::shouldProcess(
                                           message, pendingIds) &&
                                       message.get("aggregate") == "device";
                            });
                        });
                    if (projectionRequired) {
                        co_await projectAndNotify(context);
                        cleanupDeadline = std::chrono::steady_clock::now() +
                                          std::chrono::milliseconds(
                                              service::collector::config::
                                                  kSnapshotGraceMilliseconds);
                    }
                    if (alertRefreshRequired)
                        co_await service::alert::metadata::refresh(context);
                    co_await service::message::idempotency::markProcessed(
                        context, kGroup, eventIds);
                    for (const auto& batch : batches)
                        co_await service::message::redis::acknowledgeAndDeleteMany(
                            redis, batch.stream, kGroup, batch.messages);
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

    ruvia::Task<void> projectAndNotify(ruvia::WebWorkerContext& context) {
        auto transaction = co_await context.db().beginTransaction();
        // Keep snapshot activation and Collector notifications under one global order.
        // A retry can repeat the same version, but can never publish an older version
        // after a newer projection has been notified.
        (void)co_await transaction.query(
            "SELECT pg_advisory_xact_lock(5282804697543808067::bigint)");
        auto snapshot =
            co_await service::runtime::repository::loadRuntimeSnapshot(transaction);
        const auto version =
            co_await service::collector::config::project(context.redis(), snapshot);
        co_await publishWorkerNotifications(context, version);
        co_await transaction.commit();
    }

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

    std::vector<ruvia::WebWorkerHandle> workers_;
    std::vector<std::shared_future<void>> stopped_;
    std::size_t collectorWorkerCount_ = 0;
    std::atomic_bool running_{false};
};

} // namespace service::runtime
