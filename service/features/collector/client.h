#pragma once

#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/web/detail/redis/RedisRegistry.h>
#include <ruvia/web/redis/Redis.h>

namespace service::collector {

// Collector's protocol actor already owns its EventLoop and keeps Redis operations
// inside the same structured TaskScope. This worker-affine adapter provides one
// connection for business commands and one dedicated connection for blocking reads.
// Passing the owning WorkerHandle lets Ruvia arm exact deadline timers instead of
// relying on a periodic deadline scan.
class Client final {
  public:
    Client(asio::io_context& ioContext, ruvia::RedisConfig config,
           const ruvia::WorkerHandle& worker)
        : resource_(),
          pool_(ioContext,
                ruvia::detail::RedisConfigStorage(oneConnection(std::move(config)), &resource_),
                1, &resource_, &worker) {}

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    ~Client() { close(); }

    [[nodiscard]] ruvia::Task<void> connect() {
        co_await pool_.connect();
    }

    void close() noexcept {
        if (closed_)
            return;
        closed_ = true;
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

    mutable std::pmr::unsynchronized_pool_resource resource_;
    mutable ruvia::detail::RedisPool pool_;
    bool closed_ = false;
};

} // namespace service::collector
