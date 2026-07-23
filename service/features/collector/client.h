#pragma once

#include <chrono>
#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/web/detail/redis/RedisInternal.h>
#include <ruvia/web/redis/Redis.h>

#include "service/features/collector/timer.h"

namespace service::collector {

// Ruvia 0.1.5 exposes a generic EventLoopPool but not a public Redis context for
// non-HTTP workers. This worker-affine adapter keeps that limitation contained in
// one file. Every Collector Worker owns exactly one adapter and its Redis pool size is
// forced to one connection.
class Client final {
  public:
    Client(asio::io_context& ioContext, ruvia::RedisConfig config,
                      Timer& scheduler)
        : resource_(), scheduler_(scheduler), pool_(ioContext, oneConnection(std::move(config)),
                                                       &resource_) {}

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    ~Client() { close(); }

    [[nodiscard]] ruvia::Task<void> connect() {
        co_await pool_.connect();
        scheduleDeadlineScan();
    }

    void close() noexcept {
        if (closed_)
            return;
        closed_ = true;
        if (deadlineScanToken_ != 0)
            scheduler_.cancel(deadlineScanToken_);
        pool_.closeNow();
    }

    [[nodiscard]] ruvia::Task<ruvia::RedisValue>
    command(std::span<const std::string_view> args) const {
        std::pmr::vector<std::pmr::string> owned(&resource_);
        owned.reserve(args.size());
        for (const auto argument : args)
            owned.emplace_back(argument);
        co_return co_await pool_.executeOwned(std::move(owned), &resource_);
    }

    [[nodiscard]] ruvia::Task<ruvia::RedisValue>
    eval(std::string_view script, std::span<const std::string_view> keys = {},
         std::span<const std::string_view> args = {}) const {
        std::pmr::vector<std::pmr::string> commandArgs(&resource_);
        commandArgs.reserve(3 + keys.size() + args.size());
        commandArgs.emplace_back("EVAL");
        commandArgs.emplace_back(script);
        commandArgs.emplace_back(std::to_string(keys.size()));
        for (const auto key : keys)
            commandArgs.emplace_back(key);
        for (const auto argument : args)
            commandArgs.emplace_back(argument);
        co_return co_await pool_.executeOwned(std::move(commandArgs), &resource_);
    }

  private:
    static ruvia::RedisConfig oneConnection(ruvia::RedisConfig config) {
        config.poolSizePerWorker = 1;
        return config;
    }

    void scheduleDeadlineScan() {
        if (closed_)
            return;
        deadlineScanToken_ = scheduler_.scheduleAfter(kScanInterval, [this] {
            if (closed_)
                return;
            pool_.scanDeadlines(std::chrono::steady_clock::now());
            scheduleDeadlineScan();
        });
    }

    static constexpr auto kScanInterval = std::chrono::milliseconds(25);

    mutable std::pmr::unsynchronized_pool_resource resource_;
    Timer& scheduler_;
    mutable ruvia::detail::RedisPool pool_;
    Timer::Token deadlineScanToken_ = 0;
    bool closed_ = false;
};

} // namespace service::collector
