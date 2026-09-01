#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <ruvia/core/Channel.h>
#include <ruvia/core/StopToken.h>
#include <ruvia/core/Task.h>
#include <ruvia/core/Timer.h>
#include <ruvia/web/WebWorker.h>

#include "service/features/collector/stream.h"
#include "service/features/event/worker-wake.h"

namespace service::message {

// Each Service Worker consumes one durable wake Stream. Wake entries carry only a
// task name; business payload, consumer groups, pending recovery, and ACK semantics
// remain owned by the seven independent worker-local tasks.
class WorkerStreamMultiplexer final {
  public:
    WorkerStreamMultiplexer() = default;
    WorkerStreamMultiplexer(const WorkerStreamMultiplexer&) = delete;
    WorkerStreamMultiplexer& operator=(const WorkerStreamMultiplexer&) = delete;
    ~WorkerStreamMultiplexer() { stop(); }

    void configure(const std::vector<ruvia::WebWorkerHandle>& workers) {
        if (running_.load())
            throw std::runtime_error("worker stream multiplexer is running");
        if (workers.empty())
            throw std::runtime_error("worker stream multiplexer requires Service Workers");
        {
            std::lock_guard lock(mutex_);
            slots_.clear();
            slots_.resize(workers.size());
        }
        std::vector<std::future<void>> readiness;
        readiness.reserve(workers.size());
        for (std::size_t index = 0; index < workers.size(); ++index) {
            auto ready = std::make_shared<std::promise<void>>();
            readiness.push_back(ready->get_future());
            const auto posted = workers[index].post(
                [this, index, ready](ruvia::WebWorkerContext& context) {
                    return initialize(context, index, ready);
                });
            if (!posted.accepted())
                throw std::runtime_error(
                    "service worker rejected stream multiplexer initialization");
        }
        for (auto& ready : readiness)
            ready.get();
    }

    ruvia::Task<void> wait(std::size_t workerIndex, WorkerStreamTask task,
                           ruvia::StopToken stopToken,
                           std::optional<std::chrono::milliseconds> maximum = std::nullopt) {
        const ruvia::ChannelReceiver<std::uint8_t>* receiver = nullptr;
        {
            std::lock_guard lock(mutex_);
            receiver = &*requireSlot(workerIndex).receivers[taskIndex(task)];
        }
        if (maximum.has_value()) {
            if (maximum->count() <= 0)
                co_return;
            (void)co_await receiver->receiveFor(*maximum, std::move(stopToken));
        } else {
            (void)co_await receiver->receive(std::move(stopToken));
        }
    }

    void signal(WorkerStreamTask task) noexcept {
        std::lock_guard lock(mutex_);
        for (const auto& slot : slots_)
            if (slot)
                (void)slot->senders[taskIndex(task)].send(1);
    }

    void start(std::vector<ruvia::WebWorkerHandle> workers) {
        if (running_.exchange(true))
            return;
        if (workers.empty()) {
            running_.store(false);
            throw std::runtime_error("worker stream multiplexer requires Service Workers");
        }
        {
            std::lock_guard lock(mutex_);
            if (slots_.size() != workers.size()) {
                running_.store(false);
                throw std::runtime_error("worker stream multiplexer is not configured");
            }
            for (const auto& slot : slots_)
                if (!slot) {
                    running_.store(false);
                    throw std::runtime_error("worker stream multiplexer has no channel slot");
                }
        }
        workers_ = std::move(workers);
        stopSource_ = std::make_unique<ruvia::StopSource>();
        stopped_.clear();
        stopped_.reserve(workers_.size());
        std::vector<std::future<void>> readiness;
        readiness.reserve(workers_.size());
        try {
            for (std::size_t index = 0; index < workers_.size(); ++index) {
                auto ready = std::make_shared<std::promise<void>>();
                auto stopped = std::make_shared<std::promise<void>>();
                readiness.push_back(ready->get_future());
                stopped_.push_back(stopped->get_future().share());
                const auto posted = workers_[index].post(
                    [this, index, ready, stopped](ruvia::WebWorkerContext& context) {
                        return run(context, index, ready, stopped);
                    });
                if (!posted.accepted())
                    throw std::runtime_error(
                        "service worker rejected stream multiplexer");
            }
            for (auto& ready : readiness)
                ready.get();
        } catch (...) {
            stop();
            throw;
        }
    }

    void stop() noexcept {
        if (!running_.exchange(false))
            return;
        if (stopSource_)
            stopSource_->requestStop();
        signalAll();
        for (const auto& stopped : stopped_)
            if (stopped.valid())
                (void)stopped.wait_for(std::chrono::seconds(3));
        stopped_.clear();
        workers_.clear();
        stopSource_.reset();
    }

  private:
    static constexpr std::size_t kTaskCount =
        static_cast<std::size_t>(WorkerStreamTask::Count);
    static constexpr std::string_view kGroup{"iot-engine:service-worker-wake"};
    static constexpr std::size_t kBatchSize = 256;

    struct WorkerSlot final {
        std::array<ruvia::ChannelSender<std::uint8_t>, kTaskCount> senders;
        std::array<std::optional<ruvia::ChannelReceiver<std::uint8_t>>, kTaskCount>
            receivers;
    };

    [[nodiscard]] static constexpr std::size_t taskIndex(WorkerStreamTask task) {
        return static_cast<std::size_t>(task);
    }

    WorkerSlot& requireSlot(std::size_t workerIndex) {
        if (workerIndex >= slots_.size() || !slots_[workerIndex])
            throw std::out_of_range("worker stream multiplexer slot is unavailable");
        return *slots_[workerIndex];
    }

    ruvia::Task<void> initialize(ruvia::WebWorkerContext& context, std::size_t index,
                                 std::shared_ptr<std::promise<void>> ready) {
        try {
            auto slot = std::make_unique<WorkerSlot>();
            for (std::size_t task = 0; task < kTaskCount; ++task) {
                auto [sender, receiver] =
                    ruvia::makeChannel<std::uint8_t>(context.worker(), {.capacity = 1});
                slot->senders[task] = std::move(sender);
                slot->receivers[task].emplace(std::move(receiver));
            }
            {
                std::lock_guard lock(mutex_);
                slots_.at(index) = std::move(slot);
            }
            ready->set_value();
        } catch (...) {
            try {
                ready->set_exception(std::current_exception());
            } catch (...) {
            }
        }
        co_return;
    }

    void signal(std::size_t workerIndex, WorkerStreamTask task) noexcept {
        std::lock_guard lock(mutex_);
        if (workerIndex >= slots_.size() || !slots_[workerIndex])
            return;
        (void)slots_[workerIndex]->senders[taskIndex(task)].send(1);
    }

    void signalAll() noexcept {
        std::lock_guard lock(mutex_);
        for (const auto& slot : slots_) {
            if (!slot)
                continue;
            for (const auto& sender : slot->senders)
                (void)sender.send(1);
        }
    }

    ruvia::Task<void> run(ruvia::WebWorkerContext& context, std::size_t index,
                          std::shared_ptr<std::promise<void>> ready,
                          std::shared_ptr<std::promise<void>> stopped) {
        bool readySet = false;
        try {
            const auto redis = context.redis();
            const auto wakeStream = workerWakeStream(index);
            const auto consumer = "service-" + std::to_string(index);
            co_await service::message::redis::ensureGroup(redis, wakeStream, kGroup);
            ready->set_value();
            readySet = true;
            const auto stopToken = ruvia::combineStopTokens(
                context.stopToken(), stopSource_->token());
            bool recovering = true;
            while (!stopToken.stopRequested()) {
                bool failed = false;
                try {
                    std::vector<service::message::redis::StreamBatch> batches;
                    if (recovering) {
                        const std::vector<std::string> streams{wakeStream};
                        batches = co_await service::message::redis::claimGroupMany(
                            redis, streams, kGroup, consumer, kBatchSize);
                    } else {
                        auto messages =
                            co_await service::message::redis::readGroupBlocking(
                                redis, wakeStream, kGroup, consumer, stopToken,
                                kBatchSize);
                        if (!messages.empty())
                            batches.push_back({wakeStream, std::move(messages)});
                    }
                    if (recovering && batches.empty()) {
                        recovering = false;
                        continue;
                    }
                    for (const auto& batch : batches) {
                        for (const auto& message : batch.messages) {
                            const auto task = workerStreamTask(message.get("task"));
                            if (task)
                                signal(index, *task);
                            else
                                std::cerr << "service worker " << index
                                          << " received an unknown wake task\n";
                        }
                        co_await service::message::redis::acknowledgeAndDeleteMany(
                            redis, batch.stream, kGroup, batch.messages);
                    }
                } catch (const std::exception& error) {
                    if (stopToken.stopRequested())
                        break;
                    std::cerr << "service worker " << index
                              << " wake stream failed: " << error.what() << '\n';
                    recovering = true;
                    failed = true;
                }
                if (failed)
                    (void)co_await ruvia::sleepFor(
                        context.worker(), std::chrono::milliseconds(250));
            }
        } catch (...) {
            if (!readySet) {
                try {
                    ready->set_exception(std::current_exception());
                } catch (...) {
                }
            }
        }
        try {
            stopped->set_value();
        } catch (...) {
        }
    }

    std::mutex mutex_;
    std::vector<std::unique_ptr<WorkerSlot>> slots_;
    std::vector<ruvia::WebWorkerHandle> workers_;
    std::vector<std::shared_future<void>> stopped_;
    std::unique_ptr<ruvia::StopSource> stopSource_;
    std::atomic_bool running_{false};
};

inline WorkerStreamMultiplexer& workerStreamMultiplexer() {
    static WorkerStreamMultiplexer instance;
    return instance;
}

} // namespace service::message
