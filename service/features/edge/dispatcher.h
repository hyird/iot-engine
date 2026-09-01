#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/core/StopToken.h>
#include <ruvia/core/Timer.h>
#include <ruvia/web/WebWorker.h>

#include "service/features/collector/stream.h"
#include "service/features/edge/dispatch.h"
#include "service/features/event/stream-multiplexer.h"

namespace service::edge {

// One instance is created by App::useWorkerState for every Service Worker.
// The session table and Redis notification Stream are worker-local: no wakeup,
// callback, or socket is forwarded to another worker.
class Dispatcher final {
  public:
    using SessionWake = std::function<void()>;

    Dispatcher() = default;
    Dispatcher(const Dispatcher&) = delete;
    Dispatcher& operator=(const Dispatcher&) = delete;
    Dispatcher(Dispatcher&&) noexcept = default;
    Dispatcher& operator=(Dispatcher&&) = delete;

    [[nodiscard]] std::size_t workerIndex() const {
        if (!workerIndex_)
            throw std::runtime_error("edge dispatcher is not ready");
        return *workerIndex_;
    }

    void registerSession(std::string nodeId, std::uint64_t epoch, SessionWake wake) {
        if (!running_ || !wake)
            throw std::runtime_error("edge dispatcher is not ready");
        sessions_.insert_or_assign(std::move(nodeId),
                                   SessionTarget{epoch, std::move(wake)});
    }

    void unregisterSession(std::string_view nodeId, std::uint64_t epoch) noexcept {
        const auto current = sessions_.find(nodeId);
        if (current != sessions_.end() && current->second.epoch == epoch)
            sessions_.erase(current);
    }

    void requestStop() noexcept {
        if (stopSource_)
            stopSource_->requestStop();
        service::message::workerStreamMultiplexer().signal(
            service::message::WorkerStreamTask::EdgeDispatcher);
    }

    ruvia::Task<void> run(ruvia::WebWorkerContext& context, std::size_t workerIndex,
                          std::shared_ptr<std::promise<void>> ready,
                          std::shared_ptr<std::promise<void>> stopped) {
        bool readySet = false;
        try {
            if (running_)
                throw std::runtime_error("edge dispatcher is already running");
            workerIndex_ = workerIndex;
            stopSource_ = std::make_unique<ruvia::StopSource>();
            const auto redis = context.redis();
            const auto workerStream = dispatch::stream(workerIndex);
            const auto consumer = "service-edge-dispatcher-" +
                                  std::to_string(workerIndex);
            co_await service::message::redis::ensureGroup(
                redis, workerStream, dispatch::kGroup);
            running_ = true;
            ready->set_value();
            readySet = true;
            bool recovering = true;
            const auto stopToken = ruvia::combineStopTokens(
                context.stopToken(), stopSource_->token());
            while (!stopToken.stopRequested()) {
                bool failed = false;
                try {
                    auto messages = recovering
                        ? co_await service::message::redis::readGroup(
                              redis, workerStream, dispatch::kGroup, consumer, "0",
                              std::chrono::milliseconds(0), 256)
                        : co_await service::message::redis::readGroup(
                              redis, workerStream, dispatch::kGroup, consumer, ">",
                              std::chrono::milliseconds(0), 256);
                    if (recovering && messages.empty()) {
                        recovering = false;
                        continue;
                    }
                    if (messages.empty()) {
                        co_await service::message::workerStreamMultiplexer().wait(
                            workerIndex,
                            service::message::WorkerStreamTask::EdgeDispatcher,
                            stopToken);
                        continue;
                    }
                    for (const auto& message : messages) {
                        const auto event = dispatch::eventFrom(message);
                        if (event.kind == dispatch::kNodeKind &&
                            !event.nodeId.empty() && !deliver(event.nodeId)) {
                            recovering = true;
                            failed = true;
                            break;
                        }
                        co_await service::message::redis::acknowledgeAndDelete(
                            redis, workerStream, dispatch::kGroup, message.id);
                    }
                } catch (const std::exception& error) {
                    if (stopToken.stopRequested())
                        break;
                    std::cerr << "edge dispatch worker " << workerIndex
                              << " read failed: " << error.what() << '\n';
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
        running_ = false;
        sessions_.clear();
        stopSource_.reset();
        try {
            stopped->set_value();
        } catch (...) {
        }
    }

  private:
    struct SessionTarget {
        std::uint64_t epoch{};
        SessionWake wake;
    };

    bool deliver(std::string_view nodeId) {
        const auto current = sessions_.find(nodeId);
        if (current == sessions_.end())
            return true;
        try {
            current->second.wake();
            return true;
        } catch (const std::exception& error) {
            std::cerr << "edge dispatch worker " << *workerIndex_
                      << " wake failed: " << error.what() << '\n';
            return false;
        } catch (...) {
            std::cerr << "edge dispatch worker " << *workerIndex_
                      << " wake failed: unknown error\n";
            return false;
        }
    }

    std::map<std::string, SessionTarget, std::less<>> sessions_;
    std::unique_ptr<ruvia::StopSource> stopSource_;
    std::optional<std::size_t> workerIndex_;
    bool running_{false};
};

// Process-level lifecycle only. All functional state lives in the worker-local
// Dispatcher instances above.
class DispatcherRuntime final {
  public:
    DispatcherRuntime() = default;
    DispatcherRuntime(const DispatcherRuntime&) = delete;
    DispatcherRuntime& operator=(const DispatcherRuntime&) = delete;
    ~DispatcherRuntime() { stop(); }

    void start(std::vector<ruvia::WebWorkerHandle> workers) {
        if (running_.exchange(true))
            return;
        if (workers.empty()) {
            running_.store(false);
            throw std::runtime_error("edge dispatcher requires service workers");
        }
        workers_ = std::move(workers);
        stopped_.reserve(workers_.size());
        std::vector<std::future<void>> readyFutures;
        readyFutures.reserve(workers_.size());
        try {
            for (std::size_t index = 0; index < workers_.size(); ++index) {
                auto ready = std::make_shared<std::promise<void>>();
                auto stopped = std::make_shared<std::promise<void>>();
                readyFutures.push_back(ready->get_future());
                stopped_.push_back(stopped->get_future().share());
                const auto posted = workers_[index].post(
                    [index, ready, stopped](ruvia::WebWorkerContext& context) {
                        return context.workerState<Dispatcher>().run(
                            context, index, ready, stopped);
                    });
                if (!posted.accepted())
                    throw std::runtime_error(
                        "service worker rejected edge dispatcher");
            }
            for (auto& ready : readyFutures)
                ready.get();
        } catch (...) {
            stop();
            throw;
        }
    }

    void stop() noexcept {
        if (!running_.exchange(false))
            return;
        for (const auto& worker : workers_) {
            (void)worker.post([](ruvia::WebWorkerContext& context)
                                  -> ruvia::Task<void> {
                context.workerState<Dispatcher>().requestStop();
                co_return;
            });
        }
        for (auto& stopped : stopped_)
            if (stopped.valid())
                (void)stopped.wait_for(std::chrono::seconds(3));
        stopped_.clear();
        workers_.clear();
    }

  private:
    std::vector<ruvia::WebWorkerHandle> workers_;
    std::vector<std::shared_future<void>> stopped_;
    std::atomic_bool running_{false};
};

inline DispatcherRuntime& dispatcherRuntime() {
    static DispatcherRuntime instance;
    return instance;
}

} // namespace service::edge
