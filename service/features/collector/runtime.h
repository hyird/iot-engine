#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>

#include <ruvia/core/EventLoopPool.h>
#include <ruvia/core/detail/AsioAwait.h>
#include <ruvia/web/redis/Redis.h>

#include "service/features/collector/worker.h"

namespace service::collector {

class Runtime final {
  public:
    Runtime() = default;
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    ~Runtime() { stop(); }

    void start(ruvia::RedisConfig redisConfig, std::size_t workerCount) {
        if (running_.exchange(true))
            return;
        workerCount = std::max<std::size_t>(1, workerCount);
        try {
            pool_ = std::make_unique<ruvia::EventLoopPool>(
                ruvia::EventLoopPoolOptions{.loopCount = workerCount, .mailboxCapacity = 8192});
            workers_.reserve(workerCount);
            for (std::size_t index = 0; index < workerCount; ++index)
                workers_.push_back(std::make_unique<Worker>(
                    pool_->loop(index), redisConfig, index, workerCount,
                    [this](std::string linkId, Tcp::NativeSocket handle,
                           std::string remote) {
                        if (workers_.empty()) {
                            Tcp::closeNative(handle);
                            return;
                        }
                        const auto target =
                            nextAcceptedWorker_.fetch_add(1, std::memory_order_relaxed) %
                            workers_.size();
                        workers_[target]->adoptServerSocket(std::move(linkId), handle,
                                                            std::move(remote));
                    }));
            pool_->start();

            std::vector<std::future<void>> readiness;
            readiness.reserve(workerCount);
            for (auto& worker : workers_) {
                auto ready = std::make_shared<std::promise<void>>();
                readiness.push_back(ready->get_future());
                worker->start(std::move(ready));
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
        if (!pool_) {
            workers_.clear();
            return;
        }
        std::vector<std::future<void>> stopped;
        stopped.reserve(workers_.size());
        for (std::size_t index = 0; index < workers_.size(); ++index) {
            auto completion = std::make_shared<std::promise<void>>();
            stopped.push_back(completion->get_future());
            auto* worker = workers_[index].get();
            asio::co_spawn(
                pool_->loop(index).ioContext(),
                ruvia::detail::taskAsAwaitable(worker->shutdown()),
                [completion](std::exception_ptr error) {
                    try {
                        if (error)
                            completion->set_exception(std::move(error));
                        else
                            completion->set_value();
                    } catch (...) {
                    }
                });
        }
        for (auto& completion : stopped) {
            try {
                completion.get();
            } catch (...) {
            }
        }
        pool_->stop();
        try {
            pool_->join();
        } catch (...) {
        }
        workers_.clear();
        pool_.reset();
    }

    [[nodiscard]] static std::size_t defaultWorkerCount() noexcept {
        const auto cpu = std::max(2U, std::thread::hardware_concurrency());
        return std::max<std::size_t>(1, cpu / 2U);
    }

  private:
    std::unique_ptr<ruvia::EventLoopPool> pool_;
    std::vector<std::unique_ptr<Worker>> workers_;
    std::atomic_bool running_{false};
    std::atomic<std::size_t> nextAcceptedWorker_{0};
};

} // namespace service::collector
